#include "animation_timing.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct {uint32_t op,mode;float delta,start,seconds;AnimationSource source;AnimationTimeline other,before,expected;} Case;
static void read_exact(FILE *f,void *out,size_t n){if(fread(out,1,n,f)!=n)abort();}
static int compare(const AnimationTimeline *a,const AnimationTimeline *b){
    if(a->clip!=b->clip || a->previous_clip!=b->previous_clip || a->stage!=b->stage || a->generation!=b->generation)return 0;
    if(a->current.loop!=b->current.loop || a->previous.loop!=b->previous.loop)return 0;
    for(unsigned i=0;i<4;i++)if(fabsf(((const float*)&a->current)[i]-((const float*)&b->current)[i])>.00002f ||
                                  fabsf(((const float*)&a->previous)[i]-((const float*)&b->previous)[i])>.00002f)return 0;
    return fabsf(a->seconds-b->seconds)<.00002f && fabsf(a->weight-b->weight)<.00002f;
}
static void show(const AnimationTimeline *t){
 printf("stage %u clip %u prev %u generation %u blend %g weight %g\n",t->stage,t->clip,t->previous_clip,t->generation,t->seconds,t->weight);
 printf(" current %g %g %g %g %u\n",t->current.time,t->current.end,t->current.loop_start,t->current.origin,t->current.loop);
 printf(" previous %g %g %g %g %u\n",t->previous.time,t->previous.end,t->previous.loop_start,t->previous.origin,t->previous.loop);
}
int main(int argc,char **argv){
 if(argc!=2 || sizeof(Case)!=224 || sizeof(AnimationTimeline)!=64)return 2;
 FILE *f=fopen(argv[1],"rb");if(!f)return 2;struct{char tag[4];uint32_t count;} h;read_exact(f,&h,sizeof h);if(memcmp(h.tag,"XAT1",4))return 2;
 unsigned failed=0;
 for(unsigned i=0;i<h.count;i++){
  Case c;read_exact(f,&c,sizeof c);AnimationTimeline actual=c.before;
  if(c.op)animation_request(&actual,c.source,c.mode,c.start,c.seconds,&c.other);else animation_step(&actual,c.delta);
  if(!compare(&actual,&c.expected)){
   if(failed++<5){printf("CASE %u op %u mode %u dt %g start %g\n",i,c.op,c.mode,c.delta,c.start);puts("BEFORE");show(&c.before);puts("ACTUAL");show(&actual);puts("EXPECTED");show(&c.expected);}
  }
 }
 if(fgetc(f)!=EOF)return 2;fclose(f);printf("Xbox animation clock oracle: %u cases, %u failures\n",h.count,failed);return failed?1:0;
}
