#include "campaign_combat.h"
#include "campaign_file.h"
#include "campaign_world.h"
#include "director.h"
#include "audio_bank.h"
#include "particles.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char name[48],drop[32];float speed,gravity,delay,damage;
    uint16_t first,count,impact,timed;
} Type;
typedef struct {float time,inner,outer,near_damage,far_damage;} Burst;
typedef struct {Point direction;float accuracy,scatter,bounce;uint16_t decal,debris;char particle[48],sound[40],mesh[32];} Visual;
_Static_assert(sizeof(Visual)==148,"visual bank record");
typedef struct {uint16_t type,phase,frame,op,amount;char key[64];} ProjectileCue;
_Static_assert(sizeof(ProjectileCue)==74,"projectile cue layout");
typedef struct {uint16_t decal,op;char key[64];} ImpactEvent;
static ImpactEvent *impact_events;static unsigned impact_count;
static ProjectileCue *cues;
static uint16_t *cue_index;
static Visual *visuals;
static uint16_t blood_decals[3];
typedef struct {float min_speed;uint8_t until_ground,characters,on_death,small;} ContactRule;
_Static_assert(sizeof(ContactRule)==8,"projectile contact layout");
static ContactRule *contacts;
static uint32_t pierced[PROJECTILE_LIMIT][CAMPAIGN_PROPS/32];
static bool launch_projectile(const char *name,unsigned owner,unsigned room,Point origin,Point forward,const uint32_t *previous,const Point *axes);
static int projectile_id(const char *name);
static void phase_events(const Projectile *p,unsigned phase,float after,float through,Point direction,const Hit *hit,Effects *effects);
static Type *types;
static Burst *bursts;
typedef struct {uint32_t type;DropGrant grant[2];} Drop;
static Drop *drops;
static unsigned drop_count;
static unsigned type_count;
typedef struct {char skin[64];float top,bottom,radius,health,slow;uint8_t autoaim,timed,reserved[2];} Combatant;
_Static_assert(sizeof(Combatant)==88,"combatant bank layout");
static Combatant *combatants;
static const Combatant **by_actor;
static KillRewards rewards;
static bool read_combatants(void){
    FILE *f=campaign_file("combatants.bin");if(!f)return true;
    struct {char tag[4];uint32_t count;} h;
    bool ok=fread(&h,sizeof h,1,f)==1 && !memcmp(h.tag,"NPC1",4) && h.count && h.count<=128;
    if(ok){
        combatants=calloc(h.count,sizeof *combatants);by_actor=calloc(director.count,sizeof *by_actor);
        ok=combatants && by_actor && fread(combatants,sizeof *combatants,h.count,f)==h.count && fgetc(f)==EOF;
    }
    fclose(f);
    for(unsigned k=0;ok && k<h.count;k++){
        const Combatant *c=combatants+k;
        ok=memchr(c->skin,0,sizeof c->skin) && isfinite(c->top) && isfinite(c->bottom) && c->top>c->bottom &&
           c->radius>0 && c->radius<=2 && c->health>=0 && isfinite(c->health) && c->slow>=0 && c->slow<=1 && c->autoaim<=1 && c->timed<=1;
    }
    for(unsigned i=0;ok && i<director.count;i++)if(!i || (director.def[i].flags&4)){
        const char *name=director_text(director.def[i].model);
        for(unsigned k=0;k<h.count;k++)if(!strcmp(combatants[k].skin,name)){by_actor[i]=combatants+k;break;}
    }
    return ok;
}
static ShotTarget *targets;
static uint16_t *target_ids;
static unsigned target_count;
Projectile campaign_projectiles[PROJECTILE_LIMIT];

