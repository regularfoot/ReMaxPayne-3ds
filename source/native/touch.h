#ifndef MP_NATIVE_TOUCH_H
#define MP_NATIVE_TOUCH_H
#include <stdbool.h>
#include <stdint.h>
typedef enum { TOUCH_NONE, TOUCH_LOOK, TOUCH_WEAPON, TOUCH_FIRE, TOUCH_CROUCH, TOUCH_JUMP,
               TOUCH_SLOW, TOUCH_JOURNAL, TOUCH_HEAL, TOUCH_AIM } TouchZone;
typedef struct { int x,y,w,h; } TouchRect;
extern const TouchRect touch_rects[TOUCH_AIM+1];
typedef struct { bool held; TouchZone origin; int x,y; } TouchInput;
typedef struct { int dx,dy; TouchZone pressed,held; } TouchAction;
TouchAction touch_update(TouchInput *state,bool held,int x,int y,unsigned buttons);
#define TOUCH_BIT(zone) (1u<<(zone))
#define TOUCH_SHORTCUTS (TOUCH_BIT(TOUCH_WEAPON)|TOUCH_BIT(TOUCH_FIRE)|TOUCH_BIT(TOUCH_JUMP)|TOUCH_BIT(TOUCH_HEAL)|TOUCH_BIT(TOUCH_AIM))
enum { TOUCH_WEAPON_COUNT=15 };
typedef struct {bool open;} WeaponPicker;
typedef struct {bool consumed,changed;int selected;} WeaponChoice;
extern const int touch_weapon_order[16];
TouchRect touch_weapon_rect(unsigned slot);
WeaponChoice touch_weapon_update(WeaponPicker *picker,bool pressed,int x,int y,bool enabled,uint32_t owned,int current);
float touch_look_angle(float stick,int pixels,float seconds,unsigned setting,bool vertical);
float cstick_axis(int value,unsigned deadzone_setting);
#endif
