#ifndef MP_ANIMATION_TIMING_H
#define MP_ANIMATION_TIMING_H
#include <math.h>
#include <stdint.h>
#include <stdbool.h>
// Xbox 1EEEF70 clock and 1C8F60/1C8FF0/1C9190 channel transitions.
// Times are source seconds. A zero loop span means a held last frame.
typedef struct {float time,end,loop_start,origin;uint32_t loop;} AnimationClock;
typedef struct {uint32_t clip;float end,loop_start;} AnimationSource;
typedef struct {
    AnimationClock current,previous;
    uint32_t clip,previous_clip,stage,generation;
    float seconds,weight;
} AnimationTimeline;
typedef struct {float time,previous_time,current,previous,snapshot;uint32_t clip,previous_clip,stage;} AnimationEvaluation;
static inline void animation_clock_step(AnimationClock *c,float dt){
    float time=c->time+dt;c->time=time;
    if(time<c->end)return;
    float span=c->end-c->loop_start;
    if(span!=0 && span<=3.5762786865234375e-7f){span=0;c->loop_start=c->end;}
    if(c->loop && span!=0){
        c->time=c->loop_start+fmodf(time-c->loop_start,span);
        c->origin+=c->time-time;
    }else c->time=c->end;
}
static inline AnimationClock animation_clock(AnimationSource source,float time,bool loop){
    float start=source.loop_start;
    if(source.end<=start || (source.end-start!=0 && source.end-start<=3.5762786865234375e-7f))start=source.end;
    return (AnimationClock){time,source.end,start,time,loop};
}
static inline void animation_set(AnimationTimeline *t,AnimationSource source,float time){
    t->previous=t->current;t->previous_clip=t->clip;t->clip=source.clip;
    t->current=animation_clock(source,time,true);t->seconds=1;t->stage=0;
}
// Modes 0/1 blend from a captured pose; mode 0 restarts at the end of entry.
// Modes 2/3 preserve time/normalized phase and advance both source clocks.
// Mode 4 is an immediate restart. Mode 5 follows the other layer's source time.
static inline void animation_request(AnimationTimeline *t,AnimationSource source,unsigned mode,float start,float seconds,
                                     const AnimationTimeline *other){
    t->generation++;
    if(mode==4){animation_set(t,source,start);return;}
    if(mode==5){
        start=other?other->current.time:0;
        if(other && other->stage==1)start-=other->current.origin+other->seconds;
        mode=1;
    }
    AnimationClock old=t->current;uint32_t old_clip=t->clip;
    t->previous=old;t->previous_clip=old_clip;t->clip=source.clip;t->seconds=seconds;
    t->current=animation_clock(source,start,mode!=0);
    if(mode==2 || mode==3){
        float time=mode==3 && old.end>0?old.time*source.end/old.end:old.time;
        t->current.time=t->current.origin=time;t->stage=t->weight>=1?3:4;
    }else t->stage=mode==0?1:2;
}
static inline AnimationEvaluation animation_evaluation(const AnimationTimeline *t){
    AnimationEvaluation e={.time=t->current.time,.previous_time=t->previous.time,
        .current=1,.clip=t->clip,.previous_clip=t->previous_clip,.stage=t->stage};
    float w=t->weight;
    if(t->stage==1 || t->stage==2){e.current=w;e.snapshot=1-w;}
    if(t->stage==3){e.current=w;e.previous=1-w;}
    if(t->stage==4){e.current=w*w;e.previous=w-e.current;e.snapshot=1-w;}
    return e;
}
static inline AnimationEvaluation animation_step(AnimationTimeline *t,float dt){
    animation_clock_step(&t->current,dt);animation_clock_step(&t->previous,dt);
    if(t->stage){
        float age=t->current.time-t->current.origin;if(age<0)age+=t->current.end;
        t->weight=t->seconds>0?fminf(1,age/t->seconds):1;
    }
    AnimationEvaluation e=animation_evaluation(t);
    if(t->stage && t->weight>=1){
        AnimationSource source={t->clip,t->current.end,t->current.loop_start};
        animation_set(t,source,t->stage==1?0:t->current.time);
    }
    return e;
}
#endif
