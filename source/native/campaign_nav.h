#ifndef MP_CAMPAIGN_NAV_H
#define MP_CAMPAIGN_NAV_H
#include "room.h"
typedef struct { int next;float remaining,portal_travel;bool riding,following;uint16_t goal_room; } NavAgent;
bool campaign_nav_open(const char *stage);
void campaign_nav_close(void);
typedef bool (*NavPassage)(uint16_t *room,Point *from,unsigned destination,Point target,
                           bool crouched,float stride,float *yaw,float *travel);
void campaign_nav_guard(NavPassage guard);
void campaign_nav_reset(NavAgent *agent);
void campaign_nav_event(NavAgent *agent,const char *op);
bool campaign_nav_step(NavAgent *agent,Point *from,uint16_t *room,Point target,unsigned target_room,
                       float dt,unsigned visible_room,float *yaw,bool crouched);
#endif
