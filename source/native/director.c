#include "campaign_file.h"
#include "director.h"
#include "save.h"
#include "pose_bank.h"
#include "camera_path.h"
#include "campaign_world.h"
#include "campaign_bounds.h"
#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
static bool (*collision_relevant)(unsigned prop);
void director_collision_relevance(bool (*relevant)(unsigned prop)){collision_relevant=relevant;}

typedef struct {uint16_t type,key,condition,count;uint32_t first;} Event;
typedef struct {uint16_t name,reserved;float seconds,start[12],end[12];} AnimationClip;
typedef struct {uint16_t target,op,count,args[7];} Message;
typedef struct {uint16_t parent,reserved;Point forward,up;} Transform;
typedef struct {uint32_t first;uint16_t count,reserved;} Track;
typedef struct {float v[12];} Affine;
typedef struct {Affine local;uint16_t parent,child,sibling,entity;bool active,blocked,panel,invalid_contact;unsigned stamp;} PropMotion;
static const Affine identity={{1,0,0,0,1,0,0,0,1,0,0,0}};
static PropMotion motion[CAMPAIGN_PROPS];
typedef struct {char magic[4];uint32_t entities,events,clips,messages,strings,bytes;} ScriptHeader;
typedef struct {
    ScriptHeader h;
    EntityDef *def;
    Event *events;
    AnimationClip *clips;
    Message *messages;
    uint32_t *offsets;
    char *strings;
    uint16_t *by_name,*text_slots;unsigned text_capacity;
    uint16_t *characters,*contact_triggers,*use_triggers,*pose_started;
    uint32_t mesh_contacts[CAMPAIGN_PROPS/32];
    uint8_t *contacts;
    unsigned character_count,trigger_count,use_count;
    uint16_t activator,activator_name,pose_key[16];
    Transform *transform;Track *tracks;Affine *frames;
    unsigned nframes,stamp;uint32_t signature,legacy_delta;
    unsigned depth,delivered;
    DirectorAction external;
    FILE *journal;
} Program;
_Static_assert(sizeof(EntityDef)==48 && sizeof(Event)==12 && sizeof(AnimationClip)==104 && sizeof(Message)==20,"Script record layout");
static Program prog;
Director director;
const char *director_text(unsigned id) {return id<prog.h.strings?prog.strings+prog.offsets[id]:"";}
static unsigned string_slot(const char *name){
    unsigned hash=2166136261u;
    for(const unsigned char *p=(const unsigned char*)name;*p;p++)hash=(hash^*p)*16777619u;
    unsigned slot=hash&(prog.text_capacity-1);
    while(prog.text_slots[slot]!=DIRECTOR_NONE && strcmp(director_text(prog.text_slots[slot]),name))
        slot=(slot+1)&(prog.text_capacity-1);
    return slot;
}
static uint16_t text_id(const char *name){
    return prog.text_slots?prog.text_slots[string_slot(name)]:DIRECTOR_NONE;
}
bool director_has_text(const char *name){return text_id(name)!=DIRECTOR_NONE;}

static bool is_moving_panel(const EntityDef *entity){
    const char *path=director_text(entity->name),*leaf=strrchr(path,':');
    leaf=leaf?leaf+1:path;
    if(strstr(leaf,"door") || strstr(leaf,"drawer") || strstr(leaf,"cupboard") ||
       strstr(leaf,"cabinet") || strstr(leaf,"locker"))return true;
    for(unsigned i=entity->first_clip;i<entity->first_clip+entity->clips;i++){
        const char *clip=director_text(prog.clips[i].name);
        if(!strncmp(clip,"open",4) || !strncmp(clip,"close",5))return true;
    }
    return false;
}

