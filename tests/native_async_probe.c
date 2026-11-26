/* Hold real worker I/O and prove submissions, logging and polling can proceed. */
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include "async_save.h"
static atomic_bool hold_save,save_blocked,hold_log,log_blocked;
static atomic_uint writes;
static pthread_t game_thread;
static unsigned saved[8];
static void wait_a_moment(void){struct timespec t={0,1000000};nanosleep(&t,NULL);}
static void wait_blocked(atomic_bool *flag){
    for(unsigned i=0;i<5000 && !*flag;i++)wait_a_moment();
    assert(*flag);
}
bool save_write(unsigned slot,const char *stage,const char *room,uint32_t signature,const void *data,size_t size){
    assert(!pthread_equal(pthread_self(),game_thread));
    assert(slot==13 && !strcmp(stage,"tutorial") && !strcmp(room,"StartRoom") && signature==42 && size==sizeof(unsigned));
    if(hold_save){save_blocked=true;while(hold_save)wait_a_moment();}
    unsigned n=writes;assert(n<8);saved[n]=*(const unsigned*)data;writes++;return true;
}
static size_t slow_write(const void *bytes,size_t size,size_t count,FILE *file){
    assert(!pthread_equal(pthread_self(),game_thread));
    if(hold_log){log_blocked=true;while(hold_log)wait_a_moment();}
    return fwrite(bytes,size,count,file);
}
#include "../source/native/async_save.c"
#define fwrite slow_write
#include "../source/native/async_log.c"
#undef fwrite
static SaveJob job(unsigned value){
    unsigned *data=malloc(sizeof *data);assert(data);*data=value;
    SaveJob j={.slot=13,.signature=42,.size=sizeof *data,.data=data};
    strcpy(j.stage,"tutorial");strcpy(j.room,"StartRoom");return j;
}
int main(void){
    game_thread=pthread_self();
    // Failure must retain caller ownership and never fall back to a game-thread write.
    host_thread_fail=1;assert(!async_save_open());SaveJob a=job(0);
    assert(!async_save_submit(&a) && writes==0);free(a.data);async_save_close();
    assert(async_save_open());hold_save=true;a=job(1);assert(async_save_submit(&a));wait_blocked(&save_blocked);
    SaveJob b=job(2),c=job(3),result;
    assert(async_save_submit(&b));assert(async_save_submit(&c));
    for(unsigned frame=0;frame<120;frame++)assert(!async_save_result(&result));
    assert(writes==0); // The worker is still held; the game thread reached this point.
    hold_save=false;async_save_finish();
    assert(writes==2 && saved[0]==1 && saved[1]==3);
    assert(async_save_result(&result) && result.ok && result.data==NULL && result.slot==13);
    assert(!async_save_result(&result));async_save_close();
    assert(async_save_open());a=job(4);assert(async_save_submit(&a));async_save_close();assert(writes==3 && saved[2]==4);

    hold_log=true;FILE *log=async_log_open("async-test.log");assert(log);
    assert(fputs("BEGIN\n",log)>=0);assert(!fflush(log));wait_blocked(&log_blocked);
    char text[1025];memset(text,'x',sizeof text-1);text[sizeof text-1]=0;
    for(unsigned frame=0;frame<120;frame++){assert(fprintf(log,"frame=%u %s\n",frame,text)>0);assert(!fflush(log));}
    // More than the bounded ring was logged, while the filesystem remained held.
    assert(log_blocked);hold_log=false;assert(!fclose(log));
    FILE *f=fopen("async-test.log","rb");assert(f);char bytes[40000]={0};size_t n=fread(bytes,1,sizeof bytes-1,f);
    assert(n>6 && n<sizeof bytes-1 && !strncmp(bytes,"BEGIN\n",6) && strstr(bytes,"LOG dropped_bytes="));
    fclose(f);remove("async-test.log");
    // No more game-thread writes: the worker must diagnose the held phase by itself.
    log=async_log_open("async-watch.log");assert(log);
    async_log_gpu_frame(3,1234,2);
    async_log_gpu_event(0);
    async_log_gpu_event(1);
    async_log_watch("gpu-queue");atomic_store(&host_tick,4000001);
    bool reported=false;
    for(unsigned i=0;i<2000 && !reported;i++){
        wait_a_moment();f=fopen("async-watch.log","rb");assert(f);
        memset(bytes,0,sizeof bytes);fread(bytes,1,sizeof bytes-1,f);fclose(f);
        reported=strstr(bytes,"STALL phase=gpu-queue")!=NULL &&
            strstr(bytes,"gpu_frame=1 room=3 words=1234 eyes=2 p3d=1 ppf=1")!=NULL;
    }
    assert(reported);async_log_watch(NULL);assert(!fclose(log));remove("async-watch.log");
    puts("Async save/log: blocked SD, queued checkpoint replacement, failure ownership and close drain verified");
    puts("Stalled-frame phase is logged by the worker without another game-thread write");
    return 0;
}
