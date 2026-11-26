#include "portal.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct {char tag[4];uint32_t rooms,textures,start,props;Point spawn;float yaw;} Header;
typedef struct {char name[64];uint32_t batches,solids,exits,vertices;} Room;
typedef struct {uint32_t id,destination,points;float m[12];Point normal;} Exit;
static Point plus(Point a,Point b,float s){return (Point){a.x+s*b.x,a.y+s*b.y,a.z+s*b.z};}
static Point apply(const float *m,Point p){return (Point){m[9]+p.x*m[0]+p.y*m[3]+p.z*m[6],m[10]+p.x*m[1]+p.y*m[4]+p.z*m[7],m[11]+p.x*m[2]+p.y*m[5]+p.z*m[8]};}
int main(int argc,char **argv){
 assert(argc==2);FILE *f=fopen(argv[1],"rb");assert(f);Header h;assert(fread(&h,sizeof h,1,f)==1 && !memcmp(h.tag,"MPC1",4));
 uint32_t *offset=malloc((h.rooms+h.textures+2)*4);assert(offset && fread(offset,4,h.rooms+h.textures+2,f)==h.rooms+h.textures+2);
 unsigned portals=0,sweeps=0,nonvertical=0,triangles=0;
 for(unsigned room=0;room<h.rooms;room++){
  assert(!fseek(f,offset[1+room],SEEK_SET));Room r;assert(fread(&r,sizeof r,1,f)==1);
  for(unsigned i=0;i<r.exits;i++){
   Exit e;Point vertices[PORTAL_VERTICES];uint8_t indices[3*(PORTAL_VERTICES-2)];
   assert(fread(&e,sizeof e,1,f)==1 && e.points>=3 && e.points<=PORTAL_VERTICES && e.destination<h.rooms);
   assert(fread(vertices,sizeof(Point),e.points,f)==e.points);unsigned count=portal_triangles(vertices,e.points,e.normal,indices);assert(count);
   float norm=e.normal.x*e.normal.x+e.normal.y*e.normal.y+e.normal.z*e.normal.z;assert(fabsf(norm-1)<.0001f);
   portals++;nonvertical+=fabsf(e.normal.y)>.2f;
   for(unsigned k=0;k<count;k+=3){
    Point a=vertices[indices[k]],b=vertices[indices[k+1]],c=vertices[indices[k+2]];
    Point center={(a.x+b.x+c.x)/3,(a.y+b.y+c.y)/3,(a.z+b.z+c.z)/3};
    assert(portal_contains(vertices,indices,count,e.normal,center));triangles++;
    for(unsigned fast=0;fast<3;fast++){
     float step=fast==0?.03f:fast==1?.26f:1.2f;
     Point from=plus(center,e.normal,step*.5f),to=plus(center,e.normal,-step*.5f);float time;
     assert(portal_departure(vertices,indices,count,e.normal,from,to,(Point){0},&time));
     if(fabsf(time-.5f)>.01f){fprintf(stderr,"plane deviation %s %u %u t=%f step=%f\n",argv[1],room,e.id,time,step);return 3;}sweeps++;
     assert(!portal_departure(vertices,indices,count,e.normal,to,from,(Point){0},&time));sweeps++;
     assert(!portal_departure(vertices,indices,count,e.normal,from,from,(Point){0},&time));sweeps++;
    }
    // Every exit must have a receiving opening in the other room's coordinate frame.
    long saved=ftell(f);assert(!fseek(f,offset[1+e.destination],SEEK_SET));Room receiver;assert(fread(&receiver,sizeof receiver,1,f)==1);bool paired=false;
    Point other=apply(e.m,center);
    for(unsigned j=0;j<receiver.exits;j++){
     Exit back;Point bv[PORTAL_VERTICES];uint8_t bi[3*(PORTAL_VERTICES-2)];
     assert(fread(&back,sizeof back,1,f)==1 && back.points<=PORTAL_VERTICES);assert(fread(bv,sizeof(Point),back.points,f)==back.points);
     if(back.destination!=room)continue;
     unsigned bn=portal_triangles(bv,back.points,back.normal,bi);Point delta={other.x-bv[0].x,other.y-bv[0].y,other.z-bv[0].z};
     float distance=delta.x*back.normal.x+delta.y*back.normal.y+delta.z*back.normal.z;
     Point roundtrip=apply(back.m,other);
     if(fabsf(distance)<.002f && portal_contains(bv,bi,bn,back.normal,other) &&
        hypotf(roundtrip.x-center.x,hypotf(roundtrip.y-center.y,roundtrip.z-center.z))<.002f)paired=true;
    }
    if(!paired){fprintf(stderr,"Unpaired aperture: %s room %u exit %u triangle %u\n",argv[1],room,e.id,k/3);return 2;}
    assert(!fseek(f,saved,SEEK_SET));
   }
  }
 }
 printf("%s\t%u\t%u\t%u\t%u\n",argv[1],portals,triangles,sweeps,nonvertical);free(offset);fclose(f);return 0;
}
