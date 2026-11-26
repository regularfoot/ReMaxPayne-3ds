#include "save.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __3DS__
#include <3ds/archive.h>
#include <3ds/result.h>
#else
#include <sys/stat.h>
#endif

// Global generations fit in the existing header and follow legacy per-slot
// generations even when loaded by an older build. The tag distinguishes them
// from legacy counters which cannot tell us the order across slots.
enum { SAVE_ORDER_TAG=0x40000000u,SAVE_ORDER_MASK=SAVE_ORDER_TAG-1 };
static uint32_t last_generation;

static bool ordered_generation(uint32_t g) {
    return (g&~SAVE_ORDER_MASK)==SAVE_ORDER_TAG;
}

static bool later_generation(uint32_t a,uint32_t b) {
    bool a_ordered=ordered_generation(a),b_ordered=ordered_generation(b);
    if(a_ordered!=b_ordered)return a_ordered;
    if(!a_ordered)return (int32_t)(a-b)>0;
    uint32_t delta=(a-b)&SAVE_ORDER_MASK;
    return delta && delta<SAVE_ORDER_TAG/2;
}

static uint32_t hash_update(uint32_t h,const void *data,size_t n) {
    const unsigned char *p=data;
    for(size_t i=0;i<n;i++) h=(h^p[i])*16777619u;
    return h;
}
uint32_t save_hash(const void *data,size_t n) {
    return hash_update(2166136261u,data,n);
}
static void path(char *buf,unsigned slot,unsigned bank) {
    snprintf(buf,64,"sdmc:/3ds/maxpayne/save%02u%c.dat",slot,'a'+bank);
}
static bool inspect(const char *name,SaveHeader *h) {
    FILE *f=fopen(name,"rb");if(!f) return false;
    bool ok=fread(h,sizeof(*h),1,f)==1 && !memcmp(h->tag,"MPS1",4) && h->bytes && h->bytes<=SAVE_DATA_LIMIT &&
        memchr(h->stage,0,sizeof h->stage) && memchr(h->room,0,sizeof h->room) &&
        h->stage[0] && strspn(h->stage,"abcdefghijklmnopqrstuvwxyz0123456789_-")==strlen(h->stage) &&
        !fseek(f,0,SEEK_END) && ftell(f)==(long)(sizeof(*h)+h->bytes);
    fclose(f);return ok;
}
static int newest(unsigned slot,SaveHeader *h) {
    if(slot>SAVE_BONUS) return -1;
    SaveHeader headers[2];char name[64];int best=-1;
    for(unsigned i=0;i<2;i++) {
        path(name,slot,i);
        if(inspect(name,&headers[i]) && (best<0 || later_generation(headers[i].generation,headers[best].generation))) best=i;
    }
    if(best>=0) *h=headers[best];
    return best;
}
bool save_info(unsigned slot,SaveHeader *h){return newest(slot,h)>=0;}
static bool check_payload(const char *name,const SaveHeader *h,bool *intact) {
    FILE *f=fopen(name,"rb");
    if(!f)return false;
    unsigned char buffer[2048];
    uint32_t hash=2166136261u;
    bool ok=fseek(f,sizeof(*h),SEEK_SET)==0;
    for(size_t left=h->bytes;ok && left;) {
        size_t chunk=left<sizeof buffer?left:sizeof buffer;
        ok=fread(buffer,1,chunk,f)==chunk;
        if(ok)hash=hash_update(hash,buffer,chunk);
        left-=chunk;
    }
    fclose(f);
    *intact=ok && hash==h->crc;
    return ok;
}
typedef struct {
    SaveHeader header;
    uint64_t modified;
    int bank;
} SaveCandidate;

static uint64_t save_modified(const char *name) {
#ifdef __3DS__
    u64 modified=0;
    return R_SUCCEEDED(archive_getmtime(name,&modified))?modified:0;
#else
    struct stat info;
    return !stat(name,&info) && info.st_mtime>0?(uint64_t)info.st_mtime:0;
#endif
}

