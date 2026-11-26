#ifndef MP_ALPHA_PASS_H
#define MP_ALPHA_PASS_H
#include "scene.h"
#include <citro3d.h>

typedef enum {ALPHA_WORLD,ALPHA_WORLD_DEPTH,ALPHA_BEAM,ALPHA_BEACON,ALPHA_PARTICLE,ALPHA_GLOW,ALPHA_MARK,ALPHA_SKIN_LIT,ALPHA_SKIN_TINT,ALPHA_DECAL,ALPHA_WORLD_DEPTH_CULLED} AlphaStyle;
void alpha_begin(int uniform,C3D_AttrInfo *world,C3D_AttrInfo *sprite,C3D_AttrInfo *skin,C3D_TexEnv *lightmap,void (*fog)(void));
void alpha_submit(AlphaStyle style,const C3D_Mtx *transform,Point center,unsigned view,C3D_BufInfo *buffer,
                  unsigned first,unsigned count,C3D_Tex *texture,C3D_Tex *light,uint32_t color);
void alpha_decal(const C3D_Mtx *transform,Point center,unsigned view,C3D_BufInfo *buffer,
                 unsigned first,unsigned count,C3D_Tex *texture,C3D_Tex *light,float depth_bias);
void alpha_skin(const C3D_Mtx *transform,Point center,unsigned view,C3D_BufInfo *buffer,const uint16_t *indices,
                unsigned count,C3D_Tex *texture,bool tint,uint32_t color);
void alpha_finish(void);
void alpha_close(void);
#endif
