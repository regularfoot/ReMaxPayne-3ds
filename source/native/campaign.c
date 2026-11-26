#include "language.h"
#include "animation_state.h"
#include "dodge_animation.h"
#include "weapon_timing.h"
#include "campaign_file.h"
#include "campaign.h"
#include "game_rules.h"
#include "audio_bank.h"
#include "campaign_world.h"
#include "campaign_bounds.h"
#include "campaign_nav.h"
#include "campaign_combat.h"
#include "comic.h"
#include "director.h"
#include "movie.h"
#include "presentation.h"
#include "particles.h"
#include "pose_bank.h"
#include "camera_path.h"
#include "item_mesh.h"
#include "phone.h"
#include "render.h"
#include "save.h"
#include "async_save.h"
#include "async_log.h"
#include "localization.h"
#include "touch.h"
#include "gyro.h"
#include <3ds.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef enum { PISTOL,DUAL_PISTOL,EAGLE,PUMP,SAWED,JACKHAMMER,INGRAM,DUAL_INGRAM,COMMANDO,
               SNIPER,BAT,PIPE,GRENADE,MOLOTOV,M79,GUNS } GunId;
typedef struct {const char *id,*title;unsigned capacity,pocket,damage,pellets;float delay;Weapon pose;bool automatic;} Gun;
static const Gun guns[]={
    {"beretta",MP_TEXT("Беретта","Beretta"),18,162,5,1,.25f,BERETTA,false},{"berettadual",MP_TEXT("Две Беретты","Dual Berettas"),36,162,5,2,.25f,BERETTA,false},
    {"deserteagle","Desert Eagle",12,48,10,1,1.f/3,BERETTA,false},{"pumpshotgun",MP_TEXT("Дробовик","Pump Shotgun"),7,80,5,5,1.f,SHOTGUN,false},
    {"sawedshotgun",MP_TEXT("Обрез","Sawed-Off"),2,80,5,5,.5f,SHOTGUN,false},{"jackhammer","Jackhammer",12,168,5,5,1.f/3,SHOTGUN,true},
    {"ingram","Ingram",50,300,5,1,.125f,BERETTA,true},{"ingramdual",MP_TEXT("Два Ingram","Dual Ingrams"),100,300,5,2,.125f,BERETTA,true},
    {"mp5","Colt Commando",30,150,10,1,1.f/6,RIFLE,true},{"sniper",MP_TEXT("Снайперская винтовка","Sniper Rifle"),5,20,70,1,1.f,RIFLE,false},
    {"baseballbat",MP_TEXT("Бита","Baseball Bat"),0,0,15,1,.52f,BERETTA,false},{"leadpipe",MP_TEXT("Труба","Lead Pipe"),0,0,10,1,.52f,BERETTA,false},
    {"grenade",MP_TEXT("Граната","Grenade"),1,10,50,1,1.f,BERETTA,false},{"molotov",MP_TEXT("Коктейль Молотова","Molotov Cocktail"),1,10,20,1,1.f,BERETTA,false},
    {"m79","M79",1,10,70,1,1.f,RIFLE,false}
};
typedef struct {bool owned;unsigned rounds,reserve;} Stock;
typedef struct {
    Stock stock[GUNS];int equipped;unsigned pills,zoom;
    float recovery,bullet_time,dodge_time,dodge_x,dodge_z;
    bool slow;Clip dodge;
    uint16_t infinite_ammo; // Occupies the two trailing padding bytes of v2-v8 ARM saves.
} Inventory;
_Static_assert(sizeof(Inventory)==216 && offsetof(Inventory,infinite_ammo)==214,"ARM inventory save layout");
static const char *const gun_round[GUNS]={"bullet_beretta","bullet_berettadual","bullet_deserteagle","bullet_pumpshotgun",
    "bullet_sawedshotgun","bullet_jackhammer","bullet_ingram","bullet_ingram","bullet_mp5","bullet_sniper",
    "bullet_baseballbat","bullet_leadpipe",NULL,NULL,"bullet_m79"};
static Inventory inventory;
static float health_fraction;
static bool player_crouched;
static unsigned retry_deaths;
static const char *const mode_ids[]={"normal","nightmare","hell","timedmode"};
static bool sound_ready,hud_dirty;
static FILE *log_file;
typedef struct {char items[3][96],caption[192];float seconds;bool always;} Journal;
static Journal journal_state;
static char current_objective[3][160];
static char chapter[64];
static Enemy *actors;
typedef struct {
    float seconds,delay,dodge_remaining;
    uint16_t patrol[6];
    uint8_t face,default_weapon,patrol_count,waypoint,dodge,shoot_dodge;
} ActorExtra;
static ActorExtra *visuals;
static NavAgent *paths;
static PlatformContact *platforms;
static unsigned actor_ids[CAST_SLOTS];
static int find_gun(const char *name) {
    if(!strcmp(name,"coltcommando")) name="mp5";
    for(unsigned i=0;i<GUNS;i++) if(!strcmp(name,guns[i].id)) return i;
    return -1;
}
static int pool_id(int gun) {
    if(gun==DUAL_PISTOL) gun=PISTOL;
    if(gun==DUAL_INGRAM) gun=INGRAM;
    if(gun==SAWED) gun=PUMP;
    return gun;
}
static Stock *ammo_pool(int gun) {return inventory.stock+pool_id(gun);}

