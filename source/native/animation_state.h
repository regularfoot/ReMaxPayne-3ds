#ifndef MP_ANIMATION_STATE_H
#define MP_ANIMATION_STATE_H
#include "actor.h"
#include <math.h>
// Old save files used these numbers for generated clips. No new pose uses them.
static inline Clip retail_saved_clip(Clip clip,bool crouched){
    return clip>=CROUCH_DOWN?(crouched?CROUCH_IDLE:IDLE):clip;
}
static inline void retail_restore_animation(Animation *a,bool crouched){
    if(a->gait>=CROUCH_DOWN || a->previous_gait>=CROUCH_DOWN){
        a->gait=retail_saved_clip(a->gait,crouched);a->previous_gait=a->gait;
        a->gait_time=a->previous_time=a->transition=0;
    }
    if(a->action>=CROUCH_DOWN){a->action=IDLE;a->action_time=0;}
}
// Xbox evaluator 001D4370: motion axes take precedence over stationary Crouch.
static inline Clip retail_locomotion(float forward,float side,bool crouched,float dt,bool run_walk){
    float distance=hypotf(forward,side);
    if(dt<=0 || distance<=dt*.04f)return crouched?CROUCH_IDLE:IDLE;
    if(fabsf(side)>fabsf(forward))return side<0?STRAFE_LEFT:STRAFE_RIGHT;
    if(forward<0)return BACKPEDAL;
    return run_walk || distance>dt*2.1f?RUN:WALK;
}
#endif
