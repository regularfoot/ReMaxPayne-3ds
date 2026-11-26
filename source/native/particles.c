#include "particles.h"
#include "alpha_pass.h"
#include "campaign_file.h"
#include "campaign_world.h"
#include "director.h"
#include "texture.h"
#include "particle_layer.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

enum { LIMIT=160, PAGES=64, CLIP_VERTICES=10, TRIANGLE_VERTICES=(CLIP_VERTICES-2)*3 };
typedef struct {char name[64];uint16_t frames[4];uint8_t kind,loop;uint16_t reserved;float area;} Definition;
typedef struct {Point pos,velocity;float age,life,size;uint16_t effect,room,owner,layer,variant,flip;float angle,spin;Point low,high;} Sprite;
typedef struct {Point point;float u,v;} Vertex;
typedef struct {unsigned page,portal,first,count;bool additive;Point center;} Batch;
static Definition *defs;
static ParticleGroup *groups;
static ParticleLayer *layers;
static ParticleDrag *drag;
static struct {float mean,spread;} *lifetimes;
static uint16_t (*variants)[4];
typedef struct {Point low,high,origin,marker;unsigned room;float ttl;} ClipBox;
typedef struct {Point origin,forward,up;unsigned owner,room,id,seed;float age,deadline;bool active;} Burst;
enum { BURSTS=32, SPRITE_CLIPPED=0x8000 };
static Burst bursts[BURSTS];
static ClipBox boxes[PARTICLE_SOURCES+BURSTS];
static C3D_Tex pages[PAGES];
static unsigned num_defs,num_pages,cursor,seed;
static Sprite sprites[LIMIT];
ParticleSource particle_sources[PARTICLE_SOURCES];
static Vertex *vertices;
static C3D_BufInfo binding;
static Batch batches[LIMIT];
static unsigned count,blood_effect,police_effect;
static uint8_t police_curve[256][3];
static bool authored_pulse;
static float last_step;
static uint8_t (*shape_curves)[16];
_Static_assert(sizeof(Definition)==80,"RPS sprite definition layout");