void combat_close(void) {
    memset(blood_decals,0,sizeof blood_decals);
    free(target_ids);target_ids=NULL;target_count=0;
    free(contacts);contacts=NULL;
    free(impact_events);impact_events=NULL;impact_count=0;
    free(cues);cues=NULL;free(cue_index);cue_index=NULL;
    free(visuals);visuals=NULL;
    free(types);free(bursts);free(targets);free(drops);drops=NULL;drop_count=0;
    types=NULL;bursts=NULL;targets=NULL;type_count=0;rewards=(KillRewards){0};free(combatants);combatants=NULL;free(by_actor);by_actor=NULL;
    memset(campaign_projectiles,0,sizeof campaign_projectiles);
}
static bool read_drops(FILE *file,unsigned types_count){
    struct {char tag[4];uint32_t count;} h;
    size_t bytes=fread(&h,1,sizeof h,file);
    if(!bytes)return feof(file) && !ferror(file); // Original MPB1 bank without pickup scripts.
    if(bytes!=sizeof h || memcmp(h.tag,"DRO1",4) || h.count>types_count)return false;
    if(h.count){
        drops=calloc(h.count,sizeof(*drops));
        if(!drops || fread(drops,sizeof(*drops),h.count,file)!=h.count)return false;
    }
    for(unsigned i=0;i<h.count;i++){
        if(drops[i].type>=types_count || !types[drops[i].type].drop[0])return false;
        for(unsigned k=0;k<2;k++){
            const DropGrant *g=drops[i].grant+k;
            if(!memchr(g->weapons[0],0,16) || !memchr(g->weapons[1],0,16) || !memchr(g->ammo,0,16) ||
               !memchr(g->sound,0,32) || !memchr(g->caption,0,64))return false;
        }
    }
    drop_count=h.count;
    bytes=fread(&h,1,sizeof h,file);
    if(!bytes)return feof(file) && !ferror(file);
    if(bytes!=sizeof h || memcmp(h.tag,"VFX1",4) || h.count!=types_count)return false;
    visuals=calloc(h.count,sizeof *visuals);
    if(!visuals || fread(visuals,sizeof *visuals,h.count,file)!=h.count)return false;
    for(unsigned i=0;i<h.count;i++)if(!memchr(visuals[i].particle,0,48) || !memchr(visuals[i].mesh,0,32) || !memchr(visuals[i].sound,0,40))return false;
    bytes=fread(&h,1,sizeof h,file);
    if(!bytes)return feof(file) && !ferror(file);
    if(bytes!=sizeof h || memcmp(h.tag,"PHX1",4) || h.count>4096)return false;
    cues=calloc(h.count?h.count:1,sizeof *cues);cue_index=calloc(types_count+1,sizeof *cue_index);
    if(!cues || !cue_index || fread(cues,sizeof *cues,h.count,file)!=h.count)return false;
    unsigned j=0;
    for(unsigned type=0;type<types_count;type++){
        cue_index[type]=j;
        while(j<h.count && cues[j].type==type){
            ProjectileCue c=cues[j++];
            if(c.phase>4 || !c.op || c.op>5 || !memchr(c.key,0,sizeof c.key))return false;
        }
    }
    cue_index[types_count]=j;if(j!=h.count)return false;
    if(fread(&h,sizeof h,1,file)!=1 || memcmp(h.tag,"DFX1",4) || h.count>1024)return false;
    impact_events=calloc(h.count?h.count:1,sizeof *impact_events);
    if(!impact_events || fread(impact_events,sizeof *impact_events,h.count,file)!=h.count)return false;
    for(unsigned k=0;k<h.count;k++)if(!impact_events[k].decal || impact_events[k].op<1 || impact_events[k].op>2 ||
       !memchr(impact_events[k].key,0,64))return false;
    impact_count=h.count;
    bytes=fread(&h,1,sizeof h,file);
    if(!bytes)return feof(file) && !ferror(file);
    if(bytes!=sizeof h || (memcmp(h.tag,"PHY1",4) && memcmp(h.tag,"PHY2",4)) || h.count!=types_count)return false;
    contacts=calloc(types_count,sizeof *contacts);
    if(!contacts || fread(contacts,sizeof *contacts,types_count,file)!=types_count)return false;
    if(h.tag[3]=='1')for(unsigned k=0;k<types_count;k++)contacts[k].small=!strncmp(types[k].name,"bullet_",7);
    for(unsigned k=0;k<types_count;k++)
        if(!isfinite(contacts[k].min_speed) || contacts[k].min_speed<0 ||
           contacts[k].until_ground>1 || contacts[k].characters>1 || contacts[k].on_death>1 ||
           (contacts[k].on_death && !types[k].drop[0]) || contacts[k].small>1)return false;
    return fgetc(file)==EOF && !ferror(file);
}
const DropGrant *combat_drop_grant(const Projectile *p,bool first){
    for(unsigned i=0;i<drop_count;i++)if(drops[i].type==p->type)return &drops[i].grant[first?0:1];
    return NULL;
}
bool combat_open(void) {
    combat_close();
    FILE *file=campaign_file("projectiles.bin");
    if(!file) return false;
    struct {char tag[4];uint32_t types,bursts;} h;
    bool ok=fread(&h,sizeof h,1,file)==1 && !memcmp(h.tag,"MPB1",4) && h.types && h.types<512 && h.bursts<8192;
    if(ok) {
        types=calloc(h.types,sizeof(Type));bursts=calloc(h.bursts+1,sizeof(Burst));
        for(unsigned i=0;i<director.count;i++)if(!i || (director.def[i].flags&4))target_count++;
        targets=calloc(target_count,sizeof *targets);target_ids=calloc(target_count,sizeof *target_ids);
        if(target_ids){
            unsigned slot=0;
            for(unsigned i=0;i<director.count;i++)if(!i || (director.def[i].flags&4))target_ids[slot++]=i;
        }
        ok=types && bursts && targets && target_ids && fread(types,sizeof(Type),h.types,file)==h.types &&
           fread(bursts,sizeof(Burst),h.bursts,file)==h.bursts && read_drops(file,h.types);
    }
    for(unsigned i=0;ok && i<h.types;i++) {
        Type *t=types+i;
        ok=memchr(t->name,0,sizeof t->name) && memchr(t->drop,0,sizeof t->drop) &&
           t->first+t->count<=h.bursts && isfinite(t->speed) && isfinite(t->damage);
    }
    fclose(file);
    if(!ok || !read_combatants()) {combat_close();return false;}
    type_count=h.types;
    const char *const blood[]={"blood_bullet","blood_bullet_small","blood_bullet_large"};
    for(unsigned i=0;visuals && i<3;i++){
        int id=projectile_id(blood[i]);if(id>=0)blood_decals[i]=visuals[id].decal;
    }
    return true;
}
bool combat_valid(void) {
    for(unsigned i=0;i<PROJECTILE_LIMIT;i++) {
        Projectile *p=campaign_projectiles+i;
        if(p->active && (p->type>=type_count || p->owner>=director.count || p->room>=128 ||
           p->burst>types[p->type].count || !isfinite(p->age) || !isfinite(p->position.x) ||
           !isfinite(p->position.y) || !isfinite(p->position.z))) return false;
    }
    return true;
}
static Point along(Point p,Point v,float t) {return (Point){p.x+t*v.x,p.y+t*v.y,p.z+t*v.z};}
static float length(Point p) {return sqrtf(p.x*p.x+p.y*p.y+p.z*p.z);}
bool combat_waits_for_target(unsigned actor) {
    const Combatant *c=actor && by_actor?by_actor[actor]:NULL;
    const EntityState *e=director.state+actor;
    // The authored wait state can be used by allies such as the subway escort.
    // An actor excluded from targeting needs an explicit opponent or combat order.
    return director_needs_target(actor) ||
           (c && !c->autoaim && e->aim>=director.count &&
            !strcmp(director_text(e->ai),"idleuntilenemyseen"));
}
static void prepare_targets(unsigned owner,unsigned room) {
    for(unsigned slot=0;slot<target_count;slot++) {
        unsigned i=target_ids[slot];
        EntityState *e=director.state+i;
        bool active=i!=owner &&
                    e->room==room && e->enabled && !e->dead && !e->pass_projectiles && (i || !director.dodge_pass_projectiles);
        const Combatant *c=by_actor?by_actor[i]:NULL;
        targets[slot]=(ShotTarget){.feet=e->position,.active=active,.crouched=active && campaign_actor_crouched(i),
            .bottom=c?c->bottom:0,.top=c?c->top:0,.radius=c?c->radius:0};
    }
}
// Prepared once per cinematic frame. A character can cover the lens even
// when scripts allow projectiles to pass through it.
void combat_camera_bodies(unsigned subject,unsigned room){
    prepare_targets(subject,room);
    for(unsigned slot=0;slot<target_count;slot++){
        unsigned id=target_ids[slot];
        const EntityState *e=director.state+id;
        ShotTarget *t=targets+slot;
        t->active=false;
        if(id==subject || !e->enabled)continue;
        if(e->room!=room){
            unsigned door=0;
            for(;door<campaign_exit_count();door++){
                const CampaignExit *exit=campaign_exit(door);
                if(exit->room==e->room && director.exits[exit->id])break;
            }
            if(door==campaign_exit_count())continue;
            t->feet=campaign_from_neighbour(door,e->position);
        }
        if(id && !strcmp(director_text(director.def[id].model),"invisible"))continue;
        t->active=true;t->crouched=campaign_actor_crouched(id);
        if(t->radius<=0){t->radius=.30f;t->top=1.85f;}
        if(e->dead){t->top=fminf(t->top,.60f);t->radius=fminf(t->radius,.25f);}
        t->radius+=.09f;
    }
}
bool combat_camera_blocked(Point start,Point direction,float limit,Hit *hit){
    int actor;
    return scene_bodies(start,direction,limit,targets,target_count,hit,&actor);
}
static float player_scale=1,enemy_scale=1;
static bool slow_rounds;
void combat_slow_rounds(bool enabled){slow_rounds=enabled;}
void combat_set_health_scales(float player,float enemy){player_scale=player;enemy_scale=enemy;}
static void harm(unsigned owner,unsigned actor,float damage) {
    if(director.state[actor].dead) return;
    if(!actor)damage/=player_scale;
    else if(director.def[actor].flags&4)damage/=enemy_scale;
    director_hit(actor,damage);
    if(owner==0 && actor && (director.def[actor].flags&4) && director.state[actor].dead){
        const Combatant *c=by_actor?by_actor[actor]:NULL;
        rewards.timed+=c?c->timed:1;rewards.slow+=c?c->slow:.1f;rewards.health+=c?c->health:0;
    }
}
KillRewards combat_take_rewards(void){KillRewards result=rewards;rewards=(KillRewards){0};return result;}
static void direct_hit(unsigned owner,int actor,unsigned room,const Hit *hit,Point direction,float damage,bool body_parts,int type,Effects *effects) {
    if(actor<0) return;
    Point point=hit->point;
    float height=point.y-director.state[actor].position.y;
    if(body_parts && actor!=0 && (!by_actor || !by_actor[actor] || by_actor[actor]->top>1)){
        bool crouched=campaign_actor_crouched((unsigned)actor);
        damage*=height>(crouched?.92f:1.55f)?5:height<(crouched?.43f:.72f)?.5f:1;
    }
    float before=director.state[actor].hp;
    harm(owner,actor,damage);
    if(actor && director.state[actor].hp<before){
        particles_blood(actor,room,along(point,hit->normal,.045f),hit->normal);
        if(type>=0){
            Projectile contact={.type=type,.room=room,.owner=actor,.position=point};
            phase_events(&contact,4,-1,0,direction,NULL,effects);
        }
    }
}
typedef struct {
    unsigned room;
    Point start,direction;
    Hit hit;
    int actor,prop;
    float travel;
    bool geometry_only,large;
    uint32_t *pierced;
} RayPath;
static bool cast_body(RayPath *ray,float limit){
    int slot;
    Hit impact;
    while(scene_bodies(ray->start,ray->direction,limit,targets,target_count,&impact,&slot)){
        // A shot capsule can extend through cover even when its actor is
        // completely behind it. Check the space from the hit to the axis.
        Point inward={targets[slot].feet.x-impact.point.x,0,targets[slot].feet.z-impact.point.z};
        float distance=length(inward);
        Hit obstruction;int prop;
        if(distance<=.001f || !campaign_ray_projectile(ray->room,impact.point,
                along((Point){0},inward,1.f/distance),distance,ray->large,&obstruction,&prop)){
            ray->hit=impact;ray->actor=target_ids[slot];return true;
        }
        targets[slot].active=false;
    }
    return false;
}
/* Segments use each room's own coordinate frame. Both camera aiming and
   muzzle collision take the same route, including rotated doorways. */