unsigned save_autosave_slot(const char *stage) {
    return !strcmp(stage,"tutorial")?SAVE_TRAINING:!strcmp(stage,"end_combat")?SAVE_BONUS:0;
}

int save_latest(unsigned automatic_slot) {
    if(automatic_slot && automatic_slot!=SAVE_TRAINING && automatic_slot!=SAVE_BONUS)return -1;
    SaveCandidate candidates[SAVE_BONUS+1];
    char name[64];
    for(unsigned slot=0;slot<=SAVE_BONUS;slot++){
        SaveCandidate *c=candidates+slot;
        c->bank=slot<SAVE_COUNT || slot==automatic_slot?newest(slot,&c->header):-1;
        if(c->bank<0)continue;
        if(save_autosave_slot(c->header.stage)!=automatic_slot){
            // A manual slot may have been overwritten by another mode. Its
            // previous bank is available only if the current write is damaged.
            int other=1-c->bank;
            SaveHeader fallback;
            path(name,slot,other);
            if(!inspect(name,&fallback) || save_autosave_slot(fallback.stage)!=automatic_slot){
                c->bank=-1;continue;
            }
            bool intact;
            path(name,slot,c->bank);
            if(check_payload(name,&c->header,&intact) && intact){c->bank=-1;continue;}
            c->bank=other;c->header=fallback;
        }
        path(name,slot,c->bank);
        c->modified=save_modified(name);
    }
    for(;;){
        int ordered=-1,legacy=-1;
        for(unsigned slot=0;slot<=SAVE_BONUS;slot++){
            const SaveCandidate *c=candidates+slot;
            if(c->bank<0)continue;
            uint32_t generation=c->header.generation;
            if(ordered_generation(generation)){
                if(ordered<0 || later_generation(generation,candidates[ordered].header.generation) ||
                   (generation==candidates[ordered].header.generation && c->modified>candidates[ordered].modified))ordered=slot;
            }else if(legacy<0 || c->modified>candidates[legacy].modified)legacy=slot;
        }
        int slot=ordered;
        // An older build may also have written a legacy save after a modern save.
        if(legacy>=0 && (ordered<0 || (candidates[ordered].modified &&
           candidates[legacy].modified>candidates[ordered].modified)))slot=legacy;
        if(slot<0)return -1;
        SaveCandidate *c=candidates+slot;
        path(name,slot,c->bank);
        bool intact;
        if(check_payload(name,&c->header,&intact) && intact)return slot;

        int other=1-c->bank;
        path(name,slot,other);
        SaveHeader fallback;
        if(inspect(name,&fallback) && save_autosave_slot(fallback.stage)==automatic_slot &&
           check_payload(name,&fallback,&intact) && intact)
            *c=(SaveCandidate){fallback,save_modified(name),other};
        else c->bank=-1;
    }
}

