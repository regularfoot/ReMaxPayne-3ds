#include "render.h"
#include "async_log.h"
#include "campaign_world.h"
#include "campaign.h"
#include "pose_bank.h"
#include "ui.h"
#include "item_mesh.h"
#include "director.h"
#include "texture.h"
#include "effects.h"
#include "alpha_pass.h"
#include "particles.h"
#include "presentation.h"
#include "navigation.h"
#include <3ds.h>
#include <citro3d.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "shader.inc"

typedef struct { Point pos; float uv[2], light[2]; } Vertex;
typedef struct { uint32_t texture, light, count,group; } Batch;
_Static_assert(sizeof(RoomHeader)==424 && sizeof(Vertex)==28 && sizeof(Triangle)==40, "room file layout");

static RoomHeader header;
static C3D_Tex textures[160];
static unsigned loaded;
static Batch batches[256];
enum { MAX_OPENINGS=8, NEIGHBOUR_GROUP=8, MASK_GROUP=5 };
typedef struct { Point center,normal; int32_t destination; } Opening;
static Opening openings[MAX_OPENINGS];
static unsigned opening_count,mask_batch[MAX_OPENINGS],offsets[257],sky_end,active_end;
static Vertex *vertices;
static Triangle *triangles,*variants;
static unsigned active_count,shown;
static C3D_RenderTarget *target,*right_target;
static bool old_model,stereo_active,stereo_failed,pause_overlay;
void room_pause_overlay(bool active){pause_overlay=active;}
static DVLB_s *dvlb;
static shaderProgram_s program;
static bool initialized, program_ready;
static int projection_location;
static C3D_Mtx projection,monoscopic_projection;
static float near_clip,far_clip;
static C3D_AttrInfo world_attr,actor_attr,skin_attr,effect_attr;
static C3D_BufInfo world_binding,clear_binding;
static void *clear_vertices;
static C3D_TexEnv world_env;

const RoomHeader *room_header(void) { return &header; }

static bool read_room(FILE *f) {
    if (fread(&header,sizeof header,1,f)!=1 || memcmp(header.magic,"MPR7",4) ||
        !header.textures || header.textures>sizeof textures/sizeof(*textures) || !header.batches || header.batches>256 ||
        !header.vertices || header.vertices>90000 || !header.collision || header.collision>32000) return false;
    for (unsigned i=0;i<header.textures;++i) {
        if (!native_texture_read(f,&textures[i])) return false;
        ++loaded;ui_loading_progress(5+55*loaded/header.textures);
    }
    if (fread(batches,sizeof(Batch),header.batches,f)!=header.batches) return false;
    unsigned total=0;
    for (unsigned i=0;i<header.batches;++i) {
        const Batch *b=&batches[i];
        if ((b->group>5 && (b->group<8 || (b->group&7)>MASK_GROUP || b->group>=8+8*MAX_OPENINGS)) || b->texture>=loaded || b->light>=loaded || !b->count || b->count%3 || b->count>header.vertices-total) return false;
        offsets[i]=total;
        total+=b->count;
    }
    if (batches[0].group!=5 || total!=header.vertices) return false;
    uint32_t sum=0;
    for (unsigned i=0;i<5;i++) {
        if (header.group_counts[i]>header.collision-sum) return false;
        sum+=header.group_counts[i];
    }
    if (sum!=header.collision) return false;
    vertices=linearAlloc(total*sizeof(Vertex));
    triangles=malloc(header.collision*sizeof(Triangle));
    if (!vertices || !triangles || fread(vertices,sizeof(Vertex),total,f)!=total ||
        fread(triangles,sizeof(Triangle),header.collision,f)!=header.collision) return false;
    if (fread(&opening_count,4,1,f)!=1 || opening_count>MAX_OPENINGS ||
        fread(openings,sizeof(Opening),opening_count,f)!=opening_count || fgetc(f)!=EOF || ferror(f)) return false;
    ui_loading_progress(85);
    offsets[header.batches]=total;
    sky_end=0;
    while (sky_end<header.batches && batches[sky_end].group==5) ++sky_end;
    active_end=sky_end;
    while (active_end<header.batches && batches[active_end].group<5) ++active_end;
    unsigned cursor=active_end;
    for (unsigned i=0;i<opening_count;i++) {
        unsigned base=NEIGHBOUR_GROUP+i*8;
        if (openings[i].destination< -1 || openings[i].destination>=ROOM_COUNT ||
            cursor>=header.batches || batches[cursor].group!=base+MASK_GROUP) return false;
        mask_batch[i]=cursor++;
        while (cursor<header.batches && batches[cursor].group>=base && batches[cursor].group<base+5) ++cursor;
    }
    if (cursor!=header.batches) return false;
    unsigned dynamic=header.collision-header.group_counts[0];
    if (dynamic) {
        variants=malloc(dynamic*sizeof(*variants));
        if (!variants) return false;
        memcpy(variants,triangles+header.group_counts[0],dynamic*sizeof(*variants));
    }
    GSPGPU_FlushDataCache(vertices,total*sizeof(Vertex));
    return true;
}