void particles_close(void) {
    for(unsigned i=0;i<num_pages;i++)C3D_TexDelete(pages+i);
    if(vertices)linearFree(vertices);
    free(shape_curves);shape_curves=NULL;
    free(variants);variants=NULL;
    free(lifetimes);lifetimes=NULL;
    free(groups);groups=NULL;free(layers);layers=NULL;free(drag);drag=NULL;
    free(defs);defs=NULL;vertices=NULL;num_defs=num_pages=count=0;
}
bool particles_open(void) {
    particles_close();FILE *file=campaign_file("particles.pak");
    if(!file)return false;
    struct {char magic[4];uint32_t defs,pages;} h;
    bool ok=fread(&h,sizeof h,1,file)==1 && !memcmp(h.magic,"RPS1",4) && h.defs && h.defs<1024 && h.pages && h.pages<=PAGES;
    if(ok){defs=calloc(h.defs,sizeof(*defs));ok=defs && fread(defs,sizeof(*defs),h.defs,file)==h.defs;}
    for(unsigned i=0;ok && i<h.defs;i++){
        ok=memchr(defs[i].name,0,64) && defs[i].kind<=7 && isfinite(defs[i].area);
        for(unsigned k=0;k<4;k++)if(defs[i].frames[k]>=64*h.pages)ok=false;
    }
    while(ok && num_pages<h.pages){ok=native_texture_read(file,pages+num_pages);if(ok)num_pages++;}
    authored_pulse=false;
    if(ok){
        char tag[4];size_t length=fread(tag,1,4,file);
        if(length){
            ok=length==4 && !memcmp(tag,"PLC1",4) && fread(police_curve,1,sizeof police_curve,file)==sizeof police_curve;
            authored_pulse=ok;
            if(ok && (length=fread(tag,1,4,file))!=0){
                shape_curves=calloc(h.defs,sizeof *shape_curves);
                ok=length==4 && !memcmp(tag,"RSC1",4) && shape_curves &&
                    fread(shape_curves,sizeof *shape_curves,h.defs,file)==h.defs;
                if(ok && (length=fread(tag,1,4,file))!=0){
                    uint32_t n=0,sequences=0;
                    ok=length==4 && !memcmp(tag,"LAY2",4) && fread(&n,sizeof n,1,file)==1 && n && n<=4096 &&
                        fread(&sequences,sizeof sequences,1,file)==1 && sequences && sequences<=65535;
                    if(ok){
                        groups=calloc(h.defs,sizeof *groups);layers=calloc(n,sizeof *layers);variants=calloc(sequences,sizeof *variants);
                        ok=groups && layers && variants && fread(groups,sizeof *groups,h.defs,file)==h.defs &&
                            fread(layers,sizeof *layers,n,file)==n && fread(variants,sizeof *variants,sequences,file)==sequences;
                        if(ok && (length=fread(tag,1,4,file))!=0){
                            lifetimes=calloc(n,sizeof *lifetimes);
                            ok=length==4 && !memcmp(tag,"ELT1",4) && lifetimes &&
                                fread(lifetimes,sizeof *lifetimes,n,file)==n;
                            for(unsigned j=0;ok && j<n;j++)
                                ok=isfinite(lifetimes[j].mean+lifetimes[j].spread) && lifetimes[j].mean>0 && lifetimes[j].spread>=0;
                            if(ok && (length=fread(tag,1,4,file))!=0){
                                drag=calloc(n,sizeof *drag);
                                ok=length==4 && !memcmp(tag,"DRG1",4) && drag && fread(drag,sizeof *drag,n,file)==n && fgetc(file)==EOF;
                                for(unsigned j=0;ok && j<n;j++)ok=isfinite(drag[j].strength) && drag[j].size_mode<=2 && drag[j].velocity_mode<=2 && !drag[j].reserved;
                            }
                        }
                        for(unsigned k=0;ok && k<sequences;k++){
                            for(unsigned j=0;j<4;j++)if(variants[k][j]>=64*h.pages)ok=false;
                        }
                    }
                    for(unsigned i=0;ok && i<h.defs;i++){
                        ParticleGroup *g=groups+i;
                        ok=g->count && g->first+g->count<=n && g->capacity && isfinite(g->period) && g->period>=.001f;
                    }
                    for(unsigned i=0;ok && i<n;i++){
                        const ParticleLayer *l=layers+i;
                        ok=l->additive<=1 && l->sphere<=1 && l->variants && l->first_variant+l->variants<=sequences;
                        for(unsigned j=0;j<4;j++)if(l->frames[j]>=64*h.pages)ok=false;
                        float last=0;
                        for(unsigned j=0;j<8;j++){
                            float total=l->keys[j].emitted;
                            if(!isfinite(total) || total<last || total>65535)ok=false;
                            last=total;
                        }
                    }
                }
            }
            ok=ok && !ferror(file);
        }else ok=feof(file) && !ferror(file);
    }
    fclose(file);
    if(ok){
        vertices=linearAlloc(LIMIT*TRIANGLE_VERTICES*sizeof(*vertices));BufInfo_Init(&binding);
        ok=vertices && BufInfo_Add(&binding,vertices,sizeof(Vertex),2,0x10)>=0;
    }
    if(!ok){particles_close();return false;}
    num_defs=h.defs;memset(sprites,0,sizeof sprites);memset(particle_sources,0,sizeof particle_sources);
    blood_effect=police_effect=0;
    for(unsigned i=0;i<num_defs;i++){
        if(!strcmp(defs[i].name,"blood_medium"))blood_effect=i+1;
        if(!strcmp(defs[i].name,"light_policecar"))police_effect=i+1;
    }
    memset(bursts,0,sizeof bursts);memset(boxes,0,sizeof boxes);cursor=0;seed=9173;return true;
}
bool particles_valid(void) {
    for(unsigned i=0;i<PARTICLE_SOURCES;i++){
        ParticleSource *s=particle_sources+i;
        if(s->effect && (s->effect>num_defs || s->owner>=director.count || !isfinite(s->interval) || fabsf(s->interval)>1e6f))return false;
    }
    return true;
}
static float random_unit(void){seed=seed*1664525+1013904223;return (seed>>8)*(1.f/16777216.f);}
static bool nearby(unsigned room) {
    for(unsigned view=campaign_first_view(room);view!=255;view=campaign_next_view(view))
        if(campaign_view_enabled(view,director.exits))return true;
    return false;
}
static Point cross(Point a,Point b){
    return (Point){a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};
}
static Point unit(Point p){
    float len=sqrtf(p.x*p.x+p.y*p.y+p.z*p.z);
    return len>.00001f?(Point){p.x/len,p.y/len,p.z/len}:(Point){0,1,0};
}
static Point oriented(Point v,Point x,Point y,Point z){
    return (Point){v.x*x.x+v.y*y.x+v.z*z.x,v.x*x.y+v.y*y.y+v.z*z.y,v.x*x.z+v.y*y.z+v.z*z.z};
}
static Point random_direction(void){
    Point p;
    float norm;
    do {
        p=(Point){2*random_unit()-1,2*random_unit()-1,2*random_unit()-1};
        norm=p.x*p.x+p.y*p.y+p.z*p.z;
    }while(norm>1 || norm<.0001f);
    return unit(p);
}
static float emitter_period(unsigned index,const Burst *burst,float period){
    if(!burst || !lifetimes)return period;
    unsigned hash=(burst->seed+index)*2654435761u;
    float variation=2.f*(hash>>8)*(1.f/16777216.f)-1.f;
    return fmaxf(.001f,lifetimes[index].mean+variation*lifetimes[index].spread);
}
static Burst *start_burst(unsigned owner,unsigned id,unsigned room,Point at,Point forward,Point up){
    if(!nearby(room))return NULL;
    unsigned slots=id+1==blood_effect?BURSTS:BURSTS-2;
    for(unsigned i=0;i<slots;i++)if(!bursts[i].active){
        const ParticleGroup *g=groups+id;
        random_unit();
        Burst *burst=bursts+i;
        *burst=(Burst){.origin=at,.forward=forward,.up=up,.owner=owner,.room=room,.id=id,.seed=seed,.active=true};
        for(unsigned j=0;j<g->count;j++)
            burst->deadline=fmaxf(burst->deadline,emitter_period(g->first+j,burst,g->period));
        burst->deadline=fminf(burst->deadline,g->period);
        boxes[PARTICLE_SOURCES+i].ttl=0;
        return burst;
    }
    return NULL;
}
void particles_blood(unsigned owner,unsigned room,Point impact,Point direction){
    if(!defs || !blood_effect || !nearby(room))return;
    if(groups){
        start_burst(owner,blood_effect-1,room,impact,direction,(Point){0,1,0});
        return;
    }
    for(unsigned i=0;i<5;i++){
        float x=random_unit()-.5f,y=random_unit(),z=random_unit()-.5f;
        sprites[cursor]=(Sprite){.effect=blood_effect-1,.owner=owner,.room=room,.pos=impact,
            .velocity={1.2f*x-direction.x*.55f,.15f+y*.7f-direction.y*.4f,1.2f*z-direction.z*.55f},
            .size=.055f+.065f*y,.life=.22f+.15f*y};
        cursor=(cursor+1)%LIMIT;
    }
}
static void emit_at(unsigned owner,unsigned id,unsigned amount,unsigned room,Point origin) {
    const Definition *d=defs+id;
    if(d->kind==7 || !nearby(room))return;
    // The original hearth marker sits inside the stacked logs. Emit at their upper surface.
    if(d->kind==1 && !strcmp(d->name,"fire_fireplace")){
        Point above=origin;above.y+=.35f;Hit log;int prop;
        if(campaign_ray_wall(room,above,(Point){0,-1,0},.48f,&log,&prop) && log.normal.y>.15f)
            origin.y=fmaxf(origin.y,log.point.y+.04f);
    }
    Point low=origin,high=origin;
    if(d->kind==1 || d->kind==2){
        // Constrain the entire billboard to the visible side of the hearth, walls and ceiling.
        static const Point directions[]={{-1,0,0},{1,0,0},{0,-1,0},{0,1,0},{0,0,-1},{0,0,1}};
        for(unsigned axis=0;axis<6;axis++){
            Point dir=directions[axis];Hit hit;int prop;
            float reach=campaign_ray_wall(room,origin,dir,4,&hit,&prop)?fmaxf(0,hit.distance-.012f):4;
            Point *edge=axis%2?&high:&low;
            edge->x+=dir.x*reach;edge->y+=dir.y*reach;edge->z+=dir.z*reach;
        }
    }
    for(unsigned i=0;i<amount;i++){
        Sprite *p=sprites+cursor;cursor=(cursor+1)%LIMIT;
        float x=random_unit()-.5f,z=random_unit()-.5f,r=random_unit(),y=random_unit();
        *p=(Sprite){.pos=origin,.owner=owner,.low=low,.high=high,.flip=(d->kind==1 || d->kind==2)?SPRITE_CLIPPED:0,.effect=id,.room=room,.life=.35f+.65f*r,.size=.1f+.12f*y,.velocity={x*4,1+y*2,z*4}};
        switch(d->kind){
        case 0: p->pos.x+=x*d->area;p->pos.z+=z*d->area;p->pos.y+=(y-.35f)*d->area;
            p->life=2+r;p->size=.025f+.045f*r;p->velocity=(Point){.38f+x,-1.1f-y,.2f+z};break;
        case 1: p->pos.x+=x*d->area*.7f;p->pos.z+=z*d->area*.7f;p->size=.3f+.5f*r;p->velocity=(Point){x*.25f,.8f+r,z*.25f};break;
        case 2: p->size=.22f+.45f*r;p->life=1.6f+y;p->velocity=(Point){x*.5f,.45f+y*.65f,z*.5f};break;
        case 3: p->size=.025f+.06f*r;p->velocity=(Point){x*2,-1.8f+y,z*2};break;
        case 4: p->size=.4f+.65f*r;p->velocity=(Point){x*3,r*2,z*3};break;
        case 5: p->size=.028f+r*.1f;p->life=.16f+.35f*y;p->velocity=(Point){x*6,1+r*3,z*6};break;
        default: break;
        }
        if(d->kind==1 || d->kind==2){
            p->pos.x=fminf(high.x,fmaxf(low.x,p->pos.x));
            p->pos.z=fminf(high.z,fmaxf(low.z,p->pos.z));
        }
    }
}
static void update_box(ClipBox *box,unsigned room,Point marker,unsigned id){
    if(box->ttl>0 && box->room==room && !memcmp(&box->marker,&marker,sizeof marker))return;
    Point origin=marker;
    if(!strcmp(defs[id].name,"fire_fireplace")){
        Point above=origin;above.y+=.35f;Hit hit;int prop;
        if(campaign_ray_wall(room,above,(Point){0,-1,0},.48f,&hit,&prop) && hit.normal.y>.15f)
            origin.y=fmaxf(origin.y,hit.point.y+.04f);
    }
    *box=(ClipBox){.low=origin,.high=origin,.origin=origin,.marker=marker,.room=room,.ttl=.15f};
    static const Point dirs[]={{-1,0,0},{1,0,0},{0,-1,0},{0,1,0},{0,0,-1},{0,0,1}};
    for(unsigned j=0;j<6;j++){
        Point d=dirs[j];Hit hit;int prop;
        float reach=campaign_ray_wall(room,origin,d,4,&hit,&prop)?fmaxf(0,hit.distance-.012f):4;
        Point *p=j%2?&box->high:&box->low;
        p->x+=d.x*reach;p->y+=d.y*reach;p->z+=d.z*reach;
    }
}
static void emit_layers(unsigned owner,unsigned id,unsigned room,Point marker,Point forward,Point up,
                        float begin,float end,ClipBox *box,const Burst *burst){
    const ParticleGroup *g=groups+id;
    unsigned used=0;
    bool prepared=false,bounded=!burst && (defs[id].kind==1 || defs[id].kind==2);
    Point right={0},origin=marker;
    box->ttl-=end-begin;
    for(unsigned i=0;i<g->count;i++){
        unsigned index=g->first+i;
        const ParticleLayer *l=layers+index;
        float period=emitter_period(index,burst,g->period);
        float phase=(((burst?burst->seed:owner)*37u+index*53u)%127+.5f)/127.f;
        int amount=particle_emission_count(l,begin,end,period,g->period,phase,burst!=NULL);
        if(amount<=0)continue;
        float through=burst?fminf(end,period):end;
        float key=fmodf(.5f*(begin+through),period)*7/period;
        unsigned a=(unsigned)key,b=a<7?a+1:a;float mix=key-a;
        if(!prepared){
            for(unsigned j=0;j<LIMIT;j++)
                if(sprites[j].age<sprites[j].life && sprites[j].owner==owner && sprites[j].effect==id)used++;
            forward=unit(forward);right=cross(up,forward);
            if(fabsf(right.x)+fabsf(right.y)+fabsf(right.z)<.0001f)
                right=cross((Point){1,0,0},forward);
            right=unit(right);up=cross(forward,right);
            if(bounded){update_box(box,room,marker,id);origin=box->origin;}
            prepared=true;
        }
        const EmitterKey *first=l->keys+a,*next=l->keys+b;
        float life[2],speed[2],size[2];
        for(unsigned j=0;j<2;j++){
            life[j]=particle_lerp(first->life[j],next->life[j],mix);
            speed[j]=particle_lerp(first->speed[j],next->speed[j],mix);
            size[j]=particle_lerp(first->radius[j],next->radius[j],mix);
        }
        Point drift={particle_lerp(first->drift.x,next->drift.x,mix),particle_lerp(first->drift.y,next->drift.y,mix),
                     particle_lerp(first->drift.z,next->drift.z,mix)};
        for(int j=0;j<amount;j++){
            if(used>=g->capacity || used>=LIMIT)return;
            float lifetime=particle_lerp(life[0],life[1],random_unit());
            if(lifetime<=0 || (size[0]<=0 && size[1]<=0))continue;
            // Keep a few sprites available for a character hit during heavy gunfire.
            unsigned slots=id+1==blood_effect?LIMIT:LIMIT-8;
            unsigned first_slot=cursor<slots?cursor:0,free_index=first_slot;
            while(sprites[free_index].age<sprites[free_index].life){
                if(++free_index==slots)free_index=0;
                if(free_index==first_slot){
                    if(!burst)return;
                    // Snow and other looping scenery must not suppress a hit
                    // or muzzle effect when the shared 3DS pool is full.
                    int oldest=-1;float age=-1;
                    for(unsigned k=0;k<slots;k++)if(defs[sprites[k].effect].loop){
                        float fraction=sprites[k].age/sprites[k].life;
                        if(fraction>age){oldest=(int)k;age=fraction;}
                    }
                    if(oldest<0)return;
                    free_index=(unsigned)oldest;break;
                }
            }
            cursor=(free_index+1)%LIMIT;used++;
            Point q;
            if(l->sphere){
                q=random_direction();float r=particle_lerp(l->radius_first,l->radius_last,random_unit());
                q=(Point){q.x*r,q.y*r,q.z*r};
            }else q=(Point){particle_lerp(l->box_first.x,l->box_last.x,random_unit()),
                            particle_lerp(l->box_first.y,l->box_last.y,random_unit()),
                            particle_lerp(l->box_first.z,l->box_last.z,random_unit())};
            q.x+=l->offset.x;q.y+=l->offset.y;q.z+=l->offset.z;q=oriented(q,right,up,forward);
            Point v=random_direction();float length=particle_lerp(speed[0],speed[1],random_unit());
            v.x=v.x*length+drift.x+(2*random_unit()-1)*l->spread.x;
            v.y=v.y*length+drift.y+(2*random_unit()-1)*l->spread.y;
            v.z=v.z*length+drift.z+(2*random_unit()-1)*l->spread.z;
            float r=random_unit();
            float spin=particle_lerp(particle_lerp(l->spin_scale[a][0],l->spin_scale[b][0],mix),
                                     particle_lerp(l->spin_scale[a][1],l->spin_scale[b][1],mix),r);
            unsigned flip=(unsigned)(random_unit()*8)&l->flags;
            sprites[free_index]=(Sprite){.variant=l->first_variant+(unsigned)(random_unit()*l->variants),
                .angle=particle_lerp(l->angles[0],l->angles[1],random_unit()),.spin=flip&1?-spin:spin,.flip=flip|(bounded?SPRITE_CLIPPED:0),.effect=id,.layer=index,.room=room,.owner=owner,
                .pos={origin.x+q.x,origin.y+q.y,origin.z+q.z},.velocity=oriented(v,right,up,forward),
                .life=lifetime,.size=particle_lerp(size[0],size[1],random_unit()),.low=box->low,.high=box->high};
        }
    }
}
void particles_burst(unsigned owner,unsigned room,Point at,const char *name){
    // Unattached effects retain the authored world axes. Only contact effects
    // rotate their emission axis onto a surface normal.
    if(groups)particles_weapon_burst(owner,room,at,(Point){0,0,1},(Point){0,1,0},name);
    else for(unsigned i=0;i<num_defs;i++)if(!strcmp(defs[i].name,name)){
        emit_at(owner,i,4,room,at);return;
    }
}
void particles_weapon_burst(unsigned owner,unsigned room,Point origin,Point forward,Point up,const char *name){
    if(!groups)return;
    for(unsigned i=0;i<num_defs;i++)if(!strcmp(defs[i].name,name)){
        Burst *b=start_burst(owner,i,room,origin,forward,up);
        if(b){
            float end=fminf(b->deadline,last_step);
            emit_layers(owner,i,room,origin,forward,up,0,end,boxes+PARTICLE_SOURCES+(b-bursts),b);
            b->age=end;if(end>=b->deadline)b->active=false;
        }
        return;
    }
}
void particles_surface_burst(unsigned owner,unsigned room,Point at,Point normal,const char *name){
    // Impact RPS jets use local Y as their outward emission axis.
    normal=unit(normal);Point tangent=fabsf(normal.z)<.9f?(Point){0,0,1}:(Point){1,0,0};
    float dot=tangent.x*normal.x+tangent.y*normal.y+tangent.z*normal.z;
    tangent=unit((Point){tangent.x-dot*normal.x,tangent.y-dot*normal.y,tangent.z-dot*normal.z});
    at=(Point){at.x+.006f*normal.x,at.y+.006f*normal.y,at.z+.006f*normal.z};
    for(unsigned i=0;i<num_defs;i++)if(!strcmp(defs[i].name,name)){
        if(groups)start_burst(owner,i,room,at,tangent,normal);
        else emit_at(owner,i,4,room,at);
        return;
    }
}
static void emit(unsigned owner,unsigned id,unsigned amount){
    Point pos;director_pose(owner,&pos,NULL,NULL);emit_at(owner,id,amount,director.state[owner].room,pos);
}
void particles_event(unsigned owner,const char *op,const char *name) {
    if(owner>=director.count || !defs)return;
    if(!strcmp(op,"ps_stopallemissions")){
        for(unsigned i=0;i<PARTICLE_SOURCES;i++)if(particle_sources[i].owner==owner)particle_sources[i].effect=0;
        for(unsigned i=0;i<BURSTS;i++)if(bursts[i].owner==owner)bursts[i].active=false;
        return;
    }
    unsigned id=0;while(id<num_defs && strcmp(defs[id].name,name))id++;
    if(id==num_defs)return;
    if(!defs[id].loop && defs[id].kind!=7){
        if(groups){
            Point p,f,u;director_pose(owner,&p,&f,&u);
            start_burst(owner,id,director.state[owner].room,p,f,u);
        }else emit(owner,id,defs[id].kind==4?12:7);
        return;
    }
    int empty=-1;
    for(unsigned i=0;i<PARTICLE_SOURCES;i++){
        ParticleSource *s=particle_sources+i;
        if(!s->effect){if(empty<0)empty=i;continue;}
        if(s->owner==owner && (unsigned)s->effect==id+1){
            if(!defs[id].loop)s->interval=1.f;
            return;
        }
    }
    if(empty>=0){particle_sources[empty]=(ParticleSource){owner,id+1,defs[id].loop?0:1.f};boxes[empty].ttl=0;if(!groups)emit(owner,id,3);}
}
static float lamp_time;
static uint32_t police_color(unsigned owner){
    if(authored_pulse){
        unsigned index=((unsigned)(lamp_time*(256.f/5.f))+owner*17u)%256;
        const uint8_t *rgb=police_curve[index];
        return 0xff000000u | ((unsigned)rgb[2]<<16) | ((unsigned)rgb[1]<<8) | rgb[0];
    }
    float wave=sinf(12.f*lamp_time+owner);
    return 0xff000000u | ((unsigned)(255*fmaxf(0,-wave))<<16) | (unsigned)(255*fmaxf(0,wave));
}
uint32_t particles_police_color(unsigned room){
    if(!police_effect)return 0;
    for(unsigned i=0;i<PARTICLE_SOURCES;i++){
        ParticleSource s=particle_sources[i];
        if(s.effect!=police_effect || director.state[s.owner].room!=room)continue;
        return police_color(s.owner);
    }
    return 0;
}
void particles_light(unsigned room,Point target,IncidentLight *light){
    static const struct {const char *name;Point rgb;} colors[]={
        {"blue",{.08f,.17f,1}},{"cyan",{.1f,.9f,1}},{"green",{.1f,1,.15f}},{"lavender",{.65f,.47f,1}},
        {"magenta",{1,.12f,.9f}},{"orange",{1,.42f,.08f}},{"red",{1,.08f,.06f}},{"yellow",{1,.92f,.12f}},
        {"fire",{1,.45f,.16f}},{"candle",{1,.63f,.24f}},{"indicator_active",{.12f,1,.15f}},
        {"indicator_disabled",{1,.10f,.08f}}
    };
    for(unsigned i=0;i<PARTICLE_SOURCES;i++){
        ParticleSource s=particle_sources[i];
        if(!s.effect || defs[s.effect-1].kind!=7 || director.state[s.owner].room!=room)continue;
        const char *name=defs[s.effect-1].name;
        Point origin,rgb={.78f,.79f,.85f};director_pose(s.owner,&origin,NULL,NULL);
        float radius=defs[s.effect-1].area>1.5f?defs[s.effect-1].area:3.f;
        for(unsigned j=0;j<sizeof colors/sizeof *colors;j++)
            if(!strncmp(name+6,colors[j].name,strlen(colors[j].name))){rgb=colors[j].rgb;break;}
        if(strstr(name,"police")){
            uint32_t color=police_color(s.owner);
            rgb=(Point){(color&255)/255.f,((color>>8)&255)/255.f,((color>>16)&255)/255.f};radius=6.f;
        }else if(strstr(name,"warm")){
            float level=.82f+.12f*sinf(12*lamp_time)+.06f*sinf(19*lamp_time);
            rgb=(Point){level,level*.85f,level*.68f};
        }
        if(!defs[s.effect-1].loop){rgb.x*=s.interval;rgb.y*=s.interval;rgb.z*=s.interval;}
        campaign_add_light(light,target,origin,radius,rgb);
    }
}
void particles_tick(float dt) {
    last_step=dt;
    lamp_time=fmodf(lamp_time+dt,628.31853f);
    for(unsigned i=0;i<PARTICLE_SOURCES;i++){
        ParticleSource *s=particle_sources+i;if(!s->effect)continue;
        if(defs[s->effect-1].kind==7){
            if(!defs[s->effect-1].loop && (s->interval-=dt)<=0)s->effect=0;
            continue;
        }
        if(!nearby(director.state[s->owner].room))continue;
        if(groups){
            unsigned id=s->effect-1;Point pos,f,u;director_pose(s->owner,&pos,&f,&u);
            float start=fmaxf(0,s->interval),end=start+dt;
            emit_layers(s->owner,id,director.state[s->owner].room,pos,f,u,start,end,boxes+i,NULL);
            s->interval=end<groups[id].period*256?end:fmodf(end,groups[id].period*256);
            continue;
        }
        s->interval-=dt;if(s->interval>0)continue;
        s->interval=defs[s->effect-1].kind==0?.09f:.13f;emit(s->owner,s->effect-1,2);
    }
    for(unsigned i=0;i<BURSTS;i++){
        Burst *b=bursts+i;if(!b->active)continue;
        float end=fminf(b->deadline,b->age+dt);
        if(nearby(b->room))emit_layers(b->owner,b->id,b->room,b->origin,b->forward,b->up,b->age,end,boxes+PARTICLE_SOURCES+i,b);
        b->age=end;if(end>=b->deadline)b->active=false;
    }
    for(unsigned i=0;i<LIMIT;i++){
        Sprite *p=sprites+i;if(p->age>=p->life)continue;p->age+=dt;
        unsigned k=defs[p->effect].kind;if(layers)p->velocity.y=particle_vertical_step(p->velocity.y,layers[p->layer].gravity,dt);
        else if(k==3 || k==5 || k==6)p->velocity.y-=6.f*dt;
        if(drag){
            float speed=sqrtf(p->velocity.x*p->velocity.x+p->velocity.y*p->velocity.y+p->velocity.z*p->velocity.z);
            float key=fminf(15,p->age/p->life*15);unsigned a=(unsigned)key;
            const uint8_t *shape=layers[p->layer].shape;
            float diameter=2*p->size*particle_lerp(shape[a],shape[a<15?a+1:a],key-a)/255.f;
            float ratio=speed>.00001f?particle_drag_speed(speed,diameter,drag+p->layer,dt)/speed:1;
            p->velocity.x*=ratio;p->velocity.y*=ratio;p->velocity.z*=ratio;
        }
        p->pos.x+=p->velocity.x*dt;p->pos.y+=p->velocity.y*dt;p->pos.z+=p->velocity.z*dt;
        if(p->flip&SPRITE_CLIPPED){
            // Rising smoke and flame slide along a side wall until they reach the ceiling.
            p->pos.x=fminf(p->high.x,fmaxf(p->low.x,p->pos.x));
            p->pos.z=fminf(p->high.z,fmaxf(p->low.z,p->pos.z));
            if(p->pos.y<p->low.y || p->pos.y>p->high.y)p->age=p->life;
        }
    }
}

