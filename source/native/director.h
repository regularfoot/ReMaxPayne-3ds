#ifndef MP_DIRECTOR_H
#define MP_DIRECTOR_H
#include "room.h"
#include <stdio.h>
enum { DIRECTOR_NONE=65535,DIRECTOR_EXITS=256,DIRECTOR_NOTES=24 };
enum EventType { EV_START,EV_SWITCH,EV_SEND,EV_ENTITY,EV_LEAVE,EV_ARRIVE,EV_RETURN,EV_DEATH,EV_ACTIVATE,EV_SPECIAL,EV_FIRST_PICKUP,EV_PICKUP,EV_POSE,EV_CAMERA };
typedef struct {
    uint16_t name,model,state,flags,room,prop,exit,pair,trigger,first_event,events,first_clip;
    Point position;float yaw,radius;uint16_t clips,max_health;
} EntityDef;
typedef struct {
    Point position;float yaw,time,hp,pace,active_seconds,pose_seconds;
    uint16_t room,state,clip,weapon,ai,target,pose_clip,aim,custom_pose;
    uint8_t idle,pose_variant;bool wounded,force_wounded;
    bool enabled,inside,reverse,paused,invulnerable,dead,hostile,activated,infinite_ammo,loot_collected,pass_projectiles,immortal;
} EntityState;
typedef struct {uint16_t path,key,owner;bool parented;float time;} CameraPath;
typedef struct {
    bool skip_death_view,can_use,dodge_immortal,dodge_pass_projectiles;
    bool exits[DIRECTOR_EXITS],ai_exits[DIRECTOR_EXITS],controls,player_slow,collision_dirty,novel,finished,autosave,widescreen,failed,reload;
    uint16_t notes[DIRECTOR_NOTES],note_count,next_level,objective,camera;
    CameraPath path;
    float speed,speed_target,speed_remaining;
    float pause_remaining[256];
    uint32_t acoustic_revision[128];
    unsigned count;
    const EntityDef *def;
    EntityState *state;
    bool *requires_combat_target;
} Director;
typedef void (*DirectorAction)(unsigned target,const char *op,const char *const *args,unsigned count);
void director_mesh_contact(bool (*contact)(unsigned prop));
void director_collision_relevance(bool (*relevant)(unsigned prop));
extern Director director;
bool director_open(const char *stage,DirectorAction external,FILE *journal);
void director_close(void);
uint32_t director_signature(void);
bool director_signature_matches(uint32_t value);
bool director_valid_state(void);
void director_preload_sounds(void (*preload)(const char *group,const char *name));
void director_start(void);
typedef bool (*DirectorMotionBlocked)(unsigned prop,unsigned room,const float *from,const float *to);
void director_motion_guard(DirectorMotionBlocked guard);
// Doorway and use sensors may belong to another authored room. Ordinary rooms
// map through a nearby aperture; moving cabins use their visible placement.
typedef bool (*DirectorContactFrame)(unsigned from,unsigned to,Point point,Point *mapped);
void director_contact_frame(DirectorContactFrame frame);
bool director_camera_overlay(unsigned prop);
void director_tick(float dt);
void director_camera_tick(float real);
unsigned director_view_room(void);
void director_pose(unsigned entity,Point *position,Point *forward,Point *up);
const char *director_text(unsigned id);
static inline bool director_needs_target(unsigned id){
    return director.requires_combat_target && director.requires_combat_target[id] && director.state[id].aim>=director.count;
}
bool director_has_text(const char *name);
float director_prop_travel(void);
unsigned director_find(const char *name);
void director_transfer_weapon(const char *actor,const char *weapon);
void director_event(unsigned entity,enum EventType event,const char *name);
// Returns true if this press activated a use trigger, including one-shot triggers.
bool director_player(Player *p,unsigned room,bool use);
void director_activate_character(unsigned entity);
void director_character_contacts(void);
void director_pierce_prop(unsigned prop,unsigned owner,float damage);
void director_bullet(Point start,Point impact,unsigned room,int prop,float damage,unsigned owner);
void director_restore_corpses(void);
bool director_pose_started(unsigned entity);
void director_resume_pose(unsigned entity,bool started);
void director_set_health(unsigned id,float hp);
void director_hit(unsigned entity,float damage);
#endif
