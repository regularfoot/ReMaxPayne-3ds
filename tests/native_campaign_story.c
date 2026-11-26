/* Run authored event chains quickly against the same director as the 3DS build. */
#include "director.h"
#include "pose_bank.h"
#include "campaign_world.h"
#include "campaign_nav.h"
#include "campaign_bounds.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
CampaignProp campaign_props[CAMPAIGN_PROPS];
static const char *root;
static NavAgent *agents;
static unsigned training_tips;
static void record_tip(unsigned owner,const char *op,const char *const *args,unsigned count){
 if(agents && owner<director.count)campaign_nav_event(agents+owner,op);
 if(!count || strcmp(op,"mphm_printtipalways"))return;
 const char *const required[]={"tip_01","tip_06","tip_12","tip_37","tip_44","tip_49","tip_50","tip_54"};
 for(unsigned i=0;i<sizeof required/sizeof *required;i++)if(!strcmp(args[0],required[i]))training_tips|=1u<<i;
}
static unsigned room;
unsigned campaign_room_id(void){return room;}

FILE *campaign_file(const char *name){char path[512];snprintf(path,sizeof path,"%s/%s",root,name);return fopen(path,"rb");}
typedef struct {unsigned id;char action[32];} Step;
static PlatformContact *support;
void room_move(Player *p,float dx,float dz){p->position.x+=dx;p->position.z+=dz;}
static void advance(unsigned ticks){
 for(unsigned k=0;k<ticks && !director.next_level && !director.finished && !director.failed;k++){
  float dt=director.speed/30.f;
  for(unsigned i=1;i<director.count;i++) if(director.def[i].flags&4) support[i]=campaign_remote_floor(director.state[i].room,director.state[i].position);
  director_camera_tick(1.f/30.f);director_tick(dt);
  for(unsigned i=1;i<director.count;i++) if(director.def[i].flags&4) {
    float before=director.state[i].position.y;campaign_ride(support[i],&director.state[i].position);agents[i].riding=fabsf(before-director.state[i].position.y)>.0001f;
  }
  for(unsigned i=1;i<director.count;i++){
   EntityState *e=director.state+i;
   if(!(director.def[i].flags&4) || agents[i].riding || e->dead || e->target>=director.count)continue;
   EntityState *t=director.state+e->target;Point target;director_pose(e->target,&target,NULL,NULL);
   float dx=target.x-e->position.x,dz=target.z-e->position.z,dist=hypotf(dx,dz);
   if(t->room!=e->room || fabsf(e->position.y-target.y)>1.1f || (!director.controls && dist>2))campaign_nav_step(agents+i,&e->position,&e->room,target,t->room,dt,room,&e->yaw,!strncmp(director_text(e->ai),"crouch",6));
   else if(dist>.2f){e->position.x+=dx*fminf(dt*2,dist)/dist;e->position.z+=dz*fminf(dt*2,dist)/dist;}
  }
  {Player scripted={.position=director.state[0].position,.yaw=director.state[0].yaw};director_player(&scripted,director.state[0].room,false);}
  director_character_contacts();
  if(director.novel){director.note_count=0;director.novel=false;}
 }
}
static void operate(Step *s){
 unsigned i=s->id;const EntityDef *d=director.def+i;EntityState *e=director.state+i;
 if(!e->enabled)return;
 if(!strcmp(s->action,"death") || !strcmp(s->action,"do_ondeath")){
   if(!director.controls || e->invulnerable || e->immortal || e->dead)return;
   director_hit(i,e->hp);
 }else if(!strcmp(s->action,"do_bulletcollides") || (!strcmp(s->action,"t_activate") && d->trigger==2)){
   Point end;director_pose(i,&end,NULL,NULL);Point start=end;start.z-=.3f;
   director_bullet(start,end,d->room,d->prop==DIRECTOR_NONE?-1:d->prop,10,0);
 }else {
   if(!director.controls)return;
   Player p={0};director_pose(i,&p.position,NULL,NULL);p.position.y-=.85f;p.position.z-=.05f;
   room=d->room;Player out=p;out.position.x+=20;director_player(&out,room,false);
   director_player(&p,room,true);
 }
}
static void visit(const char *name,const char *event,unsigned duration) {
 for(unsigned i=0;i<director.count;i++)if(!strcmp(director_text(director.def[i].name),name)){
    Step step={.id=i};strcpy(step.action,event);operate(&step);advance(duration);
    return;
 }
}
static bool test_lockers(void){
 unsigned doors[4];
 for(unsigned i=0;i<4;i++){
  char name[64];snprintf(name,sizeof name,"::st01::locker01::t_locker_door%02u",i+1);
  doors[i]=director_find(name);
  if(doors[i]>=director.count || !director.state[doors[i]].enabled)return false;
 }
 room=director.state[doors[1]].room;
 Player p={0};director_pose(doors[1],&p.position,NULL,NULL);p.position.y-=.9f;p.position.z-=.18f;
 director.controls=true;
 if(director_player(&p,room,false) || !director.can_use || !director_player(&p,room,true))return false;
 for(unsigned i=0;i<4;i++)if(director.state[doors[i]].enabled!=(i!=1))return false;
 if(!director.can_use)return false; // Another locker remains within reach.
 p.position.x+=30;
 return !director_player(&p,room,true) && !director.can_use;
}
static bool test_wounds(void){
 unsigned id=director_find("::bridget01::e2");
 if(id>=director.count || strcmp(director_text(director.def[id].model),"c1_all_johnny"))return false;
 EntityState *e=director.state+id;
 float full=director.def[id].max_health;
 const SkinRules *rules=pose_skin_rules(pose_bank_skin("c1_all_johnny"));
 if(!rules || full<=0 || e->dead || e->invulnerable)return false;
 director_set_health(id,full);
 director_hit(id,full*(1-rules->wounded)+.01f);
 if(!e->wounded || e->dead)return false;
 director_hit(id,.01f);
 if(!e->wounded || e->dead)return false;
 director_set_health(id,full);
 return !e->wounded && !e->dead;
}

