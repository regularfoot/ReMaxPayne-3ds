#ifndef MP_NATIVE_ACTOR_H
#define MP_NATIVE_ACTOR_H
#include "room.h"
#include "animation_timing.h"

typedef enum { BERETTA, SHOTGUN, RIFLE, WEAPON_COUNT } Weapon;
enum { ENEMY_SLOTS=3,CAST_SLOTS=30 };
typedef enum { IDLE, WALK, RUN, BACKPEDAL, STRAFE_LEFT, STRAFE_RIGHT, FIRE, RELOAD, PUMP_IDLE, PUMP_FIRE, PUMP_RELOAD, FALL, JUMP, RIFLE_IDLE, RIFLE_FIRE, RIFLE_RELOAD,
               CROUCH_IDLE,DODGE_FORWARD,DODGE_BACKWARD,DODGE_LEFT,DODGE_RIGHT,SWING,THROW_ITEM,/* Reserved old-save IDs; no animation data follows THROW_ITEM. */ CROUCH_DOWN,CROUCH_UP,CROUCH_WALK,CROUCH_BACK,CROUCH_LEFT,CROUCH_RIGHT,SIDE_WALK_LEFT,SIDE_WALK_RIGHT,CROUCH_BACK_LEFT,CROUCH_BACK_RIGHT,CROUCH_FORWARD_LEFT,CROUCH_FORWARD_RIGHT,CLIP_COUNT } Clip;
typedef struct {
    Weapon weapon;
    Clip gait, action, previous_gait;
    float gait_time, action_time, previous_time, transition;
    uint16_t scripted,slot;float script_time;
} Animation;
bool actor_load(bool young,bool actor_lights);
bool actor_rifle(void);
bool actor_campaign_moves(void);
float actor_duration(Clip clip);
float actor_equipped_duration(const Animation *a,Clip clip);
void actor_preload(const char *name);
bool actor_cast(unsigned count,const char *const *skins,const unsigned *entities);
bool actor_enemies(unsigned count);
void actor_leave_enemies(void);
void actor_close(void);
void actor_step(Animation *anim, Clip gait, float speed, float dt);
int actor_locomotion_style(Animation *anim,float forward,float side,bool crouched,Clip override,float dt,bool run_walk);
int actor_locomotion(Animation *anim,float forward,float side,bool crouched,Clip override,float dt);
typedef struct {AnimationTimeline layer[2];AnimationEvaluation sample[2];float input[2],aim_age,aim_weight;uint32_t source[2];bool active,aiming;} ActorTimeline;
void actor_set_timeline(unsigned slot,const ActorTimeline *timeline);
enum {ACTOR_TRANSITION_SNAPSHOT_BYTES=16+2*40*48};
unsigned actor_save_transitions(void *data);
bool actor_load_transitions(const void *data,unsigned count);
void actor_motion(unsigned slot,unsigned gait,float time,unsigned action,float action_time,bool whole_body,bool unarmed,bool two_handed_bones,float exchange_weight,float dt);
void actor_aim(unsigned slot,const unsigned actions[3],Point target,bool turn_torso);
void actor_blend_gait(unsigned slot,unsigned secondary,float weight,unsigned group);
void actor_exchange_from(unsigned clip);
void actor_copy_motion(unsigned to,unsigned from);
void actor_action(Animation *anim, Clip action);
typedef struct { Point ambient,diffuse,direction; } ActorLight;
void actor_set_light(unsigned slot,ActorLight light);
void actor_prepare(const Animation *anim,float pitch,unsigned instance,bool embedded_weapon,uint64_t now);
bool actor_face(unsigned instance,const char *skin,unsigned variant);
Point actor_center(void);
float actor_camera_offset(const Animation *anim);
#ifdef __3DS__
#include <citro3d.h>
void actor_draw(const Animation *anim,unsigned instance,bool unarmed,const C3D_Mtx *transform,unsigned portal,bool tint);
#endif
Point actor_attachment(const Player *p,const Animation *a,bool hand);
bool actor_projectile_frame(const Player *p,const Animation *a,const char *name,Point *origin,Point axes[3]);
Point actor_muzzle(const Player *p, const Animation *anim);
Point actor_hand_muzzle(const Player *p,const Animation *anim,bool left);
Point actor_hand_point(const Player *p,const Animation *anim,bool left,Point offset);
void actor_draw_hand(unsigned instance,bool left,float result[3][4]);
#endif
