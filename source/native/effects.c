#include "effects.h"
#include "alpha_pass.h"
#include "texture.h"
#include "campaign_file.h"
#include <string.h>
#include <stdlib.h>
#include "campaign_world.h"
#include "ui.h"
#include <3ds.h>
#include <citro3d.h>
#include <math.h>

enum { RING_SEGMENTS=48, VERTICES=(IMPACT_COUNT+14+RING_SEGMENTS)*6 };
static float circle[RING_SEGMENTS+1][2];
typedef struct {Point pos;float u,v;} Vertex;
typedef struct {uint16_t id,texture;float radius;char material[32];} Decal;
static Decal *decals;static unsigned decal_count,texture_count;
static C3D_Tex *decal_tex;
static C3D_Tex pause_word;static bool pause_ready;
static Vertex *vertices;
static C3D_BufInfo binding;
static unsigned count,marks,flash,mask;
static struct {unsigned room,image;} mark_batches[IMPACT_COUNT];
static unsigned batch_count;
static bool interaction_mark;

bool effects_open(void) {
    for (unsigned i=0;i<=RING_SEGMENTS;i++) {
        float a=6.2831853f*i/RING_SEGMENTS;
        circle[i][0]=cosf(a);circle[i][1]=sinf(a);
    }
    FILE *file=campaign_file("decals.pak");
    if(file){
        struct {char tag[4];uint32_t rows,textures;} h;
        bool ok=fread(&h,sizeof h,1,file)==1 && !memcmp(h.tag,"DEC1",4) && h.rows<4096 && h.textures<512;
        if(ok){decals=calloc(h.rows,sizeof *decals);decal_tex=calloc(h.textures,sizeof *decal_tex);
            ok=decals && decal_tex && fread(decals,sizeof *decals,h.rows,file)==h.rows;}
        if(ok){decal_count=h.rows;
            for(unsigned i=0;ok && i<h.rows;i++)ok=memchr(decals[i].material,0,32) && (decals[i].texture==65535 || decals[i].texture<h.textures);
            while(ok && texture_count<h.textures){ok=native_texture_read(file,decal_tex+texture_count);if(ok)texture_count++;}}
        fclose(file);if(!ok)return false;
    }
    file=fopen("romfs:/pause.tex","rb");
    if(!file)return false;
    pause_ready=native_texture_read(file,&pause_word);fclose(file);
    if(!pause_ready)return false;
    vertices=linearAlloc(sizeof(Vertex)*VERTICES);
    if (!vertices) return false;
    BufInfo_Init(&binding);
    return BufInfo_Add(&binding,vertices,sizeof(Vertex),2,0x10)>=0;
}
void effects_close(void) {
    if(pause_ready)C3D_TexDelete(&pause_word);
    pause_ready=false;
    for(unsigned i=0;i<texture_count;i++)C3D_TexDelete(decal_tex+i);
    free(decal_tex);free(decals);decals=NULL;decal_tex=NULL;decal_count=texture_count=0;
    if(vertices)linearFree(vertices);
    vertices=NULL; }
