#ifndef MP_PRESENTATION_H
#define MP_PRESENTATION_H
#include <stdint.h>
#include <stdbool.h>
typedef struct {float first,last,time,duration;} Tween;
typedef struct {
    Tween fov,visibility,fog_near,fog_far,fog_mix,fade;
    uint32_t fog_from,fog_to,fade_from,fade_to,flash_color;
    float flash_in,flash_hold,flash_out,flash_time;
    bool fog;
} Presentation;
extern Presentation presentation;
bool presentation_open(const char *stage,char chapter[64]);
void presentation_event(const char *op,const char *const *args,unsigned count);
void presentation_tick(float dt);
float presentation_value(Tween t);
uint32_t presentation_fog(void);
uint32_t presentation_fade(void);
uint32_t presentation_flash(void);
#endif
