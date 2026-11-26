#ifndef MP_NATIVE_EFFECTS_H
#define MP_NATIVE_EFFECTS_H
#include "scene.h"
#include <citro3d.h>
bool effects_open(void);
void effects_close(void);
#define STEREO_FOCUS 3.f
#define SCOPE_FOCUS 25.f
void effects_prepare(const Effects *fx,const Camera *camera,unsigned zoom,bool interact);
void effects_queue(const C3D_Mtx *view,const Effects *fx);
void effects_draw(bool crosshair,bool standalone);
void effects_pause(int uniform);
void effects_overlay(int uniform,uint32_t color);
#endif
