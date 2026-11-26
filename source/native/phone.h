#ifndef MP_NATIVE_PHONE_H
#define MP_NATIVE_PHONE_H
#include "actor.h"
bool phone_open(bool house);
void phone_mix(float dx,float dz,float yaw);
void phone_volume(void);
void phone_answer(void);
void weapon_sound(bool reload,Weapon weapon);
void phone_finish_calls(void);
void phone_ring(bool enabled);
void door_sound(void);
void switch_sound(void);
void medicine_preload(void);
void medicine_sound(bool taking);
void enemy_sound(bool shotgun,bool loading);
void phone_close(void);
#endif
