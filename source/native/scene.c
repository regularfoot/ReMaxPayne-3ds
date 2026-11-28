#include "scene.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

enum { BLOCK_SIZE=16 };
typedef SceneBounds Bounds;
static Bounds *blocks,*owned_blocks;
static const Triangle *triangles;
static const uint8_t *masks;
static unsigned triangle_count,block_count,block_capacity;
static unsigned body_contacts[32],body_contact_count;
bool scene_body_touched(unsigned first,unsigned count){
    for(unsigned i=0;i<body_contact_count;i++)if(body_contacts[i]>=first && body_contacts[i]-first<count)return true;
    return false;
}

static Point add(Point a, Point b,float scale) { return (Point){a.x+b.x*scale,a.y+b.y*scale,a.z+b.z*scale}; }
static Point cross(Point a,Point b) { return (Point){a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x}; }
static float dot(Point a,Point b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
static Point unit(Point v) { float d=sqrtf(dot(v,v)); return d>1e-6f?(Point){v.x/d,v.y/d,v.z/d}:(Point){0}; }
static Point low(Point a,Point b) { return (Point){fminf(a.x,b.x),fminf(a.y,b.y),fminf(a.z,b.z)}; }
static Point high(Point a,Point b) { return (Point){fmaxf(a.x,b.x),fmaxf(a.y,b.y),fmaxf(a.z,b.z)}; }

void scene_face_bounds(const Triangle *faces,unsigned count,Point *lo,Point *hi) {
    *lo=*hi=faces[0].a;
    for(unsigned i=0;i<count;i++){
        *lo=low(*lo,low(faces[i].a,low(faces[i].b,faces[i].c)));
        *hi=high(*hi,high(faces[i].a,high(faces[i].b,faces[i].c)));
    }
}

bool scene_open(const Triangle *faces,unsigned count) {
    return scene_open_masks(faces,count,NULL);
}
bool scene_open_masks(const Triangle *faces,unsigned count,const uint8_t *mask) {
    return scene_open_bounds(faces,count,mask,NULL);
}
bool scene_reserve(unsigned faces){
    unsigned needed=(faces+BLOCK_SIZE-1)/BLOCK_SIZE;
    if(needed<=block_capacity)return true;
    Bounds *space=realloc(owned_blocks,needed*sizeof(*blocks));
    if(!space)return false;
    owned_blocks=space;block_capacity=needed;return true;
}
void scene_bind(const Triangle *faces,unsigned count,const uint8_t *mask,SceneBounds *bounds){
    body_contact_count=0;
    masks=mask;triangle_count=count;triangles=faces;block_count=(count+BLOCK_SIZE-1)/BLOCK_SIZE;blocks=bounds;
}
bool scene_open_bounds(const Triangle *faces,unsigned count,const uint8_t *mask,const SceneBounds *baked) {
    if(!scene_reserve(count))return false;
    scene_bind(faces,count,mask,owned_blocks);
    if(!block_count)return true;
    if(baked)memcpy(blocks,baked,block_count*sizeof(*blocks));
    else scene_refit(0,count);
    return true;
}
void scene_refit(unsigned first_face,unsigned count) {
    if(!count || first_face>=triangle_count || count>triangle_count-first_face)return;
    unsigned last=(first_face+count-1)/BLOCK_SIZE;
    for (unsigned i=first_face/BLOCK_SIZE;i<=last;i++) {
        unsigned first=i*BLOCK_SIZE,n=triangle_count-first;
        if(n>BLOCK_SIZE)n=BLOCK_SIZE;
        scene_face_bounds(triangles+first,n,&blocks[i].low,&blocks[i].high);
    }
}
void scene_close(void) { free(owned_blocks);owned_blocks=NULL;blocks=NULL;triangles=NULL;masks=NULL;triangle_count=block_count=block_capacity=0; }

static bool ray_triangle_edge(const Triangle *triangle,Point start,Point dir,float *distance){
    const float origin[]={start.x,start.y,start.z},ray[]={dir.x,dir.y,dir.z};
    unsigned axis=fabsf(dir.x)>fabsf(dir.y)?0:1;
    if(fabsf(dir.z)>fabsf(ray[axis]))axis=2;
    unsigned x=(axis+1)%3,y=(axis+2)%3;
    const Point vertices[]={triangle->a,triangle->b,triangle->c};
    double px[3],py[3],depth[3];
    double sx=(double)ray[x]/ray[axis],sy=(double)ray[y]/ray[axis];
    for(unsigned i=0;i<3;i++){
        const float v[]={vertices[i].x,vertices[i].y,vertices[i].z};
        depth[i]=(double)v[axis]-origin[axis];
        px[i]=(double)v[x]-origin[x]-sx*depth[i];
        py[i]=(double)v[y]-origin[y]-sy*depth[i];
    }
    // Project along the ray. A shared edge uses the same endpoints in both
    // triangles, so its edge tests agree even when the fast path rounded out.
    double a=px[1]*py[2]-py[1]*px[2],b=px[2]*py[0]-py[2]*px[0],c=px[0]*py[1]-py[0]*px[1];
    if((a<0 || b<0 || c<0) && (a>0 || b>0 || c>0))return false;
    double area=a+b+c;
    if(!area)return false;
    *distance=(a*depth[0]+b*depth[1]+c*depth[2])/(area*ray[axis]);
    return true;
}
bool scene_triangle(const Triangle *t,Point start,Point dir,float *nearest,Point *normal) {
    Point ab=add(t->b,t->a,-1),ac=add(t->c,t->a,-1),h=cross(dir,ac);
    float det=dot(ab,h);
    if (fabsf(det)<.000001f) return false;
    Point delta=add(start,t->a,-1);
    float u=dot(delta,h)/det;
    const float edge_margin=.0001f;
    if (u<-edge_margin || u>1+edge_margin) return false;
    Point q=cross(delta,ab);
    float v=dot(dir,q)/det;
    if (v<-edge_margin || u+v>1+edge_margin) return false;
    float distance=dot(ac,q)/det;
    if((u<0 || v<0 || u+v>1) && !ray_triangle_edge(t,start,dir,&distance))return false;
    if (distance<.001f || distance>*nearest) return false;
    *nearest=distance;*normal=cross(ab,ac);return true;
}
bool scene_trace_mask(Point start,Point dir,float length,unsigned mask,Hit *hit) {
    Point end=add(start,dir,length),lo=low(start,end),hi=high(start,end),normal={0};
    float nearest=length;
    bool found=false;unsigned face=0;
    for (unsigned i=0;i<block_count;i++) {
        const Bounds b=blocks[i];
        if (hi.x<b.low.x || lo.x>b.high.x || hi.y<b.low.y || lo.y>b.high.y || hi.z<b.low.z || lo.z>b.high.z) continue;
        unsigned limit=(i+1)*BLOCK_SIZE;
        if (limit>triangle_count) limit=triangle_count;
        for (unsigned j=i*BLOCK_SIZE;j<limit;j++) {
            if(masks && !(masks[j]&mask))continue;
            const Triangle *t=triangles+j;
            if(scene_triangle(t,start,dir,&nearest,&normal)){found=true;face=j;}
        }
    }
    if (found) {
        normal=unit(normal);
        if (dot(normal,dir)>0) normal=add((Point){0},normal,-1);
        *hit=(Hit){add(start,dir,nearest),normal,nearest,face};
    }
    return found;
}
bool scene_trace(Point start,Point dir,float length,Hit *hit) {return scene_trace_mask(start,dir,length,SCENE_BODY,hit);}
bool scene_toggle_crouch(Player *p){
    Hit ceiling;Point head=p->position;head.y+=1.06f;
    if(p->crouched && scene_trace(head,(Point){0,1,0},.65f,&ceiling))return false;
    p->crouched=!p->crouched;return true;
}
bool scene_ground(Point feet,Hit *hit) {
    feet.y+=.12f;return scene_trace(feet,(Point){0,-1,0},.24f,hit) && hit->normal.y>.58f;
}

/* Sweep the center and corners of the near plane along the camera arm. */
static Point sweep(Point pivot,Point boom,Point right,Point up,CameraTrace camera_trace) {
    float length=sqrtf(dot(boom,boom));
    if (length<.001f) return (Point){0};
    Point dir=unit(boom);
    const float corners[][2]={{0,0},{-.12f,-.08f},{.12f,-.08f},{-.12f,.08f},{.12f,.08f}};
    for (unsigned i=0;i<sizeof corners/sizeof corners[0];i++) {
        Point start=add(add(pivot,right,corners[i][0]),up,corners[i][1]);
        Hit hit;
        if (camera_trace(start,dir,length,&hit)) length=fmaxf(0,hit.distance-.10f);
    }
    return add((Point){0},dir,length);
}
void scene_clip_camera(Camera *camera,Point pivot,CameraTrace trace){
    Point arm=add(camera->eye,pivot,-1);
    Point clear=sweep(pivot,arm,camera->right,camera->up,trace);
    camera->eye=add(pivot,clear,1);
    camera->distance=sqrtf(dot(clear,clear));
}
// Keep the authored shot when the subject fits inside the lens. If the arm hits
// geometry, look back at the body from the clipped eye or find a clear side.
static const float body_clearance=1.85f;
static bool frame_body(Camera *c,Point pivot,Point arm,CameraTrace trace){
    Point forward=unit(add((Point){0},arm,-1));
    Point right=unit(cross((Point){0,1,0},forward)),up=cross(forward,right);
    Point clear=sweep(pivot,arm,right,up,trace);
    if(dot(clear,clear)<body_clearance*body_clearance || clear.y<-.30f)return false;
    c->eye=add(pivot,clear,1);c->distance=sqrtf(dot(clear,clear));
    c->forward=forward;c->right=right;c->up=up;return true;
}
bool scene_death_camera(Camera *c,Point pivot,CameraTrace trace){
    Point authored=add(c->eye,pivot,-1);
    float length=sqrtf(dot(authored,authored));
    scene_clip_camera(c,pivot,trace);
    if(c->distance>=body_clearance && authored.y>=-.30f && c->distance>=length-.001f &&
       dot(unit(authored),c->forward)<-.93f)return true;
    // Several retail orbit tracks pass within a metre of the feet or below the
    // floor. Keep their azimuth as they move through the clearance envelope.
    if(authored.x*authored.x+authored.z*authored.z>.0025f){
        Point arm=authored;arm.y=fmaxf(arm.y,.12f);
        float scale=fmaxf(1,(body_clearance+.02f)/sqrtf(dot(arm,arm)));
        if(frame_body(c,pivot,add((Point){0},arm,scale),trace))return true;
    }
    Point back={authored.x,0,authored.z};
    if(dot(back,back)<.0025f)back=(Point){0,0,-1};
    back=unit(back);
    Point arms[]={back,{-back.z,0,back.x},{back.z,0,-back.x},{-back.x,0,-back.z}};
    for(unsigned i=0;i<4;i++){
        Point arm={2.45f*arms[i].x,.85f,2.45f*arms[i].z};
        if(frame_body(c,pivot,arm,trace))return true;
    }
    return false;
}
void scene_pause_begin(PauseCamera *o,const Camera *entry,const Player *p){
    Point pivot=p->position;pivot.y+=p->crouched?.76f:1.15f;
    Point arm=add(entry->eye,pivot,-1);
    *o=(PauseCamera){.view=*entry,.pivot=pivot,.angle=atan2f(arm.x,arm.z),.direction=1,
        .radius=fminf(2.6f,fmaxf(2.2f,hypotf(arm.x,arm.z))),
        .height=fminf(.9f,fmaxf(.25f,arm.y))};
}
void scene_pause_step(PauseCamera *o,float dt,CameraTrace trace){
    // Reverse at a wall instead of orbiting through it or snapping to another side.
    float step=fminf(.05f,fmaxf(0,dt))*.22f;
    for(unsigned attempt=0;attempt<2;attempt++){
        float angle=o->angle+step*o->direction;
        Point arm={o->radius*sinf(angle),o->height,o->radius*cosf(angle)};
        Camera candidate=o->view;
        if(frame_body(&candidate,o->pivot,arm,trace) && candidate.distance>=hypotf(o->radius,o->height)-.001f){
            o->angle=angle;o->view=candidate;o->orbiting=true;return;
        }
        o->direction=-o->direction;
    }
}
void scene_camera(Camera *c,const Player *p,float dt){
    scene_camera_sweep(c,p,dt,scene_trace);
}
void scene_camera_sweep(Camera *c,const Player *p,float dt,CameraTrace camera_trace) {
    float s=sinf(p->yaw),co=cosf(p->yaw),cp=cosf(p->pitch),sp=sinf(p->pitch);
    Point forward={s*cp,sp,co*cp},right={co,0,-s},up=cross(forward,right);
    Point pivot=p->position;
    pivot.y+=p->crouched?.91f:1.42f;
    Point centered=add(add((Point){0},forward,-2.25f),up,.15f);
    Point boom=add(centered,right,.62f);
    Point wanted=sweep(pivot,boom,right,up,camera_trace);
    bool center_clear=false;
    // When a door leaf or jamb brushes the shoulder, first try sliding inward.
    // Retract toward the player only when the central sightline is blocked too.
    if(dot(wanted,forward)>-1.95f){
        Point clear=sweep(pivot,centered,right,up,camera_trace);
        if(dot(clear,forward)<dot(wanted,forward)-.18f){wanted=clear;center_clear=true;}
    }
    if (dt>0 && c->distance>.001f) {
        float blend=fminf(1,dt*12);
        Point next=add(c->offset,add(wanted,c->offset,-1),blend);
        // A shorter arm along the same ray is already covered by the five collision probes.
        Point turn=cross(boom,next);
        if(dot(turn,turn)<1.e-10f && dot(next,boom)>=0){
            if(dot(next,next)<dot(wanted,wanted))wanted=next;
        }else{
            Point safe=sweep(pivot,next,right,up,camera_trace);
            // A blend that still intersects the jamb would lurch forward for
            // one frame. Take the verified central arm as soon as it is clear.
            if(!center_clear || dot(safe,forward)<=dot(wanted,forward)+.05f)wanted=safe;
        }
    }
    c->offset=wanted;
    c->distance=sqrtf(dot(wanted,wanted));
    c->eye=add(pivot,wanted,1);
    // Keep the reticle on the original line of sight when the camera arm retracts.
    Point target=add(add(pivot,boom,1),forward,8);
    c->forward=unit(add(target,c->eye,-1));
    c->right=unit(cross((Point){0,1,0},c->forward));
    c->up=cross(c->forward,c->right);
}
// The shoulder and the lens have different sight lines at close range. Use
// the selected target depth for both the reticle and the ensuing shot.
void scene_camera_converge(Camera *c,const Player *p,float distance,float weight){
    Point pivot=p->position;
    pivot.y+=p->crouched?.91f:1.42f;
    float cp=cosf(p->pitch);
    Point sight={sinf(p->yaw)*cp,sinf(p->pitch),cosf(p->yaw)*cp};
    Point target=add(pivot,sight,distance);
    Point focused=unit(add(target,c->eye,-1));
    c->forward=unit(add(c->forward,add(focused,c->forward,-1),weight));
    c->right=unit(cross((Point){0,1,0},c->forward));
    c->up=cross(c->forward,c->right);
}
void scene_scope(Camera *c,const Player *p) {
    float s=sinf(p->yaw),co=cosf(p->yaw),cp=cosf(p->pitch);
    c->eye=p->position;
    c->eye.y+=p->crouched?.99f:1.52f;
    c->forward=(Point){s*cp,sinf(p->pitch),co*cp};
    c->right=(Point){co,0,-s};
    c->up=cross(c->forward,c->right);
    c->distance=0;
}

bool scene_bodies(Point from,Point dir,float limit,const ShotTarget *targets,unsigned count,Hit *hit,int *actor) {
    bool found=false;
    *actor=-1;
    for (unsigned i=0;i<count;i++) {
        if (!targets[i].active) continue;
        float r=targets[i].radius>0?targets[i].radius:.30f;
        float base=targets[i].feet.y;
        float lo=targets[i].radius>0?targets[i].bottom+r:.28f;
        float hi=targets[i].radius>0?targets[i].top-r:1.51f;
        if(targets[i].crouched && hi>1)hi=fminf(hi,1.08f-r);
        hi=base+fmaxf(lo,hi);lo+=base;
        // Intersect the cylinder and rounded ends. A muzzle inside another
        // actor touches it immediately, including when pointed out of the body.
        float x=from.x-targets[i].feet.x,z=from.z-targets[i].feet.z;
        float y=from.y-fmaxf(lo,fminf(hi,from.y));
        float near=INFINITY,flat=x*x+z*z;
        if(flat+y*y<=r*r)near=0;
        else {
            float a=dir.x*dir.x+dir.z*dir.z,b=x*dir.x+z*dir.z;
            float disc=b*b-a*(flat-r*r);
            if(a>1e-8f && disc>=0){
                float t=(-b-sqrtf(disc))/a,h=from.y+t*dir.y;
                if(t>=0 && h>=lo && h<=hi)near=t;
            }
            for(unsigned end=0;end<2;end++){
                float dy=from.y-(end?hi:lo),projection=b+dy*dir.y;
                float root=projection*projection-(flat+dy*dy-r*r);
                if(root<0)continue;
                float t=-projection-sqrtf(root);
                if(t>=0 && t<near)near=t;
            }
        }
        if(near>=limit)continue;
        limit=near;
        hit->distance=near;hit->point=add(from,dir,near);
        Point center=targets[i].feet;center.y=fmaxf(lo,fminf(hi,hit->point.y));
        Point normal=add(hit->point,center,-1);
        hit->normal=dot(normal,normal)>1e-10f?unit(normal):add((Point){0},dir,-1);
        *actor=(int)i;hit->face=UINT32_MAX;found=true;
    }
    return found;
}
bool scene_cast(Point from,Point dir,float limit,const ShotTarget *targets,unsigned count,Hit *hit,int *actor) {
    bool found=scene_trace_mask(from,dir,limit,SCENE_SHOT,hit);
    if (found) limit=hit->distance;
    return scene_bodies(from,dir,limit,targets,count,hit,actor) || found;
}
bool scene_fire(Effects *effects,const Camera *c,Point shoulder,Point muzzle,Hit *hit,
                const ShotTarget *targets,unsigned count,int *actor) {
    effects->muzzle=muzzle;effects->flash=.075f;effects->flash_room=0;
    Hit aimed;
    Point target=scene_cast(c->eye,c->forward,100,targets,count,&aimed,actor)?aimed.point:add(c->eye,c->forward,100);
    Point path=add(target,muzzle,-1),reach=add(muzzle,shoulder,-1);
    *actor=-1;
    if (scene_trace_mask(shoulder,unit(reach),sqrtf(dot(reach,reach)),SCENE_SHOT,hit)) effects->muzzle=hit->point;
    else if (!scene_cast(muzzle,unit(path),sqrtf(dot(path,path))+.06f,targets,count,hit,actor)) return false;
    if (*actor<0) {
        Impact *mark=&effects->marks[effects->next++%SURFACE_MARKS];
        *mark=(Impact){.point=hit->point,.normal=hit->normal,.life=18};
    }
    return true;
}
bool scene_shot(Effects *fx,const Camera *c,Point shoulder,Point muzzle,Hit *hit) {
    int actor;
    return scene_fire(fx,c,shoulder,muzzle,hit,NULL,0,&actor);
}
void scene_age(Effects *effects,float dt) {
    effects->flash=fmaxf(0,effects->flash-dt);
    for (unsigned i=0;i<SURFACE_MARKS;i++) effects->marks[i].life=fmaxf(0,effects->marks[i].life-dt);
}

/* Clip a wall to the player's vertical extent before checking its horizontal
 * footprint. This also catches hanging objects such as the tutorial payphone. */
static unsigned clip_height(const Point *input,unsigned count,Point *out,float height,bool above) {
    unsigned n=0;
    if (!count) return 0;
    Point prev=input[count-1];
    bool inside=above?prev.y>=height:prev.y<=height;
    for (unsigned i=0;i<count;i++) {
        Point v=input[i];
        bool next=above?v.y>=height:v.y<=height;
        if (inside!=next) {
            float t=(height-prev.y)/(v.y-prev.y);
            out[n++]=(Point){prev.x+t*(v.x-prev.x),height,prev.z+t*(v.z-prev.z)};
        }
        if (next) out[n++]=v;
        prev=v;inside=next;
    }
    return n;
}
static float wall_distance(const Point *vertices,unsigned count,float x,float z) {
    float nearest=INFINITY;
    bool inside=false;
    for (unsigned i=0,j=count-1;i<count;j=i++) {
        Point a=vertices[j],b=vertices[i];
        float dx=b.x-a.x,dz=b.z-a.z,len=dx*dx+dz*dz;
        float t=len>0?((x-a.x)*dx+(z-a.z)*dz)/len:0;
        t=fmaxf(0,fminf(1,t));
        float ex=x-a.x-t*dx,ez=z-a.z-t*dz;
        nearest=fminf(nearest,ex*ex+ez*ez);
        if ((a.z>z)!=(b.z>z) && x<a.x+(z-a.z)*dx/dz) inside=!inside;
    }
    return inside?0:nearest;
}

static float wall_sweep_distance(const Point *wall,unsigned count,Point from,Point to){
    float nearest=fminf(wall_distance(wall,count,from.x,from.z),wall_distance(wall,count,to.x,to.z));
    float dx=to.x-from.x,dz=to.z-from.z,length=dx*dx+dz*dz;
    if(length<1.e-12f)return nearest;
    for(unsigned i=0,j=count-1;i<count;j=i++){
        Point a=wall[j],b=wall[i];
        float t=fminf(1,fmaxf(0,((a.x-from.x)*dx+(a.z-from.z)*dz)/length));
        float x=from.x+t*dx-a.x,z=from.z+t*dz-a.z;
        nearest=fminf(nearest,x*x+z*z);
        float ex=b.x-a.x,ez=b.z-a.z,det=dx*ez-dz*ex;
        if(fabsf(det)<1.e-8f)continue;
        float ax=a.x-from.x,az=a.z-from.z;
        float along=(ax*ez-az*ex)/det,edge=(ax*dz-az*dx)/det;
        if(along>=0 && along<=1 && edge>=0 && edge<=1)return 0;
    }
    return nearest;
}

static unsigned body_wall(const Triangle *t,const Player *body,bool airborne,Point wall[6]){
    Point vertices[3]={t->a,t->b,t->c},clipped[4];
    unsigned n=clip_height(vertices,3,clipped,body->position.y+(airborne?.035f:.34f),true);
    return clip_height(clipped,n,wall,body->position.y+(body->crouched?1.08f:1.70f),false);
}
float scene_overhead_distance(const Triangle *face,const Player *body){
    Point wall[6];unsigned n=body_wall(face,body,false,wall);
    return n?wall_distance(wall,n,body->position.x,body->position.z):INFINITY;
}
float scene_panel_distance(const Triangle *face,const Player *body){
    Point u=add(face->b,face->a,-1),v=add(face->c,face->a,-1),axis=cross(u,v);
    if(axis.y*axis.y>=.62f*.62f*dot(axis,axis))return INFINITY;
    return scene_overhead_distance(face,body);
}
bool scene_panel_blocks(const Triangle *face,const Player *body,Point target,bool airborne){
    Point u=add(face->b,face->a,-1),v=add(face->c,face->a,-1),axis=cross(u,v);
    if(axis.y*axis.y>=.62f*.62f*dot(axis,axis))return false;
    Point wall[6];unsigned count=body_wall(face,body,airborne,wall);
    if(!count)return false;
    float before=wall_distance(wall,count,body->position.x,body->position.z);
    float swept=wall_sweep_distance(wall,count,body->position,target);
    // A touching capsule may slide or retreat, while crossing the leaf moves closer.
    return swept<.22f*.22f && swept<before-1.e-8f;
}
bool scene_wall_overlap(unsigned first,unsigned count,const Player *body,bool airborne){
    Point p=body->position;
    for(unsigned i=first;i<first+count;){
        Bounds b=blocks[i/BLOCK_SIZE];unsigned end=(i/BLOCK_SIZE+1)*BLOCK_SIZE;
        if(end>first+count)end=first+count;
        if(p.x+.22f<b.low.x || p.x-.22f>b.high.x || p.z+.22f<b.low.z || p.z-.22f>b.high.z ||
           p.y+1.70f<b.low.y || p.y>b.high.y){i=end;continue;}
        for(;i<end;i++){
            if(fabsf(triangles[i].normal_y)>=.62f)continue;
            Point wall[6];unsigned n=body_wall(triangles+i,body,airborne,wall);
            if(n && wall_distance(wall,n,p.x,p.z)<.22f*.22f)return true;
        }
    }
    return false;
}
static void try_step_lift(Player *body,float x,float z,bool airborne,bool *player_airborne,bool edge_step) {
    Point *p=&body->position;
    float floor_y=-INFINITY;bool blocked=false;
    float lo_x=fminf(x,p->x)-.22f,hi_x=fmaxf(x,p->x)+.22f;
    float lo_z=fminf(z,p->z)-.22f,hi_z=fmaxf(z,p->z)+.22f;
    for (unsigned group=0;group<block_count;group++) {
      const Bounds bnd=blocks[group];
      if (hi_x<bnd.low.x || lo_x>bnd.high.x || hi_z<bnd.low.z || lo_z>bnd.high.z ||
          p->y+1.7f<bnd.low.y || p->y-.61f>bnd.high.y) continue;
      unsigned end=(group+1)*BLOCK_SIZE;
      if (end>triangle_count) end=triangle_count;
      for (unsigned i=group*BLOCK_SIZE;i<end;i++) {
        if(masks && !(masks[i]&SCENE_BODY)) continue;
        const Triangle *t=&triangles[i];
        const Point a=t->a,b=t->b,c=t->c;
        if ((hi_x<a.x && hi_x<b.x && hi_x<c.x) || (lo_x>a.x && lo_x>b.x && lo_x>c.x) ||
            (hi_z<a.z && hi_z<b.z && hi_z<c.z) || (lo_z>a.z && lo_z>b.z && lo_z>c.z)) continue;
        float bx=b.x-a.x,bz=b.z-a.z,cx=c.x-a.x,cz=c.z-a.z;
        if (t->normal_y>.62f) {
            float det=bx*cz-bz*cx;
            if (fabsf(det)<1e-7f) continue;
            float toe_x=x,toe_z=z;
            if(airborne && edge_step){
                float dx=x-p->x,dz=z-p->z,length=hypotf(dx,dz);
                if(length>.00001f){toe_x+=dx*.22f/length;toe_z+=dz*.22f/length;}
            }
            float u=((toe_x-a.x)*cz-(toe_z-a.z)*cx)/det, v=(bx*(toe_z-a.z)-bz*(toe_x-a.x))/det;
            if(!airborne && (u<0 || v<0 || u+v>1)){
                // Support the capsule's footprint, not a single point between
                // its feet. Otherwise the narrow platform/train sill gap drops
                // Max onto the track before he can step into the open door.
                Point corners[3]={a,b,c};float nearest=.23f*.23f;
                for(unsigned edge=0;edge<3;edge++){
                    Point q=corners[edge],r=corners[(edge+1)%3];
                    float ex=r.x-q.x,ez=r.z-q.z,length=ex*ex+ez*ez;
                    float along=length>1.e-12f?fminf(1,fmaxf(0,((x-q.x)*ex+(z-q.z)*ez)/length)):0;
                    float px=q.x+along*ex,pz=q.z+along*ez,dx=x-px,dz=z-pz;
                    if(dx*dx+dz*dz>nearest)continue;
                    nearest=dx*dx+dz*dz;
                    u=((px-a.x)*cz-(pz-a.z)*cx)/det;
                    v=(bx*(pz-a.z)-bz*(px-a.x))/det;
                }
            }
            if (u>=-.0001f && v>=-.0001f && u+v<=1.0001f) {
                float y=a.y+u*(b.y-a.y)+v*(c.y-a.y);
                if (y<=p->y+(airborne?.20f:.32f) && y>floor_y) floor_y=y;
            }
        } else if (fabsf(t->normal_y)<.62f) {
            if(scene_panel_blocks(t,body,(Point){x,p->y,z},airborne)){
                blocked=true;
                if(body_contact_count<sizeof body_contacts/sizeof *body_contacts)body_contacts[body_contact_count++]=i;
            }
        }
    }
    }
    if(blocked){
        // Resolve the rounded capsule's lower edge onto a nearby ledge. Treating
        // the airborne body as a flat cylinder blocks the 1.5 m training wall
        // although the authored jump comes within one capsule radius of its top.
        float rise=floor_y-p->y;
        if(airborne && edge_step && rise>0 && rise<=.20f){
            Point head=*p;head.y+=body->crouched?1.06f:1.68f;Hit ceiling;
            if(!scene_trace(head,(Point){0,1,0},rise+.022f,&ceiling)){
                Player lifted=*body;lifted.position.y=floor_y+.002f;
                try_step_lift(&lifted,x,z,true,player_airborne,false);
                if(fabsf(lifted.position.x-x)<.00001f && fabsf(lifted.position.z-z)<.00001f)*body=lifted;
            }
        }
        return;
    }
    if (airborne) *p=(Point){x,p->y,z};
    else if (floor_y>=p->y-.60f) *p=(Point){x,floor_y,z};
    else if (player_airborne) { *player_airborne=true;*p=(Point){x,p->y,z}; }
}

static void try_step(Player *p,float x,float z,bool airborne,bool *state){try_step_lift(p,x,z,airborne,state,true);}

void room_move(Player *p,float dx,float dz) {
    body_contact_count=0;
    if (dx) try_step(p,p->position.x+dx,p->position.z,false,NULL);
    if (dz) try_step(p,p->position.x,p->position.z+dz,false,NULL);
    // NPC motion must not leave contacts for the player's next script pass.
    body_contact_count=0;
}


// A portal may bisect a staircase. Resolve the receiving tread from above
// within the same step-up and step-down limits as horizontal movement.
bool scene_settle_step(Player *player,Jump *jump) {
    Point from=player->position;from.y+=.321f;
    Hit hit;
    if(!scene_trace(from,(Point){0,-1,0},.921f,&hit) || hit.normal.y<=.62f)return false;
    player->position.y=hit.point.y;*jump=(Jump){0};return true;
}

void scene_move_player(Player *p,Jump *j,float dx,float dz,float dt,bool takeoff) {
    scene_move_gravity(p,j,dx,dz,dt,takeoff,14.f,8.25f);
}
void scene_move_gravity(Player *p,Jump *j,float dx,float dz,float dt,bool takeoff,float gravity,float launch){
    body_contact_count=0;
    if (!j->airborne && !takeoff) {
        // A destroyed or withdrawn support must also release a stationary body.
        if (!dx && !dz) try_step(p,p->position.x,p->position.z,false,&j->airborne);
        if (dx) try_step(p,p->position.x+dx,p->position.z,false,&j->airborne);
        if (dz) try_step(p,p->position.x,p->position.z+dz,j->airborne,&j->airborne);
        return;
    }
    if (!j->airborne) { j->airborne=true;j->velocity=launch; }
    float dy=(j->velocity-.5f*gravity*dt)*dt;
    j->velocity-=gravity*dt;
    Hit hit;
    if (dy>0) {
        Point head=p->position;head.y+=p->crouched?1.06f:1.68f;
        if (scene_trace(head,(Point){0,1,0},dy+.02f,&hit)) {
            dy=fmaxf(0,hit.distance-.02f);
            j->velocity=0;
        }
        p->position.y+=dy;
    }
    if (dx) try_step(p,p->position.x+dx,p->position.z,true,&j->airborne);
    if (dz) try_step(p,p->position.x,p->position.z+dz,true,&j->airborne);
    if (dy<=0) {
        Point feet=p->position;feet.y+=.06f;
        if (scene_trace(feet,(Point){0,-1,0},.065f-dy,&hit) && hit.normal.y>.62f) {
            p->position.y=hit.point.y;*j=(Jump){0};
        } else p->position.y+=dy;
    }
}