bool room_geometry(bool doors_open,bool gate) {
    shown=1u | (1u<<(doors_open?2:1)) | (1u<<(gate?4:3));
    active_count=header.group_counts[0];
    unsigned offset=0;
    for (unsigned i=1;i<5;i++) {
        unsigned n=header.group_counts[i];
        if (n && (shown&(1u<<i))) {
            memcpy(triangles+active_count,variants+offset,n*sizeof(Triangle));
            active_count+=n;
        }
        offset+=n;
    }
    scene_close();
    return scene_open(triangles,active_count);
}
static void clear_geometry(void) {
    nav_close();
    scene_close();
    // The queue is idle. Both units get their new room textures before the next draw.
    for (unsigned i=0;i<loaded;i++) C3D_TexDelete(&textures[i]);
    loaded=0;
    if (vertices) linearFree(vertices);
    free(triangles);free(variants);
    vertices=NULL;triangles=NULL;variants=NULL;active_count=0;
}
static const char *const room_files[ROOM_COUNT]={
    "romfs:/start.room","romfs:/alley.room","romfs:/street.room","romfs:/subway.room","romfs:/sidealley.room",
    "romfs:/deadend.room","romfs:/home.room","romfs:/homephone.room","romfs:/homeliving.room","romfs:/homestairs.room"
};
static struct {RoomId room;Thread thread;void *data;size_t size;} prefetched={.room=ROOM_COUNT};
static void read_ahead(void *unused){
    (void)unused;
    FILE *f=fopen(room_files[prefetched.room],"rb");if(!f)return;
    if(!fseek(f,0,SEEK_END)){
        long size=ftell(f);
        if(size>0 && size<=4*1024*1024 && !fseek(f,0,SEEK_SET)){
            void *buf=malloc(size);
            if(buf && fread(buf,1,size,f)==(size_t)size){prefetched.data=buf;prefetched.size=size;}
            else free(buf);
        }
    }
    fclose(f);
}
static bool finish_read_ahead(uint64_t wait){
    if(!prefetched.thread)return true;
    if(R_FAILED(threadJoin(prefetched.thread,wait)))return false;
    threadFree(prefetched.thread);prefetched.thread=NULL;return true;
}
void room_hint(Point position,float dx,float dz){
    if(dx*dx+dz*dz<.000001f || !finish_read_ahead(0))return;
    RoomId best=ROOM_COUNT;float nearest=12*12;
    for(unsigned i=0;i<4;i++){
        const Portal *e=header.exits+i;if(e->destination<0 || e->destination>=ROOM_COUNT)continue;
        float x=e->center.x-position.x,z=e->center.z-position.z,d=x*x+z*z;
        if(x*dx+z*dz>0 && d<nearest){nearest=d;best=e->destination;}
    }
    if(best==ROOM_COUNT || best==prefetched.room)return;
    free(prefetched.data);prefetched.data=NULL;prefetched.size=0;prefetched.room=best;
    prefetched.thread=threadCreate(read_ahead,NULL,16*1024,0x31,-2,false);
}
bool room_switch(RoomId id,bool doors_open,bool gate) {
    ui_loading_begin();
    FILE *f=NULL;
    if(id==prefetched.room && finish_read_ahead(UINT64_MAX) && prefetched.data)
        f=fmemopen(prefetched.data,prefetched.size,"rb");
    if(!f)f=fopen(room_files[id],"rb");
    if (!f) return false;
    if (target) C3D_RenderTargetSetOutput(NULL,GFX_TOP,GFX_LEFT,0);
    actor_leave_enemies();
    clear_geometry();
    bool ok=read_room(f);
    fclose(f);
    if(id==prefetched.room){free(prefetched.data);prefetched.data=NULL;prefetched.room=ROOM_COUNT;}
    if (!ok || !room_geometry(doors_open,gate) || (id==STREET && !nav_open())) return false;
    BufInfo_Init(&world_binding);
    if (BufInfo_Add(&world_binding,vertices,sizeof(Vertex),3,0x210)<0) return false;
    if (target) C3D_RenderTargetSetOutput(target,GFX_TOP,GFX_LEFT,
        GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8)|GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8));
    if(!old_model)campaign_training_light(id);
    ui_loading_progress(100);
    return true;
}
static void gpu_event(void *data) {
    async_log_gpu_event((unsigned)(uintptr_t)data);
}