static bool cast_path(RayPath *r,float range,unsigned owner,float damage,bool report_segments) {
    float remaining=range;r->travel=0;
    for(unsigned depth=0;depth<8;depth++){
        float boundary=remaining;CampaignExit exit;
        bool portal=campaign_ray_exit(r->room,r->start,r->direction,&boundary,NULL,&exit);
        bool wall=campaign_ray_projectile(r->room,r->start,r->direction,boundary,r->large,&r->hit,&r->prop);
        bool body=false;r->actor=-1;
        if(!r->geometry_only){
            prepare_targets(owner,r->room);
            body=cast_body(r,wall?r->hit.distance:boundary);
        }
        if(report_segments && !r->large && damage>0 && r->pierced)
            campaign_pierced_props(r->room,r->start,r->direction,(wall || body)?r->hit.distance:boundary,r->pierced,owner,damage);
        if(wall || body){
            if(body)r->prop=-1;
            r->travel+=r->hit.distance;return true;
        }
        r->hit.point=along(r->start,r->direction,boundary);
        if(portal && !director.exits[exit.id]){
            // A scripted closure can precede its physical panel. A travelling
            // round needs a contact here rather than another tick on the seam.
            r->hit=(Hit){r->hit.point,exit.normal,boundary,UINT32_MAX};
            r->prop=-1;r->travel+=boundary;return true;
        }
        if(!portal || depth==7){r->travel+=boundary;return false;}
        if(report_segments)director_bullet(r->start,r->hit.point,r->room,-1,damage,owner);
        r->room=exit.room;r->direction=campaign_transform(&exit,r->direction,true);
        float step=fminf(.002f,remaining-boundary);
        r->start=along(campaign_transform(&exit,r->hit.point,false),r->direction,step);
        remaining-=boundary+step;r->travel+=boundary+step;
    }
    return false;
}
static bool target_ray(Point eye,Point target,float range,Point *dir,float *distance){
    Point delta=along(target,eye,-1);
    *distance=length(delta);
    if(*distance<.001f || *distance>range)return false;
    *dir=along((Point){0},delta,1.f / *distance);
    return true;
}
// The first segment is in the current room, even for an NPC looking in from outside.
static bool clear_segment(unsigned room,Point start,Point dir,float distance,int target_prop){
    Hit hit;int prop;
    return !campaign_ray_projectile(room,start,dir,distance,false,&hit,&prop) ||
           (target_prop>=0 && prop==target_prop);
}
static bool sight_clear_to_prop(unsigned current,Point start,Point dir,float distance,const CampaignExit *exit,int target_prop){
    if(!exit)return clear_segment(current,start,dir,distance,target_prop);
    float boundary=distance;CampaignExit crossing;
    if(!campaign_ray_exit(current,start,dir,&boundary,director.exits,&crossing) || crossing.id!=exit->id ||
       !clear_segment(current,start,dir,boundary,-1))return false;
    Point remote=campaign_transform(exit,along(start,dir,boundary),false);
    return clear_segment(exit->room,remote,campaign_transform(exit,dir,true),distance-boundary,target_prop);
}
static bool sight_clear(unsigned current,Point start,Point dir,float distance,const CampaignExit *exit){
    return sight_clear_to_prop(current,start,dir,distance,exit,-1);
}
bool combat_sight(unsigned room,Point eye,unsigned target_room,Point target,float range,bool activation,Point *direction) {
    unsigned current=campaign_room_id();
    if(room==target_room){
        float distance;
        return room==current && target_ray(eye,target,range,direction,&distance) &&
               sight_clear(current,eye,*direction,distance,NULL);
    }
    if(room!=current && target_room!=current)return false;
    bool eye_here=room==current;
    for(unsigned i=0;i<campaign_exit_count();i++){
        const CampaignExit *exit=campaign_exit(i);
        if(exit->room!=(eye_here?target_room:room) || !director.exits[exit->id] || (activation && !director.ai_exits[exit->id]))continue;
        Point near=eye_here?eye:target,far=eye_here?target:eye;
        Point other=campaign_from_neighbour(i,far);
        Point ray;float distance;
        if(!target_ray(near,other,range,&ray,&distance) || !sight_clear(current,near,ray,distance,exit))continue;
        *direction=eye_here?ray:along((Point){0},campaign_transform(exit,ray,true),-1);
        return true;
    }
    return false;
}
unsigned combat_aim_target(const Player *p,unsigned previous,float cone,Point *direction,float *focus_distance){
    unsigned room=director.state[0].room,best=DIRECTOR_NONE;
    Point eye=p->position;eye.y+=p->crouched?.91f:1.42f;
    float cp=cosf(p->pitch),minimum=cosf(cone),best_score=minimum;
    Point forward={sinf(p->yaw)*cp,sinf(p->pitch),cosf(p->yaw)*cp};
    unsigned exits=campaign_exit_count();
    for(unsigned slot_id=1;slot_id<target_count;slot_id++){
        unsigned i=target_ids[slot_id];
        const EntityState *e=director.state+i;
        if(!e->hostile || !e->enabled || e->dead || e->pass_projectiles)continue;
        if(by_actor && by_actor[i] && !by_actor[i]->autoaim)continue;
        Point chest=e->position;chest.y+=campaign_actor_crouched(i)?.80f:1.18f;
        bool local=e->room==room;
        for(unsigned slot=0;slot<(local?1:exits);slot++){
            const CampaignExit *exit=local?NULL:campaign_exit(slot);
            if(exit && (exit->room!=e->room || !director.exits[exit->id]))continue;
            Point target=local?chest:campaign_from_neighbour(slot,chest),ray;
            float distance;
            if(!target_ray(eye,target,55,&ray,&distance))continue;
            float dot=ray.x*forward.x+ray.y*forward.y+ray.z*forward.z;
            float score=dot+(i==previous?.3f:0);
            if(dot<minimum || score<=best_score || !sight_clear(room,eye,ray,distance,exit))continue;
            *direction=ray;*focus_distance=distance;best_score=score;best=i;
        }
    }
    return best;
}
static bool blood_decal(unsigned id){
    return id && (id==blood_decals[0] || id==blood_decals[1] || id==blood_decals[2]);
}
static void surface_impact(Effects *fx,unsigned owner,unsigned room,const Hit *hit,unsigned decal){
    if(!decal || hit->face==UINT32_MAX)return;
    Impact anchored={.point=hit->point,.normal=hit->normal,.room=room,.decal=decal,.life=18};
    if(!campaign_anchor_mark(&anchored,hit->face))return;
    const char *material=campaign_room_surface(room,hit->face);
    Impact *m;
    if(hit->normal.y>.5f && blood_decal(decal)){
        for(unsigned i=SURFACE_MARKS;i<IMPACT_COUNT;i++){
            const Impact *blood=fx->marks+i;
            if(blood->life && blood->room==room && blood->decal==decal &&
               length(along(blood->point,hit->point,-1))<.18f)return;
        }
        m=fx->marks+SURFACE_MARKS+fx->next_blood++%FLOOR_BLOOD_MARKS;
    }else m=fx->marks+fx->next++%SURFACE_MARKS;
    *m=anchored;
    snprintf(m->material,sizeof m->material,"%s",material);
    for(unsigned i=0;i<impact_count;i++){
        const ImpactEvent *e=impact_events+i;if(e->decal!=decal)continue;
        if(e->op==1)particles_surface_burst(owner,room,hit->point,hit->normal,e->key);
        else bank_impact_sound(owner,room,m->point,e->key,material);
    }
}
bool combat_footstep(const char *name,unsigned owner,unsigned room,Point feet,Effects *fx){
    int type=projectile_id(name);if(type<0 || !cue_index || !fx)return false;
    Hit floor;int prop;feet.y+=.35f;
    if(!campaign_ray_wall(room,feet,(Point){0,-1,0},.85f,&floor,&prop))return false;
    for(unsigned i=cue_index[type];i<cue_index[type+1];i++)if(cues[i].op==5){
        surface_impact(fx,owner,room,&floor,cues[i].amount);return true;
    }
    return false;
}
void combat_hitscan(unsigned owner,unsigned room,const Camera *camera,Point shoulder,Point muzzle,
                    unsigned pellets,float damage,float range,const char *round,Effects *effects) {
    int type=round?projectile_id(round):-1;
    Visual *v=type>=0 && visuals?visuals+type:NULL;
    bool travelling=slow_rounds && type>=0 && range>=5 && types[type].speed>0;
    float spread=v && !travelling?v->accuracy/2500.f:0;
    for(unsigned i=0;i<pellets;i++) {
        Camera aim=*camera;
        if(spread>0){
            float angle=6.2831853f*(rand()/(float)RAND_MAX);
            float radius=spread*sqrtf(rand()/(float)RAND_MAX);
            aim.forward=along(along(aim.forward,aim.right,radius*cosf(angle)),aim.up,radius*sinf(angle));
            aim.forward=along((Point){0},aim.forward,1.f/length(aim.forward));
        }
        uint32_t crossed[CAMPAIGN_PROPS/32]={0};
        bool large=type>=0 && contacts && !contacts[type].small;
        RayPath ray={.room=room,.start=shoulder,.direction=aim.forward,.large=large,.pierced=crossed};
        bool struck;
        if(range<5)struck=cast_path(&ray,range,owner,damage,true);
        else {
            RayPath sight={.room=room,.start=aim.eye,.direction=aim.forward,.large=large};
            cast_path(&sight,range,owner,0,false);
            Point aim_at=along(aim.eye,aim.forward,sight.travel);
            Point reach=along(muzzle,shoulder,-1);float near=length(reach);
            ray.direction=along((Point){0},reach,1/fmaxf(.000001f,near));
            struck=cast_path(&ray,near,owner,damage,true);
            if(!struck){
                Point path=along(aim_at,muzzle,-1);float distance=length(path);
                /* Normally shoulder and muzzle share a room. When the muzzle
                   projects through an aperture its ray must start on that side. */
                ray.start=ray.hit.point;Point direction=along((Point){0},path,1/fmaxf(.000001f,distance));
                if(ray.room!=room){
                    float segment=near;CampaignExit exit;
                    if(campaign_ray_exit(room,shoulder,along((Point){0},reach,1/fmaxf(.000001f,near)),&segment,director.exits,&exit))
                        direction=campaign_transform(&exit,direction,true);
                }
                ray.direction=direction;
                if(travelling && launch_projectile(round,owner,ray.room,ray.start,ray.direction,crossed,NULL)){
                    continue;
                }
                struck=cast_path(&ray,distance+.06f,owner,damage,true);
            }
            if(struck && ray.actor<0)surface_impact(effects,owner,ray.room,&ray.hit,v?v->decal:1);
        }
        if(struck)direct_hit(owner,ray.actor,ray.room,&ray.hit,ray.direction,damage,true,type,effects);
        director_bullet(ray.start,ray.hit.point,ray.room,struck?ray.prop:-1,damage,owner);
    }
}
static int projectile_id(const char *name){
    for(unsigned i=0;i<type_count;i++)if(!strcmp(name,types[i].name))return (int)i;
    return -1;
}
/* Right and up for an arbitrary normalized emission direction, including
   vertical debris jets and throws from a pitched camera. */
