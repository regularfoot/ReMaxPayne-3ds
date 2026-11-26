#ifndef MP_WEAPON_TIMING_H
#define MP_WEAPON_TIMING_H
#include <math.h>
// Weapon attributes define how much player fire timing resists slow motion.
// World simulation, NPC weapons and locomotion keep their own scaled clocks.
static inline float weapon_time_step(float world,float real,float resistance){
    return world+fmaxf(0,real-world)*resistance;
}
// Return only this frame's overrun, never accumulated time spent idle.
static inline float weapon_cooldown_step(float *remaining,float dt){
    float late=*remaining>0?fmaxf(0,dt-*remaining):0;
    *remaining=fmaxf(0,*remaining-dt);
    return late;
}
static inline float weapon_cooldown_start(float *remaining,float interval,float late){
    late=fminf(late,interval);
    *remaining=interval-late;
    return late;
}
#endif
