#ifndef MP_BONE_POSE_H
#define MP_BONE_POSE_H
#include <math.h>
typedef struct {float row[3][4];} Matrix;
static inline Matrix bone_compose_frame(const Matrix *parent,const Matrix *child){
    Matrix out;
    for(unsigned i=0;i<3;i++)for(unsigned j=0;j<4;j++){
        out.row[i][j]=j==3?parent->row[i][3]:0;
        for(unsigned k=0;k<3;k++)out.row[i][j]+=parent->row[i][k]*child->row[k][j];
    }
    return out;
}
static inline Matrix bone_inverse_frame(const Matrix *m){
    const float (*a)[4]=m->row;
    float det=a[0][0]*(a[1][1]*a[2][2]-a[1][2]*a[2][1])-a[0][1]*(a[1][0]*a[2][2]-a[1][2]*a[2][0])+a[0][2]*(a[1][0]*a[2][1]-a[1][1]*a[2][0]);
    Matrix out={0};if(fabsf(det)<1e-12f)return out;
    for(unsigned i=0;i<3;i++)for(unsigned j=0;j<3;j++){
        unsigned r=(j+1)%3,s=(j+2)%3,c=(i+1)%3,d=(i+2)%3;
        out.row[i][j]=(a[r][c]*a[s][d]-a[r][d]*a[s][c])/det;
    }
    for(unsigned i=0;i<3;i++)for(unsigned j=0;j<3;j++)out.row[i][3]-=out.row[i][j]*a[j][3];
    return out;
}
static inline Matrix bone_relative_frame(const Matrix *parent,const Matrix *child){
    Matrix inverse=bone_inverse_frame(parent);return bone_compose_frame(&inverse,child);
}
static inline Matrix bone_joint_frame(Matrix skin,const float pivot[3]){
    for(unsigned i=0;i<3;i++)for(unsigned j=0;j<3;j++)skin.row[i][3]+=skin.row[i][j]*pivot[j];
    return skin;
}
// Xbox 12590: Gram-Schmidt on the source matrix rows (columns in our convention).
// Mode 1 only normalizes each axis; mode 2 removes preceding-axis projections.
static inline int bone_normalize(Matrix *m,unsigned mode){
    for(unsigned c=0;c<3;c++){
        if(mode==2)for(unsigned previous=0;previous<c;previous++){
            float dot=0;for(unsigned r=0;r<3;r++)dot+=m->row[r][c]*m->row[r][previous];
            for(unsigned r=0;r<3;r++)m->row[r][c]-=dot*m->row[r][previous];
        }
        float square=0;for(unsigned r=0;r<3;r++)square+=m->row[r][c]*m->row[r][c];
        if(square<1e-14f)return 0;
        float scale=1/sqrtf(square);for(unsigned r=0;r<3;r++)m->row[r][c]*=scale;
    }
    return 1;
}
static inline int bone_temporal_finish(Matrix *m,unsigned flags,const float scale[3]){
    if((flags&3) && !bone_normalize(m,flags&3))return 0;
    if(flags&4){
        if(!scale)return 0;
        for(unsigned i=0;i<3;i++)for(unsigned j=0;j<3;j++)m->row[i][j]*=scale[j];
    }
    return 1;
}
static inline Matrix bone_mix_local(const Matrix *a,const Matrix *b,float t){
    Matrix out;for(unsigned i=0;i<3;i++)for(unsigned j=0;j<4;j++)out.row[i][j]=a->row[i][j]*(1-t)+b->row[i][j]*t;
    return out;
}
static inline void bone_skin_to_local(const Matrix *skin,Matrix *local,const Matrix *rest,const short *parent,unsigned count){
    for(unsigned i=0;i<count;i++){
        Matrix child=bone_compose_frame(skin+i,rest+i);
        if(parent[i]>=0){Matrix p=bone_compose_frame(skin+parent[i],rest+parent[i]);child=bone_relative_frame(&p,&child);}
        local[i]=child;
    }
}
static inline void bone_local_to_skin(const Matrix *local,Matrix *out,const Matrix *inverse,const short *parent,unsigned count){
    // Keep model-space parents until all descendants have been reconstructed.
    for(unsigned i=0;i<count;i++)out[i]=parent[i]<0?local[i]:bone_compose_frame(out+parent[i],local+i);
    for(unsigned i=0;i<count;i++)out[i]=bone_compose_frame(out+i,inverse+i);
}
// Public palette helper for host probes and transitions from a displayed pose.
// The full rest axes are essential: Gram-Schmidt is not invariant to replacing
// them with identity axes and retaining only bind joint positions.
static inline void mix_bone_palette(const Matrix *a,const Matrix *b,Matrix *out,float t,
                                    const float (*pivot)[3],const short *parent,unsigned count,unsigned gun,
                                    const unsigned char *mask,const Matrix *rest){
    (void)pivot;(void)gun;
    Matrix first[count],last[count],inverse[count];
    bone_skin_to_local(a,first,rest,parent,count);bone_skin_to_local(b,last,rest,parent,count);
    for(unsigned i=0;i<count;i++){
        if(!mask || mask[i]){
            Matrix mixed=bone_mix_local(first+i,last+i,t);
            // Exactly opposite, singular endpoints need a finite numerical fallback.
            if(!bone_normalize(&mixed,2))mixed=t<.5f?first[i]:last[i];
            first[i]=mixed;
        }
        inverse[i]=bone_inverse_frame(rest+i);
    }
    bone_local_to_skin(first,out,inverse,parent,count);
}
#endif
