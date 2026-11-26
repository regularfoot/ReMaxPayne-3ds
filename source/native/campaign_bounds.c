#include "campaign_bounds.h"
#include "campaign_file.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint16_t room,prop,first,count;
    Point lo,hi;
} Bounds;
typedef struct {Point a,b,c;} Face;
static Bounds *boxes;
static Face *faces;
static Bounds *by_prop[CAMPAIGN_PROPS];
static unsigned room_begin[129];

void campaign_bounds_close(void) {
    free(boxes);free(faces);boxes=NULL;faces=NULL;
    memset(by_prop,0,sizeof by_prop);memset(room_begin,0,sizeof room_begin);
}
bool campaign_bounds_open(const char *stage) {
    campaign_bounds_close();
    char name[64];snprintf(name,sizeof name,"%s.bounds",stage);
    FILE *f=campaign_file(name);if(!f)return false;
    struct {char tag[4];uint32_t boxes,faces;} h;
    bool ok=fread(&h,sizeof h,1,f)==1 && !memcmp(h.tag,"MPB1",4) && h.boxes<=CAMPAIGN_PROPS && h.faces<=65535;
    if(ok) {
        boxes=calloc(h.boxes+1,sizeof(*boxes));faces=calloc(h.faces+1,sizeof(*faces));
        ok=boxes && faces && fread(boxes,sizeof(*boxes),h.boxes,f)==h.boxes && fread(faces,sizeof(*faces),h.faces,f)==h.faces && fgetc(f)==EOF;
    }
    for(unsigned i=0;ok && i<h.boxes;i++) {
        Bounds *b=boxes+i;
        ok=b->room<128 && (!i || boxes[i-1].room<=b->room) && b->prop<CAMPAIGN_PROPS && !by_prop[b->prop] && b->first+b->count<=h.faces;
        if(ok) {by_prop[b->prop]=b;room_begin[b->room+1]++;}
    }
    fclose(f);
    if(!ok){campaign_bounds_close();return false;}
    for(unsigned r=1;r<=128;r++)room_begin[r]+=room_begin[r-1];
    return true;
}
static float triple(Point a,Point b,Point c) {
    return a.x*(b.y*c.z-b.z*c.y)+a.y*(b.z*c.x-b.x*c.z)+a.z*(b.x*c.y-b.y*c.x);
}
static bool inverse_point(const float *m,Point feet,Point *out) {
    Point x={m[0],m[1],m[2]},y={m[3],m[4],m[5]},z={m[6],m[7],m[8]};
    Point delta={feet.x-m[9],feet.y-m[10],feet.z-m[11]};
    float det=triple(x,y,z);if(fabsf(det)<.00001f)return false;
    *out=(Point){triple(delta,y,z)/det,triple(x,delta,z)/det,triple(x,y,delta)/det};
    return true;
}
static bool local_point(unsigned prop,Point feet,Point *out) {
    const CampaignProp *p=campaign_props+prop;
    if(!p->moved){*out=feet;return true;}
    return inverse_point(p->matrix,feet,out);
}
static bool contains(const Bounds *b,Point p,float r) {
    return p.x>=b->lo.x-r && p.x<=b->hi.x+r && p.y>=b->lo.y-r && p.y<=b->hi.y+r && p.z>=b->lo.z-r && p.z<=b->hi.z+r;
}
bool campaign_bounds_overlap(unsigned prop,Point p,float radius) {
    return prop<CAMPAIGN_PROPS && by_prop[prop] && !campaign_props[prop].hidden && campaign_props[prop].solid &&
        local_point(prop,p,&p) && contains(by_prop[prop],p,radius);
}
bool campaign_bounds_character(unsigned prop,Point center,float height){
    if(prop>=CAMPAIGN_PROPS || !by_prop[prop] || campaign_props[prop].hidden || !campaign_props[prop].solid)return false;
    const float radius=.22f;
    Point a=center,b=center;a.y+=radius-.9f;b.y+=height-radius-.9f;
    if(!local_point(prop,a,&a) || !local_point(prop,b,&b))return false;
    const Bounds *box=by_prop[prop];float near=0,far=1;
    float start[]={a.x,a.y,a.z},delta[]={b.x-a.x,b.y-a.y,b.z-a.z};
    float lo[]={box->lo.x-radius,box->lo.y-radius,box->lo.z-radius};
    float hi[]={box->hi.x+radius,box->hi.y+radius,box->hi.z+radius};
    for(unsigned i=0;i<3;i++){
        if(fabsf(delta[i])<.000001f){if(start[i]<lo[i] || start[i]>hi[i])return false;continue;}
        float first=(lo[i]-start[i])/delta[i],last=(hi[i]-start[i])/delta[i];
        if(first>last){float t=first;first=last;last=t;}
        near=fmaxf(near,first);far=fminf(far,last);if(near>far)return false;
    }
    return true;
}
PlatformContact campaign_platform_contact(int prop,Point feet) {
    PlatformContact c={.prop=prop,.previous=feet};
    if(prop>=0 && !local_point(prop,feet,&c.local))c.prop=-1;
    return c;
}
PlatformContact campaign_platform_placed(int prop,Point feet,const float *placement){
    Point home;
    if(!inverse_point(placement,feet,&home))return (PlatformContact){.prop=-1};
    PlatformContact c=campaign_platform_contact(prop,home);c.previous=feet;c.placed=true;
    memcpy(c.placement,placement,sizeof c.placement);return c;
}
PlatformContact campaign_remote_floor(unsigned room,Point feet) {
    PlatformContact contact={.prop=-1};
    if(room>=128)return contact;
    float best=.15f;
    for(unsigned i=room_begin[room];i<room_begin[room+1];i++) {
        const Bounds *b=boxes+i;
        if(campaign_props[b->prop].hidden || !campaign_props[b->prop].solid || (campaign_props[b->prop].collision_mask && !(campaign_props[b->prop].collision_mask&SCENE_BODY)))continue;
        PlatformContact c=campaign_platform_contact(b->prop,feet);
        if(c.prop<0 || !contains(b,c.local,.15f))continue;
        for(unsigned j=b->first;j<b->first+b->count;j++) {
            const Face *f=faces+j;
            float x=f->b.x-f->a.x,z=f->b.z-f->a.z,xx=f->c.x-f->a.x,zz=f->c.z-f->a.z;
            float det=x*zz-z*xx;if(fabsf(det)<.000001f)continue;
            float dx=c.local.x-f->a.x,dz=c.local.z-f->a.z;
            float u=(dx*zz-dz*xx)/det,v=(x*dz-z*dx)/det;
            if(u<-.001f || v<-.001f || u+v>1.001f)continue;
            float delta=fabsf(f->a.y+u*(f->b.y-f->a.y)+v*(f->c.y-f->a.y)-c.local.y);
            if(delta<best){contact=c;best=delta;}
        }
    }
    return contact;
}
void campaign_ride(PlatformContact c,Point *feet) {
    if(c.prop<0 || campaign_props[c.prop].hidden || !campaign_props[c.prop].solid || feet->x!=c.previous.x ||
       feet->y!=c.previous.y || feet->z!=c.previous.z)return;
    const CampaignProp *p=campaign_props+c.prop;
    Point a=c.local;
    if(p->moved){
        const float *m=p->matrix;
        a=(Point){m[9]+a.x*m[0]+a.y*m[3]+a.z*m[6],m[10]+a.x*m[1]+a.y*m[4]+a.z*m[7],
                  m[11]+a.x*m[2]+a.y*m[5]+a.z*m[8]};
    }
    if(c.placed){
        const float *m=c.placement;
        a=(Point){m[9]+a.x*m[0]+a.y*m[3]+a.z*m[6],m[10]+a.x*m[1]+a.y*m[4]+a.z*m[7],
                  m[11]+a.x*m[2]+a.y*m[5]+a.z*m[8]};
    }
    *feet=a;
}
