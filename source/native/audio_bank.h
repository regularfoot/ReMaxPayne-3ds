#ifndef MP_AUDIO_BANK_H
#define MP_AUDIO_BANK_H
#include "room.h"
#include <stddef.h>
bool bank_open(void);
void bank_close(void);
void bank_time_scale(float scale);
void bank_music_level(float level);
void bank_tick(void);
void bank_preload(const char *group,const char *name);
void bank_music(const char *name);
void bank_sound(unsigned owner,const char *group,const char *name);
void bank_impact_sound(unsigned owner,unsigned room,Point point,const char *key,const char *material);
void bank_scene_sound(unsigned owner,const char *group,const char *name,Point source,bool follow,bool force_spatial);
void bank_listener(unsigned room,Point position,float yaw);
size_t bank_ambient_size(void);
void bank_store_ambient(void *data);
bool bank_restore_ambient(const void *data,size_t bytes);
void bank_stop(unsigned owner);
void bank_clear(void);
void bank_pause(bool paused);
#endif