static Point offset(Point c,Point u,float x,Point v,float y) {
    return (Point){c.x+u.x*x+v.x*y,c.y+u.y*x+v.y*y,c.z+u.z*x+v.z*y};
}
static void rect(Point c,Point u,Point v,float x,float y,float w,float h) {
    Point a=offset(c,u,x-w,v,y-h),b=offset(c,u,x+w,v,y-h);
    Point d=offset(c,u,x-w,v,y+h),e=offset(c,u,x+w,v,y+h);
    vertices[count++]=(Vertex){a,0,0};vertices[count++]=(Vertex){b,1,0};vertices[count++]=(Vertex){e,1,1};
    vertices[count++]=(Vertex){a,0,0};vertices[count++]=(Vertex){e,1,1};vertices[count++]=(Vertex){d,0,1};
}
static void draw(unsigned first,unsigned end,u32 color) {
    if (first==end) return;
    C3D_TexEnvColor(C3D_GetTexEnv(0),color);
    C3D_DrawArrays(GPU_TRIANGLES,first,end-first);
}
void effects_prepare(const Effects *fx,const Camera *camera,unsigned zoom,bool interact) {
    count=0;batch_count=0;
    for (unsigned i=0;i<IMPACT_COUNT;i++) {
        const Impact *mark=&fx->marks[i];
        if (mark->life<=0) continue;
        Point center,normal;
        if(!campaign_mark_pose(mark,&center,&normal))continue;
        center=offset(center,normal,.0015f,normal,0);
        C3D_FVec n=FVec3_New(normal.x,normal.y,normal.z);
        C3D_FVec axis=fabsf(n.y)<.9f?FVec3_New(0,1,0):FVec3_New(1,0,0);
        C3D_FVec t=FVec3_Normalize(FVec3_Cross(n,axis)),b=FVec3_Cross(n,t);
        const Decal *choice=NULL;
        if(mark->decal)for(unsigned k=0;k<decal_count;k++)if(decals[k].id==mark->decal){
            if(!strcmp(decals[k].material,mark->material)){choice=decals+k;break;}
            if(!strcmp(decals[k].material,"default"))choice=decals+k;
        }
        if(choice && choice->texture==65535)continue;
        float radius=choice?choice->radius:.039f;unsigned image=choice?choice->texture+1:0;
        rect(center,(Point){t.x,t.y,t.z},(Point){b.x,b.y,b.z},0,0,radius,radius);
        mark_batches[batch_count].room=mark->room;mark_batches[batch_count++].image=image;
    }
    marks=count;
    if (fx->flash>0) {
        rect(fx->muzzle,camera->right,camera->up,0,0,.07f,.022f);
        rect(fx->muzzle,camera->right,camera->up,0,0,.022f,.07f);
    }
    flash=count;
    float focus=zoom>1?SCOPE_FOCUS:STEREO_FOCUS;
    Point center=offset(camera->eye,camera->forward,focus,camera->right,0);
    float scale=focus/zoom;
    if (zoom>1) {
        for (unsigned i=0;i<RING_SEGMENTS;i++) {
            Point inner[2],outer[2];
            for (unsigned j=0;j<2;j++) {
                float x=circle[i+j][0]*scale,y=circle[i+j][1]*scale;
                inner[j]=offset(center,camera->right,.54f*x,camera->up,.54f*y);
                outer[j]=offset(center,camera->right,1.5f*x,camera->up,1.5f*y);
            }
            vertices[count++]=(Vertex){inner[0],0,0};vertices[count++]=(Vertex){outer[0],0,0};vertices[count++]=(Vertex){inner[1],0,0};
            vertices[count++]=(Vertex){inner[1],0,0};vertices[count++]=(Vertex){outer[0],0,0};vertices[count++]=(Vertex){outer[1],0,0};
        }
        for (int sign=-1;sign<=1;sign+=2) {
            rect(center,camera->right,camera->up,sign*.28f*scale,0,.26f*scale,.003f*scale);
            rect(center,camera->right,camera->up,0,sign*.28f*scale,.003f*scale,.26f*scale);
        }
    }
    interaction_mark=interact;
    if(interact){
        rect(center,camera->right,camera->up,0,.013f*scale,.012f*scale,.030f*scale);
        rect(center,camera->right,camera->up,0,-.030f*scale,.012f*scale,.012f*scale);
    }
    mask=count;
    if(interact){
        rect(center,camera->right,camera->up,0,.013f*scale,.008f*scale,.026f*scale);
        rect(center,camera->right,camera->up,0,-.030f*scale,.008f*scale,.008f*scale);
    } else if (zoom>1) {
        rect(center,camera->right,camera->up,0,0,.007f*scale,.0028f*scale);
        rect(center,camera->right,camera->up,0,0,.0028f*scale,.007f*scale);
    } else if(ui_settings.reticle==1) {
        rect(center,camera->right,camera->up,0,0,.0045f*scale,.0045f*scale);
    } else if(ui_settings.reticle==2) {
        for(unsigned i=0;i<8;i++){
            unsigned k=i*RING_SEGMENTS/8;
            rect(center,camera->right,camera->up,.018f*scale*circle[k][0],.018f*scale*circle[k][1],.0035f*scale,.0035f*scale);
        }
    } else for (int sign=-1;sign<=1;sign+=2) {
        rect(center,camera->right,camera->up,sign*.021f*scale,0,.009f*scale,.0032f*scale);
        rect(center,camera->right,camera->up,0,sign*.021f*scale,.0032f*scale,.009f*scale);
    }
    unsigned world_count=count;
    rect((Point){0},(Point){1,0,0},(Point){0,1,0},0,0,1,1);
    rect((Point){0},(Point){1,0,0},(Point){0,1,0},0,0,128.f/400,32.f/240);
    // Match the horizontal orientation after the LCD's quarter-turn projection.
    for(unsigned i=count-6;i<count;i++)vertices[i].u=1-vertices[i].u;
    GSPGPU_FlushDataCache(vertices,sizeof(Vertex)*count);
    count=world_count;
}
void effects_queue(const C3D_Mtx *view,const Effects *fx){
    for(unsigned i=0;i<batch_count;i++){
        unsigned image=mark_batches[i].image;
        for(unsigned v=campaign_first_view(mark_batches[i].room);v!=255;v=campaign_next_view(v)){
            const C3D_Mtx *placement=campaign_visible_transform(v);
            if(!placement)continue;
            C3D_Mtx matrix;Mtx_Multiply(&matrix,view,placement);
            alpha_submit(ALPHA_MARK,&matrix,v,&binding,6*i,6,image?decal_tex+image-1:NULL,NULL,0xff27252b);
        }
    }
    if(flash==marks)return;
    for(unsigned v=campaign_first_view(fx->flash_room);v!=255;v=campaign_next_view(v)){
        const C3D_Mtx *placement=campaign_visible_transform(v);
        if(!placement)continue;
        // The quad faces the camera already. Translate its center across the aperture.
        Point origin=fx->muzzle;
        C3D_FVec p=Mtx_MultiplyFVecH(placement,FVec3_New(origin.x,origin.y,origin.z));
        C3D_Mtx matrix=*view;Mtx_Translate(&matrix,p.x-origin.x,p.y-origin.y,p.z-origin.z,true);
        alpha_submit(ALPHA_MARK,&matrix,v,&binding,marks,flash-marks,NULL,NULL,0xff66d9ff);
    }
}
void effects_draw(bool crosshair,bool standalone){
    C3D_SetBufInfo(&binding);
    C3D_TexEnv *env=C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env,C3D_Both,GPU_CONSTANT,GPU_CONSTANT,GPU_CONSTANT);
    if(standalone){
        draw(0,marks,0xff27252b);
        draw(marks,flash,0xff66d9ff);
    }
    C3D_DepthTest(false,GPU_ALWAYS,GPU_WRITE_COLOR);
    if(crosshair){draw(flash,mask,0xff101010);draw(mask,count,interaction_mark?0xff60daff:0xffededed);}
    C3D_DepthTest(true,GPU_GEQUAL,GPU_WRITE_ALL);
}