static void emission_axes(Point forward,Point *right,Point *up){
    float horizontal=hypotf(forward.x,forward.z);
    *right=horizontal>.00001f?(Point){forward.z/horizontal,0,-forward.x/horizontal}:(Point){1,0,0};
    *up=(Point){forward.y*right->z,forward.z*right->x-forward.x*right->z,-forward.y*right->x};
}
bool combat_launch(const char *name,unsigned owner,unsigned room,Point origin,Point forward) {
    return launch_projectile(name,owner,room,origin,forward,NULL,NULL);
}
bool combat_launch_attached(const char *name,unsigned owner,unsigned room,Point origin,const Point axes[3]){
    return launch_projectile(name,owner,room,origin,axes[2],NULL,axes);
}
static bool cosmetic_projectile(unsigned id){
    const Type *type=types+id;
    if(type->damage || type->count || type->impact || type->timed || type->drop[0] ||
       !visuals || (!visuals[id].debris && !visuals[id].decal))return false;
    if(cue_index)for(unsigned i=cue_index[id];i<cue_index[id+1];i++){
        // A child projectile or an alert can affect gameplay even without damage here.
        if(cues[i].op==3 || cues[i].op==4)return false;
    }
    return true;
}
static bool launch_projectile(const char *name,unsigned owner,unsigned room,Point origin,Point forward,const uint32_t *previous,const Point *axes){
    int id=projectile_id(name);
    if(id<0 || owner>=director.count) return false;
    Visual *v=visuals?visuals+id:NULL;
    if(v){
        if(!cues && v->particle[0])particles_burst(owner,room,origin,v->particle);
        if((contacts && types[id].speed>0) || v->debris || v->decal || !strcmp(name,"frag_molotov")){
            forward=along((Point){0},forward,1.f/fmaxf(.00001f,length(forward)));
            Point right,up;emission_axes(forward,&right,&up);
            Point d=axes?along(along(along((Point){0},axes[0],v->direction.x),axes[1],v->direction.y),axes[2],v->direction.z):
                along(along(along((Point){0},forward,v->direction.z),right,v->direction.x),up,v->direction.y);
            float axis_length=length(d);
            if(axis_length>.00001f)emission_axes(along((Point){0},d,1.f/axis_length),&right,&up);
            float angle=6.2831853f*rand()/(float)RAND_MAX,r=v->accuracy/2500.f*sqrtf(rand()/(float)RAND_MAX);
            d=along(along(d,right,cosf(angle)*r),up,sinf(angle)*r);
            forward=along((Point){0},d,1.f/fmaxf(.00001f,length(d)));
        }

    }
    // Leave slots for live rounds, pickups and damage volumes during visual bursts.
    enum { COMBAT_RESERVE=16 };
    unsigned limit=cosmetic_projectile(id)?PROJECTILE_LIMIT-COMBAT_RESERVE:PROJECTILE_LIMIT;
    for(unsigned i=0;i<limit;i++) if(!campaign_projectiles[i].active) {
        float speed=types[id].speed;
        if(v && v->scatter>0)speed*=1+v->scatter*(2.f*rand()/(float)RAND_MAX-1);
        campaign_projectiles[i]=(Projectile){.position=origin,.velocity=along((Point){0},forward,speed),
            .owner=owner,.room=room,.type=id,.active=true};
        if(previous)memcpy(pierced[i],previous,sizeof pierced[i]);else memset(pierced[i],0,sizeof pierced[i]);
        phase_events(campaign_projectiles+i,0,-1,0,forward,NULL,NULL);
        return true;
    }
    return false;
}
bool combat_has_weapon_drops(void){return contacts!=NULL;}
bool combat_drop_on_death(unsigned owner,const char *weapon,Point origin,Point forward){
    if(!contacts || owner>=director.count)return false;
    EntityState *e=director.state+owner;
    if(!e->dead || e->loot_collected)return false;
    for(unsigned i=0;i<drop_count;i++){
        unsigned type=drops[i].type;
        if(contacts[type].on_death && !strcmp(weapon,types[type].drop)){
            if(!combat_launch(types[type].name,owner,e->room,origin,forward))return false;
            break;
        }
    }
    e->loot_collected=true;
    return true;
}
static void alert_enemies(const Projectile *p,float radius){
    for(unsigned slot=1;slot<target_count;slot++){
        unsigned i=target_ids[slot];
        EntityState *e=director.state+i;
        if(i==p->owner || e->hostile || !e->enabled || e->dead || combat_waits_for_target(i) ||
           (!*director_text(e->ai) && !*director_text(e->weapon)))continue;
        Point apparent;float distance;
        if(campaign_sound_path(p->room,p->position,e->room,e->position,director.exits,&apparent,&distance) &&
           distance<radius)e->hostile=true;
    }
}
static void phase_events(const Projectile *p,unsigned phase,float after,float through,Point direction,const Hit *hit,Effects *effects){
    if(!cue_index)return;
    for(unsigned i=cue_index[p->type];i<cue_index[p->type+1];i++){
        const ProjectileCue *c=cues+i;
        if(c->phase!=phase || c->frame<=after || c->frame>through)continue;
        if(c->op==1){
            if(hit)particles_surface_burst(p->owner,p->room,hit->point,hit->normal,c->key);
            else particles_burst(p->owner,p->room,p->position,c->key);
        }
        if(c->op==2)bank_impact_sound(p->owner,p->room,p->position,c->key,hit?campaign_room_surface(p->room,hit->face):"default");
        if(c->op==3){
            for(unsigned n=0;n<c->amount;n++)combat_launch(c->key,p->owner,p->room,p->position,direction);
            if(phase==4 && effects && visuals && c->amount){
                int id=projectile_id(c->key);
                if(id>=0 && blood_decal(visuals[id].decal)){
                    Hit floor;int prop;
                    if(campaign_ray_wall(p->room,p->position,(Point){0,-1,0},2.2f,&floor,&prop) && floor.normal.y>.5f)
                        surface_impact(effects,p->owner,p->room,&floor,visuals[id].decal);
                }
            }
        }
        if(c->op==4)alert_enemies(p,c->amount*.01f);
        if(c->op==5 && effects){
            Hit floor;int prop;
            const Hit *surface=hit && hit->face!=UINT32_MAX?hit:NULL;
            bool melee=!strcmp(types[p->type].name,"bullet_leadpipe") || !strcmp(types[p->type].name,"bullet_baseballbat");
            // A missed melee swing has no surface to mark. Projecting it down
            // creates a displaced puff on the floor beside the character.
            if(!surface && !melee && campaign_ray_wall(p->room,p->position,(Point){0,-1,0},1.5f,&floor,&prop))surface=&floor;
            if(surface)surface_impact(effects,p->owner,p->room,surface,c->amount);
        }
    }
}
static float blast_distance(const Projectile *p,unsigned room,Point at,float radius,int target_prop){
    if(room==p->room){
        float distance=length(along(at,p->position,-1));
        if(distance<.001f)return distance;
        Point ray=along((Point){0},along(at,p->position,-1),1.f/distance);
        return distance<=radius && sight_clear_to_prop(room,p->position,ray,distance,NULL,target_prop)?distance:INFINITY;
    }
    // A room boundary can rotate and translate the coordinate system. Follow
    // the actual aperture and check cover on both sides, including off camera.
    for(unsigned slot=0;;slot++){
        const CampaignExit *exit=campaign_room_exit(p->room,slot);
        if(!exit)break;
        if(exit->room!=room || !director.exits[exit->id])continue;
        Point target=campaign_from_room_neighbour(p->room,slot,at),ray;
        float distance;
        if(target_ray(p->position,target,radius,&ray,&distance) &&
           sight_clear_to_prop(p->room,p->position,ray,distance,exit,target_prop))return distance;
    }
    return INFINITY;
}
static void blast(const Projectile *p,const Burst *b) {
    bool melee=!strcmp(types[p->type].name,"bullet_leadpipe") || !strcmp(types[p->type].name,"bullet_baseballbat");
    for(unsigned i=0;i<director.count;i++) {
        EntityState *e=director.state+i;const EntityDef *d=director.def+i;
        if((melee && i==p->owner) || e->dead || !e->enabled || (i && !(d->flags&(4|16)))) continue;
        Point pos;director_pose(i,&pos,NULL,NULL);
        if(!i || d->flags&4){
            const Combatant *c=by_actor?by_actor[i]:NULL;
            pos.y+=c?(c->bottom+c->top)*.5f:.85f;
        }
        float distance=blast_distance(p,e->room,pos,b->outer,d->prop==DIRECTOR_NONE?-1:d->prop);
        if(distance>b->outer)continue;
        float fraction=fminf(1,fmaxf(0,(distance-b->inner)/fmaxf(.001f,b->outer-b->inner)));
        harm(p->owner,i,b->near_damage+(b->far_damage-b->near_damage)*fraction);
    }
}
void combat_tick(float dt,Effects *effects) {
    for(unsigned i=0;i<PROJECTILE_LIMIT;i++) {
        Projectile *p=campaign_projectiles+i;
        if(!p->active || p->dropped) continue;
        Type *type=types+p->type;Visual *v=visuals?visuals+p->type:NULL;
        const ContactRule *contact=contacts?contacts+p->type:NULL;
        Hit contact_hit;const Hit *explosion_hit=NULL;
        float previous_age=p->age;p->age+=dt;
        if(!p->exploding && !p->resting) {
            phase_events(p,0,previous_age*30,p->age*30,p->velocity,NULL,effects);
            p->velocity.y-=9.81f*type->gravity*dt;
            float speed=length(p->velocity);
            RayPath ray={.room=p->room,.start=p->position,.direction=along((Point){0},p->velocity,1/fmaxf(.000001f,speed)),
                .large=contact && !contact->small,.pierced=pierced[i],.geometry_only=contact?!contact->characters:type->damage<=0};
            bool struck=cast_path(&ray,speed*dt,p->owner,type->damage,type->damage>0);
            p->velocity=along((Point){0},ray.direction,speed);p->room=ray.room;
            if(struck) {
                if(type->damage>0) {
                    bool bullet=!strncmp(type->name,"bullet_",7) && !type->impact && !type->timed;
                    direct_hit(p->owner,ray.actor,ray.room,&ray.hit,ray.direction,type->damage,bullet,p->type,effects);
                    director_bullet(ray.start,ray.hit.point,ray.room,ray.prop,type->damage,p->owner);
                }
                p->position=along(ray.hit.point,ray.hit.normal,.03f);
                if(v && ray.actor<0)surface_impact(effects,p->owner,ray.room,&ray.hit,v->decal);
                if(!cues && v && v->debris && v->sound[0] && p->age>.04f){
                    char group[40];snprintf(group,sizeof group,"%s",v->sound);char *key=strchr(group,'/');
                    if(key && director.state[p->owner].room==p->room){*key++=0;bank_scene_sound(p->owner,group,key,p->position,false,true);}
                }
                if(type->impact) {
                    p->exploding=true;p->age=0;
                    if(ray.actor<0){contact_hit=ray.hit;explosion_hit=&contact_hit;}
                }
                else if(type->drop[0] || type->timed || (v && v->debris)) {
                    float dot=p->velocity.x*ray.hit.normal.x+p->velocity.y*ray.hit.normal.y+p->velocity.z*ray.hit.normal.z;
                    p->velocity=along((Point){0},along(p->velocity,ray.hit.normal,-2*dot),v?v->bounce:.42f);
                    bool ground_only=contact?contact->until_ground:type->drop[0]!=0;
                    bool exhausted=(v && v->bounce==0) || (contact?speed<=contact->min_speed:length(p->velocity)<1.1f);
                    if(exhausted && (!ground_only || ray.hit.normal.y>.5f)){
                        p->velocity=(Point){0};p->resting=true;p->dropped=type->drop[0]!=0;
                        phase_events(p,2,-1,0,ray.direction,&ray.hit,effects);
                        if(p->dropped)continue;
                    }else phase_events(p,1,-1,0,ray.direction,&ray.hit,effects);
                } else {
                    phase_events(p,1,-1,0,ray.direction,&ray.hit,effects);
                    p->active=false;continue;
                }
            } else p->position=ray.hit.point;
        }
        if(!p->exploding && type->timed && p->age>=type->delay) {p->exploding=true;p->age=0;}
        if(p->exploding) {
            phase_events(p,3,p->age==0?-1:previous_age*30,p->age*30,(Point){0,0,1},explosion_hit,effects);
            while(p->burst<type->count && bursts[type->first+p->burst].time<=p->age)
                blast(p,bursts+type->first+p->burst++);
            if(p->burst==type->count) p->active=false;
        } else if(p->age>10 || p->position.y< -1000) p->active=false;
    }
}
const char *combat_mesh(const Projectile *p,bool *pickup) {
    if(!p->active) return NULL;
    Type *t=types+p->type;*pickup=p->dropped;
    if(p->dropped) return t->drop;
    if(visuals && visuals[p->type].mesh[0])return visuals[p->type].mesh;
    if(!strcmp(t->name,"grenade") || !strcmp(t->name,"molotov")) return t->name;
    return NULL;
}