static bool renderer_open(RoomId first,const char *stage,Player *player,bool young) {
    bool skyline=stage && !strcmp(stage,"startup_level");
    near_clip=skyline?.32f:.08f;far_clip=skyline?2400.f:135.f;
    bool enhanced=false;
    APT_CheckNew3DS(&enhanced);old_model=!enhanced;
    // Three portal rooms, sorted particles and two eyes can exceed the SDK's
    // small default command list (the Street van encounter hit that limit).
    initialized=C3D_Init(4*C3D_DEFAULT_CMDBUF_SIZE);
    if (!initialized) goto fail;
    gspSetEventCallback(GSPGPU_EVENT_P3D,gpu_event,(void *)(uintptr_t)0,false);
    gspSetEventCallback(GSPGPU_EVENT_PPF,gpu_event,(void *)(uintptr_t)1,false);
    if (stage) {
        if (!campaign_open(stage,!old_model) || !campaign_enter(campaign_start(player))) goto fail;
    } else if (!room_switch(first,false,false)) goto fail;
    target=C3D_RenderTargetCreate(240,400,GPU_RB_RGBA8,GPU_RB_DEPTH24_STENCIL8);
    if (!target) goto fail;
    const unsigned flags=GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8)|GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8);
    C3D_RenderTargetSetOutput(target,GFX_TOP,GFX_LEFT,flags);
    dvlb=DVLB_ParseFile((u32 *)room_shader,sizeof room_shader);
    if (!dvlb) goto fail;
    shaderProgramInit(&program);
    program_ready=true;
    if (R_FAILED(shaderProgramSetVsh(&program,&dvlb->DVLE[0]))) goto fail;
    C3D_BindProgram(&program);
    projection_location=shaderInstanceGetUniformLocation(program.vertexShader,"projection");
    if (!actor_load(young,!old_model) || !effects_open()) goto fail;
    AttrInfo_Init(&actor_attr);
    AttrInfo_AddLoader(&actor_attr,0,GPU_FLOAT,3);
    AttrInfo_AddLoader(&actor_attr,1,GPU_FLOAT,2);
    AttrInfo_AddFixed(&actor_attr,2);
    AttrInfo_Init(&skin_attr);
    AttrInfo_AddLoader(&skin_attr,0,GPU_FLOAT,3);
    AttrInfo_AddLoader(&skin_attr,1,GPU_FLOAT,2);
    AttrInfo_AddLoader(&skin_attr,2,GPU_UNSIGNED_BYTE,4);
    AttrInfo_Init(&effect_attr);
    AttrInfo_AddLoader(&effect_attr,0,GPU_FLOAT,3);
    AttrInfo_AddLoader(&effect_attr,1,GPU_FLOAT,2);
    AttrInfo_AddFixed(&effect_attr,2);
    const float clear_triangle[3][5]={{-1,-1,0,0,0},{3,-1,0,0,0},{-1,3,0,0,0}};
    clear_vertices=linearAlloc(sizeof clear_triangle);
    if (!clear_vertices) goto fail;
    memcpy(clear_vertices,clear_triangle,sizeof clear_triangle);
    GSPGPU_FlushDataCache(clear_vertices,sizeof clear_triangle);
    BufInfo_Init(&clear_binding);
    if (BufInfo_Add(&clear_binding,clear_vertices,sizeof clear_triangle[0],2,0x10)<0) goto fail;
    C3D_AttrInfo *attr=&world_attr;
    AttrInfo_Init(attr);
    AttrInfo_AddLoader(attr,0,GPU_FLOAT,3);
    AttrInfo_AddLoader(attr,1,GPU_FLOAT,2);
    AttrInfo_AddLoader(attr,2,GPU_FLOAT,2);
    C3D_CullFace(GPU_CULL_NONE);
    C3D_DepthTest(true,GPU_GEQUAL,GPU_WRITE_ALL);
    C3D_AlphaTest(true,GPU_GREATER,63);
    C3D_TexEnv *env=&world_env;
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env,C3D_Both,GPU_TEXTURE0,GPU_TEXTURE1,GPU_PRIMARY_COLOR);
    C3D_TexEnvFunc(env,C3D_RGB,GPU_MODULATE);
    C3D_TexEnvFunc(env,C3D_Alpha,GPU_REPLACE);
    C3D_TexEnvScale(env,C3D_RGB,GPU_TEVSCALE_1);
    Mtx_PerspTilt(&projection,C3D_AngleFromDegrees(62),400.f/240.f,near_clip,far_clip,true);
    return true;
fail:
    room_close();
    return false;
}

void room_motion_range(float travel){
    // Preserve depth precision indoors while allowing rail traffic at its authored distance.
    far_clip=fmaxf(far_clip,fminf(2400.f,135.f+travel));
    Mtx_PerspTilt(&projection,C3D_AngleFromDegrees(62),400.f/240.f,near_clip,far_clip,true);
}
bool room_load(RoomId first) {return renderer_open(first,NULL,NULL,first>=HOME);}
bool room_campaign(const char *stage,Player *player,bool young) {return renderer_open(START_ROOM,stage,player,young);}
bool room_campaign_enter(unsigned room) {
    return campaign_enter(room);
}
void room_close(void) {
    finish_read_ahead(UINT64_MAX);free(prefetched.data);prefetched.data=NULL;prefetched.room=ROOM_COUNT;
    // Detaching the target drains queued draws before their inputs are freed.
    if (right_target) C3D_RenderTargetDelete(right_target);
    if (target) C3D_RenderTargetDelete(target);
    if (clear_vertices) linearFree(clear_vertices);
    clear_vertices=NULL;
    right_target=NULL;stereo_active=stereo_failed=false;gfxSet3D(false);
    campaign_training_light(ROOM_COUNT);
    campaign_close();
    item_mesh_close();
    actor_close();
    if (initialized) clear_geometry();
    particles_close();effects_close();
    if (program_ready) shaderProgramFree(&program);
    if (dvlb) DVLB_Free(dvlb);
    if (initialized) {
        gspSetEventCallback(GSPGPU_EVENT_P3D,NULL,NULL,false);
        gspSetEventCallback(GSPGPU_EVENT_PPF,NULL,NULL,false);
        C3D_Fini();
    }
    gfxSetScreenFormat(GFX_TOP,GSP_RGB565_OES);
    triangles=NULL; vertices=NULL; target=NULL; dvlb=NULL;
    loaded=0; initialized=false; program_ready=false;
}