void effects_overlay(int uniform,uint32_t color) {
    if(!(color&255))return;
    C3D_Mtx screen;Mtx_Identity(&screen);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER,uniform,&screen);
    C3D_AlphaTest(false,GPU_ALWAYS,0);
    C3D_AlphaBlend(GPU_BLEND_ADD,GPU_BLEND_ADD,GPU_SRC_ALPHA,GPU_ONE_MINUS_SRC_ALPHA,GPU_ONE,GPU_ZERO);
    C3D_DepthTest(false,GPU_ALWAYS,GPU_WRITE_COLOR);
    draw(count,count+6,__builtin_bswap32(color));
    C3D_AlphaBlend(GPU_BLEND_ADD,GPU_BLEND_ADD,GPU_ONE,GPU_ZERO,GPU_ONE,GPU_ZERO);
    C3D_AlphaTest(true,GPU_GREATER,63);
    C3D_DepthTest(true,GPU_GEQUAL,GPU_WRITE_ALL);
}

void effects_pause(int uniform){
    C3D_StencilTest(false,GPU_ALWAYS,0,255,0);
    effects_overlay(uniform,0x00000080);
    C3D_Mtx screen;Mtx_Identity(&screen);Mtx_RotateZ(&screen,C3D_AngleFromDegrees(90),true);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER,uniform,&screen);
    C3D_TexBind(0,&pause_word);
    C3D_TexEnv *env=C3D_GetTexEnv(0);C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env,C3D_Both,GPU_TEXTURE0,GPU_CONSTANT,GPU_CONSTANT);
    C3D_TexEnvFunc(env,C3D_Both,GPU_MODULATE);
    C3D_AlphaBlend(GPU_BLEND_ADD,GPU_BLEND_ADD,GPU_SRC_ALPHA,GPU_ONE_MINUS_SRC_ALPHA,GPU_ONE,GPU_ZERO);
    C3D_DepthTest(false,GPU_ALWAYS,GPU_WRITE_COLOR);
    draw(count+6,count+12,0xffffffff);
    C3D_AlphaBlend(GPU_BLEND_ADD,GPU_BLEND_ADD,GPU_ONE,GPU_ZERO,GPU_ONE,GPU_ZERO);
    C3D_DepthTest(true,GPU_GEQUAL,GPU_WRITE_ALL);
}
