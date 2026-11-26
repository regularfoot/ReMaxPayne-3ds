/* Audio-only DSP adapter for host regression tests. No rendering is emulated. */
#ifndef MP_HOST_AUDIO_3DS_H
#define MP_HOST_AUDIO_3DS_H
#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stddef.h>
#include <pthread.h>
#include <time.h>
#include <stdatomic.h>
typedef pthread_mutex_t LightLock;
typedef struct {pthread_mutex_t lock;pthread_cond_t condition;bool signalled;} LightEvent;
#define RESET_ONESHOT 0
static inline void LightLock_Init(LightLock *l){pthread_mutex_init(l,NULL);}
static inline void LightLock_Lock(LightLock *l){pthread_mutex_lock(l);}
static inline void LightLock_Unlock(LightLock *l){pthread_mutex_unlock(l);}
static inline void LightEvent_Init(LightEvent *e,int type){(void)type;pthread_mutex_init(&e->lock,NULL);pthread_cond_init(&e->condition,NULL);e->signalled=false;}
static inline void LightEvent_Signal(LightEvent *e){pthread_mutex_lock(&e->lock);e->signalled=true;pthread_cond_signal(&e->condition);pthread_mutex_unlock(&e->lock);}
static inline void LightEvent_Wait(LightEvent *e){pthread_mutex_lock(&e->lock);while(!e->signalled)pthread_cond_wait(&e->condition,&e->lock);e->signalled=false;pthread_mutex_unlock(&e->lock);}
static inline int LightEvent_WaitTimeout(LightEvent *e,int64_t ns){
    struct timespec until;clock_gettime(CLOCK_REALTIME,&until);
    until.tv_sec+=ns/1000000000LL;until.tv_nsec+=ns%1000000000LL;
    if(until.tv_nsec>=1000000000){until.tv_sec++;until.tv_nsec-=1000000000;}
    pthread_mutex_lock(&e->lock);int result=0;
    while(!e->signalled && !result)result=pthread_cond_timedwait(&e->condition,&e->lock,&until);
    if(e->signalled)e->signalled=false;
    pthread_mutex_unlock(&e->lock);return result;
}
typedef struct {pthread_t thread;void (*entry)(void*);void *arg;} *Thread;
static unsigned host_thread_fail;
static inline void *host_thread(void *arg){Thread t=arg;t->entry(t->arg);return NULL;}
static inline Thread threadCreate(void (*entry)(void*),void *arg,size_t stack,int priority,int core,bool detached){
    (void)stack;(void)priority;(void)core;(void)detached;
    if(host_thread_fail){host_thread_fail--;return NULL;}
    Thread t=malloc(sizeof *t);if(!t)return NULL;t->entry=entry;t->arg=arg;
    if(pthread_create(&t->thread,NULL,host_thread,t)){free(t);return NULL;}return t;
}
static inline int threadJoin(Thread t,uint64_t timeout){(void)timeout;return pthread_join(t->thread,NULL);}
static inline void threadFree(Thread t){free(t);}
#define R_FAILED(x) ((x)<0)
#define SYSCLOCK_ARM11 1000000u
#define NDSP_WBUF_QUEUED 1
#define NDSP_WBUF_PLAYING 2
#define NDSP_FORMAT_MONO_PCM16 0
#define NDSP_INTERP_LINEAR 0
typedef struct {int16_t *data_pcm16;unsigned nsamples,status;} ndspWaveBuf;
static unsigned host_linear_allocations,host_linear_frees;
static inline int ndspInit(void){return 0;}
static inline void ndspExit(void){}
static inline void *linearAlloc(size_t n){host_linear_allocations++;return malloc(n);}
static inline void linearFree(void *p){host_linear_frees++;free(p);}
static atomic_uint_fast64_t host_tick=1000000;
static inline uint64_t svcGetSystemTick(void){return atomic_load(&host_tick);}
static inline void ndspChnWaveBufAdd(unsigned c,ndspWaveBuf *b){(void)c;b->status=NDSP_WBUF_QUEUED;}
static inline void ndspChnWaveBufClear(unsigned c){(void)c;}
static inline void ndspChnReset(unsigned c){(void)c;}
static inline void ndspChnSetFormat(unsigned c,unsigned f){(void)c;(void)f;}
static inline void ndspChnSetInterp(unsigned c,unsigned i){(void)c;(void)i;}
static inline void ndspChnSetRate(unsigned c,float r){(void)c;(void)r;}
static inline void ndspChnSetMix(unsigned c,float *m){(void)c;(void)m;}
static inline void ndspChnSetPaused(unsigned c,bool p){(void)c;(void)p;}
static inline void DSP_FlushDataCache(void *p,size_t n){(void)p;(void)n;}
#endif