static void draw_batch(unsigned i) {
    C3D_TexBind(0,&textures[batches[i].texture]);
    C3D_TexBind(1,&textures[batches[i].light]);
    C3D_DrawArrays(GPU_TRIANGLES,offsets[i],batches[i].count);
}

// Test the actual aperture against the eye frustum, including openings crossing
// the near plane. A tilted 3DS projection already accounts for LCD orientation.
static bool opening_visible(unsigned mask,const C3D_Mtx *matrix,bool *crosses_near) {
    unsigned outside=31;
    for (unsigned i=offsets[mask];i<offsets[mask+1];i++) {
        Point p=vertices[i].pos;
        C3D_FVec v=Mtx_MultiplyFVecH(matrix,FVec3_New(p.x,p.y,p.z));
        unsigned code=(v.x < -v.w) | ((v.x>v.w)<<1) | ((v.y < -v.w)<<2) |
                      ((v.y>v.w)<<3) | ((v.w<=0)<<4);
        if(v.z<=-v.w)*crosses_near=true;
        outside&=code;
    }
    return outside==0;
}

static unsigned neighbour_states(int destination,const Mission *mission) {
    bool open=(destination==START_ROOM && mission->van) || (destination==STREET && mission->rifle) ||
              (destination==SUBWAY && mission->cabinet);
    return 1u | (1u<<(open?2:1)) | (1u<<(mission->gate?4:3));
}

static void draw_neighbours(const C3D_Mtx *combined,const Mission *mission) {
    for (unsigned i=0;i<opening_count;i++) {
        unsigned mask=mask_batch[i];
        bool crosses_near=false;
        if (!opening_visible(mask,combined,&crosses_near)) continue;
        // Mark visible aperture pixels after drawing the current room. Walls,
        // railings and doors keep their depth and can occlude the opening.
        C3D_StencilTest(true,GPU_ALWAYS,i+1,0xff,0xff);
        C3D_StencilOp(GPU_STENCIL_KEEP,GPU_STENCIL_KEEP,GPU_STENCIL_REPLACE);
        C3D_TexBind(0,&textures[batches[mask].texture]);C3D_TexBind(1,&textures[batches[mask].light]);
        draw_aperture(projection_location,combined,offsets[mask],batches[mask].count,crosses_near);
        C3D_StencilTest(true,GPU_EQUAL,i+1,0xff,0);
        C3D_StencilOp(GPU_STENCIL_KEEP,GPU_STENCIL_KEEP,GPU_STENCIL_KEEP);
        C3D_DepthTest(true,GPU_GEQUAL,GPU_WRITE_ALL);
        unsigned end=i+1<opening_count?mask_batch[i+1]:header.batches;
        unsigned states=neighbour_states(openings[i].destination,mission);
        for (unsigned j=mask+1;j<end;j++)
            if (states&(1u<<(batches[j].group&7))) draw_batch(j);
    }
    C3D_StencilTest(false,GPU_ALWAYS,0,0xff,0xff);
}

static void campaign_fog(float projection_near,uint32_t color) {
    static C3D_FogLut lut;
    static float previous_near=-INFINITY,previous_far=-INFINITY,previous_near_clip=0,previous_far_clip=0;
    if(!presentation.fog){C3D_FogGasMode(GPU_NO_FOG,GPU_PLAIN_DENSITY,false);return;}
    float near=presentation_value(presentation.fog_near),far=presentation_value(presentation.fog_far);
    if(near!=previous_near || far!=previous_far || previous_near_clip!=projection_near || previous_far_clip!=far_clip) {
        float curve[256],last=0;
        for(unsigned i=0;i<=128;i++) {
            float z=FogLut_CalcZ(i/128.f,projection_near,far_clip);
            float clear=fminf(1,fmaxf(0,(far-z)/fmaxf(.001f,far-near)));
            if(i<128)curve[i]=clear;
            if(i)curve[127+i]=clear-last;
            last=clear;
        }
        FogLut_FromArray(&lut,curve);previous_near=near;previous_far=far;
        previous_near_clip=projection_near;previous_far_clip=far_clip;
    }
    C3D_FogGasMode(GPU_FOG,GPU_PLAIN_DENSITY,false);
    C3D_FogColor(color);C3D_FogLutBind(&lut);
}
static void clear_eye(C3D_RenderTarget *screen,u32 color) {
    // Clear in the drawing command list so color, depth and stencil are ready
    // before the first draw for this eye, with no separate GX memory fill.
    C3D_FrameDrawOn(screen);
    C3D_Mtx clip;
    Mtx_Identity(&clip);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER,projection_location,&clip);
    C3D_SetAttrInfo(&effect_attr);
    C3D_SetBufInfo(&clear_binding);
    C3D_FixedAttribSet(2,0,0,0,1);
    C3D_FogGasMode(GPU_NO_FOG,GPU_PLAIN_DENSITY,false);
    C3D_DepthMap(true,-1,0);
    C3D_AlphaBlend(GPU_BLEND_ADD,GPU_BLEND_ADD,GPU_ONE,GPU_ZERO,GPU_ONE,GPU_ZERO);
    C3D_AlphaTest(false,GPU_ALWAYS,0);
    C3D_DepthTest(true,GPU_ALWAYS,GPU_WRITE_ALL);
    C3D_StencilTest(true,GPU_ALWAYS,0,0xff,0xff);
    C3D_StencilOp(GPU_STENCIL_KEEP,GPU_STENCIL_KEEP,GPU_STENCIL_REPLACE);
    C3D_CullFace(GPU_CULL_NONE);
    C3D_TexEnv *env=C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env,C3D_Both,GPU_CONSTANT,GPU_CONSTANT,GPU_CONSTANT);
    C3D_TexEnvColor(env,color);
    C3D_DrawArrays(GPU_TRIANGLES,0,3);

    C3D_DepthTest(true,GPU_GEQUAL,GPU_WRITE_ALL);
    C3D_AlphaTest(true,GPU_GREATER,63);
    C3D_StencilTest(false,GPU_ALWAYS,0,255,0);
    C3D_StencilOp(GPU_STENCIL_KEEP,GPU_STENCIL_KEEP,GPU_STENCIL_KEEP);
}

