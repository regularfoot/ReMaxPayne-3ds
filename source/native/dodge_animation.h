#ifndef MP_DODGE_ANIMATION_H
#define MP_DODGE_ANIMATION_H
#include <math.h>
typedef struct {unsigned first,second;float weight,angle;} DodgeBlend;
// Xbox 0015A850: floor(angle*4/pi), next sector wraps 7 -> 0,
// first weight = 1-fract(angle*4/pi). Source IDs at 001D2CFA..001D2D85.
// Our array is F, B, L, R, BL, BR, FL, FR; Xbox ring is F, FL, L, BL, B, BR, R, FR.
static inline DodgeBlend dodge_animation_blend(float travel_yaw,float aim_yaw){
    static const unsigned ring[]={0,6,2,4,1,5,3,7};
    const float tau=6.283185307179586f;
    float angle=fmodf(aim_yaw-travel_yaw,tau);if(angle<0)angle+=tau;
    float sector=angle*1.273239544735163f;
    if(sector>=8)sector=0;
    unsigned first=(unsigned)sector;
    return (DodgeBlend){ring[first],ring[(first+1)&7],sector-first,angle};
}
// Xbox 001D2E60..001D2F49 selects the four authored get-ups from the
// final relative angle. Do not return to the takeoff direction after aiming.
static inline unsigned dodge_animation_getup(float angle){
    const float quarter=.7853981633974483f;
    if(angle>quarter && angle<=3*quarter)return 2;
    if(angle>3*quarter && angle<=5*quarter)return 1;
    if(angle>5*quarter && angle<=7*quarter)return 3;
    return 0;
}
#endif
