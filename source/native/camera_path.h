#ifndef MP_CAMERA_PATH_H
#define MP_CAMERA_PATH_H
#include "scene.h"
bool camera_bank_open(void);
void camera_bank_close(void);
int camera_path_id(const char *name);
float camera_path_duration(unsigned id);
float camera_path_rate(unsigned id);
void camera_path_view(Camera *camera,unsigned id,float time,const Player *parent);
#endif
