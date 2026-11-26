// Walk into the actual prologue stair sensor using the production collision
// solver. Its DO_CharacterCollides message must remove its physical barrier.
#include "director.h"
#include "campaign_bounds.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
CampaignProp campaign_props[CAMPAIGN_PROPS];
FILE *campaign_file(const char *name){return fopen(name,"rb");}
unsigned campaign_room_id(void){return director.state[0].room;}
typedef struct {int32_t prop;unsigned first,count;} Range;
static Range ranges[256];static unsigned range_count;
static bool touch(unsigned prop){
 for(unsigned i=0;i<range_count;i++)if(ranges[i].prop==(int)prop && scene_body_touched(ranges[i].first,ranges[i].count))return true;
 return false;
}
int main(void){
 assert(director_open("part0_level1",NULL,NULL) && campaign_bounds_open("part0_level1"));director_start();director_tick(0);
 unsigned sensor=director_find("::entrance06::cin_trigger");assert(sensor<director.count);
 unsigned prop=director.def[sensor].prop;assert(prop<CAMPAIGN_PROPS && campaign_props[prop].solid);
 FILE *f=campaign_file("part0_level1.mpc");assert(f);unsigned char header[36];assert(fread(header,36,1,f)==1);
 uint32_t rooms,textures;memcpy(&rooms,header+4,4);memcpy(&textures,header+8,4);
 uint32_t *offsets=calloc(rooms+textures+2,4);assert(offsets && fread(offsets,4,rooms+textures+2,f)==rooms+textures+2);
 assert(!fseek(f,offsets[10],SEEK_SET)); // receiving room 9: entrance06
 struct {char name[64];uint32_t batches,solids,exits,vertices;} room;assert(fread(&room,sizeof room,1,f)==1);
 for(unsigned i=0;i<room.exits;i++){
  unsigned char e[72];uint32_t points;assert(fread(e,sizeof e,1,f)==1);memcpy(&points,e+8,4);assert(!fseek(f,12*points,SEEK_CUR));
 }
 assert(!fseek(f,12*room.batches+28*room.vertices,SEEK_CUR));
 Triangle *faces=NULL;unsigned count=0;
 for(unsigned i=0;i<room.solids;i++){
  struct {int32_t prop;uint32_t count;} s;assert(fread(&s,sizeof s,1,f)==1);
  faces=realloc(faces,(count+s.count)*sizeof *faces);assert(faces && fread(faces+count,sizeof *faces,s.count,f)==s.count);
  ranges[range_count++]=(Range){s.prop,count,s.count};count+=s.count;
 }
 fclose(f);free(offsets);uint8_t *masks=malloc(count);assert(masks);
 for(unsigned i=0;i<range_count;i++){
  Range r=ranges[i];unsigned mask=r.prop<0?15:campaign_props[r.prop].collision_mask;
  if(r.prop>=0 && (campaign_props[r.prop].hidden || !campaign_props[r.prop].solid))mask=0;
  memset(masks+r.first,mask,r.count);
 }
 assert(scene_open_masks(faces,count,masks));director_mesh_contact(touch);director.controls=true;
 Player p={.position={1,-1.75f,1.1f}};Jump jump={0};bool fired=false;
 // A point outside the actual swept path cannot activate the sensor.
 Player distant={.position={-.5f,-1.75f,1.1f}};director_player(&distant,9,false);assert(campaign_props[prop].solid);
 for(unsigned frame=0;frame<30;frame++){
  scene_move_gravity(&p,&jump,0,-.13125f,.025f,false,20,7.5f);
  director_player(&p,9,false);
  if(!campaign_props[prop].solid){fired=true;break;}
 }
 assert(fired); // previously Max stopped at z≈0.62 and the event never fired
 assert(p.position.z>.390625f); // no penetration was required to emit contact
 director_mesh_contact(NULL);scene_close();free(faces);free(masks);director_close();campaign_bounds_close();
 puts("Actual prologue stair sensor receives swept capsule contact and disables its own blocker before penetration; adjacent empty space does not trigger it.");
}