void *save_read(unsigned slot,SaveHeader *h) {
    int bank=newest(slot,h);if(bank<0) return NULL;
    for(int i=0;i<2;i++) {
        char name[64];path(name,slot,(bank+i)%2);
        if(!inspect(name,h)) continue;
        FILE *f=fopen(name,"rb");if(!f) continue;
        void *data=malloc(h->bytes);
        bool ok=data && !fseek(f,sizeof(*h),SEEK_SET) && fread(data,1,h->bytes,f)==h->bytes && save_hash(data,h->bytes)==h->crc;
        fclose(f);if(ok) return data;free(data);
    }
    return NULL;
}
static uint32_t next_generation(void) {
    if(!last_generation){
        last_generation=SAVE_ORDER_TAG;
        bool found=false;
        for(unsigned slot=0;slot<=SAVE_BONUS;slot++){
            SaveHeader h;
            if(newest(slot,&h)>=0 && ordered_generation(h.generation) &&
               (!found || later_generation(h.generation,last_generation))){
                last_generation=h.generation;
                found=true;
            }
        }
    }
    last_generation=SAVE_ORDER_TAG|((last_generation+1)&SAVE_ORDER_MASK);
    return last_generation;
}
bool save_write(unsigned slot,const char *stage,const char *room,uint32_t sig,const void *data,size_t size) {
    if(slot>SAVE_BONUS || !size || size>SAVE_DATA_LIMIT || strlen(stage)>=32) return false;
    SaveHeader old={0},h={.tag={'M','P','S','1'},.signature=sig,.bytes=size,.crc=save_hash(data,size)};
    int current=newest(slot,&old);h.generation=next_generation();
    snprintf(h.stage,sizeof h.stage,"%s",stage);snprintf(h.room,sizeof h.room,"%s",room);
    char name[64];path(name,slot,current==0?1:0);
    SaveHeader fallback;
    if(current>=0 && inspect(name,&fallback)) {
        char latest[64];path(latest,slot,(unsigned)current);
        bool intact;
        if(!check_payload(latest,&old,&intact))return false;
        // Keep the fallback when the newest bank already has a damaged payload.
        if(!intact)path(name,slot,(unsigned)current);
    }
    FILE *f=fopen(name,"wb");if(!f) return false;
    bool ok=fwrite(&h,sizeof h,1,f)==1 && fwrite(data,1,size,f)==size && fflush(f)==0;
    if(fclose(f)!=0) ok=false;
    return ok;
}

static const char completion_path[]="sdmc:/3ds/maxpayne/completed.dat";
bool save_completed(void) {
    char tag[8];FILE *f=fopen(completion_path,"rb");if(!f)return false;
    bool ok=fread(tag,sizeof tag,1,f)==1 && !memcmp(tag,"MPCLEAR1",8) && fgetc(f)==EOF;
    fclose(f);return ok;
}
bool save_completion(void) {
    if(save_completed())return true;
    FILE *f=fopen(completion_path,"wb");if(!f)return false;
    bool ok=fwrite("MPCLEAR1",8,1,f)==1;
    return fclose(f)==0 && ok;
}

unsigned save_unlocked_modes(void) {
    unsigned mask=1;
    if(save_completed())mask|=(1u<<1)|(1u<<3);
    for(unsigned i=1;i<4;i++) {
        char path[64];snprintf(path,sizeof path,"sdmc:/3ds/maxpayne/mode%u.dat",i);
        FILE *f=fopen(path,"rb");if(!f)continue;
        if(fgetc(f)==(int)i && fgetc(f)==EOF)mask|=1u<<i;
        fclose(f);
    }
    return mask;
}
bool save_unlock_mode(unsigned mode) {
    if(mode==0 || mode>=4)return false;
    if(save_unlocked_modes()&(1u<<mode))return true;
    char path[64];snprintf(path,sizeof path,"sdmc:/3ds/maxpayne/mode%u.dat",mode);
    FILE *f=fopen(path,"wb");if(!f)return false;
    bool ok=fputc(mode,f)!=EOF;return fclose(f)==0 && ok;
}

bool save_race_time(unsigned seconds,unsigned *best) {
    static const char *paths[]={"sdmc:/3ds/maxpayne/racea.dat","sdmc:/3ds/maxpayne/raceb.dat"};
    unsigned previous=0,bank=0;
    for(unsigned i=0;i<2;i++){
        FILE *f=fopen(paths[i],"rb");if(!f)continue;
        uint32_t data[3];bool valid=fread(data,sizeof data,1,f)==1 && fgetc(f)==EOF && data[0]==0x314d594e &&
            data[1] && data[2]==save_hash(data,8);fclose(f);
        if(valid && (!previous || data[1]<previous)){previous=data[1];bank=1-i;}
    }
    *best=previous;
    if(!seconds || (previous && previous<=seconds))return true;
    uint32_t data[3]={0x314d594e,seconds,0};data[2]=save_hash(data,8);
    FILE *f=fopen(paths[bank],"wb");if(!f)return false;
    bool ok=fwrite(data,sizeof data,1,f)==1 && fflush(f)==0;if(fclose(f)!=0)ok=false;
    if(ok)*best=seconds;
    return ok;
}
