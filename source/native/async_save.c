#include "async_save.h"
#include "save.h"
#include <3ds.h>
#include <stdlib.h>

static Thread worker;
static LightLock lock;
static LightEvent wake,idle;
static SaveJob queued,completed;
static bool pending,busy,stop,have_result;

static void write_jobs(void *unused){
    (void)unused;
    for(;;){
        LightEvent_Wait(&wake);
        for(;;){
            LightLock_Lock(&lock);
            bool quit=stop,run=pending;
            SaveJob job=queued;
            if(run){pending=false;busy=true;queued=(SaveJob){0};}
            LightLock_Unlock(&lock);
            if(!run){if(quit)return;break;}
            job.ok=save_write(job.slot,job.stage,job.room,job.signature,job.data,job.size);
            free(job.data);job.data=NULL;
            LightLock_Lock(&lock);
            completed=job;have_result=true;busy=false;
            LightLock_Unlock(&lock);
            LightEvent_Signal(&idle);
        }
    }
}
bool async_save_open(void){
    if(worker)return true;
    LightLock_Init(&lock);LightEvent_Init(&wake,RESET_ONESHOT);LightEvent_Init(&idle,RESET_ONESHOT);
    pending=busy=stop=have_result=false;queued=completed=(SaveJob){0};
    worker=threadCreate(write_jobs,NULL,16*1024,0x31,-2,false);
    return worker!=NULL;
}
bool async_save_submit(const SaveJob *job){
    if(!worker || !job->data || !job->size)return false;
    LightLock_Lock(&lock);
    // Preserve the in-flight checkpoint. A newer checkpoint replaces only the
    // waiting one; no join, thread creation or filesystem fallback on this path.
    void *replaced=pending?queued.data:NULL;
    queued=*job;pending=true;
    LightLock_Unlock(&lock);
    free(replaced);LightEvent_Signal(&wake);return true;
}
bool async_save_result(SaveJob *result){
    if(!worker)return false;
    LightLock_Lock(&lock);bool ready=have_result;
    if(ready){*result=completed;have_result=false;}
    LightLock_Unlock(&lock);return ready;
}
void async_save_finish(void){
    if(!worker)return;
    for(;;){
        LightLock_Lock(&lock);bool done=!pending && !busy;LightLock_Unlock(&lock);
        if(done)return;
        LightEvent_Wait(&idle);
    }
}
void async_save_close(void){
    if(!worker)return;
    async_save_finish();
    LightLock_Lock(&lock);stop=true;LightLock_Unlock(&lock);LightEvent_Signal(&wake);
    threadJoin(worker,UINT64_MAX);threadFree(worker);worker=NULL;
}