static bool test_rats(void){
 unsigned rats[2][3];
 for(unsigned team=0;team<2;team++)for(unsigned member=0;member<3;member++){
  char name[64];
  snprintf(name,sizeof name,"::rat_room2::rat_controller%u::r%u",team+1,member+1);
  rats[team][member]=director_find(name);
  if(rats[team][member]>=director.count)return false;
 }
 EntityState *first=director.state+rats[0][0];
 Player p={.position=first->position};
 director_player(&p,first->room,false);
 for(unsigned team=0;team<2;team++)for(unsigned member=0;member<3;member++){
  const EntityState *rat=director.state+rats[team][member];
  if(rat->activated || rat->hostile || strcmp(director_text(rat->weapon),"empty"))return false;
 }
 unsigned trigger=director_find("::startroom::rat_trigger"),lamp=director_find("::rat_room2::lamp");
 if(trigger>=director.count || lamp>=director.count)return false;
 Step lamp_shot={lamp,"do_bulletcollides"},trigger_shot={trigger,"do_bulletcollides"};
 operate(&lamp_shot);
 if(director.state[lamp].clip!=DIRECTOR_NONE || strcmp(director_text(director.state[lamp].state),"disabled"))return false;
 operate(&trigger_shot);
 if(first->aim!=DIRECTOR_NONE || director.state[trigger].dead)return false;
 operate(&trigger_shot);
 if(!director.state[trigger].dead || strcmp(director_text(director.state[lamp].state),"enabled"))return false;
 for(unsigned team=0;team<2;team++)for(unsigned member=0;member<3;member++)
  if(director.state[rats[team][member]].aim!=0)return false;
 operate(&lamp_shot);
 if(director.state[lamp].clip==DIRECTOR_NONE || first->aim!=0)return false;
 for(unsigned tick=0;tick<60;tick++)director_tick(1.f/30.f);
 director_player(&p,first->room,false);
 for(unsigned team=0;team<2;team++)for(unsigned member=0;member<3;member++){
  const EntityState *rat=director.state+rats[team][member];
  if(rat->aim!=rats[1-team][member] || !rat->activated || !rat->hostile ||
     !strcmp(director_text(rat->weapon),"empty"))return false;
 }
 return true;
}

