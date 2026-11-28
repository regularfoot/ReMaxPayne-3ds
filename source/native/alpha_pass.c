#include "alpha_pass.h"
#include "campaign_world.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    C3D_Mtx transform;
    C3D_BufInfo *buffer;
    C3D_Tex *texture,*light;
    float bias;
    unsigned view,first,count,color;
    AlphaStyle style;
    const uint16_t *indices;
} Draw;
static Draw initial[32],*list=initial;
static unsigned used,capacity=32;
static int location;
static C3D_AttrInfo *world_attr,*sprite_attr,*skin_attr;
static C3D_TexEnv *world_env;
static bool scene_fog_enabled;
static uint32_t scene_fog_color;

void alpha_close(void){
    if(list!=initial)free(list);
    list=initial;capacity=32;used=0;
}
void alpha_begin(int uniform,C3D_AttrInfo *world,C3D_AttrInfo *sprite,C3D_AttrInfo *skin,C3D_TexEnv *lightmap,
                 bool fog_enabled,uint32_t fog_color){
    location=uniform;world_attr=world;sprite_attr=sprite;skin_attr=skin;world_env=lightmap;
    scene_fog_enabled=fog_enabled;scene_fog_color=fog_color;used=0;
}
static unsigned next_draw(unsigned i,bool decals){
    while(i<used && (list[i].style==ALPHA_DECAL)!=decals)i++;
    return i;
}
static bool same_batch(const Draw *a,const Draw *b){
    return a->style==b->style && a->bias==b->bias && a->buffer==b->buffer && a->texture==b->texture && a->light==b->light &&
           a->view==b->view && a->color==b->color && !memcmp(&a->transform,&b->transform,sizeof a->transform);
}
static void draw_queued(void){
    if(!used)return;
    enum {FOG_UNSET,FOG_OFF,FOG_SCENE,FOG_BLACK} fog=FOG_UNSET;
    int style=-1;float bias=-1;
    const Draw *previous=NULL;
    C3D_DepthTest(true,GPU_GEQUAL,GPU_WRITE_COLOR);
    // Match Alpha Order: decals first, with submission order within each pass.
    for(unsigned pass=0;pass<2;pass++){
        bool decals=pass==0;
        for(unsigned i=next_draw(0,decals);i<used;){
            const Draw *d=list+i;
            unsigned end=d->first+d->count;
            i=next_draw(i+1,decals);
            while(!d->indices && i<used && list[i].first==end && same_batch(d,list+i)){
                end+=list[i].count;
                i=next_draw(i+1,decals);
            }
            if(bias!=d->bias){C3D_DepthMap(true,-1,d->bias);bias=d->bias;}
            bool style_changed=style!=(int)d->style;
            if(style_changed){
                style=d->style;
                bool surface_depth=style==ALPHA_WORLD_DEPTH || style==ALPHA_WORLD_DEPTH_CULLED;
                bool world=style==ALPHA_WORLD || surface_depth || style==ALPHA_DECAL || style==ALPHA_BEAM || style==ALPHA_BEACON;
                bool additive=style==ALPHA_GLOW || style==ALPHA_BEAM || style==ALPHA_BEACON;
                bool fogged=scene_fog_enabled && style!=ALPHA_MARK && style!=ALPHA_BEACON && style!=ALPHA_BEAM;
                int draw_fog=!fogged?FOG_OFF:additive?FOG_BLACK:FOG_SCENE;
                if(fog!=draw_fog){
                    // Additive sprites must fade toward zero. Scene-colored fog
                    // would add light in the black parts of their rectangular texture.
                    C3D_FogGasMode(fogged?GPU_FOG:GPU_NO_FOG,GPU_PLAIN_DENSITY,false);
                    if(fogged)C3D_FogColor(draw_fog==FOG_BLACK?0:scene_fog_color);
                    fog=draw_fog;
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
            // Reuse state within this eye. Blending and submission order stay the same.
            bool source_changed=style_changed || !previous || !!previous->texture!=!!d->texture;
            if(source_changed && (style==ALPHA_MARK || style==ALPHA_PARTICLE || style==ALPHA_GLOW))
                C3D_TexEnvSrc(C3D_GetTexEnv(0),C3D_Both,d->texture?GPU_TEXTURE0:GPU_CONSTANT,GPU_CONSTANT,GPU_CONSTANT);
            if((!d->texture || style==ALPHA_BEACON || style==ALPHA_SKIN_TINT) &&
               (source_changed || previous->color!=d->color))C3D_TexEnvColor(C3D_GetTexEnv(0),d->color);
            if(d->texture && (!previous || previous->texture!=d->texture))C3D_TexBind(0,d->texture);
            if(d->light && (!previous || previous->light!=d->light))C3D_TexBind(1,d->light);
            if(!previous || previous->view!=d->view)campaign_stencil(d->view);
            if(!previous || previous->buffer!=d->buffer)C3D_SetBufInfo(d->buffer);
            if(!previous || memcmp(&previous->transform,&d->transform,sizeof d->transform))
                C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER,location,&d->transform);
            previous=d;
            if(d->indices)C3D_DrawElements(GPU_TRIANGLES,d->count,C3D_UNSIGNED_SHORT,d->indices);
            else C3D_DrawArrays(GPU_TRIANGLES,d->first,end-d->first);
        }
    }
    used=0;
}
void alpha_submit(AlphaStyle style,const C3D_Mtx *transform,unsigned view,C3D_BufInfo *buffer,
                  unsigned first,unsigned count,C3D_Tex *texture,C3D_Tex *light,uint32_t color){
    if(used==capacity){
        unsigned next=capacity*2;
        Draw *more=list==initial?malloc(next*sizeof *more):realloc(list,next*sizeof *more);
        if(more){
            if(list==initial)memcpy(more,initial,sizeof initial);
            list=more;capacity=next;
        }else draw_queued();
    }
    list[used]=(Draw){*transform,buffer,texture,light,0,view,first,count,color,style,NULL};
    used++;
}
void alpha_decal(const C3D_Mtx *transform,unsigned view,C3D_BufInfo *buffer,
                 unsigned first,unsigned count,C3D_Tex *texture,C3D_Tex *light,float depth_bias){
    alpha_submit(ALPHA_DECAL,transform,view,buffer,first,count,texture,light,0);
    list[used-1].bias=depth_bias;
}
void alpha_skin(const C3D_Mtx *transform,unsigned view,C3D_BufInfo *buffer,const uint16_t *indices,
                unsigned count,C3D_Tex *texture,bool tint,uint32_t color){
    alpha_submit(tint?ALPHA_SKIN_TINT:ALPHA_SKIN_LIT,transform,view,buffer,0,count,texture,NULL,color);
    list[used-1].indices=indices;
}
void alpha_finish(void){
    draw_queued();C3D_CullFace(GPU_CULL_NONE);campaign_stencil(0);C3D_AlphaTest(true,GPU_GREATER,63);
    C3D_DepthMap(true,-1,0);
    C3D_DepthTest(true,GPU_GEQUAL,GPU_WRITE_ALL);
    C3D_AlphaBlend(GPU_BLEND_ADD,GPU_BLEND_ADD,GPU_ONE,GPU_ZERO,GPU_ONE,GPU_ZERO);
}
