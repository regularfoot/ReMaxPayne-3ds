/* Use the actual audio bank and its real cache/player, replacing only DSP output. */
#include <stdio.h>
#include <assert.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <pthread.h>
#include <time.h>
static atomic_uint disk_reads,main_reads;
static atomic_bool hold_reads,blocked;
static pthread_t game_thread;
static size_t observed_read(void *p,size_t size,size_t count,FILE *file){
 disk_reads++;
 if(pthread_equal(pthread_self(),game_thread))main_reads++;
 else if(hold_reads){
  blocked=true;
  while(hold_reads){struct timespec wait={0,1000000};nanosleep(&wait,NULL);}
 }
 return fread(p,size,count,file);
}
#define fread observed_read
#include "../source/native/audio_bank.c"
#undef fread
Director director;
UiSettings ui_settings={.volume=4,.music=3};
FILE *campaign_file(const char *name){return fopen(name,"rb");}
void director_pose(unsigned id,Point *p,Point *forward,Point *up){(void)id;(void)forward;(void)up;if(p)*p=(Point){0};}
bool campaign_sound_path(unsigned room,Point ear,unsigned source_room,Point source,const bool *enabled,Point *apparent,float *distance){
 (void)room;(void)ear;(void)source_room;(void)enabled;*apparent=source;*distance=0;return true;
}
int main(void){
 game_thread=pthread_self();
 assert(bank_open() && host_linear_allocations==VOICE_COUNT);
 bank_preload("projectiles","bullethit_generic");bank_preload("projectiles","casehit_generic");
 bank_preload("weapons","shoot_beretta");
 // Fill the remainder with other original weapon sounds. The hot variants
 // must remain complete when the shared stage cache reaches its limit.
 for(unsigned i=0;i<record_count;i++)if(!strncmp(records[i].key,"weapons/",8))bank_preload("weapons",records[i].key+8);
 assert(prefix_bytes<=PREFIX_LIMIT);
 unsigned variants=0;
 for(unsigned i=0;i<record_count;i++){
  const Record *r=records+i;
  if(strcmp(r->key,"projectiles/bullethit_generic") && strcmp(r->key,"projectiles/casehit_generic") && strcmp(r->key,"weapons/shoot_beretta"))continue;
  int16_t *memory=voices[1].samples;uint8_t *ahead=voices[1].read_ahead;
  disk_reads=0;play(1,r,(Emitter){0},0,1,0,0);
  for(unsigned step=0;voices[1].clip && step<256;step++){
   for(unsigned j=0;j<BUFFERS;j++)voices[1].buffers[j].status=0;
   fill(1);
  }
  assert(!voices[1].clip && !disk_reads);
  assert(voices[1].samples==memory && voices[1].read_ahead==ahead);
  variants++;
 }
 assert(variants==11 && host_linear_allocations==VOICE_COUNT);
 bank_clear();assert(!host_linear_frees && !prefix_bytes);
 // Simulate a slow FS operation on the stream worker. Frame ticks must keep
 // returning without reading disk or killing the waiting music channel.
 const Record *long_clip=NULL;
 for(unsigned i=0;i<record_count;i++)if(records[i].bytes>128*1024){long_clip=records+i;break;}
 assert(long_clip);hold_reads=true;blocked=false;main_reads=0;
 play(0,long_clip,(Emitter){0},0,1,0,0);
 for(unsigned i=0;i<1000 && !blocked;i++){struct timespec wait={0,1000000};nanosleep(&wait,NULL);}
 assert(blocked && voices[0].clip==long_clip && !voices[0].offset);
 for(unsigned i=0;i<120;i++)bank_tick();
 assert(!main_reads && voices[0].clip==long_clip && !voices[0].offset);
 hold_reads=false;
 for(unsigned i=0;i<1000 && !voices[0].offset;i++){
  bank_tick();struct timespec wait={0,1000000};nanosleep(&wait,NULL);
 }
 assert(voices[0].offset && !main_reads);
 FILE *reference=fopen("audio.bank","rb");assert(reference && !fseek(reference,long_clip->at,SEEK_SET));
 int16_t expected[SAMPLES];assert(fread(expected,2,SAMPLES,reference)==SAMPLES);fclose(reference);
 assert(!memcmp(expected,voices[0].samples,sizeof expected));

 // A door can expose a running emitter at any sample, including just before
 // the end of its preloaded prefix. Verify both PCM chunks without a reread.
 bank_clear();
 const Record *loop=find_clip("ambient/subway_train_loop");
 assert(loop && (loop->flags&3)==3 && loop->bytes>PREFIX_BYTES+READ_AHEAD);
 bank_preload("ambient","subway_train_loop");
 const unsigned tail=231,offset=PREFIX_BYTES-2*tail;
 int16_t crossing[SAMPLES+tail];
 reference=fopen("audio.bank","rb");
 assert(reference && !fseek(reference,loop->at+offset,SEEK_SET));
 assert(fread(crossing,2,SAMPLES+tail,reference)==SAMPLES+tail);
 fclose(reference);
 main_reads=0;blocked=false;hold_reads=true;
 play(1,loop,(Emitter){.spatial=true},0,1,offset,0);
 for(unsigned i=0;i<1000 && !blocked;i++){struct timespec wait={0,1000000};nanosleep(&wait,NULL);}
 assert(blocked && voices[1].offset==PREFIX_BYTES && voices[1].buffers[0].nsamples==tail);
 assert(!memcmp(crossing,voices[1].samples,tail*2));
 hold_reads=false;
 for(unsigned i=0;i<1000 && !voices[1].buffers[1].nsamples;i++){
  fill(1);struct timespec wait={0,1000000};nanosleep(&wait,NULL);
 }
 assert(!main_reads && voices[1].buffers[1].nsamples==SAMPLES);
 assert(!memcmp(crossing+tail,voices[1].samples+SAMPLES,SAMPLES*2));
 bank_close();assert(host_linear_frees==host_linear_allocations);
 printf("All %u retail shot/impact/casing variants play without disk reads or voice reallocation within the 1 MiB cache.\n",variants);
 puts("120 frame ticks stay responsive while streaming FS is blocked; pending music resumes with the original PCM.");
 puts("Unaligned positional audio continues across the preload boundary without discarding cached samples.");
}
