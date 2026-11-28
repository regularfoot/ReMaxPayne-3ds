#ifndef MP_POSE_BANK_H
#define MP_POSE_BANK_H
#include <stdint.h>
#include <stdbool.h>
#include "bone_pose.h"
bool pose_bank_two_handed(const char *weapon);
bool pose_bank_two_handed_bones(const char *weapon);
const uint8_t *pose_bank_upper(bool two_handed);
typedef struct {
    float wounded,visual_radius,general_radius,reaction_time,shooting_cone,turn_speed,aim_cone,aim_speed,shoot_frequency;
} SkinRules;
typedef struct {char name[20];float maximum,normal,slow_multiplier;} FireRate;
const FireRate *pose_fire_rate(const char *weapon);
const SkinRules *pose_skin_rules(unsigned skin);
float pose_fire_interval(const FireRate *rate,const SkinRules *rules,float fraction);
float pose_stride(unsigned action);
unsigned pose_bank_count(void);
const char *pose_bank_name(unsigned clip);
unsigned pose_bank_action(unsigned skin,const char *name);
unsigned pose_motion_action(unsigned skin,const char *name);
unsigned pose_clip_id(unsigned handle);
typedef void (*PoseMessage)(unsigned actor,const char *target,const char *op,const char *const *args,unsigned count);
void pose_events(unsigned actor,unsigned action,float after,float through,PoseMessage send);
void pose_events_advance(unsigned actor,unsigned action,float after,float through,bool looping,PoseMessage send);
bool pose_message_matches(unsigned action,const char *op,const char *first,const char *second);
bool pose_has_event(unsigned action,const char *op);
bool pose_has_target_event(unsigned action,const char *target,const char *op);
unsigned pose_bank_skin(const char *name);
bool pose_bank_open(void);
void pose_bank_close(void);
uint16_t pose_bank_find(const char *skin,unsigned variant);
float pose_bank_duration(unsigned clip);
float pose_bank_rate(unsigned clip);
bool pose_bank_loops(unsigned clip);
float pose_bank_loop_start(unsigned clip);
float pose_bank_wrap_time(unsigned clip,float seconds);
bool pose_bank_loop_frames(unsigned clip,float seconds,unsigned bones,unsigned *first,unsigned *last,float *mix);
bool pose_bank_frames(unsigned clip,float seconds,unsigned bones,unsigned *first,unsigned *last,float *mix);
void pose_bank_preload(unsigned clip);
bool pose_bank_preload_player(unsigned skin);
const Matrix *pose_bank_rest(void);
const uint8_t *pose_bank_normalization(unsigned clip);
const float (*pose_bank_scaling(unsigned clip))[3];
bool pose_bank_read_local(unsigned clip,unsigned frame,unsigned bones,void *matrix);
bool pose_bank_read(unsigned clip,unsigned frame,unsigned bones,void *matrix);
typedef struct {unsigned id,mode;float seconds;} PoseTransitionRule;
const PoseTransitionRule *pose_transition_rule(unsigned clip);
unsigned pose_transition_mode(unsigned previous,unsigned next);
#endif
