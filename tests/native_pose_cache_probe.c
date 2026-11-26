// Exercise every frame of both retail movement sets after a load. Closing the
// file underneath the cache makes any render-time I/O an immediate failure.
#include "../source/native/pose_bank.c"
#include <assert.h>
FILE *campaign_file(const char *name){return fopen(name,"rb");}
static unsigned reloads;
static void cue(unsigned actor,const char *target,const char *op,const char *const args[],unsigned count){
 (void)actor;(void)target;(void)args;(void)count;
 if(!strcmp(op,"c_reload"))reloads++;
}
int main(void){
 const char *models[]={"max_payne","young_max_payne"};
 const char *actions[]={"stand","use","run","walkbackward","crouch","strafeleft","straferight",
  "forwardleft","forwardright","backwardleft","backwardright","holsterweapon","getweapon"};
 for(unsigned model=0;model<2;model++){
  assert(pose_bank_open());unsigned skin=pose_bank_skin(models[model]);
  assert(pose_bank_preload_player(skin) && resident_bytes<=RESIDENT_LIMIT);
  FILE *saved=file;file=NULL;unsigned checked=0;
  for(unsigned i=0;i<sizeof actions/sizeof *actions;i++)for(unsigned heavy=0;heavy<2;heavy++){
   char name[64];snprintf(name,sizeof name,"%s%s",heavy?"2_":"",actions[i]);
   unsigned clip=pose_clip_id(pose_motion_action(skin,name));if(!clip)continue;
   assert(resident[clip]);Matrix pose[64];
   for(unsigned f=0;f<clips[clip].frames;f++)assert(pose_bank_read_local(clip,f,bone_count,pose));
   checked++;
  }
  printf("%s: %u movement/interaction channels, %zu resident bytes; every frame readable without a file\n",models[model],checked,resident_bytes);
  unsigned reload=pose_motion_action(skin,"reloadpumpshotgun");
  assert(reload && pose_bank_loops(reload));
  // The insertion cue is near the end of the retail loop. A single frame
  // crossing its end must deliver that cue before processing the new cycle.
  reloads=0;float duration=pose_bank_duration(reload);
  pose_events_advance(0,reload,0,duration*.8f,true,cue);
  pose_events_advance(0,reload,duration*.8f,.01f,true,cue);
  assert(reloads==1);
  pose_events_advance(0,reload,.01f,duration*.8f,true,cue);
  pose_events_advance(0,reload,duration*.8f,.01f,true,cue);
  assert(reloads==2);
  file=saved;pose_bank_close();
 }
}
