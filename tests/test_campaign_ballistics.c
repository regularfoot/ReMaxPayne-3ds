/* Two rooms joined by a quarter-turn. A wall and an actor can each stop a
   shot on the far side. Exercise the combat path used by the ARM game loop. */
#include "../source/native/campaign_combat.h"
#include "../source/native/campaign_world.h"
#include "../source/native/director.h"
#include "../source/native/particle_layer.h"
#include "../source/native/depth_bias.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

Director director;
static EntityDef defs[3];
static EntityState states[3];
static bool crouched[3];
bool campaign_actor_crouched(unsigned id){assert(id<director.count);return crouched[id];}
static const CampaignExit door={.id=0,.room=1,.normal={0,0,-1}};
static float wall_x;
static bool ground;
static unsigned clip_bounces,clip_stops;
static Point surface_at,surface_normal;
static unsigned bullet_room,blood_hits,explosions,impact_audio,impact_sparks;
static const char *retail_bank;
void particles_burst(unsigned owner,unsigned room,Point at,const char *name){(void)owner;(void)room;(void)at;if(!strcmp(name,"explosion_grenade"))explosions++;
    if(room==1 && !strcmp(name,"bullethit_generic"))impact_sparks++;}
const char *campaign_room_surface(unsigned room,unsigned face){(void)face;return room==1?"metal":"default";}
void bank_impact_sound(unsigned owner,unsigned room,Point point,const char *key,const char *material){
    (void)owner;(void)point;
    if(room==1 && !strcmp(key,"projectiles/bullethit_generic") && !strcmp(material,"metal"))impact_audio++;
    if(!strcmp(key,"projectiles/clip_ricochet"))clip_bounces++;
    if(!strcmp(key,"projectiles/clip_freeze"))clip_stops++;
}
const char *director_text(unsigned key){
    static const char *const skins[]={"","rat","scientist","young_max_payne"};
    return key<4?skins[key]:"";
}
void bank_scene_sound(unsigned owner,const char *group,const char *name,Point at,bool follow,bool spatial){
    (void)owner;(void)group;(void)name;(void)at;(void)follow;(void)spatial;
}
bool campaign_anchor_mark(Impact *mark,unsigned face){(void)mark;return face!=UINT32_MAX;}
void particles_surface_burst(unsigned owner,unsigned room,Point at,Point normal,const char *name){
    assert(fabsf(normal.x)+fabsf(normal.y)+fabsf(normal.z)>.9f);
    if(!strcmp(name,"bullethit_generic")){surface_at=at;surface_normal=normal;}
    particles_burst(owner,room,at,name);
}
void particles_blood(unsigned actor,unsigned room,Point impact,Point direction){
    assert(actor && actor<director.count && room==states[actor].room && isfinite(impact.x) && isfinite(direction.z));
    blood_hits++;
}
static int bullet_prop;
static bool drop_table;
unsigned campaign_room_id(void){return 0;}
unsigned campaign_exit_count(void){return 1;}
const CampaignExit *campaign_exit(unsigned id){return id?NULL:&door;}
Point campaign_transform(const CampaignExit *e,Point p,bool dir){
    (void)e;return (Point){p.z-(dir?0:1),p.y,-p.x};
}
Point campaign_from_neighbour(unsigned slot,Point p){
    assert(slot==0);return (Point){-p.z,p.y,p.x+1};
}
const CampaignExit *campaign_room_exit(unsigned room,unsigned slot){
    return room==0?campaign_exit(slot):NULL;
}
Point campaign_from_room_neighbour(unsigned room,unsigned slot,Point p){
    assert(room==0);
    return campaign_from_neighbour(slot,p);
}
bool campaign_sound_path(unsigned room,Point listener,unsigned source,Point at,const bool *enabled,Point *apparent,float *distance){
    if(room!=source){
        if(!enabled[door.id])return false;
        at=room==0?campaign_from_neighbour(0,at):campaign_transform(&door,at,false);
    }
    *apparent=at;
    *distance=hypotf(at.y-listener.y,hypotf(at.x-listener.x,at.z-listener.z));
    return true;
}
bool campaign_ray_exit(unsigned room,Point from,Point direction,float *limit,const bool *enabled,CampaignExit *out){
    if(room || (enabled && !enabled[0]) || direction.z<=0)return false;
    float distance=(1-from.z)/direction.z;
    if(distance<0 || distance>=*limit || fabsf(from.x+direction.x*distance)>1)return false;
    *limit=distance;*out=door;return true;
}
bool campaign_ray_wall(unsigned room,Point from,Point direction,float limit,Hit *hit,int *prop){
    *prop=-1;
    bool floor=false;
    if(ground && direction.y<0){
        float d=-from.y/direction.y;
        if(d>=0 && d<=limit){
            *hit=(Hit){{from.x+direction.x*d,0,from.z+direction.z*d},{0,1,0},d,0};
            limit=d;floor=true;
        }
    }
    if(room!=1 || !wall_x || direction.x<=0)return floor;
    float d=(wall_x-from.x)/direction.x;
    if(d<0 || d>limit)return floor;
    *hit=(Hit){{wall_x,from.y+d*direction.y,from.z+d*direction.z},{-1,0,0},d,0};
    *prop=17;return true;
}
FILE *campaign_file(const char *name){
    if(!strcmp(name,"combatants.bin")){
        if(!retail_bank)return NULL;
        char path[1024];const char *last=strrchr(retail_bank,'/');
        if(!last)return fopen(name,"rb");
        if(snprintf(path,sizeof path,"%.*s/%s",(int)(last-retail_bank),retail_bank,name)>=(int)sizeof path)return NULL;
        return fopen(path,"rb");
    }
    assert(!strcmp(name,"projectiles.bin"));
    if(retail_bank)return fopen(retail_bank,"rb");
    FILE *f=tmpfile();assert(f);
    uint32_t header[2]={1,0};fwrite("MPB1",4,1,f);fwrite(header,sizeof header,1,f);
    char id[48]="test",drop[32]={0};float values[]={15,0,0,20};uint16_t flags[4]={0};
    if(drop_table)strcpy(drop,"ingram");
    fwrite(id,sizeof id,1,f);fwrite(drop,sizeof drop,1,f);fwrite(values,sizeof values,1,f);fwrite(flags,sizeof flags,1,f);
    if(drop_table){
        uint32_t count=1,type=0;
        DropGrant grants[2]={{.weapons={"ingram"},.ammo="ingram",.amount=10},
                             {.weapons={"ingramdual"},.ammo="ingram",.amount=20}};
        fwrite("DRO1",4,1,f);fwrite(&count,sizeof count,1,f);fwrite(&type,sizeof type,1,f);
        fwrite(grants,sizeof grants,1,f);
    }
    rewind(f);return f;
}
void director_pose(unsigned id,Point *p,Point *forward,Point *up){
    (void)forward;(void)up;*p=states[id].position;
}
void director_hit(unsigned id,float damage){states[id].hp-=damage;states[id].dead=states[id].hp<=0;}
static bool glass_wall;
static unsigned glass_hits;
bool campaign_ray_projectile(unsigned room,Point start,Point dir,float limit,bool large,Hit *hit,int *prop){
    if(room==1 && glass_wall && !large){*prop=-1;return false;}
    return campaign_ray_wall(room,start,dir,limit,hit,prop);
}
void campaign_pierced_props(unsigned room,Point start,Point dir,float limit,uint32_t *seen,unsigned owner,float damage){
    (void)owner;(void)damage;
    if(room==1 && glass_wall && dir.x>0 && wall_x>=start.x && wall_x<=start.x+limit*dir.x && !(seen[0]&1)){
        seen[0]|=1;glass_hits++;
    }
}
void director_bullet(Point start,Point end,unsigned room,int prop,float damage,unsigned owner){
    (void)start;(void)end;(void)damage;(void)owner;bullet_room=room;bullet_prop=prop;
}
static void retail_pickup(const char *type,const char *air_mesh,const char *item,Effects *fx){
    assert(combat_open());ground=true;wall_x=.65f;director.exits[0]=true;
    states[1]=(EntityState){.enabled=true,.room=1,.hp=100,.position={.40f,0,0}};
    combat_launch(type,0,0,(Point){0,1.3f,.98f},(Point){0,0,1});
    Projectile *p=campaign_projectiles;bool pickup=false;
    assert(p->active && !strcmp(combat_mesh(p,&pickup),air_mesh) && !pickup && p->velocity.y>0);
    // Give every item the same path through an occupied doorway into a wall.
    p->velocity=(Point){0,0,5};
    for(unsigned i=0;i<15 && p->velocity.x>=0;i++){
        combat_tick(1.f/60,fx);
        if(p->room==1 && p->position.x>.60f && p->velocity.x<=0)break;
    }
    assert(p->room==1 && p->position.x>.60f && p->position.y>1 && p->velocity.x<=0 && !p->dropped);
    for(unsigned i=0;i<180 && !p->dropped;i++)combat_tick(1.f/60,fx);
    assert(p->active && p->dropped && p->resting && p->position.y<.04f && states[1].hp==100);
    assert(!strcmp(combat_mesh(p,&pickup),item) && pickup && combat_drop_grant(p,true));
    wall_x=0;ground=false;
}
int main(int argc,char **argv){
    // Retail heavy impact fragments use gravity 2 and an initial upward drift
    // of 3.5. They must turn downward within their visible lifetime; smoke's
    // negative gravity remains buoyant. Previously gravity was 9.81x too weak.
    float fragment=3.5f;
    for(unsigned i=0;i<20;i++)fragment=particle_vertical_step(fragment,2,1.f/60);
    assert(fragment<0 && particle_vertical_step(1,-.04f,.1f)>1);
    for(unsigned model=0;model<3;model++){
        ParticleDrag resistance={50,1,model,0};float speed=7;
        for(unsigned i=0;i<40;i++)speed=particle_drag_speed(speed,.4f,&resistance,.005f);
        float once=particle_drag_speed(7,.4f,&resistance,.2f);
        assert(speed>0 && speed<7 && fabsf(speed-once)<.0001f);
    }
    assert(particle_drag_speed(7,.4f,&(ParticleDrag){50,1,2,0},.4f)<.01f);
    for(unsigned i=0;i<3;i++){
        float depth=(float[]){.001f,.05f,.5f}[i],bias=depth_surface_bias(-depth,1);
        int exp;frexpf(depth,&exp);float ulp=ldexpf(1,exp-17);
        assert(bias>=ulp && bias<.0001f);
    }
    director=(Director){.count=2,.def=defs,.state=states};defs[1].flags=4;
    states[0]=(EntityState){.enabled=true,.hp=100};
    states[1]=(EntityState){.enabled=true,.room=1,.hp=100,.position={3,0,0}};
    Camera camera={.eye={0,1.3f,0},.forward={0,0,1},.right={1,0,0},.up={0,1,0}};
    Effects fx={0};Point muzzle={0,1.3f,.3f};director.exits[0]=director.ai_exits[0]=true;
    assert(combat_open());
    combat_hitscan(0,0,&camera,camera.eye,muzzle,1,10,100,0,&fx);
    assert(blood_hits==1 && states[1].hp==90 && bullet_room==1 && bullet_prop==-1);
    assert(!fx.next && fx.flash_room==0);
    wall_x=1;
    combat_hitscan(0,0,&camera,camera.eye,muzzle,1,10,100,0,&fx);
    assert(blood_hits==1 && states[1].hp==90 && bullet_room==1 && bullet_prop==17);
    assert(fx.next==1 && fx.marks[0].room==1 && fabsf(fx.marks[0].point.x-1.f)<.0001f);
    states[1].position.x=1.1f; // The capsule sticks through, its axis is behind cover.
    Effects covered={0};
    combat_hitscan(0,0,&camera,camera.eye,muzzle,1,10,100,0,&covered);
    assert(blood_hits==1 && states[1].hp==90 && bullet_prop==17 && covered.next==1);
    states[1].position.x=3;
    glass_wall=true;
    combat_hitscan(0,0,&camera,camera.eye,muzzle,1,10,100,0,&fx);
    assert(glass_hits==1 && blood_hits==2 && states[1].hp==80 && bullet_prop==-1);
    glass_wall=false;states[1].hp=90;
    wall_x=0;director.exits[0]=false;
    combat_hitscan(0,0,&camera,camera.eye,muzzle,1,10,100,0,&fx);
    assert(states[1].hp==90 && bullet_room==0);
    assert(fx.next==1);
    assert(combat_launch("test",0,0,muzzle,camera.forward));
    combat_tick(.10f,&fx);
    assert(!campaign_projectiles[0].active && campaign_projectiles[0].room==0);
    assert(campaign_projectiles[0].position.z<1.f);
    director.exits[0]=true;Point remote_eye={3,1.3f,0},forward;
    assert(combat_sight(1,remote_eye,0,camera.eye,10,true,&forward) && forward.x<-.99f);
    director.ai_exits[0]=false;assert(!combat_sight(1,remote_eye,0,camera.eye,10,true,&forward));
    assert(combat_sight(1,remote_eye,0,camera.eye,10,false,&forward));
    wall_x=1;assert(!combat_sight(1,remote_eye,0,camera.eye,10,false,&forward));wall_x=0;
    Player player={0};states[1].hostile=true;
    float focus=0;
    assert(combat_aim_target(&player,DIRECTOR_NONE,.3f,&forward,&focus)==1 && forward.z>.99f);
    // A close target reached through a doorway must also sit on the actual
    // camera ray, even after a wall has pushed the over-shoulder lens inward.
    Camera assisted={.eye={.1587f,1.57f,-1.09f},.forward={.04f,0,1}};
    player.pitch=atan2f(forward.y,hypotf(forward.x,forward.z));
    player.yaw=atan2f(forward.x,forward.z);
    scene_camera_converge(&assisted,&player,focus,1);
    Point chest={forward.x*focus,1.42f+forward.y*focus,forward.z*focus};
    float travel=(chest.z-assisted.eye.z)/assisted.forward.z;
    assert(travel>0 && fabsf(assisted.eye.x+travel*assisted.forward.x-chest.x)<1e-4f);
    assert(fabsf(assisted.eye.y+travel*assisted.forward.y-chest.y)<1e-4f);
    // The shooter should block a kill-camera orbit even during dodge immunity.
    director.dodge_pass_projectiles=true;
    combat_camera_bodies(1,0);
    Hit obstruction;
    assert(combat_camera_blocked((Point){0,1,1.4f},(Point){0,0,-1},2,&obstruction));
    assert(obstruction.distance>0 && obstruction.distance<1.4f);
    assert(!combat_camera_blocked((Point){0,1,1.4f},(Point){1,0,0},2,&obstruction));
    director.dodge_pass_projectiles=false;
    player.pitch=0;player.yaw=0;
    wall_x=1;assert(combat_aim_target(&player,1,.3f,&forward,&focus)==DIRECTOR_NONE);
    wall_x=0;director.exits[0]=false;assert(combat_aim_target(&player,1,.3f,&forward,&focus)==DIRECTOR_NONE);
    director.exits[0]=true;player.yaw=1.2f;
    assert(combat_aim_target(&player,1,.3f,&forward,&focus)==DIRECTOR_NONE);
    player.yaw=0;crouched[1]=true;
    assert(combat_aim_target(&player,1,.3f,&forward,&focus)==1 && forward.y<-.14f);
    Camera high=camera;high.eye.y=1.55f;
    Point barrel=high.eye;barrel.z+=.3f;
    combat_hitscan(0,0,&high,high.eye,barrel,1,1,100,0,&fx);
    assert(states[1].hp==90);
    high.eye.y=barrel.y=1.0f;
    combat_hitscan(0,0,&high,high.eye,barrel,1,1,100,0,&fx);
    assert(states[1].hp==85);
    crouched[1]=false;states[1].hp=90;
    combat_launch("test",0,0,muzzle,camera.forward);combat_tick(.1f,&fx);
    assert(campaign_projectiles[0].active && campaign_projectiles[0].room==1 && campaign_projectiles[0].velocity.x>14);
    combat_tick(.2f,&fx);assert(!campaign_projectiles[0].active && states[1].hp==70);
    drop_table=true;assert(combat_open());
    Projectile drop={.active=true,.dropped=true,.room=1};
    bool pickup=false;
    const char *mesh=combat_mesh(&drop,&pickup);
    assert(mesh && !strcmp(mesh,"ingram") && pickup);
    const DropGrant *first=combat_drop_grant(&drop,true),*repeat=combat_drop_grant(&drop,false);
    assert(first && !strcmp(first->weapons[0],"ingram") && first->amount==10);
    assert(repeat && !strcmp(repeat->weapons[0],"ingramdual") && repeat->amount==20);
    drop_table=false;assert(combat_open() && !combat_drop_grant(&drop,true));
    if(argc==2){
        ParticleLayer layer={0};
        for(unsigned k=0;k<8;k++)layer.keys[k].emitted=500.f*k/7;
        for(unsigned rate=20;rate<=120;rate+=10){
            unsigned emitted=0;
            for(unsigned i=0;i<5*rate;i++){
                int delta=particle_emission_count(&layer,(float)i/rate,(float)(i+1)/rate,.1f,5,.5f,1);
                if((float)i/rate>=.1f)assert(!delta);
                emitted+=delta;
            }
            assert(emitted==10);
        }
        retail_bank=argv[1];assert(combat_open());
        unsigned decals=0;
        for(unsigned i=0;i<PROJECTILE_LIMIT;i++){
            const char *type=i&1?"decal_random_bullet":"debris_wood_crate";
            decals+=combat_launch(type,0,0,muzzle,camera.forward);
        }
        assert(decals>0 && decals<PROJECTILE_LIMIT);
        assert(combat_launch("damage_mine",0,0,muzzle,camera.forward));
        assert(combat_launch("grenade",0,0,muzzle,camera.forward));
        assert(combat_launch("enemy_berettadual",0,0,muzzle,camera.forward));
        assert(combat_valid() && combat_open());
        states[1]=(EntityState){.enabled=true,.room=1,.hp=100,.position={1,0,0}};
        wall_x=2;ground=true;fx=(Effects){0};unsigned before=blood_hits;
        combat_hitscan(0,0,&camera,camera.eye,muzzle,1,5,100,"bullet_beretta",&fx);
        assert(blood_hits==before+1 && campaign_projectiles[0].active && campaign_projectiles[0].owner==1);
        assert(fx.next_blood==1 && fx.marks[SURFACE_MARKS].room==1 && fx.marks[SURFACE_MARKS].normal.y>.99f);
        for(unsigned i=0;i<60 && campaign_projectiles[0].active;i++)combat_tick(1.f/60,&fx);
        assert(fx.next && fx.marks[0].decal && fx.marks[0].room==1);
        states[1].enabled=false;wall_x=1;
        combat_hitscan(0,0,&camera,camera.eye,muzzle,1,5,100,"bullet_beretta",&fx);
        assert(impact_audio==1 && impact_sparks==1);
        Impact *wall_mark=&fx.marks[(fx.next-1)%SURFACE_MARKS];
        assert(fabsf(surface_at.x-wall_x)<.0001f && surface_normal.x<-.99f);
        assert(wall_mark->point.x==surface_at.x && wall_mark->point.y==surface_at.y && wall_mark->point.z==surface_at.z);
        for(unsigned i=0;i<SURFACE_MARKS+2;i++)
            combat_hitscan(0,0,&camera,camera.eye,muzzle,1,5,100,"bullet_beretta",&fx);
        scene_age(&fx,30);
        assert(fx.marks[SURFACE_MARKS].life>0 && fx.marks[SURFACE_MARKS].decal && fx.marks[0].life==0);
        ground=false;
        assert(combat_open());wall_x=0;
        combat_launch("grenade",0,0,muzzle,camera.forward);
        assert(!explosions);
        for(unsigned i=0;i<150 && !explosions;i++)combat_tick(1.f/60,&fx);
        assert(explosions==1);
        assert(fx.flash==0); // Authored particles own explosion and muzzle art.
        for(unsigned melee=0;melee<2;melee++){
            assert(combat_open());
            // Retail neck/head frame: local -Y points ahead, rather than down
            // the world's Y axis. A miss must not project a puff onto the floor.
            const Point axes[]={{1,0,0},{0,0,-1},{0,1,0}};
            states[1].enabled=false;ground=true;unsigned marks=fx.next;
            assert(combat_launch_attached(melee?"bullet_leadpipe":"bullet_baseballbat",0,0,muzzle,axes));
            assert(campaign_projectiles[0].velocity.z>0 && fabsf(campaign_projectiles[0].velocity.y)<5);
            for(unsigned i=0;i<30;i++)combat_tick(1.f/60,&fx);
            assert(fx.flash==0); // A melee damage volume must never draw fire.
            assert(fx.next==marks);ground=false;states[1].enabled=true;
        }
        // One grenade on the near side, with an NPC one metre past the rotated door.
        // A closed door stops noise and damage. A solid wall beyond an open door
        // still protects against the blast while the noise reaches the next room.
        for(unsigned scenario=0;scenario<3;scenario++){
            assert(combat_open());director.exits[0]=scenario!=0;
            wall_x=scenario==2?.55f:0;
            states[1]=(EntityState){.enabled=true,.room=1,.position={1,0,0},.hp=1000,.weapon=2};
            assert(combat_launch("grenade",0,0,(Point){0,.9f,.7f},camera.forward));
            campaign_projectiles[0].resting=true;
            for(unsigned f=0;f<300 && campaign_projectiles[0].active;f++)combat_tick(1.f/60,&fx);
            assert(states[1].hostile==(scenario!=0));
            assert((states[1].hp<1000)==(scenario==1));
        }
        for(unsigned scenario=0;scenario<3;scenario++){
            defs[1].flags=scenario==2?16:4;defs[1].prop=scenario==2?17:DIRECTOR_NONE;
            assert(combat_open());wall_x=scenario?.55f:0;
            states[1]=(EntityState){.enabled=true,.room=1,.position={1,0,0},.hp=1000};
            assert(combat_launch("grenade",0,1,(Point){.1f,.9f,0},(Point){1,0,0}));
            campaign_projectiles[0].resting=true;
            for(unsigned f=0;f<300 && campaign_projectiles[0].active;f++)combat_tick(1.f/60,&fx);
            assert((states[1].hp<1000)==(scenario!=1));
        }
        defs[1].flags=4;defs[1].prop=DIRECTOR_NONE;
        wall_x=0;director.exits[0]=true;states[1].enabled=false;
        puts("Grenade noise and damage cross an open doorway. Cover blocks damage and a closed doorway blocks both.");

        retail_pickup("enemy_berettadual","beretta","berettadual",&fx);
        retail_pickup("enemy_molotov","molotov","molotov",&fx);
        retail_pickup("enemy_painkiller","medipack_small","medipack_small",&fx);
        assert(combat_open() && combat_has_weapon_drops());
        states[1].room=0;states[1].dead=true;states[1].loot_collected=false;
        // A full pool during a death frame must allow the event to retry.
        for(unsigned i=0;i<PROJECTILE_LIMIT;i++)campaign_projectiles[i].active=true;
        assert(!combat_drop_on_death(1,"berettadual",muzzle,camera.forward) && !states[1].loot_collected);
        campaign_projectiles[0].active=false;
        assert(combat_drop_on_death(1,"berettadual",muzzle,camera.forward) && states[1].loot_collected);
        assert(campaign_projectiles[0].owner==1 && !strcmp(combat_mesh(campaign_projectiles,&pickup),"beretta") && !pickup);
        assert(!combat_drop_on_death(1,"berettadual",muzzle,camera.forward));
        assert(combat_open());states[1].loot_collected=false;
        assert(combat_drop_on_death(1,"baseballbat",muzzle,camera.forward) && !campaign_projectiles[0].active);
        assert(combat_open());ground=true;
        combat_launch("clip_beretta",0,0,(Point){0,1.3f,0},(Point){0,0,1});
        for(unsigned i=0;i<120;i++)combat_tick(1.f/60,&fx);
        assert(clip_bounces && clip_stops==1 && campaign_projectiles[0].resting);
        puts("Retail drops cross an occupied doorway, bounce off the wall and become pickups on the floor. Reload debris plays its landing cue once.");
        defs[1].model=1;assert(combat_open());combat_slow_rounds(false);wall_x=0;
        states[1]=(EntityState){.enabled=true,.hostile=true,.room=1,.hp=3,.position={1,0,0}};
        assert(combat_aim_target(&player,DIRECTOR_NONE,.8f,&forward,&focus)==DIRECTOR_NONE);
        combat_hitscan(0,0,&camera,camera.eye,muzzle,1,8,100,"bullet_beretta",&fx);
        assert(states[1].hp==3);
        Camera low=camera;low.eye.y=.1f;Point low_muzzle=low.eye;low_muzzle.z+=.25f;
        combat_hitscan(0,0,&low,low.eye,low_muzzle,1,8,100,"bullet_beretta",&fx);
        assert(states[1].dead);
        KillRewards rat=combat_take_rewards();assert(!rat.timed && !rat.health && !rat.slow);
        defs[1].model=3;assert(combat_open());
        states[1]=(EntityState){.enabled=true,.hostile=true,.room=1,.hp=1,.position={1,0,0}};
        combat_slow_rounds(true);
        combat_hitscan(0,0,&camera,camera.eye,muzzle,1,5,100,"bullet_beretta",&fx);
        assert(campaign_projectiles[0].active && states[1].hp==1);
        for(unsigned i=0;i<90 && !states[1].dead;i++)combat_tick(.001f,&fx);
        assert(states[1].dead);
        KillRewards human=combat_take_rewards();assert(human.timed==1 && fabsf(human.slow-.12f)<.0001f);
        assert(!combat_take_rewards().timed);combat_slow_rounds(false);
        director.count=3;defs[1].flags=16;defs[2]=(EntityDef){.flags=4,.model=3};
        states[2]=(EntityState){.enabled=true,.room=1,.hp=20,.position={1,0,0}};
        assert(combat_open());
        combat_hitscan(0,0,&camera,camera.eye,muzzle,1,5,100,"bullet_beretta",&fx);
        assert(states[2].hp==15 && states[1].dead);
        puts("Skin capsule, auto-aim exclusion, one-time rewards and travelling shot through a rotated door verified.");
        puts("Retail bullet hits, blood decals and grenade effects verified.");
    }
    combat_close();puts("Rotated doorway, obstruction, enemy sight and projectile checks passed.");
}