static bool test_passive_rats(void){
 unsigned count=0;
 for(unsigned id=1;id<director.count;id++){
  if(strcmp(director_text(director.def[id].model),"rat"))continue;
  count++;
  director_activate_character(id);
  const EntityState *rat=director.state+id;
  const char *weapon=director_text(rat->weapon);
  if(rat->hostile || (*weapon && strcmp(weapon,"empty")))return false;
 }
 return count>0;
}

static bool test_scripted_combat(void){
 unsigned id=director_find("::wc::junkie"),opponent=director_find("::wc::e01");
 if(id>=director.count || opponent>=director.count)return false;
 const EntityState *junkie=director.state+id;
 director_activate_character(id);
 if(junkie->activated || junkie->hostile || strcmp(director_text(junkie->weapon),"empty"))return false;
 director_activate_character(opponent);
 if(junkie->aim!=opponent)return false;
 director_activate_character(id);
 if(!junkie->activated || !junkie->hostile || strcmp(director_text(junkie->weapon),"beretta"))return false;
 director_hit(opponent,director.state[opponent].hp);
 return junkie->aim==0;
}

int main(int argc,char **argv){
 if(argc!=4 && !(argc==5 && (!strcmp(argv[4],"--lose-escort") || !strcmp(argv[4],"--lockers") ||
                             !strcmp(argv[4],"--wounds") || !strcmp(argv[4],"--rats") ||
                             !strcmp(argv[4],"--passive-rats") || !strcmp(argv[4],"--scripted-combat"))))return 2;
 root=argv[1];FILE *input=fopen(argv[3],"r");if(!input)return 3;
 if(!director_open(argv[2],record_tip,stderr))return 4;
 if(argc==5 && !strcmp(argv[4],"--wounds")){
  bool ok=test_wounds();fclose(input);director_close();return ok?0:9;
 }
 char map[96];snprintf(map,sizeof map,"%s.mpc",argv[2]);FILE *f=campaign_file(map);
 if(!f || fseek(f,12,SEEK_SET) || fread(&room,4,1,f)!=1)return 5;
 fseek(f,4,SEEK_CUR);fread(&director.state[0].position,sizeof(Point),1,f);fclose(f);director.state[0].room=room;
 Step steps[512];unsigned count=0;char name[512],action[32];
 while(fscanf(input,"%511s %31s",name,action)==2){
  for(char *p=name;*p;p++)if(*p=='|')*p=' ';
  unsigned i=0;while(i<director.count && strcmp(name,director_text(director.def[i].name)))i++;
  if(i==director.count || count==512)return 6;
  steps[count]=(Step){.id=i};strcpy(steps[count++].action,action);
 }
 fclose(input);support=calloc(director.count,sizeof(*support));if(!campaign_bounds_open(argv[2]))return 8;agents=calloc(director.count,sizeof(*agents));if(!campaign_nav_open(argv[2]))return 7;director_start();advance(600);
 if(!strcmp(argv[2],"part0_level1")){
  unsigned id=director_find("::teleport::michelle");
  if(id>=director.count || !director.state[id].dead)return 10;
  EntityState *s=director.state+id;
  if(strcmp(pose_bank_name(s->custom_pose),"michelle_dead") || s->pose_seconds!=pose_bank_duration(s->custom_pose))return 11;
  s->custom_pose=0;director_restore_corpses();
  if(strcmp(pose_bank_name(s->custom_pose),"michelle_dead") || s->pose_seconds!=pose_bank_duration(s->custom_pose))return 12;
 }
 if(argc==5){
  bool passed;
  if(!strcmp(argv[4],"--lockers"))passed=test_lockers();
  else if(!strcmp(argv[4],"--rats"))passed=test_rats();
  else if(!strcmp(argv[4],"--passive-rats"))passed=test_passive_rats();
  else if(!strcmp(argv[4],"--scripted-combat"))passed=test_scripted_combat();
  else {visit("::b::helper","death",1800);passed=director.reload;}
  campaign_nav_close();campaign_bounds_close();free(agents);free(support);director_close();
  return passed?0:9;
 }
 if(!strcmp(argv[2],"tutorial")){
  visit("::startroom::use_01","t_activate",300);
  visit("::startroom::t_van","t_activate",600);
  visit("::alley2::use_03","t_activate",300);
  visit("::alley2::use_04","t_activate",300);
  visit("::street::use_04","t_activate",450);
  visit("::street::t_dispenser","t_activate",180);
  for(unsigned i=1;i<=3;i++){
   char name[48];snprintf(name,sizeof name,"::teleport::e%02u",i);visit(name,"death",200);
  }
  visit("::subway::t_medikit","t_activate",260);
  visit("::subway::t_health","t_activate",480);
  visit("::street::t_01","t_activate",480);
  for(unsigned i=4;i<=6;i++){
   char name[48];snprintf(name,sizeof name,"::teleport::e%02u",i);visit(name,"death",200);
  }
  advance(900);visit("::secret::player collide trigger 00","t_activate",90);
  visit("::street::t_dispenser","t_activate",150);
  bool repeat=false;
  for(unsigned i=1;i<director.count;i++)
   if(!strcmp(director_text(director.def[i].name),"::teleport::e01"))repeat=!director.state[i].dead;
  printf("tutorial\t%s\n",training_tips==255 && repeat?"TRAINING COMPLETE":"PENDING");
  if(training_tips!=255 || !repeat)fprintf(stderr,"Tutorial tips %x, repeat %d\n",training_tips,repeat);
  campaign_nav_close();campaign_bounds_close();free(agents);free(support);director_close();return 0;
 }
 if(!strcmp(argv[2],"p1_l3b-4")){
  visit("::startroom::door_trigger","t_activate",2000);
  visit("::blood02::pt1","t_activate",2400);
  visit("::debrisroom::door_dummy1::door::t2","t_activate",1800);
  visit("::dirtyroom::gognitti_t1","t_activate",1900);
  visit("::hallway::door_dummy1::door::t2","t_activate",2100);
 }
 if(!strcmp(argv[2],"p1_l4-3"))visit("::startroom::gognitti_activate_trigger","t_activate",4800);
 if(!strcmp(argv[2],"p3_l3-2")){
  visit("::b::door_dummy10::trigger","t_activate",2100);
  visit("::b::trigger_scientist_to_move","t_activate",1800);
  visit("::shaft2::elevator::detect_player_trigger","t_activate",1800);
  visit("::upper::pt1","t_activate",1500);
  visit("::a::block_a_pt1","t_activate",1400);
  visit("::startroom::teleport::mercenary1","death",200);
  visit("::startroom::teleport::mercenary2","death",2100);
  visit("::airlock::player_entered","t_activate",900);
  visit("::airlock::code_trigger","t_activate",900);
  visit("::airlock::detect_player2","t_activate",900);
 }
 if(!strcmp(argv[2],"p3_l7-3")){
  const char *entrance[]={"::startroom::close_door_trigger","::startroom::ct1","::startroom::horne_shooting_trigger","::outside::kattosys::open_door_trigger"};
  for(unsigned j=0;j<4;j++)for(unsigned i=0;i<director.count;i++)if(!strcmp(director_text(director.def[i].name),entrance[j])){
   Step entry={.id=i};strcpy(entry.action,"t_activate");operate(&entry);advance(j==3?3600:120);
  }
 }
 for(unsigned pass=0;pass<32 && !director.next_level && !director.finished && !director.failed;pass++){
  for(unsigned j=0;j<count && !director.next_level && !director.finished;j++){
   operate(steps+j);advance(5);
  }
  if(pass>=4 && director.controls && !director.next_level && !director.finished) {
   for(unsigned i=1;i<director.count && director.controls && !director.next_level && !director.finished;i++) {
    const EntityDef *d=director.def+i;
    if(d->trigger==DIRECTOR_NONE || !director.state[i].enabled)continue;
    Step visit={.id=i};strcpy(visit.action,"t_activate");operate(&visit);advance(12);
   }
  }
  advance(pass%4==3?900:10);
 }
 printf("%s\t%s\n",argv[2],director.finished?"CREDITS":director.next_level?director_text(director.next_level):director.failed?"SCRIPT ERROR":"PENDING");
 campaign_nav_close();campaign_bounds_close();free(agents);free(support);
 director_close();return 0;
}