bool director_camera_overlay(unsigned prop){
    if(prop>=CAMPAIGN_PROPS || director.camera>=director.count)return false;
    unsigned id=motion[prop].entity;
    if(id>=director.count)return false;
    const char *path=director_text(prog.def[id].name),*leaf=strrchr(path,':');
    leaf=leaf?leaf+1:path;
    if(strcmp(leaf,"d_lines") && strcmp(leaf,"d_screen"))return false;
    unsigned camera=prog.def[director.camera].prop;
    for(unsigned parent=motion[prop].parent,depth=0;parent<CAMPAIGN_PROPS && depth<16;depth++){
        if(parent==camera)return true;
        parent=motion[parent].parent;
    }
    return false;
}
float director_prop_travel(void){
    float max_sq=0;
    for(unsigned i=0;i<prog.h.clips;i++){
        const AnimationClip *c=prog.clips+i;
        for(unsigned end=0;end<2;end++){
            const float *t=(end?c->end:c->start)+9;
            max_sq=fmaxf(max_sq,t[0]*t[0]+t[1]*t[1]+t[2]*t[2]);
        }
    }
    return sqrtf(max_sq);
}
void director_close(void) {
    pose_bank_close();camera_bank_close();
    free(prog.def);free(prog.events);free(prog.clips);free(prog.messages);free(prog.offsets);free(prog.strings);
    free(prog.text_slots);free(prog.by_name);free(director.state);free(director.requires_combat_target);
    free(prog.characters);free(prog.contact_triggers);free(prog.contacts);free(prog.use_triggers);free(prog.pose_started);
    free(prog.transform);free(prog.tracks);free(prog.frames);
    director=(Director){0};prog=(Program){0};
}
static bool records(FILE *f,void **data,size_t size,unsigned count) {
    *data=calloc(count?count:1,size);
    return *data && fread(*data,size,count,f)==count;
}
static bool prop_event(const EntityDef *d,unsigned key) {
    if(d->prop==DIRECTOR_NONE) return false;
    for(unsigned i=d->first_event;i<d->first_event+d->events;i++)
        if(prog.events[i].type==EV_ENTITY && prog.events[i].key==key && prog.events[i].count) return true;
    return false;
}
static bool needs_scripted_opponent(const EntityDef *d){
    const unsigned pickup=text_id("c_pickupweapon"),equip=text_id("c_removeallweapons");
    const unsigned machine=text_id("c_setstatemachine");
    unsigned initial_weapon=DIRECTOR_NONE,initial_ai=DIRECTOR_NONE;
    bool arms=false,fights=false;
    for(unsigned i=d->first_event;i<d->first_event+d->events;i++){
        const Event *event=prog.events+i;
        if(event->type!=EV_START && event->type!=EV_ACTIVATE)continue;
        for(unsigned j=event->first;j<event->first+event->count;j++){
            const Message *m=prog.messages+j;
            if(m->target!=d->name || !m->count)continue;
            if(event->type==EV_START){
                if(m->op==pickup || m->op==equip)initial_weapon=m->args[0];
                if(m->op==machine)initial_ai=m->args[0];
            }else if(m->op==pickup || m->op==equip){
                const char *weapon=director_text(m->args[0]);
                arms=*weapon && strcmp(weapon,"empty");
            }else if(m->op==machine){
                const char *ai=director_text(m->args[0]);
                if(strncmp(ai,"delay",5) && strncmp(ai,"single",6))
                    fights=strstr(ai,"combat") || strstr(ai,"shoot");
            }
        }
    }
    return initial_ai==text_id("nonreactive") && initial_weapon==text_id("empty") && arms && fights;
}
static bool find_scripted_combatants(void){
    unsigned op=text_id("c_settargetcharacter");
    if(op==DIRECTOR_NONE)return true;
    director.requires_combat_target=calloc(director.count,sizeof(bool));
    if(!director.requires_combat_target)return false;
    for(unsigned i=0;i<prog.h.messages;i++){
        const Message *m=prog.messages+i;
        if(m->op!=op || !m->count)continue;
        unsigned id=prog.by_name[m->target];
        if(id && id<director.count)director.requires_combat_target[id]=true;
    }
    // These characters start dormant and unarmed. A separate script selects
    // their opponent before the activation handler equips them for combat.
    for(unsigned id=1;id<director.count;id++){
        if(director.requires_combat_target[id])
            director.requires_combat_target[id]=(director.def[id].flags&4) && needs_scripted_opponent(director.def+id);
    }
    return true;
}
static uint32_t stage_signature(void);
bool director_open(const char *stage,DirectorAction action,FILE *journal) {
    director_close();
    if(!stage || strlen(stage)>31 || strspn(stage,"abcdefghijklmnopqrstuvwxyz0123456789_-")!=strlen(stage)) return false;
    char file[96];snprintf(file,sizeof file,"%s.evt",stage);
    FILE *f=campaign_file(file);if(!f) return false;
    ScriptHeader *h=&prog.h;
    bool ok=fread(h,sizeof(*h),1,f)==1 && !memcmp(h->magic,"EVT1",4) && h->entities && h->entities<2048 &&
        h->events<16384 && h->clips<8192 && h->messages<32768 && h->strings && h->strings<16384 && h->bytes<512*1024 &&
        records(f,(void**)&prog.def,sizeof(EntityDef),h->entities) && records(f,(void**)&prog.events,sizeof(Event),h->events) &&
        records(f,(void**)&prog.clips,sizeof(AnimationClip),h->clips) && records(f,(void**)&prog.messages,sizeof(Message),h->messages) &&
        records(f,(void**)&prog.offsets,4,h->strings) && records(f,(void**)&prog.strings,1,h->bytes);
    char extension[4];
    size_t bytes=ok?fread(extension,1,4,f):0;
    if(bytes) {
        ok=bytes==4 && !memcmp(extension,"XFR1",4) && fread(&prog.nframes,4,1,f)==1 && prog.nframes<=65536 &&
            records(f,(void**)&prog.transform,sizeof(Transform),h->entities) && records(f,(void**)&prog.tracks,sizeof(Track),h->clips) &&
            records(f,(void**)&prog.frames,sizeof(Affine),prog.nframes);
        if(ok){
            bytes=fread(extension,1,4,f);
            if(bytes)ok=bytes==4 && !memcmp(extension,"SCV1",4) &&
                fread(&prog.legacy_delta,4,1,f)==1 && fgetc(f)==EOF;
        }
        for(unsigned i=0;ok && i<h->clips;i++) {
            Track track=prog.tracks[i];
            ok=track.first<=prog.nframes && track.count<=prog.nframes-track.first && track.count!=1;
        }
        for(unsigned i=0;ok && i<h->entities;i++) {
            uint16_t parent=prog.transform[i].parent;
            unsigned hops=0;
            while(parent<h->entities && prog.def[parent].prop==DIRECTOR_NONE && hops++<h->entities)
                parent=prog.transform[parent].parent;
            ok=parent==DIRECTOR_NONE || (parent<h->entities && parent!=i && prog.def[parent].prop<CAMPAIGN_PROPS && hops<=h->entities);
            if(ok) prog.transform[i].parent=parent;
        }
    }
    fclose(f);
    if(!ok || !h->bytes || prog.strings[h->bytes-1]) goto fail;
    for(unsigned i=0;i<h->strings;i++) if(prog.offsets[i]>=h->bytes) goto fail;
    for(unsigned i=0;i<h->events;i++) {
        Event e=prog.events[i];
        if(e.type>EV_CAMERA || e.key>=h->strings || (e.type<EV_POSE && e.condition!=DIRECTOR_NONE && e.condition>=h->strings) ||
           e.first>h->messages || e.count>h->messages-e.first) goto fail;
    }
    for(unsigned i=0;i<h->messages;i++){
        Message *m=prog.messages+i;
        if(m->op>=h->strings || m->target>=h->strings || m->count>7) goto fail;
        for(unsigned j=0;j<m->count;j++) if(m->args[j]>=h->strings) goto fail;
    }
    director.state=calloc(h->entities,sizeof(EntityState));prog.by_name=malloc(h->strings*2);
    prog.pose_started=calloc(h->entities,sizeof(*prog.pose_started));
    if(!director.state || !prog.by_name || !prog.pose_started) goto fail;
    memset(prog.by_name,0xff,h->strings*2);
    prog.text_capacity=1;while(prog.text_capacity<2*h->strings)prog.text_capacity*=2;
    prog.text_slots=malloc(prog.text_capacity*sizeof(*prog.text_slots));if(!prog.text_slots)goto fail;
    memset(prog.text_slots,0xff,prog.text_capacity*sizeof(*prog.text_slots));
    for(unsigned i=0;i<h->strings;i++)prog.text_slots[string_slot(director_text(i))]=i;
    unsigned contact_key=text_id("do_charactercollides");
    unsigned invalid_move_key=text_id("do_movedtoinvalidposition");
    for(unsigned i=0;i<h->entities;i++) {
        EntityDef d=prog.def[i];
        if(d.name>=h->strings || d.model>=h->strings || d.state>=h->strings ||
           d.first_event+d.events>h->events || d.first_clip+d.clips>h->clips ||
           (d.prop!=DIRECTOR_NONE && d.prop>=CAMPAIGN_PROPS) || (d.exit!=DIRECTOR_NONE && d.exit>=DIRECTOR_EXITS) ||
           (d.pair!=DIRECTOR_NONE && d.pair>=DIRECTOR_EXITS)) goto fail;
        prog.by_name[d.name]=i;
        director.state[i]=(EntityState){.position=d.position,.room=d.room,.state=d.state,.yaw=d.yaw,.clip=DIRECTOR_NONE,
            .target=DIRECTOR_NONE,.aim=DIRECTOR_NONE,.pace=1,.pose_clip=DIRECTOR_NONE,.enabled=true,.hp=d.max_health?d.max_health:100};
        if(d.prop!=DIRECTOR_NONE) {
            uint16_t parent=prog.transform?prog.transform[i].parent:DIRECTOR_NONE;
            motion[d.prop]=(PropMotion){.local=identity,.entity=i,.panel=is_moving_panel(&d),
                .invalid_contact=prop_event(&d,invalid_move_key),.parent=parent==DIRECTOR_NONE?DIRECTOR_NONE:prog.def[parent].prop,
                .child=DIRECTOR_NONE,.sibling=DIRECTOR_NONE};
        }
        if(d.flags&4) prog.character_count++;
        if(d.trigger==0)prog.use_count++;
        if(d.trigger==3 || prop_event(&d,contact_key)) prog.trigger_count++;
        if(d.prop!=DIRECTOR_NONE) {
            campaign_props[d.prop].solid=(d.flags&(128|256))!=0;
            campaign_props[d.prop].collision_mask=d.flags&512?(d.flags>>7)&3:3;
        }
        if(d.exit!=DIRECTOR_NONE) director.exits[d.exit]=director.ai_exits[d.exit]=true;
    }
    if(strcmp(director_text(prog.def[0].name),"player")) goto fail;
    for(unsigned i=0;i<h->entities;i++){
        unsigned prop=prog.def[i].prop;
        if(prop==DIRECTOR_NONE || motion[prop].parent==DIRECTOR_NONE)continue;
        PropMotion *parent=motion+motion[prop].parent;
        motion[prop].sibling=parent->child;parent->child=prop;
    }
    director.def=prog.def;director.count=h->entities;director.controls=director.player_slow=true;director.speed=1;director.camera=DIRECTOR_NONE;
    prog.characters=calloc(prog.character_count+1,sizeof(*prog.characters));
    prog.contact_triggers=calloc(prog.trigger_count+1,sizeof(*prog.contact_triggers));
    prog.contacts=calloc((prog.character_count*prog.trigger_count+7)/8+1,1);
    prog.use_triggers=calloc(prog.use_count+1,sizeof(*prog.use_triggers));
    if(!prog.use_triggers || !prog.characters || !prog.contact_triggers || !prog.contacts) goto fail;
    unsigned c=0,t=0,u=0;
    for(unsigned i=0;i<h->entities;i++) {
        if(prog.def[i].flags&4) prog.characters[c++]=i;
        if(prog.def[i].trigger==0)prog.use_triggers[u++]=i;
        if(prog.def[i].trigger==3 || prop_event(prog.def+i,contact_key)) {
            prog.contact_triggers[t++]=i;
            unsigned prop=prog.def[i].prop;
            if(prop<CAMPAIGN_PROPS)prog.mesh_contacts[prop/32]|=1u<<(prop%32);
        }
    }
    for(unsigned i=0;i<h->strings;i++){
        if(prog.by_name[i]!=DIRECTOR_NONE)continue;
        char *name=prog.strings+prog.offsets[i],*ext=strrchr(name,'.');
        if(!ext || (strcmp(ext,".do") && strcmp(ext,".trigger")))continue;
        // Look up the untyped base without mutating the indexed dictionary.
        size_t len=ext-name;
        for(unsigned j=0;j<h->entities;j++){
            const char *base=director_text(prog.def[j].name);
            if(strlen(base)==len && !memcmp(name,base,len)){prog.by_name[i]=j;break;}
        }
    }
    director.path.path=DIRECTOR_NONE;
    if(!find_scripted_combatants() || !pose_bank_open() || !camera_bank_open())goto fail;
    for(unsigned j=0;j<16;j++){char key[16];snprintf(key,sizeof key,"pose%u",j);prog.pose_key[j]=text_id(key);}
    prog.activator_name=text_id("activator");prog.activator=0;
    prog.signature=stage_signature();
    prog.journal=journal;prog.external=action;return true;
fail:director_close();return false;
}
static bool truth(const char *s) {return !strcmp(s,"true") || atoi(s)!=0;}
static uint16_t lookup(uint16_t id) {
    if(id>=prog.h.strings) return DIRECTOR_NONE;
    if(id==prog.activator_name) return prog.activator;
    return prog.by_name[id];
}
unsigned director_find(const char *name){return lookup(text_id(name));}
void director_transfer_weapon(const char *actor,const char *weapon) {
    unsigned character=lookup(text_id(actor)),gun=text_id(weapon);
    if(character<director.count && (prog.def[character].flags&4) && gun!=DIRECTOR_NONE)director.state[character].weapon=gun;
}
static void dispatch(unsigned id,enum EventType type,uint16_t key);
void director_activate_character(unsigned id){
    EntityState *e=director.state+id;
    if(!e->activated && !e->dead && e->enabled && (director.def[id].flags&4) && !director_needs_target(id)){
        e->activated=true;dispatch(id,EV_ACTIVATE,0);
    }
}
static void select_pose(unsigned id,bool restart) {
    EntityState *e=director.state+id;
    if(e->dead)return;
    if(!e->idle){e->custom_pose=0;prog.pose_started[id]=0;return;}
    const char *weapon=director_text(e->weapon);
    bool heavy=pose_bank_two_handed(weapon);
    unsigned variant=e->idle-1+(e->wounded || e->force_wounded?4:0)+(heavy?8:0);
    unsigned clip=pose_bank_find(director_text(prog.def[id].model),variant);
    if(restart || e->custom_pose!=clip){e->pose_seconds=0;prog.pose_started[id]=0;}
    e->custom_pose=clip;e->pose_variant=variant;
}
void director_set_health(unsigned id,float hp){
    EntityState *e=director.state+id;
    float before=e->hp;e->hp=hp;
    const EntityDef *d=director.def+id;
    if(hp<=0 || (!id && !d->max_health) || (id && !(d->flags&4)))return;
    const SkinRules *rules=pose_skin_rules(pose_bank_skin(director_text(d->model)));
    if(!rules)return;
    float threshold=rules->wounded*(d->max_health?d->max_health:100);
    if((before>threshold && hp<=threshold) || (before<=threshold && hp>threshold)){
        e->wounded=hp<=threshold;
        if(e->idle)select_pose(id,false);
    }
}
static unsigned death_clip(unsigned id,int variant){
    if(variant!=1 && variant!=2)return 0;
    return pose_bank_action(pose_bank_skin(director_text(prog.def[id].model)),variant==1?"customdeath1":"customdeath2");
}
void director_restore_corpses(void){
    for(unsigned i=0;i<director.count;i++){
        const EntityDef *d=prog.def+i;
        for(unsigned j=d->first_event;j<d->first_event+d->events;j++){
            Event e=prog.events[j];if(e.type!=EV_START)continue;
            for(unsigned k=0;k<e.count;k++){
                const Message *m=prog.messages+e.first+k;unsigned target=lookup(m->target);
                if(target>=director.count || !director.state[target].dead || strcmp(director_text(m->op),"c_kill"))continue;
                EntityState *s=director.state+target;
                unsigned clip=death_clip(target,atoi(director_text(m->args[0])));
                if(clip && clip!=s->custom_pose){s->idle=0;s->custom_pose=clip;s->pose_seconds=pose_bank_duration(clip);}
            }
        }
    }
}
bool director_pose_started(unsigned id){
    return id<director.count && director.state[id].custom_pose && prog.pose_started[id]==director.state[id].custom_pose;
}
void director_resume_pose(unsigned id,bool started){
    if(id<director.count)prog.pose_started[id]=started?director.state[id].custom_pose:0;
}
static void camera_events(uint16_t key,float previous,float frame,bool exiting);
static void execute(const Message *m,unsigned owner) {
#ifdef MP_AUDIT_MESSAGE
    MP_AUDIT_MESSAGE(m-prog.messages);
#endif
    const char *op=director_text(m->op),*args[7]={"","","","","","",""};
    for(unsigned j=0;j<m->count;j++) args[j]=director_text(m->args[j]);
    uint16_t target=lookup(m->target),arg0=m->args[0];
    const char *arg=args[0];bool on=truth(arg);
    float amount=strtof(arg,NULL);
    if(!strcmp(op,"gm_enableexit") || !strcmp(op,"gm_enableexitaiactivation")) {
        bool *enabled=!strcmp(op,"gm_enableexit")?director.exits:director.ai_exits;
        unsigned ent=lookup(arg0);
        if(ent<director.count && prog.def[ent].exit!=DIRECTOR_NONE) {
            enabled[prog.def[ent].exit]=truth(args[1]);
            if(prog.def[ent].pair!=DIRECTOR_NONE) enabled[prog.def[ent].pair]=truth(args[1]);
        }
    } else if(!strcmp(op,"gm_init")) director.next_level=arg0;
    else if(!strcmp(op,"s_loadlastsavedgame")) director.reload=true;
    else if(!strcmp(op,"gm_sendendofgamemessages")) director.finished=true;
    else if(!strcmp(op,"gm_setplayercontrolledslowmotion")) director.player_slow=on;
    else if(!strcmp(op,"gm_setplayercontrols")) director.controls=on;
    else if(!strcmp(op,"gm_enablewidescreen")) director.widescreen=on;
    else if(!strcmp(op,"gm_changeglobalgamespeed")) {
        director.speed_target=fmaxf(.02f,fminf(amount,4));director.speed_remaining=fmaxf(0,strtof(args[1],NULL));
        if(!director.speed_remaining)director.speed=director.speed_target;
    }
    else if(!strcmp(op,"s_autosavegame")) director.autosave=true;
    else if(!strcmp(op,"mpgnm_markallnotesread")) director.note_count=0;
    else if(!strcmp(op,"mpgnm_pickupnote")) {
        if(director.note_count<DIRECTOR_NOTES) director.notes[director.note_count++]=arg0;
    } else if(!strcmp(op,"s_modeswitch") && !strcmp(arg,"graphicnovel")) director.novel=true;
    else if(!strcmp(op,"mphm_printobjective")) director.objective=arg0;
    else if(!strcmp(op,"cam_animateinplace") || !strcmp(op,"cam_animateparented")) {
        int path=camera_path_id(arg);
        if(path>=0) {
            if(director.path.path!=DIRECTOR_NONE)camera_events(director.path.key,0,0,true);
            director.skip_death_view=false;
            director.path=(CameraPath){.path=path,.key=arg0,.owner=target<director.count?target:0,
                .parented=!strcmp(op,"cam_animateparented")};
            camera_events(arg0,-1,0,false);
        }
    }
    else if(target<director.count) {
        EntityState *st=director.state+target;
        const EntityDef *d=prog.def+target;
        CampaignProp *prop=d->prop==DIRECTOR_NONE?NULL:&campaign_props[d->prop];
        if(!strcmp(op,"fsm_send")) dispatch(target,EV_SEND,arg0);
        else if(!strcmp(op,"fsm_switch")) {
            if(st->state!=arg0){st->state=arg0;dispatch(target,EV_SWITCH,arg0);}
        } else if(!strcmp(op,"fsm_randomize")) {
            unsigned n=0;uint16_t choice=DIRECTOR_NONE;
            for(unsigned i=d->first_event;i<d->first_event+d->events;i++) {
                Event e=prog.events[i];
                if(e.type==EV_SWITCH && !e.count && rand()%++n==0) choice=e.key;
            }
            if(choice!=DIRECTOR_NONE) {st->state=choice;dispatch(target,EV_SWITCH,choice);}
        } else if(!strcmp(op,"t_enable")) {st->enabled=on;if(!on) st->inside=false;}
        else if(!strcmp(op,"t_activate")) director_event(target,EV_ENTITY,"t_activate");
        else if(!strcmp(op,"do_hide") && prop) {
            if(prop->hidden!=on && d->room<128)director.acoustic_revision[d->room]++;
            prop->hidden=on;director.collision_dirty=true;
        }
        else if(!strcmp(op,"do_enablecollisions") && prop) {
            if(prop->solid!=on && d->room<128)director.acoustic_revision[d->room]++;
            prop->solid=on;director.collision_dirty=true;
        }
        else if(!strcmp(op,"do_animate") && prop) {
            for(unsigned i=d->first_clip;i<d->first_clip+d->clips;i++) if(prog.clips[i].name==arg0) {
                st->clip=st->pose_clip=i;st->time=0;st->reverse=st->paused=false;director.pause_remaining[d->prop]=0;
                dispatch(target,EV_LEAVE,arg0);break;
            }
        } else if(!strcmp(op,"do_stopanimation")) st->clip=DIRECTOR_NONE;
        else if(!strcmp(op,"do_invertanimation")) {st->reverse=!st->reverse;st->clip=st->pose_clip;}
        else if(!strcmp(op,"do_pauseanimation") && prop) {
            director.pause_remaining[d->prop]=fmaxf(0,amount);st->paused=amount>0;
        }
        else if(!strcmp(op,"do_resumeanimation") && prop) {director.pause_remaining[d->prop]=0;st->paused=false;}
        else if(!strcmp(op,"do_sethitpoints")) {st->hp=amount;st->dead=amount<=0;}
        else if(!strcmp(op,"c_sethealth")) {
            if(amount<=0) director_hit(target,st->hp);
            else {
                if(st->dead)st->loot_collected=false;
                st->dead=false;director_set_health(target,fminf(amount,1)*(d->max_health?d->max_health:100));
            }
        }
        else if(!strcmp(op,"c_setinvulnerable")) st->invulnerable=on;
        else if(!strcmp(op,"c_setimmortal")) st->immortal=on;
        else if(!strcmp(op,"c_enableprojectilecollisions")) st->pass_projectiles=!on;
        else if(!strcmp(op,"c_kill")) {
            if(!target)director.dodge_immortal=false;
            st->invulnerable=st->immortal=false;director_hit(target,st->hp+1);
            int variant=atoi(arg);
            if(st->dead && (variant==1 || variant==2)){
                unsigned pose=death_clip(target,variant);
                if(pose && pose!=st->custom_pose){st->custom_pose=pose;st->pose_seconds=0;}
            }
        }
        else if(!strcmp(op,"c_causedamage")) director_hit(target,amount);
        else if(!strcmp(op,"li_activatelevelitem")) st->enabled=on;
        else if(!strcmp(op,"li_removelevelitem")) st->enabled=false;
        else if(!strcmp(op,"c_teleport")) {
            unsigned dest=lookup(arg0);
            if(dest<director.count) {
                EntityState *to=director.state+dest;
                Point front;director_pose(dest,&st->position,&front,NULL);
                st->room=to->room;st->yaw=atan2f(front.x,front.z);
            }
        } else if(!strcmp(op,"c_pickupweapon") || !strcmp(op,"c_removeallweapons")) {st->weapon=arg0;select_pose(target,false);}
        else if(!strcmp(op,"c_setidle")) {
            unsigned idle=atoi(arg);st->idle=idle<=4?idle:0;st->force_wounded=strchr(arg,'w')!=NULL;select_pose(target,true);
        } else if(!strcmp(op,"c_setwoundedanimations")) {st->wounded=on;select_pose(target,false);}
        else if(!strcmp(op,"c_setinfiniteammo")) st->infinite_ammo=truth(args[1]);
        else if(!strcmp(op,"c_setstatemachine")) {
            if(strncmp(arg,"delay",5) && strncmp(arg,"single",6)){
                st->ai=arg0;st->hostile=strstr(arg,"combat") || strstr(arg,"shoot");
            }
        }
        else if(!strcmp(op,"c_gotoplayer")) {st->target=0;st->pace=amount>0?amount:1;}
        else if(!strcmp(op,"c_settargetcharacter")) st->aim=lookup(arg0);
        else if(!strcmp(op,"c_goto") || !strcmp(op,"c_gotoandshoot") || !strcmp(op,"c_patrol")) {
            st->target=lookup(arg0);st->pace=strtof(args[!strcmp(op,"c_patrol") && m->count>1?m->count-1:1],NULL);
            if(st->pace<=0 || st->pace>4) st->pace=1;
            if(strstr(op,"shoot")) st->hostile=true;
        } else if(!strcmp(op,"c_forceupdate")) {
            st->active_seconds=fmaxf(st->active_seconds,amount);
            if(amount>0)director_activate_character(target);
        } else if(!strcmp(op,"c_sendspecial")) dispatch(target,EV_SPECIAL,0);
        else if(!strcmp(op,"cam_attachtodo")) {
            if(amount>0) director.camera=target;
            else if(director.camera==target) director.camera=DIRECTOR_NONE;
        }
    }
    if(prog.external) prog.external(!strcmp(director_text(m->target),"righthandweapon")?owner:target,op,args,m->count);
}
static void dispatch(unsigned id,enum EventType type,uint16_t key) {
    if(id>=director.count || key==DIRECTOR_NONE || director.failed) return;
    if(!prog.depth) prog.delivered=0;
    if(prog.depth>=64 || ++prog.delivered>16384){
        director.failed=true;
        if(prog.journal) fprintf(prog.journal,"Script event cycle at %s\n",director_text(prog.def[id].name));
        return;
    }
    ++prog.depth;
    EntityDef *d=prog.def+id;
    uint16_t state=director.state[id].state;
    for(unsigned i=d->first_event;i<d->first_event+d->events;i++) {
        Event event=prog.events[i];
        if(event.type!=type || event.key!=key || (event.condition!=DIRECTOR_NONE && event.condition!=state)) continue;
        for(unsigned j=0;j<event.count && !director.failed;j++) execute(prog.messages+event.first+j,id);
    }
    --prog.depth;
}
void director_event(unsigned id,enum EventType type,const char *name) {dispatch(id,type,text_id(name));}
void director_preload_sounds(void (*preload)(const char *group,const char *name)){
    for(unsigned i=0;i<prog.h.messages;i++){
        const Message *m=prog.messages+i;const char *op=director_text(m->op);
        if(m->count<2 || (strcmp(op,"a_play3dsound") && strcmp(op,"a_playsound") && strcmp(op,"a_playfloating3dsound")))continue;
        const char *group=director_text(m->args[0]);
        if(!strcmp(group,"dynamic"))preload(group,director_text(m->args[1]));
    }
}
void director_start(void) {
    for(unsigned i=0;i<director.count;i++) {prog.activator=i;dispatch(i,EV_START,0);}
    prog.activator=0;
}
void director_hit(unsigned id,float damage) {
    if(id>=director.count) return;
    EntityState *e=director.state+id;
    if(e->dead || e->invulnerable) return;
    director_set_health(id,fmaxf((e->immortal || (!id && director.dodge_immortal))?.01f:0,e->hp-damage));
    if(e->hp>0)return;
    e->dead=true;e->idle=0;e->custom_pose=0;e->pose_seconds=0;
    if(prog.def[id].prop!=DIRECTOR_NONE) director_event(id,EV_ENTITY,"do_ondeath");
    if(prog.def[id].flags&4) dispatch(id,EV_DEATH,0);
}
static Point apply_transform(const float m[12],Point p,bool translate) {
    return (Point){m[0]*p.x+m[3]*p.y+m[6]*p.z+(translate?m[9]:0),
                   m[1]*p.x+m[4]*p.y+m[7]*p.z+(translate?m[10]:0),
                   m[2]*p.x+m[5]*p.y+m[8]*p.z+(translate?m[11]:0)};
}
static Affine compose_motion(const float parent[12],Affine local){
    for(unsigned i=0;i<4;i++){
        float *v=local.v+3*i;
        Point q=apply_transform(parent,(Point){v[0],v[1],v[2]},i==3);
        v[0]=q.x;v[1]=q.y;v[2]=q.z;
    }
    return local;
}
void director_pose(unsigned id,Point *position,Point *forward,Point *up) {
    const EntityState *e=director.state+id;
    Transform t=prog.transform?prog.transform[id]:(Transform){.parent=DIRECTOR_NONE,.forward={sinf(e->yaw),0,cosf(e->yaw)},.up={0,1,0}};
    uint16_t prop=prog.def[id].prop;
    if(prop==DIRECTOR_NONE && t.parent!=DIRECTOR_NONE) prop=prog.def[t.parent].prop;
    const float *m=prop!=DIRECTOR_NONE && campaign_props[prop].moved?campaign_props[prop].matrix:identity.v;
    if(position) *position=apply_transform(m,e->position,true);
    if(forward) *forward=apply_transform(m,t.forward,false);
    if(up) *up=apply_transform(m,t.up,false);
}
static void world_motion(unsigned prop,unsigned depth) {
    PropMotion *m=motion+prop;
    if(m->stamp==prog.stamp) return;
    if(depth>=CAMPAIGN_PROPS) {director.failed=true;return;}
    m->stamp=prog.stamp;
    const float *parent=NULL;
    if(m->parent!=DIRECTOR_NONE) {
        world_motion(m->parent,depth+1);
        if(campaign_props[m->parent].moved) parent=campaign_props[m->parent].matrix;
    }
    CampaignProp *p=campaign_props+prop;
    bool active=parent || m->active;
    if(!active && !p->moved) return;
    Affine world=parent?compose_motion(parent,m->local):m->local;
    if(p->solid) {
        const float *old=p->moved?p->matrix:identity.v;
        for(unsigned k=0;k<12;k++) if(fabsf(old[k]-world.v[k])>.0001f) {
            unsigned home=prog.def[m->entity].room;
            if(home<128)director.acoustic_revision[home]++;
            if(director.state[m->entity].room==campaign_room_id() || (collision_relevant && collision_relevant(prop)))
                director.collision_dirty=true;
            break;
        }
    }
    p->moved=active;memcpy(p->matrix,world.v,sizeof world.v);
}
static void camera_events(uint16_t key,float previous,float frame,bool exiting) {
    const EntityDef *player=prog.def;
    for(unsigned i=player->first_event;i<player->first_event+player->events;i++) {
        Event e=prog.events[i];if(e.type!=EV_CAMERA || e.key!=key)continue;
        bool due=e.condition==DIRECTOR_NONE?exiting:(!exiting && e.condition>previous && e.condition<=frame);
        if(due)for(unsigned j=0;j<e.count;j++)execute(prog.messages+e.first+j,0);
    }
}
void director_camera_tick(float dt) {
    if(director.speed_remaining>0){
        float t=fminf(dt,director.speed_remaining);
        director.speed+=(director.speed_target-director.speed)*(t/director.speed_remaining);
        director.speed_remaining-=t;
    }
    CameraPath *path=&director.path;if(path->path==DIRECTOR_NONE)return;
    float start=path->time,duration=camera_path_duration(path->path),rate=camera_path_rate(path->path);
    path->time=fminf(duration,start+dt);
    camera_events(path->key,start*rate,path->time*rate,false);
    if(path->time>=duration){uint16_t key=path->key;path->path=DIRECTOR_NONE;director.skip_death_view=false;camera_events(key,0,0,true);}
}
unsigned director_view_room(void) {
    if(director.state[0].dead || (director.skip_death_view && director.path.path!=DIRECTOR_NONE))return director.state[0].room;
    if(director.path.path!=DIRECTOR_NONE && director.path.parented)return director.state[director.path.owner].room;
    return director.state[!director.controls && director.camera<director.count?director.camera:0].room;
}
// A script can replace or restart its own idle in a frame message. Stop the
// outgoing interval immediately, including when the replacement uses the same KF2.
static bool pose_range(unsigned id,unsigned clip,float start,float end,bool inclusive){
    EntityState *s=director.state+id;
    const EntityDef *d=prog.def+id;
    for(unsigned j=d->first_event;j<d->first_event+d->events;j++){
        Event e=prog.events[j];
        if(e.type!=EV_POSE || e.key!=prog.pose_key[s->pose_variant])continue;
        float when=e.condition/pose_bank_rate(clip);
        if((when>start || (inclusive && when==start)) && when<=end){
            for(unsigned k=0;k<e.count;k++){
                execute(prog.messages+e.first+k,id);
                if(s->custom_pose!=clip || prog.pose_started[id]!=clip || s->pose_seconds!=end)return false;
            }
        }
    }
    return true;
}
static void pose_tick(unsigned id,float dt) {
    EntityState *s=director.state+id;
    unsigned clip=s->custom_pose;
    if(!clip)return;
    float seconds=pose_bank_duration(clip);
    if(seconds<=0)return;
    if(s->dead){s->pose_seconds=fminf(s->pose_seconds+dt,seconds);return;}
    float start=s->pose_seconds,remaining=fmaxf(0,dt);
    bool inclusive=prog.pose_started[id]!=clip && start==0;
    prog.pose_started[id]=clip;
    bool looping=pose_bank_loops(clip);
    for(;;){
        float end=fminf(seconds,start+remaining);
        s->pose_seconds=end;
        if(!pose_range(id,clip,start,end,inclusive) || !looping || end<seconds)return;
        remaining=fmaxf(0,remaining-(end-start));
        start=pose_bank_loop_start(clip);inclusive=true;
        s->pose_seconds=start;
        if(!remaining){pose_range(id,clip,start,start,true);return;}
    }
}
static DirectorContactFrame contact_frame;
static bool (*mesh_touch)(unsigned prop);
void director_mesh_contact(bool (*contact)(unsigned prop)){mesh_touch=contact;}
void director_contact_frame(DirectorContactFrame frame){contact_frame=frame;}
static DirectorMotionBlocked motion_blocked;
void director_motion_guard(DirectorMotionBlocked guard){motion_blocked=guard;}
static Affine clip_transform(unsigned clip_id,float t){
    const AnimationClip *clip=prog.clips+clip_id;
    Track track=prog.tracks?prog.tracks[clip_id]:(Track){0};
    const float *a=clip->start,*b=clip->end;
    float mix=t;
    if(track.count>=2){
        float frame=t*(track.count-1);unsigned first=fminf(track.count-2,floorf(frame));
        a=prog.frames[track.first+first].v;b=prog.frames[track.first+first+1].v;mix=frame-first;
    }
    Affine result;
    for(unsigned j=0;j<12;j++)result.v[j]=a[j]+(b[j]-a[j])*mix;
    return result;
}
static bool motion_collides(unsigned prop,unsigned room,Affine candidate,unsigned descendants){
    PropMotion *m=motion+prop;const CampaignProp *p=campaign_props+prop;
    if(p->solid && !p->hidden && motion_blocked(prop,room,p->moved?p->matrix:identity.v,candidate.v))return true;
    for(unsigned child=m->child;descendants && child!=DIRECTOR_NONE;child=motion[child].sibling){
        if(motion_collides(child,director.state[motion[child].entity].room,
                           compose_motion(candidate.v,motion[child].local),descendants-1))return true;
    }
    return false;
}
static bool prop_motion_blocked(unsigned prop,unsigned room,Affine candidate){
    const PropMotion *m=motion+prop;
    if(m->parent!=DIRECTOR_NONE && campaign_props[m->parent].moved)
        candidate=compose_motion(campaign_props[m->parent].matrix,candidate);
    return motion_collides(prop,room,candidate,m->invalid_contact?16:0);
}
void director_tick(float dt) {
    for(unsigned i=0;i<director.count;i++) {
        EntityState *s=director.state+i;
        if(s->custom_pose)pose_tick(i,dt);
        if(s->active_seconds>0) s->active_seconds=fmaxf(0,s->active_seconds-dt);
        if(s->pose_clip==DIRECTOR_NONE) continue;
        const AnimationClip *clip=prog.clips+s->pose_clip;
        float elapsed=dt;
        if(s->paused){
            float *remaining=director.pause_remaining+prog.def[i].prop;
            float wait=fminf(elapsed,*remaining);*remaining-=wait;elapsed-=wait;s->paused=*remaining>0;
        }
        bool playing=s->clip!=DIRECTOR_NONE && !s->paused;
        float previous_time=s->time;
        if(playing) s->time=fmaxf(0,fminf(clip->seconds,s->time+(s->reverse?-elapsed:elapsed)));
        float t=fmaxf(0,fminf(1,s->time/fmaxf(.001f,clip->seconds)));
        unsigned prop=prog.def[i].prop;
        PropMotion *m=motion+prop;
        Affine candidate=clip_transform(s->pose_clip,t);
        if(playing && (m->panel || m->invalid_contact) && elapsed>0 && motion_blocked && director.controls && campaign_props[prop].solid){
            bool blocked=prop_motion_blocked(prop,s->room,candidate);
            // A player can enter the swing after opening has begun. Endpoints
            // alone let a fast leaf cross the capsule between two updates.
            for(unsigned step=1;step<8 && !blocked;step++){
                float time=previous_time+(s->time-previous_time)*(step/8.f);
                blocked=prop_motion_blocked(prop,s->room,clip_transform(s->pose_clip,time/fmaxf(.001f,clip->seconds)));
            }
            if(!blocked && m->panel && previous_time==0 && !s->reverse && clip->seconds>0){
                // Check the full swing before an interactive panel starts opening.
                for(unsigned step=1;step<=8 && !blocked;step++)
                    blocked=prop_motion_blocked(prop,s->room,clip_transform(s->pose_clip,step/8.f));
            }
            if(blocked){
                s->time=previous_time;
                if(!m->blocked){
                    unsigned activator=prog.activator;
                    prog.activator=0;
                    director_event(i,EV_ENTITY,"do_movedtoinvalidposition");
                    prog.activator=activator;
                }
                m->blocked=true;continue;
            }
        }
        m->blocked=false;m->active=false;m->local=candidate;
        for(unsigned j=0;j<12;j++)if(fabsf(candidate.v[j]-identity.v[j])>.00001f)m->active=true;
        if(playing && ((s->reverse && t==0) || (!s->reverse && (t==1 || clip->seconds<=0)))) {
            bool reverse=s->reverse;s->clip=DIRECTOR_NONE;
            dispatch(i,reverse?EV_RETURN:EV_ARRIVE,clip->name);
        }
    }
    ++prog.stamp;
    for(unsigned i=0;i<director.count;i++) if(prog.def[i].prop!=DIRECTOR_NONE) world_motion(prog.def[i].prop,0);
}
static unsigned nearest_switch(Point center,unsigned room){
    unsigned nearest=DIRECTOR_NONE;
    float best=INFINITY;
    for(unsigned j=0;j<prog.use_count;j++){
        unsigned id=prog.use_triggers[j];
        const EntityState *e=director.state+id;
        if(!e->enabled)continue;
        Point contact=center;
        if(e->room!=room && !(contact_frame && contact_frame(room,e->room,center,&contact)))continue;
        float radius=prog.def[id].radius;
        Point pos;director_pose(id,&pos,NULL,NULL);
        float x=pos.x-contact.x,y=pos.y-contact.y,z=pos.z-contact.z;
        float flat=x*x+z*z,reach=radius+1.1f,distance=flat+y*y;
        if(flat<=reach*reach && fabsf(y)<radius+1 && distance<best){nearest=id;best=distance;}
    }
    return nearest;
}
bool director_player(Player *p,unsigned room,bool use) {
    prog.activator=0;director.can_use=false;
    EntityState *player=director.state;player->position=p->position;player->room=room;player->yaw=p->yaw;
    Point center=p->position;center.y+=.9f;
    for(unsigned i=1;i<director.count;i++) {
        EntityState *e=director.state+i;const EntityDef *d=prog.def+i;
        Point contact=center;
        bool same=e->room==room;
        bool mesh_contact=d->prop<CAMPAIGN_PROPS && (prog.mesh_contacts[d->prop/32]&(1u<<(d->prop%32)));
        bool automatic=d->trigger==1 || d->trigger==3 || mesh_contact;
        if(!e->enabled || (!same && !(automatic && contact_frame && contact_frame(room,e->room,center,&contact)))){
            e->inside=false;
            if(!e->enabled && (d->flags&4))e->activated=false;
            continue;
        }
        if(mesh_contact) {
            bool inside=campaign_bounds_character(d->prop,contact,p->crouched?.95f:1.65f) ||
                (mesh_touch && mesh_touch(d->prop));
            if(inside && !e->inside) director_event(i,EV_ENTITY,"do_charactercollides");
            e->inside=inside;
        }
        if((d->trigger==DIRECTOR_NONE || d->trigger==0) && !(d->flags&4)) continue;
        Point pos;director_pose(i,&pos,NULL,NULL);
        float x=pos.x-contact.x,y=pos.y-contact.y,z=pos.z-contact.z;
        float horizontal=hypotf(x,z);
        if(same && (d->flags&4))director_activate_character(i);
        if(d->trigger==DIRECTOR_NONE) continue;
        bool inside=horizontal<=d->radius+(d->trigger==0?1.1f:.38f) && fabsf(y)<d->radius+1.0f;
        bool facing=x*sinf(p->yaw)+z*cosf(p->yaw)>horizontal*.68f;
        if(d->trigger==4) inside=inside && facing;
        bool fired=(d->trigger==1 || d->trigger==3 || d->trigger==4) && inside && !e->inside;
        e->inside=inside;
        if(fired) director_event(i,EV_ENTITY,"t_activate");
    }
    if(!director.controls || player->dead || player->room!=room || player->position.x!=p->position.x ||
       player->position.y!=p->position.y || player->position.z!=p->position.z){
        director.can_use=false;return false;
    }
    unsigned nearest=nearest_switch(center,room);
    director.can_use=nearest!=DIRECTOR_NONE;
    if(!use || !director.can_use)return false;
    director_event(nearest,EV_ENTITY,"t_activate");
    Point updated=player->position;updated.y+=.9f;
    director.can_use=director.controls && !player->dead && player->room==room && nearest_switch(updated,room)!=DIRECTOR_NONE;
    return true;
}
void director_character_contacts(void) {
    for(unsigned t=0;t<prog.trigger_count;t++) {
        unsigned id=prog.contact_triggers[t];
        EntityState *trigger=director.state+id;
        unsigned prop=director.def[id].prop;
        float radius=director.def[id].radius+.38f;
        Point q;director_pose(id,&q,NULL,NULL);
        for(unsigned c=0;c<prog.character_count;c++) {
            unsigned bit=t*prog.character_count+c,actor=prog.characters[c];
            uint8_t *byte=prog.contacts+bit/8,mask=1u<<(bit%8);
            EntityState *npc=director.state+actor;
            Point p=npc->position;p.y+=.9f;
            bool same=trigger->room==npc->room;
            bool inside=trigger->enabled && npc->enabled && !npc->dead &&
                (same || ((prop!=DIRECTOR_NONE || director.def[id].trigger==3) && contact_frame &&
                          contact_frame(npc->room,trigger->room,p,&p)));
            float dx=p.x-q.x,dz=p.z-q.z;
            if(inside && prop!=DIRECTOR_NONE) inside=campaign_bounds_character(prop,p,1.65f);
            else if(inside) inside=dx*dx+dz*dz<=radius*radius && fabsf(p.y-q.y)<radius+.65f;
            if(inside && !(*byte&mask)) {
                *byte|=mask;prog.activator=actor;
                director_event(id,EV_ENTITY,prop==DIRECTOR_NONE?"t_activate":"do_charactercollides");
            } else if(!inside) *byte&=~mask;
        }
    }
    prog.activator=0;
}
static void bullet_prop(unsigned id,float damage){
    director_event(id,EV_ENTITY,"do_bulletcollides");director_hit(id,damage);
}
void director_pierce_prop(unsigned prop,unsigned owner,float damage){
    if(prop>=CAMPAIGN_PROPS)return;
    unsigned id=motion[prop].entity;
    if(id>=director.count || prog.def[id].prop!=prop || !director.state[id].enabled)return;
    unsigned previous=prog.activator;prog.activator=owner;
    bullet_prop(id,damage);
    prog.activator=previous;
}
void director_bullet(Point start,Point impact,unsigned room,int prop,float damage,unsigned owner) {
    unsigned previous=prog.activator;prog.activator=owner;
    Point line={impact.x-start.x,impact.y-start.y,impact.z-start.z};
    float length=fmaxf(.0001f,line.x*line.x+line.y*line.y+line.z*line.z);
    for(unsigned i=1;i<director.count;i++) {
        EntityState *e=director.state+i;const EntityDef *d=prog.def+i;
        if(!e->enabled) continue;
        // A prop can retain its authored room after its collision mesh moves
        // through a portal. Only trajectory sensors use the segment's room.
        if(prop>=0 && d->prop==(unsigned)prop)bullet_prop(i,damage);
        if(e->room==room && d->trigger==2) {
            Point pos;director_pose(i,&pos,NULL,NULL);
            float x=pos.x-start.x,y=pos.y-start.y,z=pos.z-start.z;
            float t=fmaxf(0,fminf(1,(x*line.x+y*line.y+z*line.z)/length));
            x-=t*line.x;y-=t*line.y;z-=t*line.z;
            if(x*x+y*y+z*z<(d->radius+.2f)*(d->radius+.2f)) director_event(i,EV_ENTITY,"t_activate");
        }
    }
    prog.activator=previous;
}

