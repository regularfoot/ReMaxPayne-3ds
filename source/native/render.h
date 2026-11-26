#ifndef MP_NATIVE_RENDER_H
#define MP_NATIVE_RENDER_H
#include "mission.h"
#include "scene.h"
void room_pause_image(bool dim);
void room_pause_overlay(bool active);
bool room_stereo(void);
typedef struct {uint64_t wait,skin,effects,commands,finish;} RoomDrawTiming;
RoomDrawTiming room_draw_timing(void);
void room_render(const Player *player,const Animation *anim,const Camera *camera,const Effects *effects,const Mission *mission,unsigned zoom);
bool room_campaign(const char *stage,Player *player,bool young);
void room_motion_range(float travel);
bool room_campaign_enter(unsigned room);
void room_render_campaign(const Player *p,const Animation *anim,const Camera *camera,const Effects *fx,
                          const Enemy *cast,unsigned count,bool unarmed,unsigned zoom);
void room_animation_gallery(void);
#endif