static float component(Point p,unsigned axis){
    return axis==0?p.x:axis==1?p.y:p.z;
}

// A plane adds at most one vertex to a convex polygon. Six planes turn a quad into at most a decagon.
static unsigned clip_plane(const Vertex *poly,unsigned length,Vertex *out,unsigned axis,float edge,float sign){
    unsigned count=0;
    Vertex previous=poly[length-1];
    float prev=sign*(component(previous.point,axis)-edge);
    for(unsigned i=0;i<length;i++){
        Vertex current=poly[i];
        float next=sign*(component(current.point,axis)-edge);
        if((prev<0 && next>0) || (prev>0 && next<0)){
            float t=prev/(prev-next);
            Point a=previous.point,b=current.point;
            out[count++]=(Vertex){{a.x+t*(b.x-a.x),a.y+t*(b.y-a.y),a.z+t*(b.z-a.z)},
                                  previous.u+t*(current.u-previous.u),previous.v+t*(current.v-previous.v)};
        }
        if(next>=0)out[count++]=current;
        previous=current;prev=next;
    }
    return count;
}

static unsigned sprite_geometry(Vertex *out,const Sprite *p,Point center,float yaw,const Camera *c,
                                unsigned frame,float size,bool clipped){
    Point right=c->right,up=c->up,origin=clipped?p->pos:center;
    float cs=1,sn=0;
    if(clipped && yaw!=0){
        cs=cosf(yaw);sn=sinf(yaw);
        right=(Point){cs*c->right.x-sn*c->right.z,c->right.y,sn*c->right.x+cs*c->right.z};
        up=(Point){cs*c->up.x-sn*c->up.z,c->up.y,sn*c->up.x+cs*c->up.z};
    }
    if(layers){
        const ParticleLayer *l=layers+p->layer;
        float key=p->age/p->life*15;unsigned i=(unsigned)key;
        float roll=p->angle+p->spin*particle_lerp(l->rotation[i],l->rotation[i<15?i+1:i],key-i);
        if(roll!=0){
            float c=cosf(roll),s=sinf(roll);Point old=right;
            right=(Point){c*old.x+s*up.x,c*old.y+s*up.y,c*old.z+s*up.z};
            up=(Point){c*up.x-s*old.x,c*up.y-s*old.y,c*up.z-s*old.z};
        }
    }
    Vertex polygon[CLIP_VERTICES],scratch[CLIP_VERTICES];
    const int signs[4][2]={{-1,-1},{1,-1},{1,1},{-1,1}};
    for(unsigned i=0;i<4;i++){
        int x=signs[i][0],y=signs[i][1];
        Point pos={origin.x+size*(x*right.x+y*up.x),origin.y+size*(x*right.y+y*up.y),origin.z+size*(x*right.z+y*up.z)};
        if(p->flip&2)x=-x;
        if(p->flip&4)y=-y;
        float inset=.5f/pages[frame/64].width;
        float u=(frame%8+(x<0?0:1))/8.f+(x<0?inset:-inset);
        float v=(frame/8%8+(y>0?0:1))/8.f+(y>0?inset:-inset);
        // The texture packer already converts the page to PICA tile order.
        polygon[i]=(Vertex){pos,u,v};
    }
    unsigned length=4;
    if(clipped){
        for(unsigned axis=0;axis<3;axis++){
            length=clip_plane(polygon,length,scratch,axis,component(p->low,axis),1);
            if(length<3)return 0;
            length=clip_plane(scratch,length,polygon,axis,component(p->high,axis),-1);
            if(length<3)return 0;
        }
        for(unsigned i=0;i<length;i++){
            Point local=polygon[i].point;
            float x=local.x-p->pos.x,z=local.z-p->pos.z;
            polygon[i].point=(Point){center.x+cs*x+sn*z,center.y+local.y-p->pos.y,center.z-sn*x+cs*z};
        }
    }
    unsigned used=0;
    for(unsigned i=1;i+1<length;i++){
        out[used++]=polygon[0];out[used++]=polygon[i];out[used++]=polygon[i+1];
    }
    return used;
}

