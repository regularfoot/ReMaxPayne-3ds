#ifndef MP_CAMPAIGN_H
#define MP_CAMPAIGN_H
#include "ui.h"
#include "actor.h"
#include <citro3d.h>
#include <stdio.h>
bool campaign_play(const char *stage,int load_slot,UiChoice *next,FILE *journal);
ActorLight room_model_light(unsigned room,const Player *body,bool campaign,bool cinematic);
ActorLight campaign_model_light(unsigned slot);
Point campaign_actor_feet(unsigned slot);
uint32_t campaign_actor_light(unsigned slot);
void campaign_draw_items(int uniform,const C3D_Mtx *combined);
bool campaign_reticle_visible(void);
void campaign_draw_weapon(unsigned instance,const Animation *animation,int uniform,const C3D_Mtx *body);
void campaign_prepare_weapon_effects(unsigned visible,unsigned count);
#endif