static void draw_eye(C3D_RenderTarget *screen,const C3D_Mtx *projection_eye,const C3D_Mtx *view,float clip,
                     const Player *p,const Animation *anim,const Mission *mission,const Enemy *cast,bool unarmed,unsigned visible,const Effects *fx) {
    clear_eye(screen,0xff181818);
    if(!mission){
        uint32_t fog_color=__builtin_bswap32(presentation_fog())>>8;
        campaign_fog(clip,fog_color);
        alpha_begin(projection_location,&world_attr,&actor_attr,&skin_attr,&world_env,presentation.fog,fog_color);
    }
    C3D_SetAttrInfo(&world_attr);
    C3D_SetBufInfo(&world_binding);
    C3D_SetTexEnv(0,&world_env);
    C3D_Mtx combined;
    Mtx_Multiply(&combined,projection_eye,view);
    // Remove camera translation for the worldsphere. Its lookup stays stable when
    // walking, and its colour never occludes finite geometry in either eye.
    C3D_Mtx sky=*view;
    sky.r[0].w=sky.r[1].w=sky.r[2].w=0;
    C3D_Mtx sky_clip;
    Mtx_Multiply(&sky_clip,projection_eye,&sky);
    if (!mission) campaign_draw(projection_location,&combined,&sky_clip,director.exits);
    else {
        C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER,projection_location,&sky_clip);
        C3D_DepthTest(false,GPU_ALWAYS,GPU_WRITE_COLOR);
        for (unsigned i=0;i<sky_end;i++) draw_batch(i);
        C3D_DepthTest(true,GPU_GEQUAL,GPU_WRITE_ALL);
        C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER,projection_location,&combined);
        for (unsigned i=sky_end;i<active_end;i++)
            if (shown&(1u<<batches[i].group)) draw_batch(i);
        draw_neighbours(&combined,mission);
    }
    // KF2 uses clockwise fronts. The lining and outside of the coat share vertices.
    C3D_CullFace(GPU_CULL_FRONT_CCW);
    C3D_SetAttrInfo(&actor_attr);
    C3D_FixedAttribSet(2,0,0,0,1);
    C3D_TexEnv *env=C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env,C3D_Both,GPU_TEXTURE0,GPU_TEXTURE0,GPU_TEXTURE0);
    if(!mission){
        C3D_TexEnvSrc(env,C3D_RGB,GPU_TEXTURE0,GPU_CONSTANT,GPU_PRIMARY_COLOR);
        C3D_TexEnvFunc(env,C3D_RGB,GPU_MODULATE);
        campaign_draw_items(projection_location,&combined);
    }
    for (unsigned i=0;i<=(mission?ENEMY_SLOTS:CAST_SLOTS);i++) {
        if (!(visible&(1u<<i))) continue;
        const Player *body=i?&cast[i-1].body:p;
        const Animation *pose=i?&cast[i-1].animation:anim;
        unsigned portal=mission?0:i?cast[i-1].portal:campaign_first_view(director.state[0].room);
        while(portal!=255){
            Player placed=*body;unsigned body_view=portal;
            if(!mission){
                if(!i && (!campaign_view_enabled(portal,director.exits) || !campaign_placement(director.state[0].room,portal,&placed))){
                    portal=campaign_next_view(portal);continue;
                }
                body_view=campaign_body_view(portal,campaign_actor_feet(i),body->crouched);
                if(!campaign_visible_transform(body_view)){portal=i?255:campaign_next_view(portal);continue;}
                campaign_stencil(body_view);
                C3D_TexEnvColor(C3D_GetTexEnv(0),campaign_actor_light(i));
            }
            C3D_Mtx model_view=combined;
            Mtx_Translate(&model_view,placed.position.x,placed.position.y,placed.position.z,true);
            Mtx_RotateY(&model_view,placed.yaw,true);
            C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER,projection_location,&model_view);
            C3D_SetAttrInfo(&skin_attr);
            env=C3D_GetTexEnv(0);
            if(old_model && mission)C3D_TexEnvColor(env,0xffffffff);
            C3D_TexEnvSrc(env,C3D_RGB,GPU_TEXTURE0,old_model?GPU_CONSTANT:GPU_PRIMARY_COLOR,GPU_CONSTANT);
            C3D_TexEnvFunc(env,C3D_RGB,GPU_MODULATE);
            C3D_TexEnvScale(env,C3D_RGB,old_model?GPU_TEVSCALE_1:GPU_TEVSCALE_2);
            actor_draw(pose,i,!mission || (i?cast[i-1].unarmed:unarmed),mission?NULL:&model_view,body_view,old_model);
            C3D_SetAttrInfo(&actor_attr);
            env=C3D_GetTexEnv(0);
            C3D_TexEnvScale(env,C3D_RGB,GPU_TEVSCALE_1);
            C3D_TexEnvSrc(env,C3D_RGB,GPU_TEXTURE0,GPU_CONSTANT,GPU_PRIMARY_COLOR);
            if(!mission){
                C3D_CullFace(GPU_CULL_NONE);
                C3D_DepthTest(true,GPU_GEQUAL,GPU_WRITE_ALL);
                C3D_AlphaTest(false,GPU_ALWAYS,0);
                campaign_draw_weapon(i,pose,projection_location,&model_view);
                C3D_AlphaTest(true,GPU_GREATER,63);
                C3D_CullFace(GPU_CULL_FRONT_CCW);
            }
            portal=mission || i?255:campaign_next_view(portal);
        }
    }
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER,projection_location,&combined);
    if(!mission){campaign_stencil(0);C3D_TexEnvFunc(C3D_GetTexEnv(0),C3D_RGB,GPU_REPLACE);}
    C3D_CullFace(GPU_CULL_NONE);
    if(!mission){
        campaign_queue_translucent(projection_location,&combined,director.exits);
        effects_queue(&combined,fx);particles_queue(&combined);
        alpha_finish();
        C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER,projection_location,&combined);
    }
    if(!mission && director.camera<director.count){
        C3D_Mtx mono;Mtx_Multiply(&mono,&monoscopic_projection,view);
        C3D_SetAttrInfo(&world_attr);C3D_SetTexEnv(0,&world_env);
        C3D_CullFace(GPU_CULL_NONE);C3D_FogGasMode(GPU_NO_FOG,GPU_PLAIN_DENSITY,false);
        campaign_draw_camera_overlay(projection_location,&mono);
    }
    C3D_SetAttrInfo(&effect_attr);
    C3D_FixedAttribSet(1,0,0,0,1);
    C3D_FixedAttribSet(2,0,0,0,1);
    C3D_FogGasMode(GPU_NO_FOG,GPU_PLAIN_DENSITY,false);
    effects_draw(!pause_overlay && (mission || (!director.state[0].dead && campaign_reticle_visible() && director.controls && presentation_value(presentation.visibility)>=.5f)),
                 mission!=NULL);
    if(!mission && !director.state[0].dead){effects_overlay(projection_location,presentation_fade());effects_overlay(projection_location,presentation_flash());}
    if(pause_overlay)effects_pause(projection_location);
}
void room_pause_image(bool dim){
    if(!target)return;
    // Resolve the last completed world frame again. Both eyes keep their original parallax.
    if(C3D_FrameBegin(0))C3D_FrameEnd(0);
    gfxSet3D(stereo_active);
    gfxSetScreenFormat(GFX_TOP,dim?GSP_RGB565_OES:GSP_BGR8_OES);
    for(unsigned eye=0;eye<(stereo_active?2u:1u);eye++){
        C3D_RenderTarget *src=eye?right_target:target;
        uint16_t *fb=(uint16_t*)gfxGetFramebuffer(GFX_TOP,eye?GFX_RIGHT:GFX_LEFT,NULL,NULL);
        C3D_SyncDisplayTransfer(src->frameBuf.colorBuf,GX_BUFFER_DIM(240,400),(u32*)fb,GX_BUFFER_DIM(240,400),
            GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8)|GX_TRANSFER_OUT_FORMAT(dim?GX_TRANSFER_FMT_RGB565:GX_TRANSFER_FMT_RGB8));
        if(dim){
            GSPGPU_InvalidateDataCache(fb,400*240*2);ui_pause_image(fb);GSPGPU_FlushDataCache(fb,400*240*2);
        }
    }
    gfxScreenSwapBuffers(GFX_TOP,stereo_active);gspWaitForVBlank();
}
bool room_stereo(void) { return stereo_active; }