void particles_prepare(const Camera *c) {
    count=0;unsigned n=0,used=0;
    for(unsigned i=0;i<LIMIT;i++){
        const Sprite *p=sprites+i;if(p->age>=p->life)continue;
        for(unsigned portal=campaign_first_view(p->room);portal!=255 && n<LIMIT;portal=campaign_next_view(portal)){
            if(!campaign_view_enabled(portal,director.exits))continue;
            Player body={.position=p->pos};if(!campaign_placement(p->room,portal,&body))continue;
            Point o=body.position;
            float depth=(o.x-c->eye.x)*c->forward.x+(o.y-c->eye.y)*c->forward.y+(o.z-c->eye.z)*c->forward.z;
            if(depth<=0 || depth>65)continue;
            float t=p->age/p->life,size=p->size*(t>.75f?4*(1-t):1);
            if(shape_curves){
                float key=t*15;unsigned a=(unsigned)key,b=a<15?a+1:a;
                const uint8_t *curve=layers?layers[p->layer].shape:shape_curves[p->effect];
                size=p->size*(curve[a]+(curve[b]-curve[a])*(key-a))/255.f;
            }
            const uint16_t *frames=variants?variants[p->variant]:defs[p->effect].frames;
            unsigned frame=frames[(unsigned)(t*4)],page=frame/64;
            unsigned kind=defs[p->effect].kind;bool additive=layers?layers[p->layer].additive:(kind==1 || kind==4 || kind==5);
            unsigned added=sprite_geometry(vertices+used,p,o,body.yaw,c,frame,size,(p->flip&SPRITE_CLIPPED)!=0);
            if(!added)continue;
            batches[count++]=(Batch){page,portal,used,added,additive,o};
            used+=added;n++;
        }
    }
    if(used)GSPGPU_FlushDataCache(vertices,used*sizeof(Vertex));
}
void particles_queue(const C3D_Mtx *view){
    for(unsigned i=0;i<count;i++){
        const Batch *b=batches+i;
        if(campaign_visible_transform(b->portal))
            alpha_submit(b->additive?ALPHA_GLOW:ALPHA_PARTICLE,view,b->center,b->portal,&binding,
                         b->first,b->count,pages+b->page,NULL,0);
    }
}
