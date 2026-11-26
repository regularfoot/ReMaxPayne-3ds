#include "async_log.h"
#include <3ds.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

enum { LOG_BYTES=32*1024 };
typedef struct {
    FILE *disk;Thread worker;LightLock lock;LightEvent wake;
    unsigned first,count,dropped;bool stop;
    const char *phase;uint64_t since,reported;
    unsigned gpu_frame,gpu_room,gpu_words,gpu_eyes;
    unsigned gpu_lists_before,gpu_transfers_before;
    char bytes[LOG_BYTES];
} Journal;
static Journal *active;
static atomic_uint gpu_lists,gpu_transfers;

void async_log_gpu_event(unsigned event){
    atomic_fetch_add_explicit(event?&gpu_transfers:&gpu_lists,1,memory_order_relaxed);
}

void async_log_gpu_frame(unsigned room,unsigned words,unsigned eyes){
    Journal *j=active;if(!j)return;
    LightLock_Lock(&j->lock);
    j->gpu_frame++;
    j->gpu_room=room;
    j->gpu_words=words;
    j->gpu_eyes=eyes;
    j->gpu_lists_before=atomic_load_explicit(&gpu_lists,memory_order_relaxed);
    j->gpu_transfers_before=atomic_load_explicit(&gpu_transfers,memory_order_relaxed);
    LightLock_Unlock(&j->lock);
}
void async_log_watch(const char *phase){
    Journal *j=active;if(!j)return;
    LightLock_Lock(&j->lock);j->phase=phase;j->since=svcGetSystemTick();LightLock_Unlock(&j->lock);
}

static void drain(void *arg){
    Journal *j=arg;char chunk[1024];
    for(;;){
        LightEvent_WaitTimeout(&j->wake,500000000LL);
        uint64_t now=svcGetSystemTick();
        LightLock_Lock(&j->lock);
        const char *phase=j->phase;uint64_t since=j->since;
        bool stalled=phase && now>since && now-since>2ULL*SYSCLOCK_ARM11 && j->reported!=since;
        unsigned frame=0,room=0,words=0,eyes=0,lists=0,transfers=0;
        if(stalled){
            j->reported=since;
            frame=j->gpu_frame;room=j->gpu_room;words=j->gpu_words;eyes=j->gpu_eyes;
            lists=atomic_load_explicit(&gpu_lists,memory_order_relaxed)-j->gpu_lists_before;
            transfers=atomic_load_explicit(&gpu_transfers,memory_order_relaxed)-j->gpu_transfers_before;
        }
        LightLock_Unlock(&j->lock);
        if(stalled){
            int n=snprintf(chunk,sizeof chunk,"\nSTALL phase=%s elapsed_ms=%.1f gpu_frame=%u room=%u words=%u eyes=%u p3d=%u ppf=%u\n",
                           phase,(now-since)*(1000./SYSCLOCK_ARM11),frame,room,words,eyes,lists,transfers);
            if(n>0)fwrite(chunk,1,(unsigned)n<sizeof chunk?(unsigned)n:sizeof chunk-1,j->disk);
            fflush(j->disk);
        }
        for(;;){
            LightLock_Lock(&j->lock);
            unsigned n=j->count;if(n>sizeof chunk)n=sizeof chunk;
            for(unsigned i=0;i<n;i++)chunk[i]=j->bytes[(j->first+i)%LOG_BYTES];
            j->first=(j->first+n)%LOG_BYTES;j->count-=n;
            unsigned dropped=j->dropped;j->dropped=0;bool stop=j->stop;
            LightLock_Unlock(&j->lock);
            // Never hold the RAM lock across an SD request.
            if(n)fwrite(chunk,1,n,j->disk);
            if(dropped)fprintf(j->disk,"\nLOG dropped_bytes=%u\n",dropped);
            if(!n){fflush(j->disk);if(stop)return;break;}
        }
    }
}
static int append(void *arg,const char *bytes,int size){
    Journal *j=arg;
    LightLock_Lock(&j->lock);
    unsigned n=(unsigned)size;if(n>LOG_BYTES-j->count)n=LOG_BYTES-j->count;
    for(unsigned i=0;i<n;i++)j->bytes[(j->first+j->count+i)%LOG_BYTES]=bytes[i];
    j->count+=n;j->dropped+=(unsigned)size-n;
    LightLock_Unlock(&j->lock);LightEvent_Signal(&j->wake);
    return size;
}
static int close_journal(void *arg){
    Journal *j=arg;
    if(active==j)active=NULL;
    LightLock_Lock(&j->lock);j->phase=NULL;j->stop=true;LightLock_Unlock(&j->lock);LightEvent_Signal(&j->wake);
    threadJoin(j->worker,UINT64_MAX);threadFree(j->worker);
    int result=fclose(j->disk);free(j);return result;
}
FILE *async_log_open(const char *path){
    Journal *j=calloc(1,sizeof *j);if(!j)return NULL;
    j->disk=fopen(path,"w");if(!j->disk){free(j);return NULL;}
    LightLock_Init(&j->lock);LightEvent_Init(&j->wake,RESET_ONESHOT);
    j->worker=threadCreate(drain,j,16*1024,0x31,-2,false);
    if(!j->worker){fclose(j->disk);free(j);return NULL;}
    FILE *file=funopen(j,NULL,append,NULL,close_journal);
    if(!file){close_journal(j);return NULL;}
    setvbuf(file,NULL,_IONBF,0);active=j;return file;
}