static RoomDrawTiming draw_timing;
RoomDrawTiming room_draw_timing(void){return draw_timing;}
static float cinematic_near_clip(void){
    float clip=near_clip;
    if(director.controls || director.camera>=director.count || director.path.path!=DIRECTOR_NONE)return clip;
    // Authored close-ups can sit just above a tabletop. The gameplay near
    // plane cut the surface open below the phone even though its mesh was whole.
    Point eye;director_pose(director.camera,&eye,NULL,NULL);
    static const Point axes[]={{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
    for(unsigned i=0;i<6;i++){
        Hit hit;int prop;
        if(campaign_ray_wall(director.state[director.camera].room,eye,axes[i],4*near_clip,&hit,&prop))
            clip=fminf(clip,fmaxf(.012f,hit.distance*.25f));
    }
    return clip;
}
static void render_frame(const Player *p,const Animation *anim,const Camera *camera,const Effects *fx,const Mission *mission,
                         const Enemy *cast,unsigned count,bool unarmed,unsigned zoom) {
    u64 section=svcGetSystemTick();draw_timing=(RoomDrawTiming){0};
    async_log_watch("gpu-vblank");
    gspWaitForVBlank0();
    async_log_watch("gpu-queue");
    if (!C3D_FrameBegin(0)){async_log_watch(NULL);return;}
    draw_timing.wait=svcGetSystemTick()-section;section=svcGetSystemTick();
    async_log_watch("skin-and-poses");
    // 24-bit scanout preserves the filtered lighting; 565 rounded the final
    // lit image back to 32 red/blue levels even with an RGBA8 render target.
    gfxSetScreenFormat(GFX_TOP,GSP_BGR8_OES);
    float slider=osGet3DSliderState();
    if (slider>0 && !right_target && !stereo_failed) {
        right_target=C3D_RenderTargetCreate(240,400,GPU_RB_RGBA8,GPU_RB_DEPTH24_STENCIL8);
        if (right_target) C3D_RenderTargetSetOutput(right_target,GFX_TOP,GFX_RIGHT,
            GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8)|GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8));
        else stereo_failed=true;
    }
    stereo_active=right_target && slider>0;
    gfxSet3D(stereo_active);
    unsigned visible=0;
    unsigned instances=1+count;
    u64 now=svcGetSystemTick();
    for (unsigned i=0;i<instances;i++) {
        const Player *body=i?&cast[i-1].body:p;
        Point pos=body->position;
        float depth=(pos.x-camera->eye.x)*camera->forward.x+(pos.y+1-camera->eye.y)*camera->forward.y+
                    (pos.z-camera->eye.z)*camera->forward.z;
        bool cinematic=!mission && (!director.controls || director.path.path!=DIRECTOR_NONE || director.state[0].room!=campaign_room_id());
        if(!i && cinematic){
            if(campaign_first_view(director.state[0].room)==255)continue;
        }else if((!i && camera->distance<=.40f) || depth< -2.5f)continue;
        if(!old_model)actor_set_light(i,mission?room_model_light(mission->room,body,false,false):campaign_model_light(i));
        actor_prepare(i?&cast[i-1].animation:anim,body->pitch,i,mission && (i?!cast[i-1].unarmed:!unarmed),now);
        visible|=1u<<i;
    }
    draw_timing.skin=svcGetSystemTick()-section;section=svcGetSystemTick();
    async_log_watch("effects");
    if(!mission){campaign_prepare_weapon_effects(visible,count);particles_prepare(camera);}
    effects_prepare(fx,camera,zoom,!mission && director.controls && director.can_use);
    draw_timing.effects=svcGetSystemTick()-section;section=svcGetSystemTick();
    async_log_watch("draw-commands");
    C3D_FVec eye=FVec3_New(camera->eye.x,camera->eye.y,camera->eye.z);
    C3D_FVec aim=FVec3_Add(eye,FVec3_New(camera->forward.x,camera->forward.y,camera->forward.z));
    C3D_Mtx view;
    Mtx_LookAt(&view,eye,aim,FVec3_New(camera->up.x,camera->up.y,camera->up.z),true);
    float scale=(mission || director.state[0].dead)?1:presentation_value(presentation.fov);
    float clip=mission?near_clip:cinematic_near_clip();
    float fov=fminf(C3D_AngleFromDegrees(155),C3D_AngleFromDegrees(62)*scale);
    if (zoom>1) fov=2*atanf(tanf(fov*.5f)/zoom);
    Mtx_PerspTilt(&monoscopic_projection,fov,400.f/240.f,clip,far_clip,true);
    unsigned first_eye_words=0;
    if (stereo_active) {
        C3D_Mtx lens;
        for (int side=0;side<2;side++) {
            // Keep the framebuffer cache flush between stereo command lists.
            if(side){
                first_eye_words=gpuCmdBufOffset;
                C3D_FrameSplit(GX_CMDLIST_FLUSH);
            }
            float iod=(side?1.f:-1.f)*.064f*slider/zoom;
            Mtx_PerspStereoTilt(&lens,fov,400.f/240.f,clip,far_clip,iod,zoom>1?SCOPE_FOCUS:STEREO_FOCUS,true);
            draw_eye(side?right_target:target,&lens,&view,clip,p,anim,mission,cast,unarmed,visible,fx);
        }
    } else if (zoom>1 || scale!=1 || clip!=near_clip) {
        C3D_Mtx lens;
        Mtx_PerspTilt(&lens,fov,400.f/240.f,clip,far_clip,true);
        draw_eye(target,&lens,&view,clip,p,anim,mission,cast,unarmed,visible,fx);
    } else draw_eye(target,&projection,&view,clip,p,anim,mission,cast,unarmed,visible,fx);
    draw_timing.commands=svcGetSystemTick()-section;section=svcGetSystemTick();
    async_log_gpu_frame(mission?(unsigned)mission->room:campaign_room_id(),first_eye_words+gpuCmdBufOffset,stereo_active?2:1);
    async_log_watch("gpu-submit");
    C3D_FrameEnd(GX_CMDLIST_FLUSH);
    draw_timing.finish=svcGetSystemTick()-section;
    async_log_watch(NULL);
}

