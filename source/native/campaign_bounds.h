#ifndef MP_CAMPAIGN_BOUNDS_H
#define MP_CAMPAIGN_BOUNDS_H
#include "campaign_world.h"

bool campaign_bounds_open(const char *stage);
void campaign_bounds_close(void);
bool campaign_bounds_overlap(unsigned prop,Point position,float radius);
// Center uses the director's feet + 0.9 m contact reference.
bool campaign_bounds_character(unsigned prop,Point center,float height);
PlatformContact campaign_platform_contact(int prop,Point feet);
PlatformContact campaign_platform_placed(int prop,Point feet,const float *placement);
PlatformContact campaign_remote_floor(unsigned room,Point feet);
#endif
