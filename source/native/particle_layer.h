// Packed Xbox emitter curves, sampled at eight points in the emitter lifetime.
#ifndef MP_PARTICLE_LAYER_H
#define MP_PARTICLE_LAYER_H
#include <math.h>
#include <stdint.h>
#include "room.h"

typedef struct {uint16_t first,count,capacity,reserved;float period;} ParticleGroup;
typedef struct {float emitted,life[2],speed[2],radius[2];Point drift;} EmitterKey;
typedef struct {float strength;uint8_t size_mode,velocity_mode;uint16_t reserved;} ParticleDrag;
typedef struct {
    uint16_t frames[4];
    uint8_t shape[16],additive,sphere;
    uint16_t reserved;
    float gravity;
    Point spread,offset,box_first,box_last;
    float radius_first,radius_last;
    EmitterKey keys[8];
    uint16_t first_variant,variants;
    uint32_t flags;
    float angles[2],rotation[16],spin_scale[8][2];
} ParticleLayer;
_Static_assert(sizeof(ParticleLayer)==552 && sizeof(ParticleGroup)==12,"Xbox emitter bank layout");

static inline float particle_lerp(float a,float b,float t){return a+(b-a)*t;}
// RPS stores a gravity multiplier, like the projectile database, not m/s².
static inline float particle_vertical_step(float velocity,float gravity,float dt){return velocity-9.81f*gravity*dt;}
// Integrate dv/dt = -k*v^p analytically; explicit Euler reverses a fast smoke
// particle when its original resistance is high and the 3DS frame is long.
static inline float particle_drag_speed(float speed,float diameter,const ParticleDrag *drag,float dt){
    float k=drag->strength;
    if(drag->size_mode)k*=drag->size_mode==1?diameter:diameter*diameter;
    float factor;
    if(drag->velocity_mode==2)factor=expf(-k*dt);
    else if(drag->velocity_mode==1)factor=1/fmaxf(.01f,1+k*speed*dt);
    else factor=1/sqrtf(fmaxf(.0001f,1+2*k*speed*speed*dt));
    return speed*fminf(100,factor);
}

// The integral retains sub-frame emissions, including when a tick crosses a loop boundary.
static inline float particle_integral(const ParticleLayer *layer,float seconds,float period){
    float time=seconds/period,cycle=floorf(time),key=(time-cycle)*7.f;
    unsigned i=(unsigned)key;
    return cycle*layer->keys[7].emitted+
        particle_lerp(layer->keys[i].emitted,layer->keys[i<7?i+1:i].emitted,key-i);
}

static inline int particle_emission_count(const ParticleLayer *layer,float begin,float end,float period,
                                         float packed_period,float phase,int once){
    if(once){
        if(begin>=period)return 0;
        end=fminf(end,period);
    }
    float scale=period/packed_period;
    return (int)floorf(particle_integral(layer,end,period)*scale+phase)-
           (int)floorf(particle_integral(layer,begin,period)*scale+phase);
}
#endif