void room_animation_gallery(void){
    unsigned clip=1,total=pose_bank_count();float seconds=0,yaw=.25f;
    if(!total)return;
    ui_animation_card(pose_bank_name(clip),clip,total);
    u64 previous=svcGetSystemTick();
    while(aptMainLoop()){
        hidScanInput();u32 down=hidKeysDown(),held=hidKeysHeld();
        if(down&(KEY_B|KEY_START))break;
        int step=(down&KEY_DRIGHT)?1:(down&KEY_DLEFT)?-1:(down&KEY_R)?10:(down&KEY_L)?-10:0;
        if(down&KEY_TOUCH){touchPosition pos;hidTouchRead(&pos);if(pos.py>=153 && pos.py<190)step=pos.px<160?-1:1;}
        if(step){clip=1+((int)clip-1+step+(int)total)%(int)total;seconds=0;ui_animation_card(pose_bank_name(clip),clip,total);}
        u64 now=svcGetSystemTick();float dt=fminf(.08f,(now-previous)/(float)SYSCLOCK_ARM11);previous=now;
        seconds=fmodf(seconds+dt,pose_bank_duration(clip));yaw+=dt*(!!(held&KEY_DUP)-!!(held&KEY_DDOWN));
        Animation a={.scripted=clip,.script_time=seconds};
        if(!C3D_FrameBegin(C3D_FRAME_SYNCDRAW))continue;
        C3D_DepthTest(true,GPU_GEQUAL,GPU_WRITE_ALL);C3D_StencilTest(false,GPU_ALWAYS,0,255,255);
        C3D_AlphaTest(true,GPU_GREATER,63);C3D_CullFace(GPU_CULL_FRONT_CCW);
        C3D_FogGasMode(GPU_NO_FOG,GPU_PLAIN_DENSITY,false);
        C3D_SetAttrInfo(&actor_attr);C3D_FixedAttribSet(2,0,0,0,1);
        C3D_TexEnv *env=C3D_GetTexEnv(0);C3D_TexEnvInit(env);C3D_TexEnvSrc(env,C3D_Both,GPU_TEXTURE0,GPU_TEXTURE0,GPU_TEXTURE0);
        actor_prepare(&a,0,0,false,now);Point c=actor_center();
        C3D_Mtx view;Mtx_LookAt(&view,FVec3_New(c.x+2.9f*sinf(yaw),c.y+.13f,c.z+2.9f*cosf(yaw)),
                                  FVec3_New(c.x,c.y,c.z),FVec3_New(0,1,0),true);
        float slider=osGet3DSliderState();bool stereo=slider>0 && right_target;gfxSet3D(stereo);
        for(unsigned eye=0;eye<=(unsigned)stereo;eye++){
            C3D_RenderTarget *out=eye?right_target:target;
            clear_eye(out,0xff302b28);
            C3D_Mtx lens,m;Mtx_PerspStereoTilt(&lens,C3D_AngleFromDegrees(48),400.f/240.f,.08f,25.f,stereo?(eye?1:-1)*.045f*slider:0,2.9f,true);
            Mtx_Multiply(&m,&lens,&view);C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER,projection_location,&m);C3D_SetAttrInfo(&skin_attr);
            C3D_TexEnv *skin_env=C3D_GetTexEnv(0);
            C3D_TexEnvInit(skin_env);C3D_TexEnvSrc(skin_env,C3D_Both,GPU_TEXTURE0,GPU_TEXTURE0,GPU_TEXTURE0);
            C3D_CullFace(GPU_CULL_FRONT_CCW);actor_draw(&a,0,true,NULL,0,old_model);
        }
        C3D_CullFace(GPU_CULL_NONE);C3D_FrameEnd(GX_CMDLIST_FLUSH);
    }
}
void room_render(const Player *p,const Animation *anim,const Camera *camera,const Effects *fx,const Mission *m,unsigned zoom) {
    EnemyGroup group=mission_group(m,m->room);
    render_frame(p,anim,camera,fx,m,m->enemies+group.first,group.count,m->room>=HOME && !m->house.armed,zoom);
}
void room_render_campaign(const Player *p,const Animation *anim,const Camera *camera,const Effects *fx,
                          const Enemy *cast,unsigned count,bool unarmed,unsigned zoom) {
    render_frame(p,anim,camera,fx,NULL,cast,count,unarmed,zoom);
}

bool room_near(const Player *p,Point at,float radius) {
    float dx=p->position.x-at.x,dz=p->position.z-at.z;
    return dx*dx+dz*dz<radius*radius && fabsf(p->position.y+1.40f-at.y)<1.8f;
}
bool room_portal(const Player *p,const Portal *exit) {
    float x=p->position.x-exit->center.x,z=p->position.z-exit->center.z;
    float depth=x*exit->normal.x+z*exit->normal.z;
    return exit->destination>=0 && depth>-.40f && depth<.52f &&
           fabsf(x*exit->normal.z-z*exit->normal.x)<exit->width-.20f &&
           fabsf(p->position.y-exit->center.y)<1.8f;
}