// A fresh NPC weapon has a loaded magazine. An explicit ammo grant replaces
// the default spare, including when the script grants ammunition before the gun.
typedef struct {
    uint16_t issued,infinite,supplied,reserve[GUNS];
    uint8_t rounds[GUNS],held;
} NpcAmmo;
static NpcAmmo **arsenals,*npc_stock;
static unsigned npc_count;
static bool open_arsenals(void){
    npc_count=0;
    for(unsigned i=1;i<director.count;i++)if(director.def[i].flags&4)npc_count++;
    arsenals=calloc(director.count,sizeof(*arsenals));
    npc_stock=calloc(npc_count?npc_count:1,sizeof(*npc_stock));
    if(!arsenals || !npc_stock)return false;
    unsigned next=0;
    for(unsigned i=1;i<director.count;i++)if(director.def[i].flags&4)arsenals[i]=npc_stock+next++;
    return true;
}
static void equip_npc(unsigned id,int gun){
    NpcAmmo *a=arsenals[id];
    if(a->held!=gun+1 && actors[id].animation.action==RELOAD)actor_action(&actors[id].animation,IDLE);
    a->held=gun+1;
    if(gun<0 || (a->issued&(1u<<gun)))return;
    a->issued|=1u<<gun;a->rounds[gun]=guns[gun].capacity;
    int pool=pool_id(gun);
    if(!(a->supplied&(1u<<pool)) && !a->reserve[pool])a->reserve[pool]=guns[gun].capacity;
}
static void npc_inventory(unsigned id,const char *op,const char *const *args,unsigned count){
    NpcAmmo *a=arsenals[id];
    if(!strcmp(op,"c_removeallweapons")){
        uint16_t infinite=a->infinite;
        *a=(NpcAmmo){.infinite=infinite};
        if(actors[id].animation.action==RELOAD)actor_action(&actors[id].animation,IDLE);
    }
    if(!strcmp(op,"c_pickupweapon") || !strcmp(op,"c_removeallweapons")){
        equip_npc(id,count?find_gun(args[0]):-1);return;
    }
    if(count<2)return;
    if(!strcmp(op,"c_pickupammo")){
        int gun=find_gun(args[0]);if(gun<0)return;
        int pool=pool_id(gun);long amount=strtol(args[1],NULL,10);
        if(!(a->supplied&(1u<<pool)))a->reserve[pool]=0;
        a->supplied|=1u<<pool;
        if(amount>0)a->reserve[pool]+=amount>UINT16_MAX-a->reserve[pool]?UINT16_MAX-a->reserve[pool]:amount;
    }else if(!strcmp(op,"c_setinfiniteammo")){
        int gun=find_gun(args[0]);if(gun<0)return;
        if(!strcmp(args[1],"true") || atoi(args[1])!=0)a->infinite|=1u<<gun;
        else a->infinite&=~(1u<<gun);
    }
}
static bool infinite_rounds(int gun){return (inventory.infinite_ammo&(1u<<gun))!=0;}
static bool spare_rounds(int gun){return ammo_pool(gun)->reserve || infinite_rounds(gun);}
static void grant_ammo(const char *name,unsigned amount){
    if(!strcmp(name,"painkiller"))inventory.pills+=amount>8-inventory.pills?8-inventory.pills:amount;
    else {
        int gun=find_gun(name);if(gun<0)return;
        Stock *pool=ammo_pool(gun);
        pool->reserve=fminf(guns[gun].pocket,pool->reserve+roundf(amount*game_tuning()->ammo));
    }
    hud_dirty=true;
}
static bool npc_has_spare(const NpcAmmo *a,int gun){
    return (a->infinite&(1u<<gun)) || a->reserve[pool_id(gun)];
}
static void refill_npc(NpcAmmo *a,int gun){
    unsigned fill=guns[gun].capacity-a->rounds[gun];
    if(gun==PUMP && fill>1)fill=1;
    if(!(a->infinite&(1u<<gun))){
        uint16_t *reserve=a->reserve+pool_id(gun);
        if(fill>*reserve)fill=*reserve;
        *reserve-=fill;
    }
    a->rounds[gun]+=fill;
}
static unsigned maximum_health(void) {
    return director.def[0].max_health?director.def[0].max_health:100;
}
static float axis(int value) {
    int dead=5+ui_settings.deadzone*10;
    if(abs(value)<=dead) return 0;
    return copysignf(fminf(1,(abs(value)-dead)/(150.f-dead)),value);
}
static void pickup(const char *name) {
    if(!strcmp(name,"painkiller")) return;
    int gun=find_gun(name);if(gun<0) return;
    Stock *s=inventory.stock+gun;
    bool fresh=!s->owned;
    if(fresh){s->owned=true;s->rounds=guns[gun].capacity;}
    if(inventory.equipped<0 || (ui_settings.autoswitch && fresh)){inventory.equipped=gun;inventory.zoom=1;}
    hud_dirty=true;
}
static void localized(const char *key,char *text,size_t capacity) {
    localization_read(key,text,capacity);
}
static void objective(const char *key,unsigned line) {
    if(line<3){localized(key,current_objective[line],sizeof current_objective[line]);hud_dirty=true;}
}
static void objective_text(char result[483]) {
    result[0]=0;
    for(unsigned i=0;i<3;i++)if(current_objective[i][0]){
        if(result[0])strcat(result," ");
        strcat(result,current_objective[i]);
    }
}
static void action(unsigned id,const char *op,const char *const *args,unsigned count) {
    presentation_event(op,args,count);
    if(paths && id<director.count)campaign_nav_event(paths+id,op);
    if(visuals && id<director.count){
        ActorExtra *extra=visuals+id;
        if(!strcmp(op,"c_patrol") && count>1){
            extra->patrol_count=0;extra->waypoint=0;
            for(unsigned i=0;i+1<count && i<6;i++){
                unsigned waypoint=director_find(args[i]);
                if(waypoint<director.count)extra->patrol[extra->patrol_count++]=waypoint;
            }
        }
        if(!strcmp(op,"c_goto") || !strcmp(op,"c_gotoplayer") || !strcmp(op,"c_gotoandshoot"))extra->patrol_count=0;
        if(!strcmp(op,"c_setstatemachine")){
            const char *ai=args[0];
            extra->delay=0;extra->dodge_remaining=0;
            if(!strncmp(ai,"delay",5))extra->delay=strtof(ai+5,NULL)*(strstr(ai,"ms")?.001f:1);
            if(!strncmp(ai,"single",6)){
                extra->dodge=strstr(ai,"left")?DODGE_LEFT:strstr(ai,"right")?DODGE_RIGHT:strstr(ai,"backward")?DODGE_BACKWARD:DODGE_FORWARD;
                extra->dodge_remaining=actor_duration(extra->dodge);extra->shoot_dodge=strstr(ai,"shoot")!=NULL;
            }
        }
        if(!strcmp(op,"c_animatetexture") && count>=2 && !strcmp(args[0],"facetexture_id"))
            visuals[id].face=!strcmp(args[1],"facetexture_death_id")?1:!strcmp(args[1],"facetexture_wounded_id")?2:0;
        if(!strcmp(op,"ws_animate") && !strcmp(args[0],"weaponanim_default")){
            visuals[id].default_weapon=true;visuals[id].seconds=0;
        }
    }
    if(!strcmp(op,"gm_enablegamelevel") && count==2 && !strcmp(args[0],mode_ids[game_run.mode])){
        for(unsigned i=1;i<GAME_MODES;i++)if(!strcmp(args[1],mode_ids[i]) && !save_unlock_mode(i))
            ui_notice(MP_TEXT("Не удалось записать открытый режим","Could not save the unlocked mode"));
    }
    if(!strcmp(op,"ps_starteffect") || !strcmp(op,"ps_stopallemissions"))particles_event(id,op,args[0]);
    if(!strcmp(op,"mphm_changehudvisibility") || !strcmp(op,"mphm_enablehud"))hud_dirty=true;
    if(!strcmp(op,"gm_setplayercontrolledslowmotion")){if(!director.player_slow)inventory.slow=false;hud_dirty=true;}
    if(!strcmp(op,"gm_resetlevelitems")){memset(journal_state.items,0,sizeof journal_state.items);hud_dirty=true;}
    if(!strcmp(op,"mphm_printinventory") && count>=2) {
        unsigned line=atoi(args[1]);
        if(line<3)localized(args[0],journal_state.items[line],sizeof journal_state.items[line]);
        hud_dirty=true;
    }
    if(!strcmp(op,"mphm_print") || !strcmp(op,"mphm_printtip") || !strcmp(op,"mphm_printtipalways") || (!strcmp(op,"mphm_printtipnym") && game_run.mode==NEW_YORK_MINUTE)){
        localized(args[0],journal_state.caption,sizeof journal_state.caption);
        journal_state.seconds=6;journal_state.always=strcmp(op,"mphm_printtip")!=0;hud_dirty=true;
    }
    if(!strcmp(op,"gm_transferplayerweaponto") && count)
        director_transfer_weapon(args[0],inventory.equipped>=0?guns[inventory.equipped].id:"");
    if(id<director.count && (!strcmp(op,"p_createprojectile") || !strcmp(op,"p_createprojectiletobone"))) {
        Point origin,forward;director_pose(id,&origin,&forward,NULL);
        if(!strcmp(op,"p_createprojectiletobone") && count>2 && actors && (director.def[id].flags&4)){
            Player body=actors[id].body;body.position=director.state[id].position;body.yaw=director.state[id].yaw;
            origin=actor_attachment(&body,&actors[id].animation,!strcmp(args[2],"gun"));
        }
        unsigned copies=count>1?atoi(args[1]):1;
        for(unsigned i=0;i<(copies?copies:1) && i<16;i++) combat_launch(args[0],id,director.state[id].room,origin,forward);
    }
    if(id && id<director.count && (director.def[id].flags&4) && arsenals && op[0]=='c')npc_inventory(id,op,args,count);
    if(!id) {
        if(!strcmp(op,"c_pickupweapon")) pickup(args[0]);
        else if(!strcmp(op,"c_removeallweapons")) {
            memset(inventory.stock,0,sizeof inventory.stock);inventory.equipped=-1;inventory.zoom=1;hud_dirty=true;
            pickup(args[0]);
        } else if(!strcmp(op,"c_pickupammo")) {
            grant_ammo(args[0],strtoul(args[1],NULL,10));
        } else if(!strcmp(op,"c_setinfiniteammo") && count>=2){
            int gun=find_gun(args[0]);
            if(gun>=0){
                if(!strcmp(args[1],"true") || atoi(args[1]))inventory.infinite_ammo|=1u<<gun;
                else inventory.infinite_ammo&=~(1u<<gun);
            }
        }
    }
    if(!strcmp(op,"mphm_printobjective")) objective(args[0],count>1?(unsigned)atoi(args[1]):0);
    if(!strcmp(op,"a_playmusic")) bank_music(args[0]);
    if(!strcmp(op,"a_stopall3dsounds")) bank_stop(id);
    if(!strcmp(op,"a_play3dsound") || !strcmp(op,"a_playsound") || !strcmp(op,"a_playfloating3dsound")) {
        if(id && id<director.count){
            bool floating=!strcmp(op,"a_playfloating3dsound");
            Point pos=director.state[id].position;
            if(floating && !(director.def[id].flags&4))director_pose(id,&pos,NULL,NULL);
            bank_scene_sound(id,args[0],args[1],pos,!floating,strcmp(op,"a_playsound")!=0);
        }else bank_sound(id,args[0],args[1]);
    }
    if(log_file && (!strcmp(op,"gm_init") || !strcmp(op,"mpgnm_pickupnote") || !strcmp(op,"gm_setplayercontrols") ||
        !strcmp(op,"c_teleport") || !strcmp(op,"mphm_printobjective"))) {
        fprintf(log_file,"CAMPAIGN event=%s entity=%u arg=%s\n",op,id,args[0]);fflush(log_file);
    }
}
static bool collect_drop(const Projectile *drop,const char *name){
    int gun=find_gun(name);bool pills=!strcmp(name,"medipack_small");
    if(gun<0 && !pills)return false;
    bool fresh=!pills && !inventory.stock[gun].owned;
    const DropGrant *g=combat_drop_grant(drop,fresh);
    if(pills && inventory.pills>=8)return false;
    if(!pills && !fresh && ammo_pool(gun)->reserve>=guns[gun].pocket){
        bool missing=false;
        if(g)for(unsigned i=0;i<2;i++){
            int companion=find_gun(g->weapons[i]);
            if(companion>=0 && !inventory.stock[companion].owned)missing=true;
        }
        if(!missing)return false;
    }
    if(g){
        for(unsigned i=0;i<2 && g->weapons[i][0];i++)pickup(g->weapons[i]);
        grant_ammo(g->ammo,g->amount);
        bank_sound(0,"level_items",g->sound);
        localized(g->caption,journal_state.caption,sizeof journal_state.caption);
        journal_state.seconds=6;journal_state.always=true;
    }else if(pills)grant_ammo("painkiller",1);
    else{
        pickup(name);
        if(!fresh)grant_ammo(name,guns[gun].capacity);
    }
    return true;
}
static bool read_notes(void) {
    const char *names[DIRECTOR_NOTES];
    for(unsigned i=0;i<director.note_count;i++) names[i]=director_text(director.notes[i]);
    if(C3D_FrameBegin(0)) C3D_FrameEnd(0);
    bool ok=comic_campaign(names,director.note_count,log_file);
    director.novel=false;director.note_count=0;hud_dirty=true;return ok;
}
static bool enter(unsigned id) {
    u64 start=svcGetSystemTick();
    if(!room_campaign_enter(id)) return false;
    for(unsigned k=0;k<CAST_SLOTS;k++) actor_ids[k]=DIRECTOR_NONE;
    if(log_file){
        fprintf(log_file,"CAMPAIGN room=%u %s\nPERF load_ms=%.1f linear_free=%lu\n",id,campaign_room_name(),
                (svcGetSystemTick()-start)*(1000./SYSCLOCK_ARM11),(unsigned long)linearSpaceFree());
    }
    return true;
}
typedef struct {Point feet;unsigned room;} GroundAnchor;
static void remember_ground(GroundAnchor *anchor,const Player *p){
    Hit floor;
    if(!scene_ground(p->position,&floor) || campaign_hit_prop(floor.face)>=0)return;
    *anchor=(GroundAnchor){floor.point,campaign_room_id()};
}
// Input, falling and moving platforms must transfer the same gameplay state.
static bool transfer_player(Player *p,Jump *jump,Camera *camera,Point *fall_reference,
                            unsigned *physics_room,unsigned slot,bool settle,u64 *previous){
    CampaignExit exit=*campaign_exit(slot);campaign_cross(p,&exit);
    camera->offset=campaign_transform(&exit,camera->offset,true);
    if(inventory.dodge_time>0){
        Point direction=campaign_transform(&exit,(Point){inventory.dodge_x,0,inventory.dodge_z},true);
        inventory.dodge_x=direction.x;inventory.dodge_z=direction.z;
    }
    u64 load_start=svcGetSystemTick();
    if(!enter(exit.room))return false;
    *previous+=svcGetSystemTick()-load_start;*physics_room=exit.room;
    if(settle && !scene_settle_step(p,jump))jump->airborne=true;
    *fall_reference=jump->airborne?campaign_transform(&exit,*fall_reference,false):p->position;
    return true;
}
typedef struct {u64 audio,room,contact,save,gyro,input;} SetupTiming;
typedef struct {
    unsigned room,frames,over_budget,worst_from;
    u64 total,worst,setup,simulation,prepare,draw;
    SetupTiming parts;
    RoomDrawTiming render;
} RoomTiming;
static void log_room_timing(const char *stage,const RoomTiming *t) {
    if(!log_file || !t->frames)return;
    const double ms=1000./SYSCLOCK_ARM11;
    fprintf(log_file,"PERF stage=%s room=%u frames=%u mean_ms=%.2f worst_ms=%.2f over_20ms=%u "
            "worst_from=%u setup_ms=%.2f sim_ms=%.2f prepare_ms=%.2f draw_ms=%.2f "
            "audio_ms=%.2f room_ms=%.2f contact_ms=%.2f save_ms=%.2f gyro_ms=%.2f input_ms=%.2f "
            "gpu_wait_ms=%.2f skin_ms=%.2f effects_ms=%.2f commands_ms=%.2f gpu_end_ms=%.2f\n",
            stage,t->room,t->frames,t->total*ms/t->frames,t->worst*ms,t->over_budget,
            t->worst_from,t->setup*ms,t->simulation*ms,t->prepare*ms,t->draw*ms,
            t->parts.audio*ms,t->parts.room*ms,t->parts.contact*ms,t->parts.save*ms,t->parts.gyro*ms,t->parts.input*ms,
            t->render.wait*ms,t->render.skin*ms,t->render.effects*ms,t->render.commands*ms,t->render.finish*ms);
}
static bool camera_obstacle(Point start,Point dir,float length,Hit *hit){
    return campaign_camera_trace(start,dir,length,director.exits,hit);
}
static bool cinematic_obstacle(Point start,Point dir,float length,Hit *hit){
    bool wall=camera_obstacle(start,dir,length,hit);
    return combat_camera_blocked(start,dir,wall?hit->distance:length,hit) || wall;
}
static bool death_camera(void){
    return !director.state[0].dead && director.path.path!=DIRECTOR_NONE && director.path.parented &&
           director.path.owner>0 && director.path.owner<director.count &&
           director.state[director.path.owner].dead && (director.def[director.path.owner].flags&4);
}
static Point death_camera_pivot(void){
    Point pivot=director.state[director.path.owner].position;
    pivot.y+=.60f;
    return pivot;
}
static bool local_cast(const Player *player,const Camera *camera,const Point *subject,Enemy render[],unsigned *count) {
    Point pivot=player->position;pivot.y+=player->crouched?.91f:1.42f;
    bool follow=(director.controls || director.state[0].dead || director.skip_death_view) && director.state[0].room==campaign_room_id();
    if(subject){pivot=*subject;follow=true;}
    // A DO camera retains its authored room while its animation can fly through
    // a portal. Resolve the eye from that camera's anchor, not Max's remote room.
    if(!follow && !director.controls && director.camera<director.count){
        pivot=director.def[director.camera].position;follow=true;
    }
    campaign_camera_room(pivot,camera,follow?director.exits:NULL);
    campaign_update_moving_rooms();
    float distance[CAST_SLOTS];const char *names[CAST_SLOTS];unsigned n=0;
    for(unsigned i=1;i<director.count;i++) {
        const EntityDef *def=director.def+i;const EntityState *e=director.state+i;
        if(!(def->flags&4) || !e->enabled)continue;
        const char *model=director_text(def->model);
        if(!strcmp(model,"rat") || !strcmp(model,"invisible"))continue;
        for(unsigned portal=campaign_first_view(e->room);portal!=255;portal=campaign_next_view(portal)){
            if(!campaign_view_enabled(portal,director.exits))continue;
            Player body=actors[i].body;
            // The prologue nursery places the baby 15.5 cm above the bedding.
            if(!strcmp(model,"dead_baby") && !strcmp(director_text(def->name),"::babys_room::character 00"))
                body.position.y-=.155f;
            if(!campaign_placement(e->room,portal,&body))continue;
            float dx=body.position.x-camera->eye.x,dy=body.position.y+.9f-camera->eye.y,dz=body.position.z-camera->eye.z;
            float depth=dx*camera->forward.x+dy*camera->forward.y+dz*camera->forward.z;
            float side=fabsf(dx*camera->right.x+dy*camera->right.y+dz*camera->right.z);
            if(depth< -2.f || side>fmaxf(1,depth)*2.f+2.f)continue;
            float d=dx*dx+dy*dy+dz*dz;unsigned k=0;while(k<n && distance[k]<=d)k++;
            if(k>=CAST_SLOTS)continue;
            if(n<CAST_SLOTS)n++;
            for(unsigned j=n-1;j>k;j--){actor_ids[j]=actor_ids[j-1];distance[j]=distance[j-1];names[j]=names[j-1];render[j]=render[j-1];}
            actor_ids[k]=i;names[k]=model;distance[k]=d;render[k]=actors[i];render[k].body=body;render[k].portal=portal;
        }
    }
    for(unsigned i=0;i<n;i++)render[i].animation.slot=i+1;
    *count=n;return actor_cast(n,names,actor_ids);
}
static const char *weapon_mesh(int gun) {
    if(gun==DUAL_PISTOL) gun=PISTOL;
    if(gun==DUAL_INGRAM) gun=INGRAM;
    return guns[gun].id;
}
static void player_listener(unsigned room,const Player *p){
    Point ear=p->position;ear.y+=p->crouched?.91f:1.42f;
    bank_listener(room,ear,p->yaw);
}
// WEAPONANIM_SHOOT and WEAPONANIM_RELOAD events from the disc's weapon definitions.
typedef struct {const char *mesh;float delay,speed;bool reload;} CaseEvent;
static const CaseEvent case_events[GUNS]={
    [PISTOL]={"case_9mm",1.f/30,1.5f,false},[DUAL_PISTOL]={"case_9mm",1.f/30,1.5f,false},
    [EAGLE]={"case_50cal",1.f/30,2,false},[PUMP]={"case_12ga",16.f/30,2.1f,false},
    [SAWED]={"case_12ga",0,6,true},[INGRAM]={"case_9mm",0,1.5f,false},
    [DUAL_INGRAM]={"case_9mm",0,1.5f,false},[COMMANDO]={"case_300cal",0,3,false},
    [SNIPER]={"case_300cal",18.f/10,3,false},[M79]={"case_40mm",0,2,true}
};
typedef struct {
    Point position,velocity;float delay,life,floor,spin;
    unsigned owner,room;int gun,side;bool launched,settled;
} Casing;
static Casing casings[24];
static unsigned next_case;
static void eject_cases(unsigned owner,int gun,bool reload,unsigned count,int side){
    if(gun<0 || !case_events[gun].mesh || case_events[gun].reload!=reload)return;
    if(campaign_first_view(director.state[owner].room)==255)return;
    for(unsigned i=0;i<count;i++){
        unsigned index=next_case++%(sizeof casings/sizeof *casings);
        casings[index]=(Casing){.owner=owner,.gun=gun,.delay=case_events[gun].delay,.life=4,
            .side=side,.spin=(float)(index%5+8)};
    }
}
static void tick_casings(float dt){
    for(unsigned i=0;i<sizeof casings/sizeof *casings;i++){
        Casing *b=casings+i;if(b->life<=0)continue;
        if(!b->launched){
            b->delay-=dt;if(b->delay>0)continue;
            EntityState *e=director.state+b->owner;
            if(!e->enabled || e->dead || campaign_first_view(e->room)==255){b->life=0;continue;}
            const CaseEvent *event=case_events+b->gun;
            float sn=sinf(e->yaw),cs=cosf(e->yaw),hand=.18f*b->side;
            b->room=e->room;b->position=(Point){e->position.x+.28f*sn+hand*cs,
                e->position.y+(campaign_actor_crouched(b->owner)?.90f:1.35f),e->position.z+.28f*cs-hand*sn};
            float right=b->side*.7071f,up=.7071f,forward=0;
            if(b->gun==SAWED){right=(i%2?.12f:-.12f);up=.8f;forward=-.6f;}
            if(b->gun==M79){right=up=0;forward=-1;}
            b->velocity=(Point){event->speed*(right*cs+forward*sn),event->speed*up,event->speed*(forward*cs-right*sn)};
            Hit ground;int prop;
            b->floor=campaign_ray_wall(b->room,b->position,(Point){0,-1,0},4,&ground,&prop)?ground.point.y+.012f:e->position.y+.012f;
            b->launched=true;
        }
        b->life-=dt;if(b->settled)continue;
        b->velocity.y-=9.81f*dt;
        b->position.x+=b->velocity.x*dt;b->position.y+=b->velocity.y*dt;b->position.z+=b->velocity.z*dt;
        if(b->position.y<=b->floor){
            b->position.y=b->floor;
            if(b->velocity.y< -2.5f){
                b->velocity.y*=-.26f;b->velocity.x*=.38f;b->velocity.z*=.38f;
                if(director.state[b->owner].room==b->room)bank_scene_sound(b->owner,"projectiles","casehit_generic",b->position,false,true);
            }else b->settled=true;
        }
    }
}
static void gun_sound(int gun,bool reload,unsigned owner,Point where,unsigned clip) {
    const char *name=weapon_mesh(gun);
    if(!reload && (gun==GRENADE || gun==MOLOTOV)) name="throwable";
    if(!reload && gun==DUAL_INGRAM) name="ingramdual";
    if(reload && gun==MOLOTOV) name="grenade";
    char key[48];snprintf(key,sizeof key,"%s_%s",reload?"reload":"shoot",name);
    if(pose_message_matches(clip,"a_play3dsound","weapons",key))return;
    if(owner)bank_scene_sound(owner,"weapons",key,where,reload,true);
    else bank_sound(0,"weapons",key);
}
static int held_weapon(unsigned instance) {
    if(!instance) return inventory.equipped;
    unsigned id=actor_ids[instance-1];
    return id>=director.count || director.state[id].dead?-1:find_gun(director_text(director.state[id].weapon));
}
typedef struct {
    bool ready,wounded,holstering,drawing;int gun,old_gun;unsigned skin,last_gait,clock_clip;float hp,yaw,clock,manual_turn,forward_travel,side_travel;
    uint8_t dive_direction;unsigned dive_getup;
    uint16_t dive[8],shoot_dive[8],medicine,scope,holster,death_action[2];
    uint16_t gait[CLIP_COUNT],shots[3],reload,turn[2],diagonal[4],land,hit,crouch_hit,getup[4],draw,use,jump[3];
    unsigned transient;float age;bool recovery;
    struct {unsigned clip;float time;} cursor[2];
    unsigned render_gait,render_action,weapon_track[2];
    float render_time,render_overlay,weapon_time[2],exchange_weight;
    bool whole,hidden,weapon_cued[2],skip_past,attack_events,previous_fire_paid;
    float pickup_retry;
    bool close_combat,rat,rat_started;RatTrack rat_track;float rat_time;
    const SkinRules *rules;
    const FireRate *fire_rate;
    ActorTimeline timeline;
    uint8_t muzzle_pending; // Runtime only; beyond the compatible save prefix.

} MotionStyle;
static MotionStyle *styles;
static Jump *motion_jump;
static Effects *motion_effects;
static const Player *motion_player;
static const Camera *motion_camera;
static const Animation *motion_player_animation;
static unsigned pending_jump;
static bool dodge_block,reticle_visible=true;
static float dodge_scale=1,dodge_target=1,dodge_fade;
static float controlled_scale,music_mix,heartbeat,camera_pose_offset;
static void refill_player(int gun){
    if(gun<0)return;
    Stock *stock=inventory.stock+gun,*pool=ammo_pool(gun);
    unsigned fill=guns[gun].capacity-stock->rounds;
    if(gun==PUMP && fill>1)fill=1;
    if(!infinite_rounds(gun)){if(fill>pool->reserve)fill=pool->reserve;pool->reserve-=fill;}
    stock->rounds+=fill;hud_dirty=true;
}
bool campaign_reticle_visible(void){return reticle_visible;}
static bool attacking(Clip action){
    return action==FIRE || action==THROW_ITEM || action==SWING;
}
static void release_round(unsigned id,int gun,int side,bool quiet){
    EntityState *e=director.state+id;
    if(!e->enabled || e->dead || !motion_effects || combat_waits_for_target(id))return;
    const Player *body=id?&actors[id].body:motion_player;
    Point eye=body->position;eye.y+=styles[id].rat?.13f:body->crouched?.91f:id?1.35f:1.42f;
    Camera aim;
    Point muzzle;
    if(id){
        float sn=sinf(body->yaw),cs=cosf(body->yaw),h=cosf(body->pitch),v=sinf(body->pitch);
        aim=(Camera){.eye=eye,.forward={sn*h,v,cs*h},.right={cs,0,-sn},.up={-sn*v,h,-cs*v}};
        muzzle=(Point){eye.x+.24f*aim.forward.x,eye.y+.24f*v,eye.z+.24f*aim.forward.z};
    }else{
        Point offset={0};if(gun>=0)item_weapon_effect(weapon_mesh(gun),&offset);
        aim=*motion_camera;muzzle=actor_hand_point(body,motion_player_animation,side<0,offset);
    }
    if(id && (gun==DUAL_PISTOL || gun==DUAL_INGRAM)){
        muzzle.x+=side*.18f*aim.right.x;muzzle.z+=side*.18f*aim.right.z;
    }
    bool melee=gun<0 || !guns[gun].capacity;
    if(!melee){
        if(id){if(!arsenals[id]->rounds[gun])return;arsenals[id]->rounds[gun]--;}
        else{if(!inventory.stock[gun].rounds)return;inventory.stock[gun].rounds--;hud_dirty=true;}
        eject_cases(id,gun,false,1,side);
        if(!quiet)styles[id].muzzle_pending|=side<0?2:1;
    }
    if(gun==BAT || gun==PIPE){
        Point origin,axes[3];const Animation *a=id?&actors[id].animation:motion_player_animation;
        if(actor_projectile_frame(body,a,gun==PIPE?"neck":"head",&origin,axes))
            combat_launch_attached(gun_round[gun],id,e->room,origin,axes);
    }else if(gun==GRENADE || gun==MOLOTOV || gun==M79)
        combat_launch(gun==M79?"bullet_m79":guns[gun].id,id,e->room,muzzle,aim.forward);
    else if(gun==PUMP || gun==SAWED || gun==JACKHAMMER){
        const char *group=gun==PUMP?"bullet_pumpshotgun_group":gun==SAWED?"bullet_sawedshotgun_group":"bullet_jackhammer_group";
        combat_hitscan(id,e->room,&aim,eye,muzzle,2,guns[gun].damage,100,gun_round[gun],motion_effects);
        combat_hitscan(id,e->room,&aim,eye,muzzle,2,guns[gun].damage,100,group,motion_effects);
        combat_hitscan(id,e->room,&aim,eye,muzzle,1,guns[gun].damage,100,"bullet_shotgun_straight",motion_effects);
    }else{
        unsigned pellets=(gun<0 || gun==DUAL_PISTOL || gun==DUAL_INGRAM)?1:guns[gun].pellets;
        combat_hitscan(id,e->room,&aim,eye,muzzle,pellets,gun<0?5:guns[gun].damage,
                       melee?(id?1.7f:1.8f):100.f,gun<0?NULL:(quiet && (gun==INGRAM || gun==DUAL_INGRAM))?"bullet_ingram_noeffects":gun_round[gun],motion_effects);
    }
    if(gun>=0)gun_sound(gun,false,id,muzzle,styles[id].shots[0]);
}
static void frame_message(unsigned id,const char *target,const char *op,const char *const *args,unsigned n);
static float animation_logical_time(const AnimationTimeline *t);
static void begin_attack(unsigned id,Animation *animation,int gun,float late){
    MotionStyle *s=styles+id;
    bool melee=gun<0 || !guns[gun].capacity;
    visuals[id].default_weapon=false;
    if(attacking(animation->action) && s->cursor[1].clip==s->shots[0]){
        s->attack_events=true;
        // A repeat can fall between two rendered frames. Events after that
        // instant belong to the replacement attack, not the outgoing tail.
        float end=fmaxf(0,animation->action_time-late);
        if(s->timeline.active && s->timeline.source[0]==s->shots[0]){
            AnimationTimeline clock=s->timeline.layer[0];
            // Stop at the replacement's real source time, including entry delay
            // and mode-0 restart. Simulation age includes time spent blending in.
            animation_step(&clock,fmaxf(0,animation->action_time-s->timeline.input[0]-late));
            end=animation_logical_time(&clock);
        }
        pose_events(id,s->shots[0],s->cursor[1].time,end,frame_message);
        s->attack_events=false;
    }
    s->cursor[1].clip=0;s->previous_fire_paid=false;
    actor_action(animation,melee?SWING:(gun==GRENADE || gun==MOLOTOV)?THROW_ITEM:FIRE);
    // A few NPC skins and the resident fallback have no weapon-event channel.
    bool authored=melee?(gun>=0 && pose_message_matches(s->shots[0],"p_createprojectiletobone",gun_round[gun],NULL)):
        pose_has_event(s->shots[0],"ws_animateshooting");
    if(!authored)release_round(id,gun,1,false);
}
static void frame_message(unsigned id,const char *target,const char *op,const char *const *args,unsigned n){
    MotionStyle *s=styles+id;EntityState *e=director.state+id;
    bool on=!strcmp(args[0],"true") || atoi(args[0])!=0;
    int gun=s->gun;
    if(!strcmp(op,"c_reload")){
        if(gun>=0){if(id)refill_npc(arsenals[id],gun);else refill_player(gun);}
    }else if(!strcmp(op,"c_setdodgeimmortal")){if(!id)director.dodge_immortal=on;}
    else if(!strcmp(op,"gm_enablebullettimeblock")){if(!id)dodge_block=on;}
    else if(!strcmp(op,"gm_enablebullettime")){if(!id && !on){inventory.slow=false;hud_dirty=true;}}
    else if(!strcmp(op,"gm_changegamespeed")){if(!id){dodge_target=strtof(args[0],NULL);dodge_fade=strtof(args[1],NULL);if(!dodge_fade)dodge_scale=dodge_target;}}
    else if(!strcmp(op,"c_enableprojectilecollisions")){if(id)e->pass_projectiles=!on;else director.dodge_pass_projectiles=!on;}
    else if(!strcmp(op,"c_displaycrosshair")){if(!id)reticle_visible=on;}
    else if(!strcmp(op,"c_addhealth")){
        if(!e->dead && (id || !s->medicine || s->transient!=s->medicine))
            director_set_health(id,fminf(id?director.def[id].max_health:maximum_health(),e->hp+strtof(args[0],NULL)));
    }else if(!strcmp(op,"c_jump")){
        if(!id && motion_jump && (pending_jump || inventory.dodge_time>0) && !motion_jump->airborne){motion_jump->velocity=strtof(args[0],NULL);motion_jump->airborne=true;pending_jump=0;}
    }else if(!strcmp(op,"ws_hide"))s->hidden=on;
    else if(!strcmp(op,"ws_animate") || !strcmp(op,"ws_animateshooting")){
        unsigned hand=!strcmp(target,"lefthandweapon");
        s->weapon_track[hand]=strstr(args[0],"reload")?2:strstr(args[0],"shoot")?1:0;
        s->weapon_cued[hand]=true;s->weapon_time[hand]=0;
        if(s->attack_events && !s->previous_fire_paid && !strcmp(op,"ws_animateshooting"))
            release_round(id,gun,!strcmp(target,"lefthandweapon")?-1:1,strstr(args[0],"_noeffects")!=NULL);
    }else if(!strcmp(op,"cam_animateinplace") || !strcmp(op,"cam_animateplayerparented")){
        if(!id && director.path.path==DIRECTOR_NONE){
            int path=camera_path_id(args[0]);
            if(path>=0)director.path=(CameraPath){.path=path,.key=DIRECTOR_NONE,.owner=id,.parented=!strcmp(op,"cam_animateplayerparented")};
        }
    }else if(!strcmp(op,"p_createprojectiletobone")){
        // Footstep projectiles select a sound from the surface under the foot.
        if(!strncmp(args[0],"dummy_footstep",14)){
            float offset=!strcmp(args[2],"foot-l")?-.13f:!strcmp(args[2],"foot-r")?.13f:0;
            Point feet=e->position;feet.x+=cosf(e->yaw)*offset;feet.z-=sinf(e->yaw)*offset;
            combat_footstep(args[0],id,e->room,feet,motion_effects);return;
        }
        if(!strncmp(args[0],"bullet_",7)){
            if(s->attack_events && !s->previous_fire_paid && (gun==BAT || gun==PIPE) && !strcmp(gun_round[gun],args[0]))
                release_round(id,gun,1,false);
            return;
        }
        const Player *body=id?&actors[id].body:motion_player;const Animation *a=id?&actors[id].animation:motion_player_animation;
        Point pos,axes[3];
        if(body && a && actor_projectile_frame(body,a,args[2],&pos,axes))
            for(int i=0;i<atoi(args[1]);i++)combat_launch_attached(args[0],id,e->room,pos,axes);
    }else action(id,op,args,n);
}
static void advance_cues(MotionStyle *s,unsigned id,unsigned layer,unsigned clip,float time,bool looping){
    unsigned prev=s->cursor[layer].clip;float start=s->cursor[layer].time;
    if(s->skip_past && !(layer==1 && s->attack_events && prev==clip && clip)){s->cursor[layer].clip=clip;s->cursor[layer].time=time;return;}
    if(clip!=prev)start=-1;
    if(clip)pose_events_advance(id,clip,start,time,looping,frame_message);
    s->cursor[layer].clip=clip;s->cursor[layer].time=time;
}
static unsigned variant(unsigned skin,const char *action,bool heavy,bool wounded){
    char key[64];unsigned clip=0;
    if(wounded){
        snprintf(key,sizeof key,heavy?"w_2_%s":"w_%s",action);
        clip=pose_motion_action(skin,key);
    }
    if(!clip && heavy){snprintf(key,sizeof key,"2_%s",action);clip=pose_motion_action(skin,key);}
    return clip?clip:pose_motion_action(skin,action);
}
static void configure_motion(MotionStyle *s,unsigned id,int gun){
    const EntityState *e=director.state+id;
    bool wounded=e->wounded || e->force_wounded;
    if(s->drawing && gun!=s->gun && !e->dead)return; // Finish drawing before holstering the next selection.
    if(s->ready && s->gun==gun && s->wounded==wounded)return;
    bool switching=s->ready && gun!=s->gun;
    int outgoing=s->holstering?s->old_gun:s->gun;
    const char *model=director_text(director.def[id].model);
    s->rat=!strcmp(model,"rat");
    if(id && switching && attacking(actors[id].animation.action))actors[id].animation.action=IDLE;
    s->skin=pose_bank_skin(model);s->gun=gun;s->wounded=wounded;
    const char *weapon=gun>=0?guns[gun].id:"empty";
    s->rules=pose_skin_rules(s->skin);s->fire_rate=pose_fire_rate(weapon);
    bool heavy=pose_bank_two_handed(weapon);
    const char *const gait[]={"stand","walk","run","walkbackward","strafeleft","straferight"};
    for(unsigned i=0;i<sizeof gait/sizeof *gait;i++)s->gait[i]=variant(s->skin,gait[i],heavy,wounded);
    s->gait[CROUCH_IDLE]=variant(s->skin,"crouch",heavy,wounded);
    s->gait[JUMP]=variant(s->skin,"jump",heavy,wounded);
    char death[24];snprintf(death,sizeof death,"randomdeath%u",1+id%4);
    s->gait[FALL]=variant(s->skin,death,heavy,wounded);
    s->death_action[0]=pose_motion_action(s->skin,"customdeath1");
    s->death_action[1]=pose_motion_action(s->skin,"customdeath2");
    const char *const directions[]={"forward","backward","left","right","backwardleft","backwardright","forwardleft","forwardright"};
    for(unsigned i=0;i<8;i++){
        char key[48];snprintf(key,sizeof key,"dodge%s",directions[i]);s->dive[i]=variant(s->skin,key,heavy,wounded);
        snprintf(key,sizeof key,"shootdodge%s",directions[i]);s->shoot_dive[i]=variant(s->skin,key,heavy,wounded);
    }
    s->medicine=pose_motion_action(s->skin,"shootpainkiller");
    s->scope=pose_motion_action(s->skin,"modeswitchsniper");
    s->holster=variant(s->skin,"holsterweapon",outgoing>=0 && pose_bank_two_handed(guns[outgoing].id),wounded);
    const char *const direction[]={"shoot","shootlow","shoothigh"};
    for(unsigned i=0;i<3;i++){
        char key[64];snprintf(key,sizeof key,"%s%s",direction[i],weapon);
        s->shots[i]=pose_motion_action(s->skin,key);
    }
    char key[64];snprintf(key,sizeof key,"reload%s",weapon);s->reload=pose_motion_action(s->skin,key);
    s->land=variant(s->skin,"land",heavy,wounded);s->use=variant(s->skin,"use",heavy,wounded);
    s->hit=variant(s->skin,"getdamage",heavy,wounded);s->crouch_hit=variant(s->skin,"getdamagecrouched",heavy,wounded);
    s->draw=variant(s->skin,"getweapon",heavy,wounded);
    const char *diag[]={"backwardleft","backwardright","forwardleft","forwardright"};
    const char *up[]={"shootdodgeforward_getup","shootdodgebackward_getup","shootdodgeleft_getup","shootdodgeright_getup"};
    for(unsigned i=0;i<4;i++){s->diagonal[i]=variant(s->skin,diag[i],heavy,wounded);s->getup[i]=variant(s->skin,up[i],heavy,wounded);}
    s->turn[0]=variant(s->skin,"turnleft",heavy,wounded);s->turn[1]=variant(s->skin,"turnright",heavy,wounded);
    s->jump[0]=variant(s->skin,"jumpforwardleft",heavy,wounded);s->jump[1]=variant(s->skin,"jumpforward",heavy,wounded);
    s->jump[2]=variant(s->skin,"jumpforwardright",heavy,wounded);
    if(!s->ready){s->hp=e->hp;s->yaw=e->yaw;s->ready=true;}
    if(switching){
        if(s->holstering)return; // Retarget the draw without rewinding the outgoing hand.
        s->old_gun=outgoing;s->holstering=outgoing>=0 && s->holster;
        s->drawing=!s->holstering && gun>=0 && s->draw;
        s->age=0;s->recovery=false;s->transient=s->holstering?s->holster:s->drawing?s->draw:0;
        if(!id && s->holstering){inventory.slow=false;hud_dirty=true;}
    }
}
static void aiming_motion(unsigned slot,unsigned id,const Player *body,const Camera *view){
    const MotionStyle *s=styles+id;
    float sn=sinf(body->yaw),cs=cosf(body->yaw),y=body->crouched?.91f:id?1.35f:1.42f;
    Point target={0,y+100*sinf(body->pitch),100*cosf(body->pitch)};
    if(view){
        float x=view->eye.x+100*view->forward.x-body->position.x;
        float z=view->eye.z+100*view->forward.z-body->position.z;
        target=(Point){x*cs-z*sn,view->eye.y+100*view->forward.y-body->position.y,x*sn+z*cs};
    }
    const unsigned actions[3]={s->shots[0],s->shots[1],s->shots[2]};
    actor_aim(slot,actions,target,s->gun>=0 && guns[s->gun].capacity && s->gun!=GRENADE && s->gun!=MOLOTOV);
}
static unsigned weapon_clip(const MotionStyle *s,Clip state,float pitch){
    if(state==RELOAD)return s->reload;
    if(!attacking(state))return 0;
    (void)pitch; // The actor blends neutral/low/high continuously; events retain this action.
    return s->shots[0];
}
// actor_step advances seconds even when the resident fallback has ended.
// Keep the authored action alive until its own clock reaches the last frame.
static void finish_weapon_step(unsigned id,Animation *a,Clip previous){
    MotionStyle *s=styles+id;unsigned clip=weapon_clip(s,previous,0);
    if(!clip)return;
    float seconds=pose_bank_duration(clip);
    const AnimationTimeline *clock=s->timeline.layer;
    bool scheduled=s->timeline.active && clock->clip==clip;
    if(previous==RELOAD && pose_bank_loops(clip)){
        int gun=s->gun;
        bool needs_rounds=gun>=0 && (id?arsenals[id]->rounds[gun]:inventory.stock[gun].rounds)<guns[gun].capacity &&
            (id?npc_has_spare(arsenals[id],gun):spare_rounds(gun));
        // Pump reload is one authored shell cycle. Repeat only while it can
        // transfer another shell, then finish the current cycle before leaving.
        float step=scheduled?fmaxf(0,a->action_time-s->timeline.input[0]):0;
        if(needs_rounds || (scheduled && (clock->stage==1 || clock->current.time+step<seconds))){
            a->action=previous;return;
        }
    }else
    if(scheduled?(clock->stage==1 || clock->current.time<seconds):a->action_time<seconds){a->action=previous;return;}
    if(s->cursor[1].clip==clip){
        s->attack_events=attacking(previous);
        pose_events(id,clip,s->cursor[1].time,seconds,frame_message);
        s->attack_events=false;
    }
    a->action=IDLE;
}
// The original event channel waits through mode-0 entry, then restarts at zero.
static float animation_logical_time(const AnimationTimeline *t){
    return t->stage==1?t->current.time-t->current.origin-t->seconds:t->current.time;
}
static void motion_timeline(MotionStyle *s,const Animation *a,unsigned gait,unsigned action,unsigned secondary,bool whole,float dt,float gait_dt,float *gait_time,float *action_time){
    ActorTimeline *t=&s->timeline;
    unsigned lower=a->scripted?a->scripted:secondary?s->shoot_dive[0]:gait;
    unsigned upper=a->scripted?a->scripted:whole || secondary || a->gait==JUMP || a->gait==FALL?lower:action?action:lower;
    unsigned requested[2]={upper,lower};float supplied[2]={*action_time,*gait_time};
    if(a->scripted)supplied[0]=supplied[1]=a->script_time;
    else if(upper==lower)supplied[0]=supplied[1];
    float delta[2]={gait_dt,gait_dt};
    if(action && upper==action && t->source[0]==upper && supplied[0]>=t->input[0])delta[0]=supplied[0]-t->input[0];
    bool initialized=t->active,separated=t->source[0]!=t->source[1];
    // The lower channel is updated first, so mode 5 can follow its new phase.
    for(int layer=1;layer>=0;layer--){
        unsigned clip=requested[layer];if(!clip)continue;
        AnimationSource source={clip,pose_bank_duration(clip),pose_bank_loops(clip)?pose_bank_loop_start(clip):pose_bank_duration(clip)};
        if(!(source.end>0))continue;
        AnimationTimeline *clock=t->layer+layer;
        bool restart=initialized && clip==t->source[layer] && supplied[layer]+.00001f<t->input[layer] &&
            ((a->scripted && !pose_bank_loops(clip)) || (layer==0 && action==clip));
        if(!initialized || !clock->clip){
            clock->weight=1;animation_set(clock,source,fmaxf(0,supplied[layer]));clock->generation++;
            t->sample[layer]=animation_evaluation(clock);
        }else{
            bool change=clock->clip!=clip || restart;
            if(change){
                unsigned mode=restart?4:pose_transition_mode(clock->clip,clip);
                if(a->gait==FALL)mode=4; // All original death entries have a zero transition time.
                if(!layer && upper==lower && separated && !whole && !secondary && !a->scripted)mode=5;
                animation_request(clock,source,mode,restart?fmaxf(0,supplied[layer]):0,pose_transition_rule(clip)->seconds,t->layer+1);
                if(layer==1 && (mode==2 || mode==3)){
                    s->cursor[0].clip=secondary?s->shoot_dive[0]:gait;
                    s->cursor[0].time=clock->current.time;
                }
            }
            float step=change && !layer && action==upper?0:delta[layer];
            if(a->scripted && !change){
                step=supplied[layer]-t->input[layer];
                if(step<0 && pose_bank_loops(clip))step+=pose_bank_duration(clip)-pose_bank_loop_start(clip);
                step=fmaxf(0,step);
            }
            t->sample[layer]=animation_step(clock,step);
        }
        t->source[layer]=clip;t->input[layer]=supplied[layer];
    }
    bool aiming=action && upper==action && action==s->shots[0] && !whole && !secondary && !a->scripted;
    if(!aiming)t->aim_age=0;
    t->aim_weight=aiming?fminf(1,t->aim_age/.08f):0;
    if(aiming)t->aim_age=fminf(.08f,t->aim_age+dt);
    t->aiming=aiming;t->active=true;
    *gait_time=t->sample[1].time;*action_time=t->sample[0].time;
}
static float player_run_speed(MotionStyle *s,float forward,float side,float dt){
    Clip state=retail_locomotion(forward,side,false,1,true);
    unsigned clip=s->gait[state];
    if(fabsf(side)>.22f*hypotf(forward,side) && fabsf(forward)>.22f*hypotf(forward,side)){
        unsigned diagonal=s->diagonal[(forward>0?2:0)+(side>0)];if(diagonal)clip=diagonal;
    }
    float distance=pose_stride(clip),duration=pose_bank_duration(clip);
    float speed=duration>0?distance/duration:0;
    if(!s->timeline.active || !s->timeline.layer[1].clip || duration<=0)return speed;
    // Xbox locomotion travel uses the live source weights: a captured pose has
    // no travel. In particular, Stand -> Run ramps over the authored 0.3 s.
    AnimationTimeline clock=s->timeline.layer[1];
    if(clock.clip!=clip){
        AnimationSource source={clip,duration,pose_bank_loops(clip)?pose_bank_loop_start(clip):duration};
        animation_request(&clock,source,pose_transition_mode(clock.clip,clip),0,pose_transition_rule(clip)->seconds,NULL);
    }
    AnimationEvaluation sample=animation_step(&clock,dt);
    float previous_duration=pose_bank_duration(sample.previous_clip);
    float previous_speed=previous_duration>0?pose_stride(sample.previous_clip)/previous_duration:0;
    return speed*sample.current+previous_speed*sample.previous;
}
typedef enum { MOTION_NONE, MOTION_USE, MOTION_MEDICINE, MOTION_SCOPE } MotionEvent;
static void rat_motion(MotionStyle *s,unsigned id,const Animation *a,const Player *p,float forward,float side,float dt){
    RatTrack track=RAT_STAND;
    float turn=remainderf(p->yaw-s->yaw,6.2831853f);s->yaw=p->yaw;
    if(director.state[id].dead)track=id%2?RAT_DEATH_A:RAT_DEATH_B;
    else if(attacking(a->action))track=p->pitch>.34f?RAT_SHOOT_UP:p->pitch<-.34f?RAT_SHOOT_DOWN:RAT_SHOOT;
    else if(fabsf(side)>.22f*hypotf(forward,side)){
        if(forward>fabsf(side)*.22f)track=side<0?RAT_FORWARD_LEFT:RAT_FORWARD_RIGHT;
        else track=side<0?RAT_LEFT:RAT_RIGHT;
    }else if(a->gait==RUN || a->gait==JUMP)track=RAT_RUN;
    else if(a->gait==WALK || a->gait==BACKPEDAL)track=RAT_WALK;
    else if(fabsf(turn)>.001f)track=turn>0?RAT_TURN_RIGHT:RAT_TURN_LEFT;
    float duration=item_rat_duration(track);
    if(duration<=0)return;
    bool entry=!s->rat_started || track!=s->rat_track;
    bool dead=track==RAT_DEATH_A || track==RAT_DEATH_B;
    float from=entry?-.001f:s->rat_time,to=(entry?0:s->rat_time)+dt;
    if(entry && dead){
        Point torso=p->position;torso.y+=.12f;
        combat_launch("blood_dummy",id,director.state[id].room,torso,(Point){0,-1,0});
    }
    float cue=dead?10.f/30:track==RAT_STAND?40.f/30:0;
    const char *sound=dead?"rat_death":track==RAT_STAND?"rat_idle":track==RAT_RUN?"rat_run":track==RAT_WALK?"rat_walk":NULL;
    if(!dead && to>=duration){
        if(sound && from<cue && cue<duration && cue>fmodf(to,duration))
            bank_scene_sound(id,"characters",sound,p->position,true,true);
        from=-.001f;to=fmodf(to,duration);
    }
    if(sound && from<cue && to>=cue)bank_scene_sound(id,"characters",sound,p->position,true,true);
    s->rat_started=true;s->rat_track=track;s->rat_time=fminf(to,duration);
}
static void retail_motion(unsigned slot,unsigned id,const Animation *a,const Player *p,int gun,float dt,float forward,float side,MotionEvent interaction){
    MotionStyle *s=styles+id;configure_motion(s,id,gun);
    if(s->rat){rat_motion(s,id,a,p,forward,side,dt);return;}
    unsigned gait=s->gait[a->gait],action=0,event=0,secondary=0;float direction_weight=0,gait_dt=dt;
    float duration=actor_duration(a->gait),phase=duration>0?a->gait_time/duration:0;
    float turn=id?remainderf(p->yaw-s->yaw,6.2831853f):s->manual_turn;s->yaw=p->yaw;
    if(id && a->gait==IDLE && fabsf(turn)>.001f && s->turn[turn>0]){
        gait=s->turn[turn>0];
    }else if(a->gait>=WALK && a->gait<=STRAFE_RIGHT && fabsf(side)>.22f*hypotf(forward,side) && fabsf(forward)>.22f*hypotf(forward,side)){
        unsigned diagonal=s->diagonal[(forward>0?2:0)+(side>0)];if(diagonal)gait=diagonal;
    }else if(a->gait==JUMP && forward>.0001f){
        unsigned jumping=s->jump[side<-.3f*forward?0:side>.3f*forward?2:1];if(jumping)gait=jumping;
    }
    if(!id && a->gait==JUMP){
        if(pending_jump)gait=pending_jump;
        else if(s->last_gait==JUMP && s->clock_clip)gait=s->clock_clip;
    }
    bool diving=a->gait>=DODGE_FORWARD && a->gait<=DODGE_RIGHT;
    if(diving){
        if(id && (unsigned)a->gait!=s->last_gait){
            s->dive_direction=a->gait-DODGE_FORWARD;

        }
        gait=(id==0 || visuals[id].shoot_dodge)?s->shoot_dive[s->dive_direction]:s->dive[s->dive_direction];
        if(!id){
            float travel_yaw=atan2f(inventory.dodge_x,inventory.dodge_z);
            DodgeBlend blend=dodge_animation_blend(travel_yaw,p->yaw);
            unsigned first=s->shoot_dive[blend.first],second=s->shoot_dive[blend.second];
            if(first && second){gait=first;secondary=second;direction_weight=blend.weight;}
            s->dive_getup=dodge_animation_getup(blend.angle);
        }
    }
    if(s->last_gait==JUMP && a->gait!=JUMP)event=s->land;
    if(s->last_gait>=DODGE_FORWARD && s->last_gait<=DODGE_RIGHT && a->gait!=s->last_gait){
        if(!id)s->dive_getup=dodge_animation_getup(dodge_animation_blend(atan2f(inventory.dodge_x,inventory.dodge_z),p->yaw).angle);
        event=s->getup[id?s->last_gait-DODGE_FORWARD:s->dive_getup];
    }
    if(interaction==MOTION_USE)event=s->use;
    if(interaction==MOTION_MEDICINE)event=s->medicine;
    if(interaction==MOTION_SCOPE)event=s->scope;
    bool playing_hit=s->transient && (s->transient==s->hit || s->transient==s->crouch_hit);
    if(!playing_hit && !director.state[id].dead && director.state[id].hp<s->hp)event=p->crouched?s->crouch_hit:s->hit;
    s->hp=director.state[id].hp;s->last_gait=a->gait;
    if(event){
        s->holstering=s->drawing=false;s->transient=event;s->age=0;
        s->recovery=event==s->land || event==s->getup[0] || event==s->getup[1] ||
                    event==s->getup[2] || event==s->getup[3];
    }
    if(diving || a->gait==JUMP){s->transient=0;s->holstering=s->drawing=false;}
    bool whole_body=false;
    float overlay_time=0;
    s->exchange_weight=-1;
    float time=phase*pose_bank_duration(gait);
    if(s->clock_clip!=gait){s->clock_clip=gait;s->clock=0;}
    if(diving && gait)time=fminf(a->gait_time,pose_bank_duration(gait));
    if(gait && (a->gait==IDLE || a->gait==CROUCH_IDLE)){
        s->clock=pose_bank_wrap_time(gait,s->clock+dt);time=s->clock;
    }else if(gait && a->gait>=WALK && a->gait<=STRAFE_RIGHT){
        float travel=pose_stride(gait),duration=pose_bank_duration(gait);
        float step=travel>0?hypotf(forward,side)*duration/travel:dt;
        s->clock=pose_bank_wrap_time(gait,s->clock+step);time=s->clock;gait_dt=id?step:dt;
    }else if(a->gait==FALL){
        time=fminf(a->gait_time,pose_bank_duration(gait));
    }
    if(s->transient && !director.state[id].dead){
        s->age+=dt;
        if(s->age>=pose_bank_duration(s->transient)){
            // The last source-frame message can fall between two renders.
            // Drain whichever layer owned this transient before discarding it.
            for(unsigned layer=0;layer<2;layer++)if(s->cursor[layer].clip==s->transient){
                float end=pose_bank_duration(s->transient);
                pose_events(id,s->transient,s->cursor[layer].time,end,frame_message);
                s->cursor[layer].time=end;
            }
            s->age-=pose_bank_duration(s->transient);
            s->drawing=s->holstering && s->gun>=0 && s->draw;
            s->holstering=false;s->transient=s->drawing?s->draw:0;
        }
        if(s->transient && (s->holstering || s->drawing) && a->action==IDLE){
            action=s->transient;overlay_time=s->age;
            s->exchange_weight=1;
        }else if(s->transient && (a->gait==IDLE || s->recovery)){
            gait=s->transient;time=s->age;
            bool hit=s->transient==s->hit || s->transient==s->crouch_hit;
            // Damage may animate the lower channel, while the live weapon
            // channel retains its pose, clock and frame events.
            whole_body=!(hit && (attacking(a->action) || a->action==RELOAD));
        }
        else if(s->transient && a->action==IDLE){action=s->transient;overlay_time=s->age;}
    }
    if(a->action==RELOAD)action=s->reload;
    if(attacking(a->action))action=weapon_clip(s,a->action,p->pitch);
    if(a->action!=IDLE && action)overlay_time=a->action_time;
    unsigned cue_action=attacking(a->action)?s->shots[0]:action;
    if(!id && cue_action!=s->cursor[1].clip && pose_has_event(s->cursor[1].clip,"c_displaycrosshair"))reticle_visible=true;
    bool getting_up=s->recovery && s->transient && s->transient!=s->land;
    if(!id && inventory.dodge_time<=0 && !getting_up && (dodge_block || dodge_target!=1)){
        dodge_block=false;dodge_target=1;dodge_fade=.5f;
    }
    for(unsigned hand=0;hand<2;hand++){
        if(cue_action!=s->cursor[1].clip)s->weapon_cued[hand]=false;
        s->weapon_time[hand]+=dt;
    }
    // The director already advances scripted idles with their own frame channel.
    unsigned gait_cues=a->scripted?0:gait;
    // The directional shoot-dodge poses share the forward track's timed controls.
    if(!id && diving && gait_cues)gait_cues=s->shoot_dive[0];
    if(a->scripted && director.state[id].dead){
        for(unsigned k=0;k<2;k++)if(s->death_action[k] && pose_clip_id(s->death_action[k])==a->scripted){
            gait_cues=s->death_action[k];break;
        }
    }
    if(!id && s->cursor[0].clip!=gait_cues){
        director.dodge_immortal=false;director.dodge_pass_projectiles=false;
    }
    float weapon_cue_time=overlay_time;
    motion_timeline(s,a,gait,action,secondary,whole_body,dt,gait_dt,&time,&overlay_time);
    float gait_cue_time=a->scripted?a->script_time:animation_logical_time(s->timeline.layer+1);
    // Shoot-dodge owns both pose channels, while weapon messages still follow
    // their independent attack. Its source age must never become dodge age.
    float action_cue_time=s->timeline.source[0]==cue_action?animation_logical_time(s->timeline.layer):weapon_cue_time;
    advance_cues(s,id,0,gait_cues,gait_cue_time,!a->scripted && !whole_body && (a->gait<FIRE || a->gait==CROUCH_IDLE));
    s->attack_events=attacking(a->action) && !director.state[id].dead;
    // Aim blending changes only the pose. The neutral action owns chronological weapon events.
    advance_cues(s,id,1,a->scripted?0:cue_action,action_cue_time,a->action==RELOAD && pose_bank_loops(cue_action));
    s->attack_events=false;
    if(action && cue_action && action!=cue_action)
        overlay_time*=pose_bank_duration(action)/fmaxf(.001f,pose_bank_duration(cue_action));
    s->skip_past=false;
    s->render_gait=gait;s->render_action=action;s->render_time=time;s->render_overlay=overlay_time;s->whole=whole_body;
    if(!id)actor_exchange_from(s->drawing && s->old_gun>=0?s->holster:0);
    actor_motion(slot,gait,time,action,overlay_time,whole_body,s->gun<0,pose_bank_two_handed_bones(gun<0?"empty":guns[gun].id),s->exchange_weight,dt);
    actor_set_timeline(slot,&s->timeline);
    aiming_motion(slot,id,p,id?NULL:motion_camera);
    if(secondary)actor_blend_gait(slot,secondary,direction_weight,s->shoot_dive[0]);
}
static const char *world_item(unsigned id,const char **category) {
    const EntityDef *def=director.def+id;const EntityState *actor=director.state+id;
    if(!actor->enabled || campaign_first_view(actor->room)==255) return NULL;
    const char *name=director_text(def->model);
    if(def->flags&2) {*category="items";return name;}
    if((def->flags&4) && !strcmp(name,"rat")) {*category="cast";return name;}
    if((def->flags&4) && actor->dead && !actor->loot_collected && !combat_has_weapon_drops()) {
        int gun=find_gun(director_text(actor->weapon));
        if(gun>=0) {*category="weapons";return weapon_mesh(gun);}
    }
    return NULL;
}
static void footstep_name(char name[64],bool right,const char *material){
    const char *suffix=strcmp(material,"default")?material:"";
    snprintf(name,64,"run_%sfoot%s%s",right?"right":"left",*suffix?"/":"",suffix);
}
static void preload_footsteps(const char *material){
    for(unsigned foot=0;foot<2;foot++){
        char name[64];footstep_name(name,foot!=0,material);bank_preload("characters",name);
    }
}
static void preload_pose_sound(unsigned actor,const char *target,const char *op,const char *const *args,unsigned count){
    (void)actor;(void)target;
    if(count>=2 && (!strcmp(op,"a_play3dsound") || !strcmp(op,"a_playsound")))bank_preload(args[0],args[1]);
}
static void preload_weapon_sounds(bool equipped_only){
    unsigned skin=pose_bank_skin(director_text(director.def[0].model));
    for(unsigned j=0;j<GUNS;j++){
        if(((int)j==inventory.equipped)!=equipped_only)continue;
        if(!inventory.stock[j].owned && !director_has_text(guns[j].id) && !(j==COMMANDO && director_has_text("coltcommando")))continue;
        for(unsigned reload=0;reload<2;reload++){
            const char *name=weapon_mesh(j);
            if(!reload && (j==GRENADE || j==MOLOTOV))name="throwable";
            if(!reload && j==DUAL_INGRAM)name="ingramdual";
            if(reload && j==MOLOTOV)name="grenade";
            char key[64];snprintf(key,sizeof key,"%s_%s",reload?"reload":"shoot",name);bank_preload("weapons",key);
            snprintf(key,sizeof key,"%s%s",reload?"reload":"shoot",guns[j].id);
            unsigned clip=pose_motion_action(skin,key);
            pose_events(0,clip,-1,pose_bank_duration(clip),preload_pose_sound);
        }
    }
}
static bool preload_stage_models(void){
    const char *const debris[]={"wood_crate","debris_wood_crate","debris_blue_sign"};
    for(unsigned i=0;i<3;i++)if(!item_mesh_load("debris",debris[i]))return false;
    memset(casings,0,sizeof casings);next_case=0;bank_preload("projectiles","casehit_generic");
    for(unsigned j=0;j<GUNS;j++){
        bool relevant=inventory.stock[j].owned || director_has_text(guns[j].id) ||
                      (j==COMMANDO && director_has_text("coltcommando"));
        if(relevant && (!item_mesh_load("weapons",weapon_mesh(j)) ||
           (case_events[j].mesh && !item_mesh_load("cases",case_events[j].mesh))))return false;
    }
    unsigned player_skin=pose_bank_skin(director_text(director.def[0].model));
    // Load every movement direction before optional attacks and NPC idles.
    if(!pose_bank_preload_player(player_skin))return false;
    pose_bank_preload(director.state[0].custom_pose);
    if(inventory.equipped>=0){
        const char *const attack[]={"shoot","shootlow","shoothigh","reload"};
        for(unsigned i=0;i<4;i++){
            char key[64];snprintf(key,sizeof key,"%s%s",attack[i],guns[inventory.equipped].id);
            pose_bank_preload(pose_motion_action(player_skin,key));
        }
    }
    for(unsigned i=0;i<director.count;i++){
        const EntityDef *d=director.def+i;const char *name=director_text(d->model);
        if(d->flags&2){if(!item_mesh_load("items",name))return false;}
        else if((d->flags&4) && !strcmp(name,"rat")){if(!item_mesh_load("cast",name))return false;}
        else if(!i || (d->flags&4)){
            if(i && strcmp(name,"invisible"))actor_preload(name);
            unsigned skin=pose_bank_skin(name);
            const char *const common[]={"use","2_use","stand","2_stand"};
            for(unsigned k=0;k<sizeof common/sizeof *common;k++)pose_bank_preload(pose_bank_action(skin,common[k]));
        }
        ui_loading_progress(91+7*i/director.count);
    }
    return true;
}
static const char *projectile_folder(const char *name,bool pickup){
    if(pickup || !strcmp(name,"medipack_small"))return "items";
    if(!strncmp(name,"case_",5))return "cases";
    if(!strncmp(name,"debris_",7) || !strcmp(name,"wood_crate"))return "debris";
    return "weapons";
}
static int visible_weapon(unsigned instance){
    unsigned id=instance?actor_ids[instance-1]:0;
    if(!director.state[id].dead && (styles[id].holstering || styles[id].drawing))
        return styles[id].holstering?styles[id].old_gun:styles[id].gun;
    return held_weapon(instance);
}
static bool prepare_models(unsigned count) {
    for(unsigned i=0;i<=count;i++) {
        unsigned id=i?actor_ids[i-1]:0;
        unsigned face=director.state[id].dead?1:visuals[id].face;
        if(!actor_face(i,director_text(director.def[id].model),face))return false;
        int gun=visible_weapon(i),next=held_weapon(i);
        if(gun>=0 && !item_mesh_load("weapons",weapon_mesh(gun))) return false;
        if(next!=gun && next>=0 && !item_mesh_load("weapons",weapon_mesh(next))) return false;
    }
    for(unsigned i=0;i<PROJECTILE_LIMIT;i++) {
        if(!campaign_projectiles[i].active || campaign_first_view(campaign_projectiles[i].room)==255)continue;
        bool pickup=false;const char *name=combat_mesh(campaign_projectiles+i,&pickup);
        if(name && !item_mesh_load(projectile_folder(name,pickup),name)) return false;
    }
    return true;
}
typedef struct {const Player *player;const Animation *animation;const Effects *effects;Camera entry;PauseCamera orbit;} CampaignPause;
static void pause_frame(float dt,void *context){
    CampaignPause *pause=context;
    if(dt>=0)scene_pause_step(&pause->orbit,dt,camera_obstacle);
    const Camera *view=dt<0?&pause->entry:&pause->orbit.view;
    Enemy cast[CAST_SLOTS]={0};unsigned count=0;
    if(!local_cast(pause->player,view,NULL,cast,&count) || !prepare_models(count))return;
    for(unsigned k=0;k<count;k++){
        MotionStyle *s=styles+actor_ids[k];
        actor_motion(k+1,s->render_gait,s->render_time,s->render_action,s->render_overlay,s->whole,s->gun<0,
                     pose_bank_two_handed_bones(s->gun<0?"empty":guns[s->gun].id),s->exchange_weight,0);
        actor_set_timeline(k+1,&s->timeline);
        aiming_motion(k+1,actor_ids[k],&actors[actor_ids[k]].body,NULL);
    }
    room_render_campaign(pause->player,pause->animation,view,pause->effects,cast,count,inventory.equipped<0,1);
}
Point campaign_actor_feet(unsigned slot){return director.state[slot?actor_ids[slot-1]:0].position;}
uint32_t campaign_actor_light(unsigned slot) {
    const EntityState *e=director.state+(slot?actor_ids[slot-1]:0);
    uint32_t light=campaign_light(e->room,e->position);
    if(!director.controls){
        // A soft fill keeps faces readable in the low-key rooftop and house close-ups.
        uint32_t lit=0xff000000;
        for(unsigned i=0;i<3;i++){
            unsigned channel=(light>>(8*i))&255;
            lit|=(channel+(255-channel)*3/5)<<(8*i);
        }
        light=lit;
    }
    return light;
}
ActorLight room_model_light(unsigned room,const Player *body,bool campaign,bool cinematic){
    Point chest=body->position;chest.y+=body->crouched?.76f:1.15f;
    IncidentLight incident=campaign_incident(room,chest);
    if(campaign)particles_light(room,chest,&incident);
    uint32_t probe=campaign_light(room,chest);
    float fill=cinematic?34.f:18.f;
    Point base={fill+(probe&255)*.68f,fill+((probe>>8)&255)*.68f,fill+((probe>>16)&255)*.68f};
    Point direction=incident.vector;
    direction.y+=.12f;direction.z+=.07f;
    float inv=1.f/fmaxf(.0001f,sqrtf(direction.x*direction.x+direction.y*direction.y+direction.z*direction.z));
    float c=cosf(body->yaw),s=sinf(body->yaw);
    Point key={(direction.x*c-direction.z*s)*inv,direction.y*inv,(direction.x*s+direction.z*c)*inv};
    Point rgb=incident.amount;
    Point diffuse={base.x*.65f+300*rgb.x/(.5f+rgb.x),base.y*.65f+300*rgb.y/(.5f+rgb.y),base.z*.65f+300*rgb.z/(.5f+rgb.z)};
    return (ActorLight){base,diffuse,key};
}
bool campaign_actor_crouched(unsigned id){return id?actors[id].body.crouched:player_crouched;}
ActorLight campaign_model_light(unsigned slot){
    unsigned id=slot?actor_ids[slot-1]:0;
    const EntityState *e=director.state+id;
    Player body={.position=e->position,.yaw=e->yaw,.crouched=campaign_actor_crouched(id)};
    return room_model_light(e->room,&body,true,!director.controls);
}
static void draw_item(unsigned room,Point pos,float yaw,float roll,float spin,const char *category,const char *name,
                      int uniform,const C3D_Mtx *combined,const MotionStyle *animal) {
    for(unsigned portal=campaign_first_view(room);portal!=255;portal=campaign_next_view(portal)){
        if(!campaign_view_enabled(portal,director.exits) || !campaign_visible_transform(portal))continue;
        Player body={.position=pos,.yaw=yaw};
        if(!campaign_placement(room,portal,&body))continue;
        C3D_Mtx model=*combined;
        Mtx_Translate(&model,body.position.x,body.position.y,body.position.z,true);
        Mtx_RotateY(&model,body.yaw,true);
        if(roll)Mtx_RotateZ(&model,roll,true);
        if(spin)Mtx_RotateX(&model,spin,true);
        campaign_stencil(portal);
        C3D_TexEnvColor(C3D_GetTexEnv(0),campaign_light(room,pos));
        if(animal)item_rat_draw(animal->rat_track,animal->rat_time,uniform,&model);
        else item_mesh_draw(category,name,uniform,&model);
    }
}
void campaign_draw_items(int uniform,const C3D_Mtx *combined) {
    for(unsigned i=0;i<sizeof casings/sizeof *casings;i++){
        const Casing *b=casings+i;
        if(b->life>0 && b->launched)draw_item(b->room,b->position,0,b->settled?1.57f:b->life*b->spin,
            b->settled?0:b->life*6,"cases",case_events[b->gun].mesh,uniform,combined,NULL);
    }
    for(unsigned i=1;i<director.count;i++) {
        const char *category=NULL,*name=world_item(i,&category);
        if(!name)continue;
        const EntityState *e=director.state+i;
        Point pos;director_pose(i,&pos,NULL,NULL);
        draw_item(e->room,pos,e->yaw,e->dead && strcmp(category,"cast")?1.5707963f:0,0,category,name,uniform,combined,
            !strcmp(category,"cast")?styles+i:NULL);
    }
    for(unsigned i=0;i<PROJECTILE_LIMIT;i++) {
        const Projectile *p=campaign_projectiles+i;bool pickup=false;const char *name=combat_mesh(p,&pickup);
        if(name)draw_item(p->room,p->position,0,0,p->age*4,projectile_folder(name,pickup),name,uniform,combined,NULL);
    }
}
void campaign_draw_weapon(unsigned instance,const Animation *animation,int uniform,const C3D_Mtx *body) {
    unsigned id=instance?actor_ids[instance-1]:0;
    int gun=visible_weapon(instance);
    if(gun<0 || styles[id].hidden) return;
    const ActorExtra *v=visuals+id;
    unsigned hands=(gun==DUAL_PISTOL || gun==DUAL_INGRAM)?2:1;
    for(unsigned index=0;index<hands;index++){
        float rows[3][4];actor_draw_hand(instance,index!=0,rows);
        C3D_Mtx hand,combined;Mtx_Identity(&hand);
        for(unsigned i=0;i<3;i++)hand.r[i]=FVec4_New(rows[i][0],rows[i][1],rows[i][2],rows[i][3]);
        Mtx_Multiply(&combined,body,&hand);
        unsigned track=0;float time=v->default_weapon?v->seconds:animation->action_time;
        if(!v->default_weapon && hands==1)track=animation->action==RELOAD?2:attacking(animation->action)?1:0;
        if(styles[id].weapon_cued[index]){track=styles[id].weapon_track[index];time=styles[id].weapon_time[index];}
        item_weapon_draw(weapon_mesh(gun),uniform,&combined,track,time);
    }
}
void campaign_prepare_weapon_effects(unsigned visible,unsigned count){
    // Actor palettes have now been prepared for this frame. Both eyes share
    // one emission at the very same socket used to draw each weapon.
    for(unsigned instance=0;instance<=count;instance++){
        unsigned id=instance?actor_ids[instance-1]:0;
        MotionStyle *s=styles+id;int gun=s->gun;
        if(!(visible&(1u<<instance)) || !s->muzzle_pending || gun<0 || s->hidden)continue;
        const char *effect=item_weapon_effect(weapon_mesh(gun),NULL);
        if(!effect || !effect[0])continue;
        const Player *body=instance?&actors[id].body:motion_player;
        float cs=cosf(body->yaw),sn=sinf(body->yaw);
        for(unsigned hand=0;hand<2;hand++)if(s->muzzle_pending&(1u<<hand)){
            float m[3][4];actor_draw_hand(instance,hand!=0,m);
            Point origin={body->position.x+cs*m[0][3]+sn*m[2][3],body->position.y+m[1][3],body->position.z-sn*m[0][3]+cs*m[2][3]};
            Point forward={cs*m[0][2]+sn*m[2][2],m[1][2],-sn*m[0][2]+cs*m[2][2]};
            Point up={cs*m[0][1]+sn*m[2][1],m[1][1],-sn*m[0][1]+cs*m[2][1]};
            particles_weapon_burst(id,director.state[id].room,origin,forward,up,effect);
        }
    }
    for(unsigned id=0;id<director.count;id++)styles[id].muzzle_pending=0;
}
static void simulate_actors(Player *p,float dt) {
    for(unsigned i=1;i<director.count;i++) {
        const EntityDef *d=director.def+i;EntityState *st=director.state+i;
        if(!(d->flags&4) || !st->enabled) continue;
        Point previous=st->position;unsigned previous_room=st->room;float previous_yaw=st->yaw;
        styles[i].forward_travel=styles[i].side_travel=0;
        Enemy *actor=actors+i;actor->body.position=st->position;actor->body.yaw=st->yaw;
        const char *name=director_text(st->weapon),*ai=director_text(st->ai);
        int gun=find_gun(name);
        equip_npc(i,gun);NpcAmmo *ammo=arsenals[i];
        actor->unarmed=gun<0;actor->animation.weapon=gun<0?BERETTA:guns[gun].pose;
        actor->animation.scripted=0;
        if(st->dead){
            if(!st->loot_collected && combat_has_weapon_drops()){
                Point hand=gun>=0?actor_attachment(&actor->body,&actor->animation,true):st->position;
                Point facing={sinf(st->yaw),0,cosf(st->yaw)};
                combat_drop_on_death(i,gun>=0?guns[gun].id:"",hand,facing);
            }
            actor->animation.action=IDLE;actor_step(&actor->animation,FALL,1,dt);
            actor->animation.scripted=st->custom_pose;actor->animation.script_time=st->pose_seconds;continue;
        }
        configure_motion(styles+i,i,gun);
        const SkinRules *rules=styles[i].rules;
        ActorExtra *extra=visuals+i;
        if(extra->delay>0){
            extra->delay=fmaxf(0,extra->delay-dt);
            Clip previous_action=actor->animation.action;bool loading=previous_action==RELOAD;
            actor_step(&actor->animation,IDLE,1,dt);
            finish_weapon_step(i,&actor->animation,previous_action);
            if(loading && actor->animation.action!=RELOAD && gun>=0 && !pose_has_event(styles[i].reload,"c_reload"))refill_npc(ammo,gun);
            continue;
        }
        bool dodging=extra->dodge_remaining>0;
        actor->body.crouched=!dodging && !strncmp(ai,"crouch",6);
        if(dodging){
            float sideways=extra->dodge==DODGE_LEFT?-1:extra->dodge==DODGE_RIGHT?1:0;
            float front=extra->dodge==DODGE_FORWARD?1:extra->dodge==DODGE_BACKWARD?-1:0;
            float t=fminf(dt,extra->dodge_remaining),sn=sinf(st->yaw),cs=cosf(st->yaw);
            Point shift={(sideways*cs+front*sn)*3.4f*t,0,(front*cs-sideways*sn)*3.4f*t};
            if(st->room==campaign_room_id())room_move(&actor->body,shift.x,shift.z);
            else{actor->body.position.x+=shift.x;actor->body.position.z+=shift.z;}
            st->position=actor->body.position;extra->dodge_remaining-=t;
        }
        if(extra->patrol_count && !st->hostile){
            unsigned waypoint=extra->patrol[extra->waypoint];Point at;director_pose(waypoint,&at,NULL,NULL);
            if(st->room==director.state[waypoint].room && hypotf(st->position.x-at.x,st->position.z-at.z)<.45f &&
               fabsf(st->position.y-at.y)<1.5f)extra->waypoint=(extra->waypoint+1)%extra->patrol_count;
            st->target=extra->patrol[extra->waypoint];
        }
        bool remote=st->room!=director.state[0].room;
        // Older saves can contain the hostility acquired before this check.
        bool awaiting_target=combat_waits_for_target(i);
        if(awaiting_target)st->hostile=false;
        bool watching=!awaiting_target && (!strcmp(ai,"idleuntilenemyseen") ||
            (!*ai && gun>=0 && strncmp(director_text(d->model),"enemy_max",9)));
        if(!st->hostile && director.controls && (watching || remote)){
            bool noticed=false;
            if(!remote || campaign_first_view(st->room)!=255){
                Point eye=st->position;eye.y+=styles[i].rat?.13f:actor->body.crouched?.91f:1.35f;
                Point target=p->position,dir;target.y+=p->crouched?.91f:1.35f;
                if(combat_sight(st->room,eye,director.state[0].room,target,rules?rules->visual_radius:16,true,&dir)){
                    float facing=sinf(st->yaw)*dir.x+cosf(st->yaw)*dir.z;
                    noticed=facing>=0 || (!remote && rules && hypotf(target.x-eye.x,target.z-eye.z)<=rules->general_radius);
                }
            }
            if(noticed){
                if(!st->activated){director_activate_character(i);continue;}
                if(watching)st->hostile=true;
            }else if(remote && st->active_seconds<=0)st->activated=false;
        }
        // An explicit scripted target stays explicit after death. BB must not
        // silently retarget Max when Alex dies; a script can select a new target.
        if(st->aim==i)st->aim=DIRECTOR_NONE;
        unsigned victim=st->aim<director.count?st->aim:0;
        if(st->aim<director.count && (!director.state[st->aim].enabled || director.state[st->aim].dead) && attacking(actor->animation.action)){
            actor_action(&actor->animation,IDLE);styles[i].cursor[1].clip=0;
        }
        unsigned destination=st->target<director.count && !(extra->patrol_count && st->hostile)?st->target:st->hostile?victim:DIRECTOR_NONE;
        bool moving=false,melee=gun<0 || gun==BAT || gun==PIPE;
        if(destination<director.count) {
            EntityState *to=director.state+destination;
            Point target;director_pose(destination,&target,NULL,NULL);
            float dx=target.x-st->position.x,dz=target.z-st->position.z,dy=target.y-st->position.y;
            float distance=hypotf(dx,dz),pace=dt*st->pace;
            if(destination && !extra->patrol_count && to->room==st->room && distance<=.20f && fabsf(dy)<1.5f)
                st->target=DIRECTOR_NONE;
            bool mob=!strcmp(ai,"mobstercombat"),merc=!strcmp(ai,"merccombat"),merc_def=!strcmp(ai,"merccombatdefensive");
            float close_range=mob?4.f:merc?6.4f:merc_def?8.f:!strcmp(ai,"mobstercombatdefensive")?7.2f:6.f;
            float stop=destination==victim && st->hostile?(melee?1.1f:close_range):.18f;
            // MobsterCombat_BackAway and MercCombat_CloseCombat_CrouchAndDodge
            // are separate Xbox substates. Leave authored movement orders in control.
            bool close=destination==victim && st->hostile && st->target==DIRECTOR_NONE && !melee &&
                       st->room==campaign_room_id() && st->room==to->room && distance<stop;
            if(close && !paths[i].riding && !dodging){
                if(mob && distance<1.f && distance>.01f){
                    float retreat=fminf(2*pace,1.f-distance)/distance;
                    room_move(&actor->body,-dx*retreat,-dz*retreat);
                    st->position=actor->body.position;moving=true;
                }else if(merc || merc_def){
                    actor->body.crouched=true;
                    if(!styles[i].close_combat && actor->animation.action!=RELOAD){
                        extra->dodge=i%2?DODGE_LEFT:DODGE_RIGHT;
                        extra->dodge_remaining=actor_duration(extra->dodge);extra->shoot_dodge=false;
                    }
                }
            }
            styles[i].close_combat=close || (styles[i].close_combat && !melee && distance<stop+1 && st->hostile);
            if(!dodging && !paths[i].riding && (st->target<director.count || !strstr(ai,"static")) && (to->room!=st->room || distance>stop)) {
                actor->body.crouched=false;
                Point eye=st->position;eye.y+=styles[i].rat?.13f:1.3f;
                Hit wall;float len=fmaxf(.001f,hypotf(distance,dy));
                bool follow_nodes=!director.controls && st->target<director.count && distance>2.f;
                bool direct=!follow_nodes && st->room==to->room && st->room==campaign_room_id() && fabsf(dy)<.7f &&
                    !scene_trace(eye,(Point){dx/len,dy/len,dz/len},len,&wall);
                if(direct) {
                    float step=fminf(2*pace,distance-stop)/fmaxf(.001f,distance);
                    room_move(&actor->body,dx*step,dz*step);
                    moving=hypotf(actor->body.position.x-st->position.x,actor->body.position.z-st->position.z)>.001f;
                    st->position=actor->body.position;
                    if(moving){st->yaw=atan2f(dx,dz);paths[i].following=false;}
                }
                if(!moving) moving=campaign_nav_step(paths+i,&st->position,&st->room,target,to->room,pace,campaign_room_id(),&st->yaw,actor->body.crouched);
            }
        }
        if(moving)actor->body.crouched=false;
        actor->body.position=st->position;actor->body.yaw=st->yaw;
        EntityState *target=director.state+victim;
        bool engaging=!awaiting_target && st->enabled && (dodging?extra->shoot_dodge:st->hostile) && target->enabled && !target->dead &&
            (st->room==campaign_room_id() || target->room==campaign_room_id()) &&
            (director.controls || st->aim<director.count || st->target<director.count);
        bool throwable=gun==GRENADE || gun==MOLOTOV;
        MotionStyle *style=styles+i;
        unsigned hit_clip=actor->body.crouched?style->crouch_hit:style->hit;
        bool flinching=(hit_clip && st->hp<style->hp) || (style->transient &&
            (style->transient==style->hit || style->transient==style->crouch_hit));
        if(flinching && actor->animation.action!=RELOAD)actor->animation.action=IDLE;
        if(!flinching && !melee && !ammo->rounds[gun] && actor->animation.action==IDLE && npc_has_spare(ammo,gun)){
            if(throwable)refill_npc(ammo,gun);
            else{
                extra->default_weapon=false;
                eject_cases(i,gun,true,gun==SAWED?guns[gun].capacity-ammo->rounds[gun]:1,1);
                actor_action(&actor->animation,RELOAD);gun_sound(gun,true,i,st->position,styles[i].reload);
            }
        }
        if(!engaging)actor->sees=false;
        if(engaging) {
            Point eye=st->position;eye.y+=styles[i].rat?.13f:actor->body.crouched?.91f:1.35f;
            Point aim_at=target->position,forward;
            aim_at.y+=styles[victim].rat?.13f:(victim?actors[victim].body.crouched:p->crouched)?.91f:1.35f;
            bool visible=combat_sight(st->room,eye,target->room,aim_at,melee?1.65f:(rules?rules->visual_radius:20.f),false,&forward);
            if(!visible && st->room==target->room){
                float dx=aim_at.x-eye.x,dz=aim_at.z-eye.z;
                forward=(Point){dx,aim_at.y-eye.y,dz};
            }
            bool aligned=false;
            if(visible || st->room==target->room){
                const float radians=.01745329252f;
                float delta=remainderf(atan2f(forward.x,forward.z)-previous_yaw,6.2831853f);
                float rate=rules?(fabsf(delta)<rules->aim_cone*radians?rules->aim_speed:rules->turn_speed)*radians:6.f;
                float step=fmaxf(-rate*dt,fminf(rate*dt,delta));
                actor->body.yaw=st->yaw=previous_yaw+step;
                actor->body.pitch=atan2f(forward.y,hypotf(forward.x,forward.z));
                aligned=fabsf(delta-step)<=(rules?rules->shooting_cone*radians:1.f);
            }
            if(visible && !actor->sees)actor->attack=fmaxf(actor->attack,rules?rules->reaction_time:0.f);
            actor->sees=visible;
            float attack_late=weapon_cooldown_step(&actor->attack,dt);
            if(visible && aligned && !flinching && actor->attack<=0 && (actor->animation.action==IDLE || actor->animation.action==FIRE) &&
               !style->holstering && !style->drawing && (melee || ammo->rounds[gun])) {
                bool repeating=actor->animation.action==FIRE;
                begin_attack(i,&actor->animation,gun,repeating?attack_late:0);
                const FireRate *rate=styles[i].fire_rate;
                float interval=rate && rules?pose_fire_interval(rate,rules,rand()/(float)RAND_MAX):gun>=0?guns[gun].delay:1.2f;
                actor->animation.action_time=weapon_cooldown_start(&actor->attack,interval,repeating?attack_late:0);
                hud_dirty=true;
            }
        }
        actor->animation.scripted=(moving || engaging || dodging || actor->animation.action==RELOAD)?0:st->custom_pose;
        actor->animation.script_time=st->pose_seconds;
        float dx=st->position.x-previous.x,dz=st->position.z-previous.z;
        if(previous_room==st->room){
            float sn=sinf(st->yaw),cs=cosf(st->yaw);
            styles[i].forward_travel=dx*sn+dz*cs;
            styles[i].side_travel=dx*cs-dz*sn;
        }else if(moving)styles[i].forward_travel=paths[i].portal_travel;
        Clip previous_action=actor->animation.action;bool loading=previous_action==RELOAD;
        actor_locomotion(&actor->animation,styles[i].forward_travel,styles[i].side_travel,
                         actor->body.crouched,dodging?(Clip)extra->dodge:IDLE,dt);
        finish_weapon_step(i,&actor->animation,previous_action);
        if(loading && actor->animation.action!=RELOAD && gun>=0 && !pose_has_event(styles[i].reload,"c_reload"))refill_npc(ammo,gun);
    }
    (void)p;
}
static void hud(bool reloading) {
    bool caption=journal_state.caption[0] && journal_state.seconds>0 && (journal_state.always || ui_settings.tips);
    if(presentation_value(presentation.visibility)<.5f && !director.state[0].dead) {
        ui_caption(chapter,caption?journal_state.caption:"");hud_dirty=false;return;
    }
    int gun=inventory.equipped;
    Stock s=gun<0?(Stock){0}:inventory.stock[gun];
    char task[483];objective_text(task);
    ui_begin_update();
    ui_hud(chapter,director.state[0].dead?MP_TEXT("A   Продолжить","A   Continue"):caption?journal_state.caption:task,gun>=0?guns[gun].title:NULL,
           (unsigned)fmaxf(0,ceilf(director.state[0].hp*100/maximum_health())),inventory.pills,gun<0?-1:(int)guns[gun].pose,
           s.rounds,gun<0?0:ammo_pool(gun)->reserve,reloading,gun==SNIPER?inventory.zoom:0,gun>=0,0,0,caption && journal_state.always);
    ui_journal_link();ui_combat_hud(inventory.bullet_time,inventory.slow,player_crouched,director.player_slow);
    int saves=game_save_limit()?(int)game_save_limit()-(int)game_run.saves:-1;
    ui_run_status(game_run.mode==NEW_YORK_MINUTE?(int)ceilf(game_run.remaining):-1,saves);
    uint32_t owned=0,loaded=0;
    for(unsigned i=0;i<GUNS;i++)if(inventory.stock[i].owned){
        owned|=1u<<i;if(inventory.stock[i].rounds || !guns[i].capacity || spare_rounds(i))loaded|=1u<<i;
    }
    ui_weapon_palette(owned,loaded,inventory.equipped);
    ui_end_update();
    hud_dirty=false;
}
typedef struct {
    uint32_t version,count;
    Player player;Jump jump;Animation animation;Inventory inventory;
    float cooldown,speed;bool controls,widescreen;
    uint16_t camera,objective;
    Presentation presentation;CameraPath path;
    char prompt[3][160];
    bool exits[DIRECTOR_EXITS];
    CampaignProp props[CAMPAIGN_PROPS];
    Projectile projectiles[PROJECTILE_LIMIT];
} Checkpoint;
typedef struct {Journal journal;bool ai_exits[DIRECTOR_EXITS];float speed_target,speed_remaining;} SaveJournal;
static size_t checkpoint_size(void) {return sizeof(Checkpoint)+director.count*(sizeof(EntityState)+sizeof(Enemy));}
static size_t arsenal_size(void){return npc_count*sizeof(*npc_stock);}
// Version 11 introduced the compatible playback prefix. Version 12 adds both
// source clocks and active local snapshots; no runtime pointers are saved.
enum { MOTION_SAVE_BYTES=offsetof(MotionStyle,rules) };
typedef struct {
    char tag[4];uint32_t bytes,actors;
    float controlled,music,heartbeat,camera_height,scale,target,fade;
    uint16_t pending;uint8_t flags,reserved;
} SavedPlayback;
_Static_assert(MOTION_SAVE_BYTES==296 && sizeof(SavedPlayback)==44,"version 11 playback save layout");
_Static_assert(sizeof(ActorTimeline)==220,"version 12 channel clock save layout");
static size_t playback_size_for(unsigned version,unsigned snapshots){
    return sizeof(SavedPlayback)+(npc_count+1)*(4+MOTION_SAVE_BYTES+(version>=12?sizeof(ActorTimeline):0))+
        (version>=12?snapshots*ACTOR_TRANSITION_SNAPSHOT_BYTES:0);
}
static size_t playback_size(void){return playback_size_for(12,actor_save_transitions(NULL));}
static bool motion_entity(unsigned id){return !id || (director.def[id].flags&4);}
static void store_playback(void *data){
    SavedPlayback h={.tag={'A','M','C','2'},.bytes=MOTION_SAVE_BYTES+sizeof(ActorTimeline),.actors=npc_count+1,.reserved=actor_save_transitions(NULL),
        .controlled=controlled_scale,.music=music_mix,.heartbeat=heartbeat,.camera_height=camera_pose_offset,
        .scale=dodge_scale,.target=dodge_target,.fade=dodge_fade,.pending=pending_jump,
        .flags=dodge_block | (reticle_visible<<1) | (director.dodge_immortal<<2) | (director.dodge_pass_projectiles<<3)};
    uint8_t *out=data;memcpy(out,&h,sizeof h);out+=sizeof h;
    for(unsigned id=0;id<director.count;id++)if(motion_entity(id)){
        uint16_t prefix[2]={id,director_pose_started(id)};
        memcpy(out,prefix,sizeof prefix);out+=sizeof prefix;
        memcpy(out,styles+id,MOTION_SAVE_BYTES);out+=MOTION_SAVE_BYTES;
        memcpy(out,&styles[id].timeline,sizeof(ActorTimeline));out+=sizeof(ActorTimeline);
    }
    actor_save_transitions(out);
}
static bool valid_motion_clip(unsigned clip){return !clip || pose_bank_duration(clip)>0;}
static bool valid_timeline(const ActorTimeline *t){
    if(!t->active)return true;
    if(!isfinite(t->aim_age) || !isfinite(t->aim_weight) || t->aim_age<0 || t->aim_age>.081f || t->aim_weight<0 || t->aim_weight>1)return false;
    for(unsigned i=0;i<2;i++){
        const AnimationTimeline *c=t->layer+i;const AnimationEvaluation *e=t->sample+i;
        if(c->stage>4 || !valid_motion_clip(c->clip) || !valid_motion_clip(c->previous_clip) ||
           !valid_motion_clip(t->source[i]) || !valid_motion_clip(e->clip) || !valid_motion_clip(e->previous_clip) ||
           !isfinite(c->seconds) || !isfinite(c->weight) || !isfinite(t->input[i]) || c->seconds<0 || c->seconds>1 ||
           c->weight<0 || c->weight>1 || e->stage>4)return false;
        const AnimationClock *clocks[]={&c->current,&c->previous};
        for(unsigned j=0;j<2;j++){
            const AnimationClock *k=clocks[j];
            if(!isfinite(k->time) || !isfinite(k->origin) || !isfinite(k->end) || !isfinite(k->loop_start) ||
               k->end<0 || k->loop_start<0 || k->loop_start>k->end || k->loop>1)return false;
        }
        if(!isfinite(e->time) || !isfinite(e->previous_time) || !isfinite(e->current) || !isfinite(e->previous) || !isfinite(e->snapshot) ||
           e->current<-.0001f || e->previous<-.0001f || e->snapshot<-.0001f || fabsf(e->current+e->previous+e->snapshot-1)>.0001f)return false;
    }
    return true;
}
static bool restore_playback(const void *data,unsigned version){
    const uint8_t *in=data;SavedPlayback h;memcpy(&h,in,sizeof h);in+=sizeof h;
    if(memcmp(h.tag,version>=12?"AMC2":"AMC1",4) || h.bytes!=MOTION_SAVE_BYTES+(version>=12?sizeof(ActorTimeline):0) || h.actors!=npc_count+1 ||
       (version>=12?h.reserved>CAST_SLOTS+1:h.reserved!=0) || h.flags>15 ||
       !isfinite(h.controlled) || !isfinite(h.music) || !isfinite(h.heartbeat) || !isfinite(h.camera_height) ||
       !isfinite(h.scale) || !isfinite(h.target) || !isfinite(h.fade) || h.controlled<0 || h.controlled>4 ||
       h.music<0 || h.music>1 || h.heartbeat<0 || h.scale<=0 || h.target<=0 || h.fade<0 || !valid_motion_clip(h.pending))return false;
    for(unsigned id=0;id<director.count;id++)if(motion_entity(id)){
        uint16_t prefix[2];MotionStyle m={0};
        memcpy(prefix,in,sizeof prefix);in+=sizeof prefix;memcpy(&m,in,MOTION_SAVE_BYTES);in+=MOTION_SAVE_BYTES;
        if(version>=12){memcpy(&m.timeline,in,sizeof m.timeline);in+=sizeof m.timeline;if(!valid_timeline(&m.timeline))return false;}
        if(prefix[0]!=id || prefix[1]>1 || m.gun< -1 || m.gun>=GUNS || m.old_gun< -1 || m.old_gun>=GUNS ||
           m.last_gait>=CLIP_COUNT || m.dive_direction>=8 || m.dive_getup>=4 || m.rat_track>=RAT_TRACKS ||
           !isfinite(m.clock) || !isfinite(m.age) || !isfinite(m.hp) || !isfinite(m.yaw) || !isfinite(m.rat_time) ||
           !isfinite(m.pickup_retry) || !isfinite(m.render_time) || !isfinite(m.render_overlay) || !isfinite(m.exchange_weight) ||
           !isfinite(m.manual_turn) || !isfinite(m.forward_travel) || !isfinite(m.side_travel) ||
           m.clock<0 || m.age<0 || m.rat_time<0 ||
           !valid_motion_clip(m.clock_clip) || !valid_motion_clip(m.transient))return false;
        for(unsigned layer=0;layer<2;layer++)
            if(!valid_motion_clip(m.cursor[layer].clip) || !isfinite(m.cursor[layer].time) ||
               !isfinite(m.weapon_time[layer]))return false;
        if(m.ready){
            if(m.skin!=pose_bank_skin(director_text(director.def[id].model)))return false;
            m.rules=pose_skin_rules(m.skin);m.fire_rate=pose_fire_rate(m.gun<0?"empty":guns[m.gun].id);
        }
        styles[id]=m;director_resume_pose(id,prefix[1]!=0);
    }
    if(!actor_load_transitions(in,version>=12?h.reserved:0))return false;
    controlled_scale=h.controlled;music_mix=h.music;heartbeat=h.heartbeat;camera_pose_offset=h.camera_height;
    dodge_scale=h.scale;dodge_target=h.target;dodge_fade=h.fade;pending_jump=h.pending;
    dodge_block=h.flags&1;reticle_visible=(h.flags&2)!=0;
    director.dodge_immortal=(h.flags&4)!=0;director.dodge_pass_projectiles=(h.flags&8)!=0;
    MotionStyle *p=styles;
    if(p->ready)actor_motion(0,p->render_gait,p->render_time,p->render_action,p->render_overlay,p->whole,p->gun<0,pose_bank_two_handed_bones(p->gun<0?"empty":guns[p->gun].id),p->exchange_weight,0);
    if(p->ready)actor_set_timeline(0,&p->timeline);
    if(p->ready)aiming_motion(0,0,motion_player,NULL);
    return true;
}
static void report_autosave(void){
    SaveJob result;
    if(async_save_result(&result) && log_file)
        fprintf(log_file,"SAVE slot=%u stage=%s ok=%u\n",result.slot,result.stage,result.ok);
}
static void finish_autosave(void){
    async_save_finish();report_autosave();
}
static bool snapshot(bool background,unsigned slot,const char *stage,const Player *p,const Jump *jump,const Animation *a,float cooldown) {
    size_t size=checkpoint_size()+sizeof(SaveJournal)+sizeof particle_sources+1+sizeof director.pause_remaining+sizeof(GameRun)+director.count*sizeof(ActorExtra)+arsenal_size()+playback_size()+bank_ambient_size();Checkpoint *c=calloc(1,size);if(!c) return false;
    c->version=12;c->count=director.count;c->player=*p;c->jump=*jump;c->animation=*a;c->inventory=inventory;
    c->cooldown=cooldown;c->speed=director.speed;c->controls=director.controls;c->widescreen=director.widescreen;
    c->presentation=presentation;c->path=director.path;
    c->camera=director.camera;c->objective=director.objective;
    memcpy(c->prompt,current_objective,sizeof c->prompt);memcpy(c->exits,director.exits,sizeof c->exits);
    memcpy(c->props,campaign_props,sizeof c->props);
    memcpy(c->projectiles,campaign_projectiles,sizeof c->projectiles);
    EntityState *state=(EntityState*)(c+1);memcpy(state,director.state,director.count*sizeof(*state));
    memcpy(state+director.count,actors,director.count*sizeof(*actors));
    SaveJournal *extra=(SaveJournal*)((char*)c+checkpoint_size());extra->journal=journal_state;
    memcpy(extra->ai_exits,director.ai_exits,sizeof extra->ai_exits);
    extra->speed_target=director.speed_target;extra->speed_remaining=director.speed_remaining;
    memcpy(extra+1,particle_sources,sizeof particle_sources);uint8_t *tail=(uint8_t*)(extra+1)+sizeof particle_sources;
    *tail=director.player_slow;memcpy(tail+1,director.pause_remaining,sizeof director.pause_remaining);
    memcpy(tail+1+sizeof director.pause_remaining,&game_run,sizeof game_run);
    memcpy(tail+1+sizeof director.pause_remaining+sizeof game_run,visuals,director.count*sizeof(*visuals));
    uint8_t *ammo=tail+1+sizeof director.pause_remaining+sizeof game_run+director.count*sizeof(*visuals);
    memcpy(ammo,npc_stock,arsenal_size());
    store_playback(ammo+arsenal_size());
    bank_store_ambient(ammo+arsenal_size()+playback_size());
    SaveJob job={.slot=slot,.signature=director_signature(),.data=c,.size=size};
    snprintf(job.stage,sizeof job.stage,"%s",stage);
    snprintf(job.room,sizeof job.room,"%s",campaign_room_name());
    if(background){
        if(async_save_submit(&job))return true;
        free(c);return false;
    }
    finish_autosave();
    job.ok=save_write(job.slot,job.stage,job.room,job.signature,job.data,job.size);free(c);
    if(log_file)fprintf(log_file,"SAVE slot=%u stage=%s ok=%u\n",slot,stage,job.ok);
    return job.ok;
}
static bool store_state(unsigned slot,const char *stage,const Player *p,const Jump *jump,const Animation *a,float cooldown){
    return snapshot(false,slot,stage,p,jump,a,cooldown);
}
static bool valid_animation(const Animation *a){
    return (unsigned)a->gait<CLIP_COUNT && (unsigned)a->previous_gait<CLIP_COUNT && (unsigned)a->action<CLIP_COUNT &&
           (unsigned)a->weapon<WEAPON_COUNT && isfinite(a->gait_time) && isfinite(a->previous_time) && isfinite(a->action_time) &&
           isfinite(a->transition) && isfinite(a->script_time);
}
static bool restore_state(const SaveHeader *h,const Checkpoint *c,Player *p,Jump *j,Animation *a,float *cooldown) {
    size_t fixed=checkpoint_size()+(c->version>=3?sizeof(SaveJournal):0)+(c->version>=4?sizeof particle_sources:0)+
        (c->version>=5)+(c->version>=6?sizeof director.pause_remaining:0)+(c->version>=7?sizeof(GameRun)+director.count*sizeof(ActorExtra):0)+
        (c->version>=8?arsenal_size():0);
    size_t playback=0;
    if(c->version>=11){
        if(h->bytes<fixed+sizeof(SavedPlayback))return false;
        SavedPlayback saved;memcpy(&saved,(const uint8_t*)c+fixed,sizeof saved);
        if(saved.reserved>CAST_SLOTS+1)return false;
        playback=playback_size_for(c->version,saved.reserved);fixed+=playback;
    }
    if(!director_signature_matches(h->signature) || c->version<2 || c->version>12 || c->count!=director.count ||
       (c->version>=8?h->bytes<fixed:h->bytes!=fixed) ||
       (c->version>=9 && (c->inventory.infinite_ammo>>GUNS)) || c->inventory.pills>8 || c->inventory.equipped< -1 || c->inventory.equipped>=GUNS || (!memchr(c->prompt[0],0,160) || !memchr(c->prompt[1],0,160) || !memchr(c->prompt[2],0,160)) ||
       (c->camera!=DIRECTOR_NONE && c->camera>=director.count) || !valid_animation(&c->animation) || !isfinite(c->speed) || !isfinite(c->cooldown) ||
       (c->inventory.zoom!=1 && c->inventory.zoom!=2 && c->inventory.zoom!=4)) return false;
    const EntityState *st=(const EntityState*)(c+1);
    memcpy(director.state,st,c->count*sizeof(*st));
    memcpy(campaign_projectiles,c->projectiles,sizeof c->projectiles);
    if(!director_valid_state() || !combat_valid()) return false;
    // A save made after premature activation must still get its real
    // activation event when the script assigns the opponent.
    for(unsigned id=1;id<director.count;id++)
        if(director_needs_target(id))director.state[id].activated=false;
    director_restore_corpses();
    memcpy(actors,st+c->count,c->count*sizeof(*actors));
    for(unsigned i=0;i<c->count;i++){
        if(!valid_animation(&actors[i].animation))return false;
        retail_restore_animation(&actors[i].animation,actors[i].body.crouched);
    }
    if(c->version>=8){
        const uint8_t *data=(const uint8_t*)c+fixed-arsenal_size()-playback;
        memcpy(npc_stock,data,arsenal_size());
        for(unsigned i=0;i<npc_count;i++){
            const NpcAmmo *ammo=npc_stock+i;
            if(ammo->held>GUNS || ((ammo->issued|ammo->infinite|ammo->supplied)>>GUNS))return false;
            for(int g=0;g<GUNS;g++)if(ammo->rounds[g]>guns[g].capacity)return false;
        }
        if(playback && !restore_playback(data+arsenal_size(),c->version))return false;
        if(!bank_restore_ambient(data+arsenal_size()+playback,h->bytes-fixed))return false;
    }else{
        for(unsigned i=1;i<director.count;i++)if(director.def[i].flags&4){
            int gun=find_gun(director_text(st[i].weapon));equip_npc(i,gun);
            if(st[i].infinite_ammo && gun>=0)arsenals[i]->infinite|=1u<<gun;
        }
    }
    if(c->version>=3){
        const SaveJournal *extra=(const SaveJournal*)((const char*)c+checkpoint_size());
        for(unsigned i=0;i<3;i++)if(!memchr(extra->journal.items[i],0,sizeof extra->journal.items[i]))return false;
        if(!memchr(extra->journal.caption,0,sizeof extra->journal.caption) || !isfinite(extra->journal.seconds) ||
           !isfinite(extra->speed_remaining) || !isfinite(extra->speed_target))return false;
        if(c->version>=4){memcpy(particle_sources,extra+1,sizeof particle_sources);if(!particles_valid())return false;}
        if(c->version>=5){
            const uint8_t *tail=(const uint8_t*)(extra+1)+sizeof particle_sources;director.player_slow=*tail!=0;
            if(c->version>=6){
                memcpy(director.pause_remaining,tail+1,sizeof director.pause_remaining);
                if(c->version>=7){
                    GameRun restored;memcpy(&restored,tail+1+sizeof director.pause_remaining,sizeof restored);
                    if(!game_run_valid(&restored))return false;
                    game_run=restored;
                    memcpy(visuals,tail+1+sizeof director.pause_remaining+sizeof game_run,director.count*sizeof(*visuals));
                    for(unsigned i=0;i<director.count;i++){
                        const ActorExtra *v=visuals+i;
                        if(!isfinite(v->seconds) || !isfinite(v->delay) || !isfinite(v->dodge_remaining) ||
                           v->seconds<0 || v->face>2 || v->default_weapon>1 || v->dodge>=CLIP_COUNT ||
                           v->patrol_count>6 || (v->patrol_count && v->waypoint>=v->patrol_count))return false;
                        for(unsigned k=0;k<v->patrol_count;k++)if(v->patrol[k]>=director.count)return false;
                    }
                }
                for(unsigned i=0;i<CAMPAIGN_PROPS;i++)if(!isfinite(director.pause_remaining[i]) || director.pause_remaining[i]<0)return false;
            }
        }
        journal_state=extra->journal;memcpy(director.ai_exits,extra->ai_exits,sizeof director.ai_exits);
        director.speed_target=extra->speed_target;director.speed_remaining=extra->speed_remaining;
    }
    memcpy(director.exits,c->exits,sizeof c->exits);memcpy(campaign_props,c->props,sizeof c->props);
    director.speed=c->speed;director.controls=c->controls;director.camera=c->camera;director.widescreen=c->widescreen;
    if(c->path.path!=DIRECTOR_NONE && (camera_path_duration(c->path.path)<=0 || c->path.owner>=director.count))return false;
    presentation=c->presentation;director.path=c->path;
    director.objective=c->objective;memcpy(current_objective,c->prompt,sizeof c->prompt);
    *p=c->player;*j=c->jump;*a=c->animation;inventory=c->inventory;*cooldown=c->cooldown;
    retail_restore_animation(a,p->crouched);
    if(inventory.equipped!=SNIPER)inventory.zoom=1;
    // Retail scripts only grant the player infinite ammo for the bat and pipe.
    // Older saves recorded a single flag even when a different weapon was drawn.
    if(c->version<9)inventory.infinite_ammo=0;
    return enter(director.state[0].room);
}
static bool run_map(const char *stage,UiChoice *next,char following[64],const SaveHeader *header,const Checkpoint *saved) {
    u64 load_begin=svcGetSystemTick();
    unsigned automatic_slot=save_autosave_slot(stage);
    RoomTiming timing={0};
    Player p;
    ui_level_loading(stage);
    if(!room_campaign(stage,&p,!strcmp(stage,"part0_level1"))) return false;
    GroundAnchor ground_anchor={p.position,campaign_room_id()};
    ui_loading_progress(75);
    bool ok=false;
    director_motion_guard(campaign_panel_blocked);
    if(!localization_open() || !game_rules_enter(stage) || !presentation_open(stage,chapter) || !director_open(stage,action,log_file) || !campaign_collision()) goto done;
    director_contact_frame(campaign_contact_frame);
    director_mesh_contact(campaign_body_contact);
    director_collision_relevance(campaign_collision_relevant);
    room_motion_range(director_prop_travel());
    styles=calloc(director.count,sizeof(*styles));
    visuals=calloc(director.count,sizeof(*visuals));actors=calloc(director.count,sizeof(*actors));paths=calloc(director.count,sizeof(*paths));
    platforms=calloc(director.count,sizeof(*platforms));
    if(!styles || !visuals || !actors || !paths || !platforms || !open_arsenals() || !campaign_nav_open(stage) || !combat_open() || !particles_open() || !actor_campaign_moves() || !actor_rifle()) goto done;
    campaign_nav_guard(campaign_npc_passage);
    ui_loading_progress(85);
    sound_ready=phone_open(false);
    if(sound_ready) phone_finish_calls();
    for(unsigned i=0;i<CAST_SLOTS;i++) actor_ids[i]=DIRECTOR_NONE;
    EntityState *player=director.state;
    player->position=p.position;player->room=campaign_room_id();player->yaw=p.yaw;
    player_listener(player->room,&p);
    if(!saved) {director_set_health(0,health_fraction*maximum_health());director_start();}
    if(log_file) {fprintf(log_file,"CAMPAIGN start=%s room=%u entities=%u\n",stage,campaign_room_id(),director.count);fflush(log_file);}
    Camera camera={0};Effects fx={0};Animation anim={0};Jump jump={0};TouchInput touch={0};
    motion_effects=&fx;motion_player=&p;motion_camera=&camera;motion_player_animation=&anim;motion_jump=&jump;pending_jump=0;dodge_scale=dodge_target=1;dodge_fade=0;dodge_block=false;reticle_visible=true;
    controlled_scale=1;music_mix=1;heartbeat=0;camera_pose_offset=0;
    float cooldown=0,death_time=0;
    unsigned count=0,frames=0;
    if(saved && !restore_state(header,saved,&p,&jump,&anim,&cooldown)) goto done;
    if(saved && saved->version<11)for(unsigned i=0;i<director.count;i++){
        styles[i].skip_past=true;
        const Animation *restored=i?&actors[i].animation:&anim;
        if(attacking(restored->action)){
            int gun=i?find_gun(director_text(director.state[i].weapon)):inventory.equipped;
            configure_motion(styles+i,i,gun);
            styles[i].previous_fire_paid=saved->version<10;
            styles[i].cursor[1].clip=styles[i].shots[0];
            styles[i].cursor[1].time=restored->action_time;
        }
    }
    player_listener(player->room,&p);
    const GameTuning *tuning=game_tuning();
    combat_set_health_scales(tuning->player_health,tuning->enemy_health);
    if(saved && retry_deaths>game_run.deaths)game_run.deaths=retry_deaths;
    retry_deaths=0;unsigned locked=DIRECTOR_NONE;float look_idle=0,since_hit=0;
    float focus_distance=0,focus_weight=0;
    player_crouched=p.crouched;
    if(saved && log_file){fprintf(log_file,"LOAD stage=%s room=%u\n",stage,player->room);fflush(log_file);}
    if(!saved) director.autosave=true;
    Point fall_reference=p.position;
    unsigned physics_room=player->room;
    if(!jump.airborne && physics_room==campaign_room_id())remember_ground(&ground_anchor,&p);
    // These recur for every bullet/casing. Reserve their complete variants
    // before optional weapon tails can consume the bounded stage cache.
    bank_preload("projectiles","bullethit_generic");
    bank_preload("projectiles","casehit_generic");
    preload_weapon_sounds(true);
    ui_loading_progress(87);
    campaign_visit_surfaces(preload_footsteps);
    ui_loading_progress(88);
    director_preload_sounds(bank_preload);
    ui_loading_progress(89);
    // Optional weapon tails must not evict the budget for steps or opening props.
    preload_weapon_sounds(false);
    ui_loading_progress(90);
    if(!preload_stage_models() || !async_save_open())goto done;
    u64 gyro_begin=svcGetSystemTick();gyro_prepare(ui_settings.gyro);
    if(log_file)fprintf(log_file,"LOAD gyro stage=%s enabled=%u elapsed_ms=%.2f\n",stage,ui_settings.gyro,(svcGetSystemTick()-gyro_begin)*(1000./SYSCLOCK_ARM11));
    campaign_update_moving_rooms();
    // Select the opening camera's room while the loading screen is still up.
    if(director_view_room()!=campaign_room_id() && !enter(director_view_room()))goto done;
    ui_loading_progress(100);
    if(log_file){fprintf(log_file,"LOAD ready stage=%s elapsed_ms=%.1f\n",stage,(svcGetSystemTick()-load_begin)*(1000./SYSCLOCK_ARM11));fflush(log_file);}
    hud_dirty=true;ok=true;
    u64 previous=svcGetSystemTick(),ready_tick=previous;bool control_logged=false,control_draw_logged=false;
    while(aptMainLoop()) {
        u64 frame_start=svcGetSystemTick();
        SetupTiming setup={0};u64 section=frame_start;
        unsigned frame_room=campaign_room_id();
        async_log_watch("audio");
        bank_tick();
        async_log_watch("setup");
        setup.audio=svcGetSystemTick()-section;section=svcGetSystemTick();
        report_autosave();
        if(director.failed){ok=false;break;}
        if(director.reload) {
            async_log_watch(NULL);
            finish_autosave();
            int retry_slot=save_latest(automatic_slot);
            if(retry_slot>=0){ui_save_slot=(unsigned)retry_slot;*next=UI_LOAD;}
            else snprintf(following,64,"%s",stage);
            break;
        }
        if(director.novel) {
            async_log_watch(NULL);
            gyro_set_active(false);
            if(!(ok=read_notes())) break;
            previous=svcGetSystemTick();continue;
        }
        if(director.next_level || director.finished) {
            async_log_watch(NULL);
            gyro_set_active(false);
            if(director.next_level)game_rules_finish(player->hp/maximum_health());
            if(director.next_level) snprintf(following,64,"%s",director_text(director.next_level));
            if(director.finished) {
                following[0]=0;
                if(!strcmp(stage,"p3_l7-3") && game_run.mode==DEAD_ON_ARRIVAL && !save_completion())ui_notice(MP_TEXT("Не удалось записать прохождение на SD","Could not save progress to SD"));
                if(C3D_FrameBegin(0)) C3D_FrameEnd(0);
                bank_clear();movie_play("credits.mpv");
                if(!strcmp(stage,"p3_l7-3") && game_run.mode==NEW_YORK_MINUTE){
                    unsigned best,seconds=(unsigned)ceilf(game_run.elapsed);
                    if(!save_race_time(seconds,&best))ui_notice(MP_TEXT("Не удалось записать рекорд","Could not save the record"));
                    ui_race_finish(seconds,best);
                }
                if(!strcmp(stage,"p3_l7-3") && game_run.mode==DEAD_ON_ARRIVAL)snprintf(following,64,"end_combat");
            }
            break;
        }
        int previous_gun=inventory.equipped;
        unsigned view_room=director_view_room();
        if(view_room!=campaign_room_id()) {
            u64 load_start=svcGetSystemTick();
            if(!(ok=enter(view_room))) break;
            previous+=svcGetSystemTick()-load_start;
            camera=(Camera){0};
        }
        if(physics_room!=player->room){
            physics_room=player->room;jump=(Jump){0};fall_reference=player->position;
        }
        p.position=player->position;p.yaw=player->yaw;
        bool player_input=!player->dead && director.controls && player->room==view_room && !death_camera();
        if(player_input && !control_logged){
            control_logged=true;
            if(log_file)fprintf(log_file,"LOAD controls stage=%s after_ready_ms=%.1f\n",stage,(svcGetSystemTick()-ready_tick)*(1000./SYSCLOCK_ARM11));
        }
        setup.room=svcGetSystemTick()-section;section=svcGetSystemTick();
        if(player_input)campaign_panel_contact(&p,jump.airborne);
        setup.contact=svcGetSystemTick()-section;section=svcGetSystemTick();
        if(director.autosave && player_input && !player->dead) {
            u64 save_start=svcGetSystemTick();
            if(snapshot(true,automatic_slot,stage,&p,&jump,&anim,cooldown))director.autosave=false;
            previous+=svcGetSystemTick()-save_start;
        }
        setup.save=svcGetSystemTick()-section;section=svcGetSystemTick();
        gyro_set_active(ui_settings.gyro && player_input && !player->dead);
        setup.gyro=svcGetSystemTick()-section;section=svcGetSystemTick();
        hidScanInput();u32 down=hidKeysDown(),held=hidKeysHeld();
        setup.input=svcGetSystemTick()-section;
        if(down&KEY_START) {
            async_log_watch(NULL);
            ui_weapon_close();gyro_set_active(false);
            finish_autosave();bank_pause(true);
            bool can_save=!player->dead && (!game_save_limit() || game_run.saves<game_save_limit());
            CampaignPause pause={.player=&p,.animation=&anim,.effects=&fx,.entry=camera};
            scene_pause_begin(&pause.orbit,&camera,&p);
            UiChoice choice=player_input?ui_pause_menu(can_save,pause_frame,&pause):ui_menu(true,can_save);
            if(choice==UI_ANIMATIONS){room_animation_gallery();choice=UI_RESUME;}
            bank_pause(false);
            if(choice==UI_SAVE) {
                if(game_save_limit())game_run.saves++;
                if(!store_state(ui_save_slot,stage,&p,&jump,&anim,cooldown)){
                    if(game_save_limit())game_run.saves--;
                    ui_notice(MP_TEXT("Ошибка сохранения","Save failed"));
                }
                choice=UI_RESUME;
            }
            if(choice!=UI_RESUME){*next=choice;break;}
            if(sound_ready) phone_volume();
            gyro_prepare(ui_settings.gyro);
            previous=svcGetSystemTick();touch=(TouchInput){0};hud_dirty=true;continue;
        }
        if(player->dead){
            if(!death_time){
                hud_dirty=true;camera=(Camera){0};inventory.slow=false;bank_time_scale(1);
                pending_jump=0;director.can_use=false;
            }
            if(hud_dirty) hud(anim.action==RELOAD);
            if(down&KEY_A){retry_deaths=game_run.deaths+1;director.reload=true;continue;}
            float death_length=styles[0].ready && styles[0].gait[FALL]?pose_bank_duration(styles[0].gait[FALL]):actor_duration(FALL);

            u64 tick=svcGetSystemTick();
            float step=fminf(.05f,(float)(tick-previous)/SYSCLOCK_ARM11);
            previous=tick;
            presentation_tick(step);director_camera_tick(step);director_tick(step*director.speed);
            if(director.reload || !player->dead || player->room!=view_room)continue;
            if(director.collision_dirty){if(!(ok=campaign_collision()))break;director.collision_dirty=false;}
            p.position=player->position;p.yaw=player->yaw;p.pitch=-.18f;
            float advance=fminf(step,fmaxf(0,death_length-death_time));
            death_time+=step;anim.scripted=0;anim.action=IDLE;
            actor_step(&anim,FALL,1,advance);scene_camera_sweep(&camera,&p,0,camera_obstacle);
            Point pivot=p.position;pivot.y+=.6f;
            Camera framed=camera;
            if(director.path.path!=DIRECTOR_NONE && director.path.parented && !director.path.owner)
                camera_path_view(&framed,director.path.path,director.path.time,&p);
            combat_camera_bodies(0,player->room);
            bool framed_body=scene_death_camera(&framed,pivot,cinematic_obstacle);
            if(framed_body)camera=framed;
            player_listener(player->room,&p);
            Enemy cast[CAST_SLOTS]={0};
            if(!(ok=local_cast(&p,&camera,framed_body?&pivot:NULL,cast,&count) && prepare_models(count)))break;
            retail_motion(0,0,&anim,&p,inventory.equipped,advance,0,0,MOTION_NONE);
            room_render_campaign(&p,&anim,&camera,&fx,cast,count,inventory.equipped<0,1);
            continue;
        }
        death_time=0;
        async_log_watch("player");
        u64 now=svcGetSystemTick();
        float wall_time=(float)(now-previous)/SYSCLOCK_ARM11;
        float real=fminf(.05f,wall_time);previous=now;
        if(player_input && !player->dead){
            game_run.elapsed+=wall_time;game_run.chapter_time+=wall_time;
            if(game_run.mode==NEW_YORK_MINUTE){
                int before=(int)ceilf(game_run.remaining);
                game_run.remaining=fmaxf(0,game_run.remaining-wall_time);
                int seconds=(int)ceilf(game_run.remaining);
                if(before!=seconds){hud_dirty=true;if(seconds<=10)bank_sound(0,"menu","timeout");}
                if(!seconds){player->dead=true;player->hp=0;hud_dirty=true;continue;}
            }
        }
        down=ui_game_keys(down);held=ui_game_keys(held);
        if(journal_state.seconds>0){
            journal_state.seconds=fmaxf(0,journal_state.seconds-real);
            if(!journal_state.seconds)hud_dirty=true;
        }
        touchPosition pen={0};if(held&KEY_TOUCH)hidTouchRead(&pen);
        unsigned buttons=TOUCH_BIT(TOUCH_CROUCH)|TOUCH_BIT(TOUCH_WEAPON);
        if(director.player_slow && ui_touch_slow())buttons|=TOUCH_BIT(TOUCH_SLOW);
        if(presentation_value(presentation.visibility)>=.5f)buttons|=TOUCH_BIT(TOUCH_JOURNAL);
        if(ui_touch_shortcuts())buttons|=TOUCH_SHORTCUTS;
        TouchAction gesture=touch_update(&touch,held&KEY_TOUCH,pen.px,pen.py,buttons);
        uint32_t owned_weapons=0;
        for(unsigned i=0;i<GUNS;i++)if(inventory.stock[i].owned)owned_weapons|=1u<<i;
        WeaponChoice choice=ui_weapon_input(down&KEY_TOUCH,pen.px,pen.py,
            player_input && presentation_value(presentation.visibility)>=.5f,owned_weapons,inventory.equipped);
        if(choice.consumed){touch.origin=TOUCH_WEAPON;gesture=(TouchAction){0};}
        if(choice.changed)hud_dirty=true;
        if(choice.selected!=inventory.equipped){
            inventory.equipped=choice.selected;inventory.zoom=1;actor_action(&anim,IDLE);
            bank_sound(0,"hud","weaponselectionconfirmed");
        }else if(choice.changed)bank_sound(0,"hud","weaponselectionchanges");
        if(player_input && (down&KEY_TOUCH)) {
            if(gesture.pressed==TOUCH_JOURNAL){
                char task[483];objective_text(task);bank_pause(true);
                gyro_set_active(false);
                ui_journal(task,journal_state.items,journal_state.caption);bank_pause(false);
                previous=svcGetSystemTick();touch=(TouchInput){0};hud_dirty=true;continue;
            }
            if(gesture.pressed==TOUCH_HEAL) down|=KEY_SELECT;
            if(gesture.pressed==TOUCH_CROUCH && inventory.dodge_time<=0 && !jump.airborne && !pending_jump &&
               !(styles[0].recovery && styles[0].transient && styles[0].transient!=styles[0].land)) {
                scene_toggle_crouch(&p);
                player_crouched=p.crouched;hud_dirty=true;
            }
        }
        bool toggle_slow=(down&KEY_ZL) || ((down&KEY_TOUCH) && gesture.pressed==TOUCH_SLOW);
        if(player_input && toggle_slow && director.player_slow && !dodge_block && (inventory.slow || inventory.bullet_time>.001f)){
            inventory.slow=!inventory.slow;hud_dirty=true;
        }
        if((ui_touch_shortcuts() && gesture.pressed==TOUCH_FIRE) || (down&KEY_ZR)) down|=KEY_R;
        if(ui_touch_shortcuts() && gesture.held==TOUCH_FIRE)held|=KEY_R;

        MotionEvent interaction=MOTION_NONE;
        bool used_object=false;
        if(player_input && (down&KEY_SELECT) && inventory.pills && player->hp<maximum_health() && inventory.recovery<=0) {
            float immediate=fminf(tuning->immediate,game_pill_fraction());
            director_set_health(0,fminf(maximum_health(),player->hp+maximum_health()*immediate));
            // The retail ShootPainkiller frame channel owns its sound.
            interaction=MOTION_MEDICINE;inventory.pills--;inventory.recovery=2;hud_dirty=true;
        }
        if(inventory.recovery>0){float step=fminf(real,inventory.recovery);director_set_health(0,fminf(maximum_health(),player->hp+(game_pill_fraction()-fminf(tuning->immediate,game_pill_fraction()))*.5f*maximum_health()*step));
            inventory.recovery-=step;hud_dirty=true;}
        if(player_input && inventory.slow && !dodge_block) {
            int before=(int)(inventory.bullet_time*86);
            // Charge only the unscaled time actually advanced by gameplay.
            inventory.bullet_time=fmaxf(0,inventory.bullet_time-real/15.f);
            if(inventory.bullet_time==0) inventory.slow=false;
            if(before!=(int)(inventory.bullet_time*86)) hud_dirty=true;
        }
        bool slowed=player_input && inventory.slow && !dodge_block;
        float target=slowed?.2f:1.f;
        controlled_scale+=fminf(fabsf(target-controlled_scale),real*.8f/(slowed?.3f:.5f))*((target>controlled_scale)-(target<controlled_scale));
        if(dodge_fade>0){float step=fminf(real,dodge_fade);dodge_scale+=(dodge_target-dodge_scale)*(step/dodge_fade);dodge_fade-=step;}
        float time_scale=player_input?fminf(controlled_scale,dodge_scale):1;
        bool quiet=slowed || (player_input && dodge_scale<.99f);
        music_mix=quiet?fmaxf(0,music_mix-real*2):fminf(1,music_mix+real*2);
        bank_music_level(music_mix);
        if(quiet){heartbeat+=real;if(heartbeat>=1.25f){bank_sound(0,"menu","slomo_heartbeat");heartbeat=fmodf(heartbeat,1.25f);}}
        else heartbeat=0;
        float dt=real*director.speed*time_scale;
        combat_slow_rounds(director.speed*time_scale<.99f);
        bank_time_scale(player_input && (inventory.slow || inventory.dodge_time>0)?.72f:1.f);
        if(!player_input) down=held=0;
        if((down&KEY_Y) && inventory.equipped>=0) {
            const unsigned order[]={BAT,PIPE,EAGLE,DUAL_PISTOL,PISTOL,JACKHAMMER,PUMP,SAWED,COMMANDO,DUAL_INGRAM,INGRAM,M79,GRENADE,MOLOTOV,SNIPER};
            unsigned start=0;while(start<GUNS && order[start]!=(unsigned)inventory.equipped)start++;
            for(int delta=1;delta<=GUNS;delta++){int k=order[(start+delta)%GUNS];if(inventory.stock[k].owned){inventory.equipped=k;break;}}
            hud_dirty=true;inventory.zoom=1;actor_action(&anim,IDLE);
        }
        circlePosition move={0},look={0};hidCircleRead(&move);hidCstickRead(&look);
        styles[0].manual_turn=0;
        float x=player_input?axis(move.dx):0,z=player_input?axis(move.dy):0;
        bool focused=false;
        if(player_input) {
            GyroLook motion=gyro_read(real,ui_settings.gyro_horizontal,ui_settings.gyro_vertical);
            float turn=cstick_axis(look.dx,ui_settings.deadzone),pitch=cstick_axis(look.dy,ui_settings.deadzone);
            float manual_yaw=touch_look_angle(turn+!!(held&KEY_DRIGHT)-!!(held&KEY_DLEFT),gesture.dx,real,ui_settings.horizontal,false);
            float manual_pitch=touch_look_angle(pitch+!!(held&KEY_DUP)-!!(held&KEY_DDOWN),gesture.dy,real,ui_settings.vertical,true);
            styles[0].manual_turn=manual_yaw/inventory.zoom;
            p.yaw+=styles[0].manual_turn+motion.yaw/inventory.zoom;
            float angle=(manual_pitch+motion.pitch)/inventory.zoom;
            p.pitch=fminf(1.25f,fmaxf(-1.25f,p.pitch+(ui_settings.inverted?-angle:angle)));
            bool looking=motion.yaw || motion.pitch || turn || pitch || gesture.dx || gesture.dy || (held&(KEY_DUP|KEY_DDOWN|KEY_DLEFT|KEY_DRIGHT));
            look_idle=looking?0:look_idle+real;
            if(ui_settings.recenter && look_idle>.65f && fabsf(x)+fabsf(z)>.1f && inventory.zoom==1)
                p.pitch*=fmaxf(0,1-4*real);
            bool tap_aim=gesture.pressed==TOUCH_AIM;
            bool snap=ui_settings.quickaim && ((down&KEY_R) || tap_aim);
            bool tracking=ui_settings.lock && (locked!=DIRECTOR_NONE || (held&KEY_R) || gesture.held==TOUCH_AIM);
            if(inventory.equipped>=0 && inventory.equipped!=BAT && inventory.equipped!=PIPE && inventory.equipped!=M79 && inventory.equipped!=GRENADE && inventory.equipped!=MOLOTOV && inventory.equipped!=SNIPER && (snap || tracking || (ui_settings.autoaim && (held&KEY_R)))){
                Point ray;locked=combat_aim_target(&p,ui_settings.lock?locked:DIRECTOR_NONE,
                    snap?.52f:tracking?.40f:.035f*tuning->aim_radius,&ray,&focus_distance);
                if(locked!=DIRECTOR_NONE){
                    float mix=snap?1:fminf(1,real*(tracking?12:18));
                    focused=true;focus_weight+=mix*(1-focus_weight);
                    float delta=atan2f(ray.x,ray.z)-p.yaw;
                    p.yaw+=atan2f(sinf(delta),cosf(delta))*mix;
                    p.pitch+=(atan2f(ray.y,hypotf(ray.x,ray.z))-p.pitch)*mix;
                }
            }else locked=DIRECTOR_NONE;
        }
        if(!player_input)focus_weight=0;
        else if(!focused && anim.action!=FIRE)focus_weight=fmaxf(0,focus_weight-8*real);
        float forward_travel=0,side_travel=0;
        if(player_input) {
            // Retail Max binds Walk to Run: stick travel selects direction,
            // while the authored animation controls speed, even at half tilt.
            float length=fmaxf(.0001f,hypotf(x,z)),si=sinf(p.yaw),co=cosf(p.yaw);
            float vx=(co*x+si*z)/length,vz=(co*z-si*x)/length;
            bool touch_jump=gesture.pressed==TOUCH_JUMP;
            bool rising=styles[0].recovery && styles[0].transient && styles[0].transient!=styles[0].land;
            bool jump_pressed=!rising && ((down&KEY_B) || touch_jump);
            if(!rising && !pending_jump && (down&KEY_L) && director.player_slow && !jump.airborne && !p.crouched && fabsf(x)+fabsf(z)>.35f && inventory.dodge_time<=0 &&
               inventory.bullet_time>=.01f && inventory.equipped>=0 && inventory.equipped!=SNIPER && inventory.equipped!=GRENADE && inventory.equipped!=MOLOTOV && inventory.equipped!=BAT && inventory.equipped!=PIPE) {
                inventory.bullet_time=fmaxf(0,inventory.bullet_time-.10f);hud_dirty=true;
                inventory.dodge=fabsf(x)>fabsf(z)?(x<0?DODGE_LEFT:DODGE_RIGHT):(z<0?DODGE_BACKWARD:DODGE_FORWARD);
                configure_motion(styles,0,inventory.equipped);
                unsigned dir=inventory.dodge-DODGE_FORWARD;
                if(fabsf(x)>.4142f*fabsf(z) && fabsf(z)>.4142f*fabsf(x))dir=4+(z>0?2:0)+(x>0);
                styles[0].dive_direction=dir;
                float duration=pose_bank_duration(styles[0].shoot_dive[dir]);
                inventory.dodge_time=duration>0?duration:actor_duration(inventory.dodge);
                float magnitude=fmaxf(.001f,hypotf(vx,vz));
                inventory.dodge_x=vx/magnitude;inventory.dodge_z=vz/magnitude;
                hud_dirty=true;
            }
            // Retail has a stationary kneel, not crouch locomotion. Stand before
            // moving; if there is no headroom, stay put instead of crawling.
            if(p.crouched && hypotf(x,z)>.04f)scene_toggle_crouch(&p);
            if(player_crouched!=p.crouched){player_crouched=p.crouched;hud_dirty=true;}
            configure_motion(styles,0,inventory.equipped);
            float speed=p.crouched?0:player_run_speed(styles,z,x,dt);
            // The original Max skins specify AirborneSpeed=2.5 m/s for steering.
            if(jump.airborne && inventory.dodge_time<=0)speed=2.5f;
            if(inventory.dodge_time>0) {
                jump_pressed=false;
                vx=inventory.dodge_x;vz=inventory.dodge_z;
                unsigned clip=styles[0].shoot_dive[styles[0].dive_direction];
                float duration=pose_bank_duration(clip),distance=pose_stride(clip);
                speed=(duration>0 && distance>0)?distance/duration:4.5f;
                inventory.dodge_time=fmaxf(0,inventory.dodge_time-dt);
            }
            Point feet=p.position;bool airborne=jump.airborne;float impact_speed=-jump.velocity+game_gravity()*dt;
            bool takeoff=jump_pressed && !p.crouched && !airborne && !pending_jump;
            if(takeoff){
                configure_motion(styles,0,inventory.equipped);
                unsigned clip=styles[0].gait[JUMP];
                unsigned forward_jump=styles[0].jump[x<-.3f*z?0:x>.3f*z?2:1];
                if(z>.01f && forward_jump)clip=forward_jump;
                if(pose_has_event(clip,"c_jump")){pending_jump=clip;takeoff=false;}
            }
            Jump before_step=jump;
            scene_move_gravity(&p,&jump,vx*speed*dt,vz*speed*dt,dt,takeoff,game_gravity(),7.5f);
            int crossing=campaign_crossing(&p,feet,(Point){vx*speed*dt,0,vz*speed*dt},director.exits);
            if(crossing==CAMPAIGN_CROSS_BLOCKED){
                // Re-evaluate the vertical step over the source floor. The rejected
                // destination can lie outside this room's BSP floor triangles.
                p.position=feet;jump=before_step;
                scene_move_gravity(&p,&jump,0,0,dt,takeoff,game_gravity(),7.5f);
            }
            if(airborne!=jump.airborne)bank_sound(0,"characters",jump.airborne?"jump_medium":"land_lowheight");
            if(airborne && !jump.airborne && impact_speed>15)director_hit(0,.01f*maximum_health()*powf(impact_speed-10,2.1f));
            float dx=p.position.x-feet.x,dz=p.position.z-feet.z;
            forward_travel=dx*si+dz*co;side_travel=dx*co-dz*si;
            if(crossing>=0 && !(ok=transfer_player(&p,&jump,&camera,&fall_reference,&physics_room,
                                                   crossing,!airborne && !jump_pressed,&previous)))break;
            if(!jump.airborne){
                fall_reference=p.position;remember_ground(&ground_anchor,&p);
            }else if(p.position.y<fall_reference.y-12.f){
                u64 start=svcGetSystemTick();
                if(campaign_room_id()!=ground_anchor.room && !(ok=enter(ground_anchor.room)))break;
                previous+=svcGetSystemTick()-start;physics_room=ground_anchor.room;
                if(log_file)fprintf(log_file,"RECOVERY fall room=%u\n",physics_room);
                p.position=fall_reference=ground_anchor.feet;jump=(Jump){0};camera=(Camera){0};
                pending_jump=0;inventory.dodge_time=0;forward_travel=side_travel=0;
            }
            player_listener(campaign_room_id(),&p);
            used_object=director_player(&p,campaign_room_id(),down&KEY_A);
            if(used_object && interaction==MOTION_NONE)interaction=MOTION_USE;
            for(unsigned i=1;i<director.count;i++) {
                const EntityDef *d=director.def+i;EntityState *e=director.state+i;
                if((d->flags&4) && e->room==campaign_room_id() && e->dead && e->enabled && !e->loot_collected && !combat_has_weapon_drops() &&
                   room_near(&p,e->position,1.f)) {
                    int weapon=find_gun(director_text(e->weapon));
                    if(weapon>=0) {
                        bool owned=inventory.stock[weapon].owned;
                        pickup(guns[weapon].id);
                        if(owned) {Stock *pool=ammo_pool(weapon);pool->reserve=fminf(guns[weapon].pocket,pool->reserve+guns[weapon].capacity);}
                    }
                    e->loot_collected=true;
                }
                styles[i].pickup_retry=fmaxf(0,styles[i].pickup_retry-dt);
                if((d->flags&2) && e->enabled && e->room==campaign_room_id() &&
                   hypotf(p.position.x-e->position.x,p.position.z-e->position.z)<d->radius+.15f && fabsf(p.position.y-e->position.y)<1.8f) {
                    const char *model=director_text(d->model);int item=find_gun(model);
                    bool pills=!strcmp(model,"medipack_small");
                    const char *key=!strncmp(model,"ammo_",5)?model+5:model;
                    int type=find_gun(!strcmp(key,"shotgun")?"pumpshotgun":key);
                    bool full=type>=0 && (item<0 || inventory.stock[item].owned) && ammo_pool(type)->reserve>=guns[type].pocket;
                    if((pills && inventory.pills>=8) || full){
                        if(styles[i].pickup_retry<=0){
                            const char *caption=type<0?"":guns[pool_id(type)].id;
                            if(type==PUMP || type==SAWED)caption="shotgun";
                            if(type==COMMANDO)caption="coltcommando";
                            char label[80];
                            if(pills)snprintf(label,sizeof label,"hudprint_pickup_painkillers_full");
                            else snprintf(label,sizeof label,"hudprint_pickup_%s_ammo_full",caption);
                            localized(label,journal_state.caption,sizeof journal_state.caption);
                            bank_sound(0,"level_items","pickup_ammo_full");journal_state.always=true;
                            journal_state.seconds=5;styles[i].pickup_retry=5;hud_dirty=true;
                        }
                        continue;
                    }
                    director_event(i,item>=0 && !inventory.stock[item].owned?EV_FIRST_PICKUP:EV_PICKUP,"");e->enabled=false;
                }
            }
        }
        if(!player_input) {
            pending_jump=0;
            Player scripted={.position=player->position,.yaw=player->yaw};
            director_player(&scripted,player->room,false);
        }
        PlatformContact support={.prop=-1};
        unsigned previous_room=player->room;
        if(!jump.airborne)
            support=player->room==campaign_room_id()?campaign_standing_on(player->position):campaign_remote_floor(player->room,player->position);
        Point before=player->position;
        for(unsigned i=1;i<director.count;i++) {
            EntityState *e=director.state+i;platforms[i].prop=-1;
            if((director.def[i].flags&4) && e->enabled && !e->dead)
                platforms[i]=e->room==campaign_room_id()?campaign_standing_on(e->position):campaign_remote_floor(e->room,e->position);
        }
        bool hud_before=presentation_value(presentation.visibility)>=.5f;
        presentation_tick(real);director_camera_tick(real);
        if(hud_before!=(presentation_value(presentation.visibility)>=.5f))hud_dirty=true;
        async_log_watch("scripts");director_tick(dt);
        if(previous_room==player->room && before.x==player->position.x && before.y==player->position.y && before.z==player->position.z){
            campaign_ride(support,&player->position);
            if(player_input && player->room==campaign_room_id() &&
               (before.x!=player->position.x || before.y!=player->position.y || before.z!=player->position.z)){
                Player carried=p;carried.position=player->position;carried.yaw=player->yaw;
                Point travel={carried.position.x-before.x,carried.position.y-before.y,carried.position.z-before.z};
                int crossing=campaign_crossing(&carried,before,travel,director.exits);
                if(crossing==CAMPAIGN_CROSS_BLOCKED)player->position=before;
                else if(crossing>=0){
                    if(!(ok=transfer_player(&carried,&jump,&camera,&fall_reference,&physics_room,crossing,!jump.airborne,&previous)))break;
                    director_player(&carried,campaign_room_id(),false);
                }
            }
        }
        for(unsigned i=1;i<director.count;i++) {
            EntityState *e=director.state+i;
            Point before_ride=e->position;
            campaign_ride(platforms[i],&e->position);
            paths[i].riding=fabsf(before_ride.y-e->position.y)+fabsf(before_ride.x-e->position.x)+fabsf(before_ride.z-e->position.z)>.0001f;
        }
        if(director.collision_dirty){if(!(ok=campaign_collision())) break;director.collision_dirty=false;}
        if(player->dead || director_view_room()!=campaign_room_id()) continue;
        p.position=player->position;p.yaw=player->yaw;
        float before_combat=player->hp;
        async_log_watch("combat");simulate_actors(&p,dt);
        for(unsigned i=1;i<director.count;i++){
            if(!(director.def[i].flags&4) || !director.state[i].enabled)continue;
            retail_motion(CAST_SLOTS+1,i,&actors[i].animation,&actors[i].body,find_gun(director_text(director.state[i].weapon)),dt,styles[i].forward_travel,styles[i].side_travel,MOTION_NONE);
        }
        director_character_contacts();
        for(unsigned i=0;i<director.count;i++)if(visuals[i].default_weapon)visuals[i].seconds+=dt;
        particles_tick(dt);combat_tick(dt,&fx);tick_casings(dt);
        if(player->dead)continue;
        if(player->hp<before_combat){
            since_hit=0;hud_dirty=true;
            if(ui_settings.slow_assist && director.controls && director.player_slow && player->hp>0 &&
               player->hp<maximum_health()*.25f && inventory.bullet_time>.15f)inventory.slow=true;
        } else if(director.controls && !player->dead){
            since_hit+=real;
            float floor=maximum_health()*tuning->regenerate;
            if(since_hit>3 && player->hp<floor){director_set_health(0,fminf(floor,player->hp+real));hud_dirty=true;}
        }
        KillRewards rewards=combat_take_rewards();
        if(rewards.timed && game_run.mode==NEW_YORK_MINUTE)game_run.remaining+=game_kill_bonus()*rewards.timed;
        if(rewards.health>0)director_set_health(0,fminf(maximum_health(),player->hp+rewards.health));
        if(rewards.slow>0){inventory.bullet_time=fminf(1,inventory.bullet_time+rewards.slow*tuning->bullet_gain);hud_dirty=true;}
        for(unsigned i=0;i<PROJECTILE_LIMIT;i++) {
            Projectile *drop=campaign_projectiles+i;bool collect=false;const char *name=combat_mesh(drop,&collect);
            if(collect && name && drop->room==player->room && director.controls && room_near(&p,drop->position,.9f)) {
                if(collect_drop(drop,name))drop->active=false;
            }
        }
        if(previous_gun!=inventory.equipped)actor_action(&anim,IDLE);
        Clip previous_action=anim.action;bool was_loading=previous_action==RELOAD;
        int gun=inventory.equipped;
        if(was_loading && gun==PUMP && inventory.stock[gun].rounds && player_input && (down&KEY_R)){
            anim.action=previous_action=IDLE;
        }
        if(gun>=0) {
            anim.weapon=guns[gun].pose;
            if(anim.weapon==RIFLE && !actor_rifle()){ok=false;break;}
        }
        u64 prepare_start=svcGetSystemTick();
        async_log_watch("prepare-models");
        Clip override=inventory.dodge_time>0?inventory.dodge:(jump.airborne || pending_jump)?JUMP:IDLE;
        anim.scripted=player->custom_pose;anim.script_time=player->pose_seconds;
        configure_motion(styles,0,inventory.equipped);
        if(player_input && (styles[0].drawing || styles[0].holstering))anim.scripted=0;
        int contact=actor_locomotion_style(&anim,forward_travel,side_travel,p.crouched,override,dt,
            styles[0].gait[WALK] && pose_clip_id(styles[0].gait[WALK])==pose_clip_id(styles[0].gait[RUN]));
        const FireRate *fire_rate=styles[0].fire_rate;
        float fire_dt=weapon_time_step(dt,real*director.speed,fire_rate?fire_rate->slow_multiplier:0);
        if(attacking(previous_action))anim.action_time+=fire_dt-dt;
        finish_weapon_step(0,&anim,previous_action);
        scene_age(&fx,dt);
        float fire_late=weapon_cooldown_step(&cooldown,fire_dt);
        bool death_view=death_camera();
        bool gameplay_camera=director.controls && director.path.path==DIRECTOR_NONE && !player->dead;
        if(gameplay_camera && gun==SNIPER && gun==previous_gun && (down&KEY_A) && !used_object){
            interaction=MOTION_SCOPE;inventory.zoom=inventory.zoom==4?1:inventory.zoom*2;hud_dirty=true;
        }
        unsigned view_zoom=gameplay_camera && gun==SNIPER?inventory.zoom:1;
        if(death_view && director.skip_death_view){
            scene_camera_sweep(&camera,&p,0,camera_obstacle);
        }else if(death_view){
            camera=(Camera){0};
        }else if(!director.controls && director.camera<director.count) {
            Point origin,forward,up;director_pose(director.camera,&origin,&forward,&up);
            Player view={.position=origin,.yaw=atan2f(forward.x,forward.z),
                         .pitch=atan2f(forward.y,hypotf(forward.x,forward.z))};
            scene_scope(&camera,&view);camera.eye=origin;
            if(hypotf(up.x,hypotf(up.y,up.z))>.01f) camera.up=up;
        } else if(view_zoom>1) scene_scope(&camera,&p);
        else {
            bool dodge_pose=(anim.gait>=DODGE_FORWARD && anim.gait<=DODGE_RIGHT) ||
                (styles[0].last_gait>=DODGE_FORWARD && styles[0].last_gait<=DODGE_RIGHT);
            bool getup_pose=styles[0].recovery && styles[0].transient && styles[0].transient!=styles[0].land;
            float target=gameplay_camera && (dodge_pose || getup_pose)?actor_camera_offset(&anim):0;
            camera_pose_offset+=(target-camera_pose_offset)*fminf(1,real*12);
            Player follow=p;follow.position.y+=camera_pose_offset;
            scene_camera_sweep(&camera,&follow,dt,camera_obstacle);
            if(gameplay_camera && focus_weight>0)
                scene_camera_converge(&camera,&p,focus_distance,focus_weight);
        }
        if(director.path.path!=DIRECTOR_NONE && !(death_view && director.skip_death_view)) {
            EntityState *owner=director.state+director.path.owner;
            Player anchor={.position=owner->position,.yaw=owner->yaw};
            camera_path_view(&camera,director.path.path,director.path.time,director.path.parented?&anchor:NULL);
        }
        Point subject={0};
        if(death_view && !director.skip_death_view){
            subject=death_camera_pivot();
            combat_camera_bodies(director.path.owner,campaign_room_id());
        }
        if(death_view && !director.skip_death_view && !scene_death_camera(&camera,subject,cinematic_obstacle)){
            director.skip_death_view=true;
            if(campaign_room_id()!=player->room)continue;
            scene_camera_sweep(&camera,&p,0,camera_obstacle);
        }
        if(!director.controls || death_view)bank_listener(director_view_room(),camera.eye,atan2f(camera.forward.x,camera.forward.z));
        Enemy render[CAST_SLOTS]={0};
        if(!(ok=local_cast(&p,&camera,death_view && !director.skip_death_view?&subject:NULL,render,&count) && prepare_models(count))) break;
        for(unsigned k=0;k<count;k++){
            MotionStyle *s=styles+actor_ids[k];
            actor_motion(k+1,s->render_gait,s->render_time,s->render_action,s->render_overlay,s->whole,s->gun<0,pose_bank_two_handed_bones(s->gun<0?"empty":guns[s->gun].id),s->exchange_weight,dt);
            actor_set_timeline(k+1,&s->timeline);
        aiming_motion(k+1,actor_ids[k],&actors[actor_ids[k]].body,NULL);
        }
        if(gun>=0 && gun==styles[0].gun && !styles[0].holstering && !styles[0].drawing) {
            Stock *stock=inventory.stock+gun;const Gun *g=guns+gun;
            if(was_loading && anim.action!=RELOAD && !pose_has_event(styles[0].reload,"c_reload"))refill_player(gun);
            bool fire=player_input && !death_view && (anim.action==IDLE || anim.action==FIRE) && ((down&KEY_R) || (g->automatic && (held&(KEY_R|KEY_ZR))));
            if(fire && ui_settings.autoswitch && g->capacity && !stock->rounds && !spare_rounds(gun)){
                const int best[]={JACKHAMMER,COMMANDO,DUAL_INGRAM,INGRAM,PUMP,SAWED,EAGLE,DUAL_PISTOL,PISTOL,BAT,PIPE};
                for(unsigned i=0;i<sizeof best/sizeof *best;i++){
                    int k=best[i];
                    if(inventory.stock[k].owned && (inventory.stock[k].rounds || spare_rounds(k) || !guns[k].capacity)){
                        inventory.equipped=k;inventory.zoom=1;hud_dirty=true;break;
                    }
                }
            }
            if(player_input && !death_view && ((down&KEY_X) || (fire && !stock->rounds)) && spare_rounds(gun) && stock->rounds<g->capacity && anim.action==IDLE) {
                eject_cases(0,gun,true,gun==SAWED?g->capacity-stock->rounds:1,1);
                visuals[0].default_weapon=false;actor_action(&anim,RELOAD);gun_sound(gun,true,0,p.position,styles[0].reload);hud_dirty=true;
            } else if(fire && (stock->rounds || !g->capacity) && !cooldown && anim.action!=RELOAD) {
                bool repeating=anim.action==FIRE;
                begin_attack(0,&anim,gun,repeating?fire_late:0);
                float interval=fire_rate?1.f/fire_rate->maximum:g->delay;
                anim.action_time=weapon_cooldown_start(&cooldown,interval,repeating?fire_late:0);
                hud_dirty=true;
            }
        }
        retail_motion(0,0,&anim,&p,gun,dt,forward_travel,side_travel,interaction);
        if(contact>=0 && director.controls && !pose_message_matches(styles[0].render_gait,"p_createprojectiletobone","dummy_footstep",NULL) &&
           !combat_footstep(contact?"dummy_footstep_right":"dummy_footstep_left",0,player->room,p.position,&fx)){
            Hit floor;const char *material=scene_ground(p.position,&floor)?campaign_surface(floor.face):"default";
            char name[64];footstep_name(name,contact!=0,material);
            bank_sound(0,"characters",name);
        }
        if(hud_dirty) hud(anim.action==RELOAD);
        u64 draw_start=svcGetSystemTick();
        room_render_campaign(&p,&anim,&camera,&fx,render,count,gun<0,view_zoom);
        if(log_file && director.controls && !player->dead){
            u64 end=svcGetSystemTick(),ticks=end-frame_start;
            unsigned room=campaign_room_id();
            if(!control_draw_logged || ticks>SYSCLOCK_ARM11/2){
                if(!control_draw_logged)fprintf(log_file,"LOAD first_control_frame stage=%s\n",stage);
                RoomTiming frame={.room=room,.frames=1,.worst_from=frame_room,.total=ticks,.worst=ticks,
                    .setup=now-frame_start,.simulation=prepare_start-now,.prepare=draw_start-prepare_start,
                    .draw=end-draw_start,.parts=setup,.render=room_draw_timing(),.over_budget=ticks>SYSCLOCK_ARM11/50};
                log_room_timing(stage,&frame);control_draw_logged=true;
            }
            if(timing.room!=room || timing.frames==300){
                log_room_timing(stage,&timing);
                timing=(RoomTiming){.room=room};
                previous+=svcGetSystemTick()-end;
            }
            timing.frames++;timing.total+=ticks;
            if(ticks>SYSCLOCK_ARM11/50)timing.over_budget++;
            if(ticks>timing.worst){
                timing.worst=ticks;timing.worst_from=frame_room;
                timing.setup=now-frame_start;timing.simulation=prepare_start-now;
                timing.prepare=draw_start-prepare_start;timing.draw=end-draw_start;
                timing.parts=setup;
                timing.render=room_draw_timing();
            }
        }
        if(!frames++ && log_file){fprintf(log_file,"CAMPAIGN draw %s actors=%u hp=%.1f\n",campaign_room_name(),count,player->hp);fflush(log_file);}
    }
done:
    async_log_watch(NULL);
    gyro_prepare(false);
    finish_autosave();async_save_close();log_room_timing(stage,&timing);
    if(ok && director.count) health_fraction=fmaxf(0,director.state[0].hp)/maximum_health();
    if(!ok && log_file){fprintf(log_file,"CAMPAIGN load failed %s room=%u\n",stage,campaign_room_id());fflush(log_file);}
    motion_effects=NULL;motion_player=NULL;motion_camera=NULL;motion_player_animation=NULL;motion_jump=NULL;bank_clear();bank_time_scale(1);bank_music_level(1);
    if(sound_ready) phone_close();
    sound_ready=false;free(npc_stock);npc_stock=NULL;free(arsenals);arsenals=NULL;free(styles);styles=NULL;free(visuals);visuals=NULL;free(actors);actors=NULL;free(paths);paths=NULL;free(platforms);platforms=NULL;
    combat_close();campaign_nav_close();director_close();room_close();localization_close();return ok;
}
bool campaign_play(const char *stage,int load_slot,UiChoice *next,FILE *journal) {
    game_run=(GameRun){.mode=load_slot<0 && strcmp(stage,"tutorial") && strcmp(stage,"end_combat")?selected_mode:FUGITIVE};
    if(load_slot<0)retry_deaths=0;
    log_file=journal;journal_state=(Journal){0};inventory=(Inventory){.equipped=-1,.zoom=1,.bullet_time=1};health_fraction=1;
    char current[64],following[64];
    SaveHeader header={0};Checkpoint *saved=NULL;
    if(load_slot>=0) {
        saved=save_read(load_slot,&header);
        if(!saved || header.bytes<sizeof(Checkpoint)){free(saved);return false;}
        stage=header.stage;
    }
    snprintf(current,sizeof current,"%s",stage);
    while(current[0] && aptMainLoop()) {
        journal_state.caption[0]=0;journal_state.seconds=0;
        following[0]=0;memset(current_objective,0,sizeof current_objective);
        Inventory initial=inventory;float initial_health=health_fraction;
        bool ok=run_map(current,next,following,&header,saved);free(saved);saved=NULL;
        if(!ok) return false;
        if(!strcmp(following,current)) {inventory=initial;health_fraction=initial_health;}
        snprintf(current,sizeof current,"%s",following);
    }
    free(saved);return true;
}