uint32_t director_signature(void){return prog.signature;}
bool director_signature_matches(uint32_t value){return value==prog.signature || value==(prog.signature^prog.legacy_delta);}
static uint32_t stage_signature(void) {
    uint32_t signature=save_hash(prog.def,prog.h.entities*sizeof(EntityDef)) ^ save_hash(prog.events,prog.h.events*sizeof(Event)) ^
        save_hash(prog.clips,prog.h.clips*sizeof(AnimationClip)) ^ save_hash(prog.messages,prog.h.messages*sizeof(Message)) ^
        save_hash(prog.strings,prog.h.bytes);
    if(prog.transform) signature^=save_hash(prog.transform,prog.h.entities*sizeof(Transform)) ^
        save_hash(prog.tracks,prog.h.clips*sizeof(Track)) ^ save_hash(prog.frames,prog.nframes*sizeof(Affine));
    return signature;
}
bool director_valid_state(void) {
    for(unsigned i=0;i<director.count;i++) {
        const EntityState *s=director.state+i;
        if((s->room>=128 && s->room!=DIRECTOR_NONE) || s->state>=prog.h.strings || s->weapon>=prog.h.strings || s->ai>=prog.h.strings || !isfinite(s->yaw) || !isfinite(s->time) ||
           (s->clip!=DIRECTOR_NONE && (s->clip>=prog.h.clips || prog.def[i].prop==DIRECTOR_NONE)) ||
           (s->pose_clip!=DIRECTOR_NONE && (s->pose_clip>=prog.h.clips || prog.def[i].prop==DIRECTOR_NONE)) ||
           (s->target!=DIRECTOR_NONE && s->target>=director.count) ||
           (s->aim!=DIRECTOR_NONE && s->aim>=director.count) || s->idle>4 || s->pose_variant>=16 ||
           (s->custom_pose && pose_bank_duration(s->custom_pose)<=0) || !isfinite(s->pose_seconds) || !isfinite(s->hp) || !isfinite(s->pace) ||
           !isfinite(s->position.x) || !isfinite(s->position.y) || !isfinite(s->position.z)) return false;
    }
    return true;
}
