#ifndef MP_PORTAL_H
#define MP_PORTAL_H
#include "room.h"
enum {PORTAL_VERTICES=32};
unsigned portal_triangles(const Point *vertices,unsigned count,Point normal,uint8_t *indices);
bool portal_contains(const Point *vertices,const uint8_t *indices,unsigned count,Point normal,Point position);
// A nearby contact projected into the opening, including closed door sensors.
bool portal_nearby(const Point *vertices,const uint8_t *indices,unsigned count,Point normal,Point center);
// Test the swept body center, including gravity/platform motion. Intent also
// permits transfer when the capsule stops on a BSP seam just before its plane.
bool portal_departure(const Point *vertices,const uint8_t *indices,unsigned count,Point normal,
                      Point previous,Point position,Point intent,float *fraction);
#endif
