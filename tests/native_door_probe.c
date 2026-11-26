/* Approach the actual closed bathroom/metro doors from their receiving rooms.
   No manual t_activate, forced exit flags, or disabled panel scripts. */
#include "director.h"
#include "campaign_bounds.h"
#include "portal.h"
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
CampaignProp campaign_props[CAMPAIGN_PROPS];
FILE *campaign_file(const char *name){return fopen(name,"rb");}
unsigned campaign_room_id(void){return director.state[0].room;}
typedef struct {char tag[4];uint32_t rooms,textures,start,props;Point spawn;float yaw;} Header;
typedef struct {char name[64];uint32_t batches,solids,exits,vertices;} Room;
typedef struct {uint32_t id,destination,points;float m[12];Point normal;} Exit;
static FILE *map;
static Header header;
static uint32_t *offsets;
static bool contact(unsigned from,unsigned to,Point center,Point *mapped){
 if(from>=header.rooms || to>=header.rooms)return false;
 assert(!fseek(map,offsets[1+from],SEEK_SET));Room room;assert(fread(&room,sizeof room,1,map)==1);
 for(unsigned i=0;i<room.exits;i++){
  Exit e;Point v[PORTAL_VERTICES];uint8_t indices[3*(PORTAL_VERTICES-2)];
  assert(fread(&e,sizeof e,1,map)==1 && e.points<=PORTAL_VERTICES);
  assert(fread(v,sizeof *v,e.points,map)==e.points);
  if(e.destination!=to)continue;
  unsigned count=portal_triangles(v,e.points,e.normal,indices);
  if(!portal_nearby(v,indices,count,e.normal,center))continue;
  *mapped=(Point){e.m[9]+e.m[0]*center.x+e.m[3]*center.y+e.m[6]*center.z,
    e.m[10]+e.m[1]*center.x+e.m[4]*center.y+e.m[7]*center.z,
    e.m[11]+e.m[2]*center.x+e.m[5]*center.y+e.m[8]*center.z};return true;
 }
 return false;
}
static void open_stage(const char *name){
 assert(director_open(name,NULL,NULL) && campaign_bounds_open(name));director_start();director_tick(0);
 char path[80];snprintf(path,sizeof path,"%s.mpc",name);map=fopen(path,"rb");assert(map);
 assert(fread(&header,sizeof header,1,map)==1 && !memcmp(header.tag,"MPC1",4));
 unsigned n=header.rooms+header.textures+2;offsets=calloc(n,4);assert(offsets && fread(offsets,4,n,map)==n);
 director_contact_frame(contact);director.controls=true;
}
static void close_stage(void){free(offsets);fclose(map);director_close();campaign_bounds_close();}
static unsigned passing_train_prop;
static bool train_contact(unsigned prop,unsigned room,const float *from,const float *to){
 return prop==passing_train_prop && room==4 && fabsf(from[9]-to[9])>.00001f;
}
int main(void){
 open_stage("part0_level1");
 unsigned sensor=director_find("::bedroom::door1::t_door_wc"),door=director_find("::bedroom::door1");
 assert(sensor<director.count && door<director.count && director.state[sensor].room==3);
 Player p={.position={-3.8f,-2,.5f}};
 // The same coordinates in a non-neighbour or behind a wall cannot fire it.
 director_player(&p,1,false);assert(director.state[sensor].enabled && director.state[door].clip==DIRECTOR_NONE);
 Player wall=p;wall.position.z=2.5f;
 director_player(&wall,4,false);assert(director.state[sensor].enabled);
 director_contact_frame(NULL);director_player(&p,4,false);assert(director.state[sensor].enabled);
 director_contact_frame(contact);director_player(&p,4,false);
 assert(!director.state[sensor].enabled && director.state[door].clip!=DIRECTOR_NONE);
 director_tick(.1f);float age=director.state[door].time;
 director_player(&p,4,false);assert(director.state[door].time==age);
 close_stage();
 open_stage("p1_l1-2");
 sensor=director_find("::control_room1::door_dummy::door_trigger");
 unsigned left=director_find("::control_room1::door_dummy::door1"),right=director_find("::control_room1::door_dummy::door2");
 assert(sensor<director.count && left<director.count && right<director.count);
 assert(!director.exits[3] && !director.exits[5]);
 p=(Player){.position={-3.3f,-2,-7.8f}};director_player(&p,2,false);
 assert(!director.state[sensor].enabled && director.state[left].clip!=DIRECTOR_NONE && director.state[right].clip!=DIRECTOR_NONE);
 assert(director.exits[3] && director.exits[5]);
 unsigned power=director_find("::p2::electricity_controller");
 unsigned train=director_find("::p2::service_train"),off=director_find("::p2::service_train_off");
 assert(power<director.count && train<director.count && off<director.count);
 unsigned train_prop=director.def[train].prop,off_prop=director.def[off].prop;
 Point cabin={16.8f,-1.4f,-12.4f};
 assert(campaign_props[train_prop].hidden && !campaign_props[off_prop].hidden);
 assert(!campaign_bounds_overlap(train_prop,cabin,0) && campaign_bounds_overlap(off_prop,cabin,0));
 director.collision_dirty=false;unsigned revision=director.acoustic_revision[11];
 director_event(power,EV_SEND,"start");
 assert(!campaign_props[train_prop].hidden && campaign_props[off_prop].hidden);
 assert(director.collision_dirty && director.acoustic_revision[11]>revision);
 assert(campaign_bounds_overlap(train_prop,cabin,0) && !campaign_bounds_overlap(off_prop,cabin,0));
 assert(campaign_props[train_prop].solid && campaign_props[off_prop].solid); // hiding preserves authored collision settings
 // The rear cab is in room 7; the train's DO is authored in room 11.
 // Retain the portal frame while applying the DO's next pose exactly once.
 float placement[12]={1,0,0,0,1,0,0,0,1,-38,0,11.5f};
 Point rear={-10.7f,-1.41f,-.9f};
 PlatformContact support=campaign_platform_placed(train_prop,rear,placement);
 assert(support.prop==(int)train_prop);
 CampaignProp saved_train=campaign_props[train_prop];
 campaign_props[train_prop].moved=true;
 float shift[12]={1,0,0,0,1,0,0,0,1,-2,0,0};memcpy(campaign_props[train_prop].matrix,shift,sizeof shift);
 campaign_ride(support,&rear);assert(fabsf(rear.x+12.7f)<.0001f && fabsf(rear.y+1.41f)<.0001f && fabsf(rear.z+.9f)<.0001f);
 campaign_props[train_prop]=saved_train;
 // The floor is hit in room 7, but damage belongs to the object in room 11.
 float hp=director.state[train].hp;
 assert(hp>10 && director.state[train].room==11);
 director_bullet((Point){-10.7f,-1.1f,-.9f},(Point){-10.7f,-1.41f,-.9f},7,train_prop,10,0);
 assert(director.state[train].hp==hp-10);
 close_stage();puts("Bathroom and metro sensors open their actual door scripts across a closed aperture; walls and other rooms do not activate them.");
 puts("Original electricity script swaps train variants and invalidates collision/acoustics; hidden variants cannot support or trap Max.");
 puts("A train contact keeps its receiving-room frame while the original DO moves; no duplicate translation or floor drop.");
 puts("A shot in the rear cab applies damage to the original prop in its owning room.");
 open_stage("tutorial");
 unsigned volume=director_find("::street::trigger");assert(volume<director.count);
 unsigned volume_prop=director.def[volume].prop;assert(volume_prop<CAMPAIGN_PROPS && !campaign_props[volume_prop].hidden);
 // On the middle of the wall, Max's torso is above this receiving-room
 // volume. His legs still touch it; a centre-only sphere missed that contact.
 p=(Player){.position={23.548f,-6.4f,.5f}};
 director_player(&p,2,false);assert(!campaign_props[volume_prop].hidden);
 Player outside=p;outside.position.z=7;
 director_player(&outside,1,false);assert(!campaign_props[volume_prop].hidden);
 director.collision_dirty=false;director_player(&p,1,false);
 assert(campaign_props[volume_prop].hidden && director.collision_dirty);
 close_stage();puts("The real tutorial exit volume detects Max's legs on the middle of the wall before it can block room transfer.");
 open_stage("p1_l1-1");
 train=director_find("::startroom::train");assert(train<director.count);
 passing_train_prop=director.def[train].prop;
 assert(passing_train_prop<CAMPAIGN_PROPS && campaign_props[passing_train_prop].solid);
 director.state[0].invulnerable=director.state[0].immortal=false;
 director_set_health(0,100);director_motion_guard(train_contact);
 director_event(train,EV_SEND,"start");
 assert(director.state[train].clip!=DIRECTOR_NONE);
 for(unsigned frame=0;frame<600 && !director.state[0].dead;frame++)director_tick(1.f/60);
 assert(director.state[0].dead && director.state[0].hp<=0);
 director_motion_guard(NULL);close_stage();
 puts("A collision in the passing train's motion dispatches its original fatal player event.");
}
