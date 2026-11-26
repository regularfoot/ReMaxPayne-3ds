#ifndef MP_ITEM_MESH_H
#define MP_ITEM_MESH_H
#include "room.h"
#include <citro3d.h>
bool item_mesh_load(const char *category,const char *name);
void item_mesh_draw(const char *category,const char *name,int uniform,const C3D_Mtx *matrix);
void item_weapon_draw(const char *name,int uniform,const C3D_Mtx *hand,unsigned action,float time);
const char *item_weapon_effect(const char *name,Point *offset);
typedef enum { RAT_STAND,RAT_WALK,RAT_RUN,RAT_LEFT,RAT_RIGHT,RAT_FORWARD_LEFT,RAT_FORWARD_RIGHT,
               RAT_TURN_LEFT,RAT_TURN_RIGHT,RAT_DEATH_A,RAT_DEATH_B,RAT_SHOOT,RAT_SHOOT_UP,RAT_SHOOT_DOWN,
               RAT_STAND_POSE,RAT_TRACKS } RatTrack;
float item_rat_duration(RatTrack track);
void item_rat_draw(RatTrack track,float time,int uniform,const C3D_Mtx *matrix);
void item_mesh_close(void);
#endif
