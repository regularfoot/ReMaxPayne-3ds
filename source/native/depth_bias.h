#ifndef MP_DEPTH_BIAS_H
#define MP_DEPTH_BIAS_H
#include <math.h>
// PICA's float24 calculations retain 16 mantissa bits. A constant four
// depth-buffer units can be smaller than one calculation ULP near the camera.
// Scale the bias with projected depth, retaining a small far-distance floor.
static inline float depth_surface_bias(float z,float w){
    float depth=fabsf(w)>.000001f?fminf(1,fabsf(z/w)):1;
    return fmaxf(4.f/16777215.f,depth*(2.f/65536.f));
}
#endif
