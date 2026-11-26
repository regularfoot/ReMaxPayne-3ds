#ifndef MP_AIM_POSE_H
#define MP_AIM_POSE_H
#include "bone_pose.h"
// Xbox 18B320: sixteen blends of the neutral and extreme shooting clips,
// recording the Gun node's elevation and model-space position at frame zero.
typedef struct {float angle[16],position[16][3];} AimTable;
static inline int aim_calibrate(const Matrix *neutral,const Matrix *extreme,Matrix *scratch,
                                const Matrix *inverse,const short *parent,unsigned count,unsigned socket,AimTable *table){
    for(unsigned frame=0;frame<16;frame++){
        for(unsigned i=0;i<count;i++){
            scratch[i]=bone_mix_local(neutral+i,extreme+i,frame/15.f);
            if(!bone_normalize(scratch+i,2))return 0;
        }
        bone_local_to_skin(scratch,scratch,inverse,parent,count);
        Matrix gun=scratch[socket];if(!bone_normalize(&gun,2))return 0;
        table->angle[frame]=asinf(fmaxf(-1,fminf(1,gun.row[1][2])));
        for(unsigned i=0;i<3;i++)table->position[frame][i]=gun.row[i][3];
    }
    return 1;
}
static inline float aim_ray_angle(const AimTable *t,unsigned i,const float target[3]){
    float y=target[1]-t->position[i][1],z=target[2]-t->position[i][2];
    float x=target[0]-t->position[i][0];
    float angle=asinf(fmaxf(-1,fminf(1,y/fmaxf(.000001f,sqrtf(x*x+y*y+z*z)))));
    return z<0?3.14159265358979323846f-angle:angle;
}
// Xbox 18AD20. Locate adjacent calibrated rays, intersect both with the line
// through the target parallel to their muzzle baseline, and interpolate there.
// This accounts for a muzzle that moves as the arms rise; angle/90 does not.
static inline float aim_extreme_weight(const AimTable *t,const float target[3],int high){
    unsigned lo=0,hi=15;
    while(lo+1<hi){
        unsigned mid=(lo+hi)/2;
        float angle=aim_ray_angle(t,mid,target);
        if(target[2]<t->position[mid][2] || (high?angle>t->angle[mid]:angle<t->angle[mid]))lo=mid;
        else hi=mid;
    }
    float y0=t->position[lo][1],z0=t->position[lo][2];
    float y1=t->position[hi][1],z1=t->position[hi][2];
    float s0=sinf(t->angle[lo]),s1=sinf(t->angle[hi]);
    float c0=sqrtf(fmaxf(0,1-s0*s0)),c1=sqrtf(fmaxf(0,1-s1*s1));
    float n=(y0-y1)*target[2]+y1*z0-target[1]*z0+target[1]*z1-z1*y0;
    float d0=c0*(y1-y0)-s0*(z1-z0),d1=c1*(y1-y0)-s1*(z1-z0);
    float a=d0==0?0:-n/d0,b=d1==0?0:-n/d1;
    float qy=y0+s0*a,qz=z0+c0*a;
    float span=hypotf(y1+s1*b-qy,z1+c1*b-qz);
    if(span==0)return 1; // Coincident rays saturate in the retail controller.
    float mix=hypotf(target[1]-qy,target[2]-qz)/span;
    return fminf(1,(lo+mix)/15.f);
}
static inline int aim_high(const AimTable *neutral,const float target[3]){
    float y=target[1]-neutral->position[0][1],z=target[2]-neutral->position[0][2];
    float s=sinf(neutral->angle[0]);
    return s*z-sqrtf(fmaxf(0,1-s*s))*y<0;
}
// Xbox 18B870 projects Gun forward out of the target's vertical plane normal.
// Its residual correction is yaw about Torso, not an extra pitch of the spine.
static inline float aim_torso_yaw(const float gun[3],const float pivot[3],const float target[3],float weight){
    float y=target[1]-pivot[1],z=target[2]-pivot[2],length=hypotf(y,z);
    if(length<.000001f)return 0;
    return -atan2f(gun[0],(gun[1]*y+gun[2]*z)/length)*weight;
}
static inline void aim_turn_torso(Matrix *palette,unsigned count,unsigned torso,
                                  const short *parent,const float pivot[3],float angle){
    float s=sinf(angle),c=cosf(angle);
    Matrix turn={.row={{c,0,s,pivot[0]*(1-c)-s*pivot[2]},
                      {0,1,0,0},{-s,0,c,pivot[2]*(1-c)+s*pivot[0]}}};
    for(unsigned i=0;i<count;i++){
        int ancestor=i;while(ancestor>=0 && ancestor!=(int)torso)ancestor=parent[ancestor];
        if(ancestor==(int)torso)palette[i]=bone_compose_frame(&turn,palette+i);
    }
}
#endif
