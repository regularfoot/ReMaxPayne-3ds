#include "alpha_pass.h"
#include "campaign_world.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    C3D_Mtx transform;
    C3D_BufInfo *buffer;
    C3D_Tex *texture,*light;
    float depth,bias;
    unsigned view,first,count,color,order;
    AlphaStyle style;
    const uint16_t *indices;
} Draw;
static Draw initial[32],*list=initial;
static unsigned used,capacity=32;
static int location;
static C3D_AttrInfo *world_attr,*sprite_attr,*skin_attr;
static C3D_TexEnv *world_env;
static void (*scene_fog)(void);

void alpha_close(void){
    if(list!=initial)free(list);
    list=initial;capacity=32;used=0;
}
void alpha_begin(int uniform,C3D_AttrInfo *world,C3D_AttrInfo *sprite,C3D_AttrInfo *skin,C3D_TexEnv *lightmap,void (*fog)(void)){
    location=uniform;world_attr=world;sprite_attr=sprite;skin_attr=skin;world_env=lightmap;scene_fog=fog;used=0;
}
static int compare_depth(const void *a,const void *b){
    const Draw *left=a,*right=b;
    // Paint is part of the opaque receiver. Apply it before glass, foliage
    // and particles regardless of the center depths of the blended batches.
    if((left->style==ALPHA_DECAL)!=(right->style==ALPHA_DECAL))return left->style==ALPHA_DECAL?-1:1;
    if(left->depth!=right->depth)return left->depth<right->depth?1:-1;
    return (left->order>right->order)-(left->order<right->order);
}
static bool same_batch(const Draw *a,const Draw *b){
    return a->style==b->style && a->bias==b->bias && a->buffer==b->buffer && a->texture==b->texture && a->light==b->light &&
           a->view==b->view && a->color==b->color && !memcmp(&a->transform,&b->transform,sizeof a->transform);
}
static void draw_sorted(void){
    if(!used)return;
    qsort(list,used,sizeof *list,compare_depth);
    int style=-1,fog=-1;float bias=-1;
    C3D_DepthTest(true,GPU_GEQUAL,GPU_WRITE_COLOR);
    for(unsigned i=0;i<used;){
        const Draw *d=list+i++;unsigned end=d->first+d->count;
        while(!d->indices && i<used && list[i].first==end && same_batch(d,list+i))end+=list[i++].count;
        if(bias!=d->bias){C3D_DepthMap(true,-1,d->bias);bias=d->bias;}
        if(style!=(int)d->style){
            style=d->style;
            bool surface_depth=style==ALPHA_WORLD_DEPTH || style==ALPHA_WORLD_DEPTH_CULLED;
            bool world=style==ALPHA_WORLD || surface_depth || style==ALPHA_DECAL || style==ALPHA_BEAM || style==ALPHA_BEACON;
            bool additive=style==ALPHA_GLOW || style==ALPHA_BEAM || style==ALPHA_BEACON;
            bool fogged=style!=ALPHA_MARK && style!=ALPHA_BEACON && style!=ALPHA_BEAM;
            if(fog!=fogged){
                if(fogged)scene_fog();else C3D_FogGasMode(GPU_NO_FOG,GPU_PLAIN_DENSITY,false);
                fog=fogged;
            }
            bool skin=style==ALPHA_SKIN_LIT || style==ALPHA_SKIN_TINT;
            C3D_SetAttrInfo(world?world_attr:skin?skin_attr:sprite_attr);
            C3D_CullFace(skin || style==ALPHA_WORLD_DEPTH_CULLED?GPU_CULL_FRONT_CCW:GPU_CULL_NONE);
            C3D_DepthTest(true,GPU_GEQUAL,surface_depth?GPU_WRITE_ALL:GPU_WRITE_COLOR);
            C3D_AlphaTest(true,GPU_GREATER,style==ALPHA_WORLD || surface_depth || style==ALPHA_DECAL?15:(world || skin)?0:style==ALPHA_MARK?4:3);
            C3D_AlphaBlend(GPU_BLEND_ADD,GPU_BLEND_ADD,GPU_SRC_ALPHA,additive?GPU_ONE:GPU_ONE_MINUS_SRC_ALPHA,GPU_ONE,GPU_ZERO);
            if(world){
                C3D_SetTexEnv(0,world_env);
                if(style==ALPHA_BEACON)C3D_TexEnvSrc(C3D_GetTexEnv(0),C3D_RGB,GPU_TEXTURE0,GPU_CONSTANT,GPU_PRIMARY_COLOR);
            }else{
                C3D_TexEnv *env=C3D_GetTexEnv(0);C3D_TexEnvInit(env);
                if(skin){
                    C3D_TexEnvSrc(env,C3D_Both,GPU_TEXTURE0,style==ALPHA_SKIN_TINT?GPU_CONSTANT:GPU_PRIMARY_COLOR,GPU_CONSTANT);
                    C3D_TexEnvFunc(env,C3D_RGB,GPU_MODULATE);
                    C3D_TexEnvScale(env,C3D_RGB,style==ALPHA_SKIN_TINT?GPU_TEVSCALE_1:GPU_TEVSCALE_2);
                }
            }
        }
        if(d->style==ALPHA_MARK || d->style==ALPHA_PARTICLE || d->style==ALPHA_GLOW)
            C3D_TexEnvSrc(C3D_GetTexEnv(0),C3D_Both,d->texture?GPU_TEXTURE0:GPU_CONSTANT,GPU_CONSTANT,GPU_CONSTANT);
        if(!d->texture || d->style==ALPHA_BEACON || d->style==ALPHA_SKIN_TINT)C3D_TexEnvColor(C3D_GetTexEnv(0),d->color);
        if(d->texture)C3D_TexBind(0,d->texture);
        if(d->light)C3D_TexBind(1,d->light);
        campaign_stencil(d->view);C3D_SetBufInfo(d->buffer);
        C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER,location,&d->transform);
        if(d->indices)C3D_DrawElements(GPU_TRIANGLES,d->count,C3D_UNSIGNED_SHORT,d->indices);
        else C3D_DrawArrays(GPU_TRIANGLES,d->first,end-d->first);
    }
    used=0;
}
void alpha_submit(AlphaStyle style,const C3D_Mtx *transform,Point center,unsigned view,C3D_BufInfo *buffer,
                  unsigned first,unsigned count,C3D_Tex *texture,C3D_Tex *light,uint32_t color){
    if(used==capacity){
        unsigned next=capacity*2;
        Draw *more=list==initial?malloc(next*sizeof *more):realloc(list,next*sizeof *more);
        if(more){
            if(list==initial)memcpy(more,initial,sizeof initial);
            list=more;capacity=next;
        }else draw_sorted();
    }
    C3D_FVec w=transform->r[3];
    float depth=w.w+w.x*center.x+w.y*center.y+w.z*center.z;
    list[used]=(Draw){*transform,buffer,texture,light,depth,0,view,first,count,color,used,style,NULL};
    used++;
}
void alpha_decal(const C3D_Mtx *transform,Point center,unsigned view,C3D_BufInfo *buffer,
                 unsigned first,unsigned count,C3D_Tex *texture,C3D_Tex *light,float depth_bias){
    alpha_submit(ALPHA_DECAL,transform,center,view,buffer,first,count,texture,light,0);
    list[used-1].bias=depth_bias;
}
void alpha_skin(const C3D_Mtx *transform,Point center,unsigned view,C3D_BufInfo *buffer,const uint16_t *indices,
                unsigned count,C3D_Tex *texture,bool tint,uint32_t color){
    alpha_submit(tint?ALPHA_SKIN_TINT:ALPHA_SKIN_LIT,transform,center,view,buffer,0,count,texture,NULL,color);
    list[used-1].indices=indices;
}
void alpha_finish(void){
    draw_sorted();C3D_CullFace(GPU_CULL_NONE);campaign_stencil(0);C3D_AlphaTest(true,GPU_GREATER,63);
    C3D_DepthMap(true,-1,0);
    C3D_DepthTest(true,GPU_GEQUAL,GPU_WRITE_ALL);
    C3D_AlphaBlend(GPU_BLEND_ADD,GPU_BLEND_ADD,GPU_ONE,GPU_ZERO,GPU_ONE,GPU_ZERO);
}
