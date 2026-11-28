#include "campaign_file.h"
#include "audio_bank.h"
#include "director.h"
#include "campaign_world.h"
#include "ui.h"
#include <3ds.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
enum { FIRST_CHANNEL=4,VOICE_COUNT=8,SAMPLES=2048,BUFFERS=3 };
typedef struct {char key[80];uint32_t at,bytes;float gain;uint32_t flags;} Record;
typedef struct {unsigned owner,room;Point position;bool spatial,follow;} Emitter;
typedef struct {
    const Record *clip;Emitter emitter;
    double start;
    unsigned token,slot;
} SceneSound;
typedef struct {
    const Record *clip;
    uint32_t offset,next,read_start,read_bytes;
    uint8_t *read_ahead;
    int16_t *samples;
    ndspWaveBuf buffers[BUFFERS];
    Emitter emitter;
    uint64_t started;
    unsigned token;float strength;
} Voice;
static FILE *bank;
static Record *records;
typedef struct {uint8_t *data;unsigned bytes;bool owner;} CachedAudio;
static CachedAudio *prefixes;
enum { PREFIX_BYTES=2*SAMPLES*BUFFERS, PREFIX_LIMIT=1024*1024, SHORT_CLIP_BYTES=32*1024 };
static unsigned prefix_bytes;
static unsigned record_count;
static Voice voices[VOICE_COUNT];
enum { READ_AHEAD=16*1024 };
typedef struct {
    const Record *clip;unsigned start,bytes;
    uint8_t *data;bool pending,busy,ready,failed;
} StreamRead;
static StreamRead reads[VOICE_COUNT];
static Thread reader;
static LightLock read_lock;
static LightEvent read_event;
static bool read_stop;
// SD writes (notably autosaves) can hold the FS service for seconds. Only this
// worker touches streaming PCM; the game thread consumes completed RAM pages.
static void stream_reader(void *arg){
    (void)arg;FILE *stream=campaign_file("audio.bank");
    for(;;){
        LightEvent_Wait(&read_event);
        LightLock_Lock(&read_lock);bool stop=read_stop;LightLock_Unlock(&read_lock);
        if(stop)break;
        for(unsigned i=0;i<VOICE_COUNT;i++){
            StreamRead *r=reads+i;
            LightLock_Lock(&read_lock);
            bool pending=r->pending;const Record *clip=r->clip;unsigned start=r->start,bytes=r->bytes;
            if(pending){r->pending=false;r->busy=true;}
            LightLock_Unlock(&read_lock);
            if(!pending)continue;
            bool ok=stream && !fseek(stream,clip->at+start,SEEK_SET) && fread(r->data,1,bytes,stream)==bytes;
            LightLock_Lock(&read_lock);r->busy=false;r->ready=ok;r->failed=!ok;LightLock_Unlock(&read_lock);
        }
    }
    if(stream)fclose(stream);
}
static void request_read(Voice *v,unsigned start){
    StreamRead *r=reads+(v-voices);
    if(start>=v->clip->bytes){if(v->clip->flags&1)start=0;else return;}
    LightLock_Lock(&read_lock);
    bool covered=r->clip==v->clip && start>=r->start && start-r->start<r->bytes;
    bool queue=!r->busy && !(covered && (r->pending || r->ready || r->failed));
    if(queue){
        r->clip=v->clip;r->start=start;r->bytes=v->clip->bytes-start;
        if(r->bytes>READ_AHEAD)r->bytes=READ_AHEAD;
        r->pending=true;r->ready=r->failed=false;
    }
    LightLock_Unlock(&read_lock);
    if(queue)LightEvent_Signal(&read_event);
}
static Point listener;
static unsigned listener_room,scene_count,scene_capacity,next_token;
static SceneSound *sounds;
static float music_level=1;
void bank_music_level(float level){music_level=level;}
static uint64_t last_tick;
static double sample_clock;
static float heading;
static bool ready,paused;
static float effect_rate=1;
_Static_assert(sizeof(Record)==96,"Audio bank record");
bool bank_open(void) {
    bank=campaign_file("audio.bank");if(!bank)return false;
    char magic[4];
    if(fread(magic,4,1,bank)!=1 || memcmp(magic,"MPA1",4) || fread(&record_count,4,1,bank)!=1 || !record_count || record_count>8192) goto fail;
    records=malloc(record_count*sizeof(Record));
    if(!records || fread(records,sizeof(Record),record_count,bank)!=record_count || fseek(bank,0,SEEK_END))goto fail;
    long size=ftell(bank);
    for(unsigned i=0;i<record_count;i++) {
        Record *r=records+i;
        if(!memchr(r->key,0,sizeof r->key) || !r->bytes || (r->bytes&1) || size<0 || r->at>(unsigned long)size ||
           r->bytes>(unsigned long)size-r->at || (i && strcmp(r[-1].key,r->key)>0)) goto fail;
    }
    if(R_FAILED(ndspInit()))goto fail;
    ready=true;
    prefixes=calloc(record_count,sizeof(*prefixes));
    if(!prefixes)goto fail;
    for(unsigned i=0;i<VOICE_COUNT;i++){
        voices[i].samples=linearAlloc(SAMPLES*BUFFERS*sizeof(int16_t));
        voices[i].read_ahead=malloc(READ_AHEAD);reads[i].data=malloc(READ_AHEAD);
        if(!voices[i].samples || !voices[i].read_ahead || !reads[i].data)goto fail;
    }
    LightLock_Init(&read_lock);LightEvent_Init(&read_event,RESET_ONESHOT);read_stop=false;
    reader=threadCreate(stream_reader,NULL,16*1024,0x31,-2,false);
    if(!reader)goto fail;
    ready=true;return true;
fail:bank_close();return false;
}
static void silence(unsigned slot) {
    Voice *v=voices+slot;
    if(ready) ndspChnWaveBufClear(FIRST_CHANNEL+slot);
    int16_t *samples=v->samples;uint8_t *read_ahead=v->read_ahead;
    *v=(Voice){.samples=samples,.read_ahead=read_ahead};
}
void bank_clear(void) {
    for(unsigned i=0;i<VOICE_COUNT;i++)silence(i);
    if(prefixes){
        for(unsigned i=0;i<record_count;i++)if(prefixes[i].owner)free(prefixes[i].data);
        memset(prefixes,0,record_count*sizeof(*prefixes));
    }
    prefix_bytes=0;
    free(sounds);sounds=NULL;scene_count=scene_capacity=0;last_tick=0;sample_clock=0;
}
void bank_close(void) {
    if(reader){
        LightLock_Lock(&read_lock);read_stop=true;LightLock_Unlock(&read_lock);LightEvent_Signal(&read_event);
        threadJoin(reader,UINT64_MAX);threadFree(reader);reader=NULL;
    }
    bank_clear();
    for(unsigned i=0;i<VOICE_COUNT;i++){
        if(voices[i].samples)linearFree(voices[i].samples);
        free(voices[i].read_ahead);voices[i]=(Voice){0};
        free(reads[i].data);reads[i]=(StreamRead){0};
    }
    if(ready) ndspExit();
    if(bank) fclose(bank);
    free(prefixes);prefixes=NULL;free(records);bank=NULL;records=NULL;record_count=0;ready=paused=false;
}
static const Record *find_clip(const char *key) {
    unsigned lo=0,hi=record_count;
    while(lo<hi){unsigned mid=(lo+hi)/2;if(strcmp(records[mid].key,key)<0)lo=mid+1;else hi=mid;}
    if(lo==record_count || strcmp(records[lo].key,key))return NULL;
    unsigned limit=lo+1;while(limit<record_count && !strcmp(records[limit].key,key))limit++;
    return records+lo+rand()%(limit-lo);
}
static double sound_time(void){
    uint64_t now=svcGetSystemTick();
    if(last_tick && !paused)sample_clock+=(now-last_tick)*(22050.0/SYSCLOCK_ARM11)*effect_rate;
    last_tick=now;return sample_clock;
}
static unsigned token(void){if(!++next_token)++next_token;return next_token;}
static bool sound_levels(const Emitter *e,float *pan,float *attenuation){
    *pan=0;*attenuation=1;
    if(!e->spatial)return true;
    Point pos=e->position,apparent;unsigned room=e->room;
    if(e->follow){
        if(e->owner>=director.count)return false;
        room=director.state[e->owner].room;
        if(director.def[e->owner].flags&4)pos=director.state[e->owner].position;
        else director_pose(e->owner,&pos,NULL,NULL);
    }
    float distance;
    if(!campaign_sound_path(listener_room,listener,room,pos,director.exits,&apparent,&distance))return false;
    float dx=apparent.x-listener.x,dz=apparent.z-listener.z;
    *pan=(cosf(heading)*dx-sinf(heading)*dz)/fmaxf(1,hypotf(dx,dz));
    *attenuation=1/(1+.13f*distance);return true;
}
static void mix(unsigned slot,float pan,float attenuation) {
    Voice *v=voices+slot;
    float gain=v->clip->gain*(slot?ui_settings.volume:ui_settings.music*music_level)*.25f*attenuation;
    v->strength=v->clip->gain*attenuation;
    float levels[12]={gain*(1-pan*.65f),gain*(1+pan*.65f)};
    ndspChnSetMix(FIRST_CHANNEL+slot,levels);
}
void bank_preload(const char *group,const char *name){
    if(!prefixes)return;
    char key[80];if(snprintf(key,sizeof key,"%s/%s",group,name)>=(int)sizeof key)return;
    unsigned lo=0,hi=record_count;
    while(lo<hi){unsigned mid=(lo+hi)/2;if(strcmp(records[mid].key,key)<0)lo=mid+1;else hi=mid;}
    for(unsigned i=lo;i<record_count && !strcmp(records[i].key,key);i++){
        if(prefixes[i].data)continue;
        // Weapon tails recur during rapid fire. Keep the complete short effect,
        // within the same fixed stage cache, instead of reading its tail mid-shot.
        bool hot=(!strcmp(group,"weapons") && !strncmp(name,"shoot",5)) ||
                 (!strcmp(group,"projectiles") && !strncmp(name,"bullethit",9));
        unsigned limit=hot?96*1024:SHORT_CLIP_BYTES;
        unsigned bytes=records[i].bytes<=limit?records[i].bytes:PREFIX_BYTES;
        if(bytes>PREFIX_LIMIT-prefix_bytes)return;
        uint8_t *data=malloc(bytes);if(!data)return;
        if(fseek(bank,records[i].at,SEEK_SET) || fread(data,1,bytes,bank)!=bytes){free(data);continue;}
        // Different materials and sound names can share one PCM range in the bank.
        for(unsigned j=0;j<record_count;j++)
            if(records[j].at==records[i].at && records[j].bytes==records[i].bytes)
                prefixes[j]=(CachedAudio){data,bytes,j==i};
        prefix_bytes+=bytes;
    }
}
// Return the bytes available now, 0 while reading or -1 on I/O failure.
// A spatial sound can resume mid-block when a door opens. Queue the cached
// part immediately and let the next DSP buffer start on the stream boundary.
static int read_voice(Voice *v,void *out,unsigned bytes){
    const CachedAudio *cached=prefixes?prefixes+(v->clip-records):NULL;
    if(cached && v->offset<cached->bytes){
        unsigned available=cached->bytes-v->offset;
        if(bytes>available)bytes=available;
        memcpy(out,cached->data+v->offset,bytes);
        if(cached->bytes<v->clip->bytes)request_read(v,cached->bytes);
        return (int)bytes;
    }
    // Keep consecutive samples from each independent stream across DSP refills.
    if(v->offset<v->read_start || v->offset>=v->read_start+v->read_bytes){
        StreamRead *r=reads+(v-voices);int result=0;
        LightLock_Lock(&read_lock);
        if(r->clip==v->clip && v->offset>=r->start && v->offset-r->start<r->bytes){
            if(r->ready){
                memcpy(v->read_ahead,r->data,r->bytes);v->read_start=r->start;v->read_bytes=r->bytes;
                r->ready=false;result=1;
            }else if(r->failed)result=-1;
        }
        LightLock_Unlock(&read_lock);
        if(result<=0){if(!result)request_read(v,v->offset);return result;}
    }
    unsigned start=v->offset-v->read_start,available=v->read_bytes-start;
    if(bytes>available)bytes=available;
    memcpy(out,v->read_ahead+start,bytes);
    if(available-bytes<=2*SAMPLES)request_read(v,v->read_start+v->read_bytes);
    return (int)bytes;
}
static void fill(unsigned slot) {
    Voice *v=voices+slot;if(!v->clip) return;
    for(unsigned k=0;k<BUFFERS;k++) {
        ndspWaveBuf *b=v->buffers+v->next;
        if(b->status==NDSP_WBUF_PLAYING || b->status==NDSP_WBUF_QUEUED) return;
        if(v->offset==v->clip->bytes) {
            if(v->clip->flags&1) v->offset=0;
            else {
                for(unsigned j=0;j<BUFFERS;j++) if(v->buffers[j].status==NDSP_WBUF_PLAYING || v->buffers[j].status==NDSP_WBUF_QUEUED)return;
                silence(slot);return;
            }
        }
        unsigned n=(v->clip->bytes-v->offset)/2;if(n>SAMPLES)n=SAMPLES;
        int16_t *data=v->samples+v->next*SAMPLES;
        int result=read_voice(v,data,2*n);
        if(result<=0){if(result<0)silence(slot);return;}
        v->offset+=(unsigned)result;DSP_FlushDataCache(data,(unsigned)result);
        *b=(ndspWaveBuf){.data_pcm16=data,.nsamples=(unsigned)result/2};ndspChnWaveBufAdd(FIRST_CHANNEL+slot,b);
        v->next=(v->next+1)%BUFFERS;
    }
}
static void play(unsigned slot,const Record *record,Emitter emitter,float pan,float attenuation,uint32_t offset,unsigned serial) {
    silence(slot);Voice *v=voices+slot;
    if(!v->samples)return;
    v->clip=record;v->emitter=emitter;v->offset=offset;v->token=serial;v->started=svcGetSystemTick();
    ndspChnReset(FIRST_CHANNEL+slot);ndspChnSetFormat(FIRST_CHANNEL+slot,NDSP_FORMAT_MONO_PCM16);
    ndspChnSetRate(FIRST_CHANNEL+slot,22050*(slot?effect_rate:1));ndspChnSetInterp(FIRST_CHANNEL+slot,NDSP_INTERP_LINEAR);
    mix(slot,pan,attenuation);fill(slot);
}
void bank_music(const char *name) {
    if(!ready)return;
    char key[80];if(snprintf(key,sizeof key,"music/%s",name)>=(int)sizeof key)return;
    if(voices[0].clip && !strcmp(voices[0].clip->key,key))return;
    const Record *r=find_clip(key);
    if(r)play(0,r,(Emitter){0},0,1,0,0);
    else if(!name[0] || !strcmp(name,"none"))silence(0);
}
static unsigned effect_slot(float strength,bool immediate){
    unsigned quiet=0,oldest=1;
    for(unsigned i=1;i<VOICE_COUNT;i++){
        const Voice *v=voices+i;
        if(!v->clip)return i;
        if(v->token && (v->clip->flags&1) && (!quiet || v->strength<voices[quiet].strength))quiet=i;
        if(v->started<voices[oldest].started)oldest=i;
    }
    if(quiet && (immediate || strength>1.2f*voices[quiet].strength))return quiet;
    return immediate?oldest:0;
}
// Keep time for inaudible emitters without reading PCM or holding a channel.
// Opening a door halfway through a line reveals the part currently being spoken.
static bool update_sound(SceneSound *s,double now,bool immediate){
    const Record *r=s->clip;
    bool active=s->slot && voices[s->slot].token==s->token;
    uint64_t frame=(uint64_t)(now-s->start);
    unsigned samples=r->bytes/2;
    if(!active && !(r->flags&1) && frame>=samples)return false;
    float pan,level;
    if(!sound_levels(&s->emitter,&pan,&level)){
        if(active)silence(s->slot);
        s->slot=0;return (r->flags&1) || frame<samples;
    }
    if(active){
        mix(s->slot,pan,level);fill(s->slot);
        return voices[s->slot].token==s->token || (r->flags&1);
    }
    unsigned slot=effect_slot(r->gain*level,immediate && !(r->flags&1));
    if(slot){
        uint32_t offset=(r->flags&1?frame%samples:frame)*2;
        play(slot,r,s->emitter,pan,level,offset,s->token);s->slot=slot;
    }
    return true;
}
static void effect(Emitter e,const char *group,const char *name,bool force_spatial) {
    if(!ready)return;
    char key[80];if(snprintf(key,sizeof key,"%s/%s",group,name)>=(int)sizeof key)return;
    const Record *r=find_clip(key);if(!r)return;
    e.spatial=e.spatial && (force_spatial || (r->flags&2));
    if(e.spatial){
        if(r->flags&1)for(unsigned i=0;i<scene_count;i++){
            if(sounds[i].emitter.owner==e.owner && !strcmp(sounds[i].clip->key,key))return;
        }
        if(scene_count==scene_capacity){
            unsigned capacity=scene_capacity?2*scene_capacity:8;
            SceneSound *grown=realloc(sounds,capacity*sizeof(*grown));if(!grown)return;
            sounds=grown;scene_capacity=capacity;
        }
        double now=sound_time();
        SceneSound *s=sounds+scene_count++;
        *s=(SceneSound){.clip=r,.emitter=e,.start=now,.token=token()};
        update_sound(s,now,true);
    }else{
        if(r->flags&1)for(unsigned i=1;i<VOICE_COUNT;i++){
            if(voices[i].clip && voices[i].emitter.owner==e.owner && !strcmp(voices[i].clip->key,key))return;
        }
        play(effect_slot(r->gain,true),r,e,0,1,0,0);
    }
}
void bank_sound(unsigned owner,const char *group,const char *name){
    effect((Emitter){.owner=owner},group,name,false);
}
void bank_impact_sound(unsigned owner,unsigned room,Point point,const char *key,const char *material){
    if(!ready)return;
    char sound[80];snprintf(sound,sizeof sound,"%s/%s",key,material);
    if(!strcmp(material,"default") || !find_clip(sound))snprintf(sound,sizeof sound,"%s",key);
    char *name=strchr(sound,'/');if(!name)return;
    *name++=0;effect((Emitter){owner,room,point,true,false},sound,name,true);
}
void bank_scene_sound(unsigned owner,const char *group,const char *name,Point pos,bool follow,bool force_spatial){
    if(owner>=director.count)return;
    effect((Emitter){owner,director.state[owner].room,pos,true,follow},group,name,force_spatial);
}
typedef struct {char key[80];Point pos;uint16_t owner,room;uint8_t follow,padding[3];} SavedAmbient;
size_t bank_ambient_size(void){
    unsigned n=0;for(unsigned i=0;i<scene_count;i++)if(sounds[i].clip->flags&1)n++;
    return n*sizeof(SavedAmbient);
}
void bank_store_ambient(void *data){
    uint8_t *out=data;
    for(unsigned i=0;i<scene_count;i++){
        const SceneSound *a=sounds+i;if(!(a->clip->flags&1))continue;
        SavedAmbient saved={.pos=a->emitter.position,.owner=a->emitter.owner,.room=a->emitter.room,.follow=a->emitter.follow};
        memcpy(saved.key,a->clip->key,sizeof saved.key);
        memcpy(out,&saved,sizeof saved);out+=sizeof saved;
    }
}
bool bank_restore_ambient(const void *data,size_t bytes){
    if(bytes%sizeof(SavedAmbient))return false;
    unsigned count=bytes/sizeof(SavedAmbient);
    SceneSound *restored=count && ready?malloc(count*sizeof(*restored)):NULL;
    if(count && ready && !restored)return false;
    const uint8_t *in=data;unsigned used=0;double now=sound_time();
    for(unsigned i=0;i<count;i++,in+=sizeof(SavedAmbient)){
        SavedAmbient s;memcpy(&s,in,sizeof s);
        if(!memchr(s.key,0,sizeof s.key) || s.owner>=director.count || s.follow>1 ||
           !isfinite(s.pos.x) || !isfinite(s.pos.y) || !isfinite(s.pos.z)){free(restored);return false;}
        const Record *r=ready?find_clip(s.key):NULL;
        if(r && (r->flags&1))restored[used++]=(SceneSound){.clip=r,.emitter={s.owner,s.room,s.pos,true,s.follow!=0},.start=now,.token=token()};
    }
    for(unsigned i=1;i<VOICE_COUNT;i++)if(voices[i].clip && voices[i].emitter.spatial)silence(i);
    free(sounds);sounds=restored;scene_count=used;scene_capacity=restored?count:0;
    return true;
}
void bank_stop(unsigned owner) {
    if(!ready)return;
    for(unsigned i=1;i<VOICE_COUNT;i++)if(voices[i].clip && voices[i].emitter.owner==owner)silence(i);
    for(unsigned i=scene_count;i>0;i--)if(sounds[i-1].emitter.owner==owner)sounds[i-1]=sounds[--scene_count];
}
void bank_time_scale(float scale) {
    if(scale==effect_rate)return;
    sound_time();effect_rate=scale;
    if(ready)for(unsigned i=1;i<VOICE_COUNT;i++)if(voices[i].clip)ndspChnSetRate(FIRST_CHANNEL+i,22050*scale);
}
void bank_tick(void) {
    if(!ready || paused)return;
    double now=sound_time();
    for(unsigned i=0;i<VOICE_COUNT;i++)if(voices[i].clip && !voices[i].token){mix(i,0,1);fill(i);}
    for(unsigned i=0;i<scene_count;){
        if(update_sound(sounds+i,now,false))i++;
        else sounds[i]=sounds[--scene_count];
    }
}
void bank_listener(unsigned room,Point pos,float yaw){listener_room=room;listener=pos;heading=yaw;}
void bank_pause(bool pause) {
    sound_time();paused=pause;if(!ready)return;
    for(unsigned i=0;i<VOICE_COUNT;i++)if(voices[i].clip)ndspChnSetPaused(FIRST_CHANNEL+i,pause);
}
