#include "item_mesh.h"
#include "campaign_file.h"
#include "texture.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

enum { MATERIALS=12, MODELS=56, RUN_CULL_BACK_FACES=0x8000 };
typedef struct { Point position; float uv[2]; } Vertex;
typedef struct {uint16_t node,material;uint32_t count;} DrawRun;
typedef struct {float row[3][4];} NodePose;
typedef struct {unsigned count;float duration;NodePose *poses;} Track;
typedef struct {
    DrawRun *runs;unsigned run_count,nodes;Track tracks[3];
    Point projectile_offset;char muzzle_effect[64];
    char file[96];C3D_Tex textures[MATERIALS];unsigned counts[MATERIALS],loaded;
    C3D_BufInfo binding;Vertex *vertices;
} Model;
static Model models[MODELS];
static unsigned model_count;

// The nonhuman skeleton shares the indexed geometry and textures across every rat.
static struct {
    uint16_t *indices;
    float *uv;
    struct { unsigned frames;float seconds;int16_t *positions; } tracks[RAT_TRACKS];
    unsigned points;
    C3D_AttrInfo attributes;
} rat;
static void close_rat(void){
    if(rat.indices)linearFree(rat.indices);
    if(rat.uv)linearFree(rat.uv);
    for(unsigned i=0;i<RAT_TRACKS;i++)if(rat.tracks[i].positions)linearFree(rat.tracks[i].positions);
    memset(&rat,0,sizeof rat);
}
static void *read_vertices(FILE *f,size_t bytes){
    void *p=linearAlloc(bytes);
    if(!p)return NULL;
    if(fread(p,1,bytes,f)!=bytes){linearFree(p);return NULL;}
    GSPGPU_FlushDataCache(p,bytes);
    return p;
}
static bool open_rat(unsigned total){
    FILE *f=campaign_file("cast/rat.anim");
    if(!f)return false;
    struct {char tag[4];unsigned points,indices,tracks;} h;
    bool ok=fread(&h,sizeof h,1,f)==1 && !memcmp(h.tag,"RAT1",4) && h.points && h.points<=512 &&
        h.indices==total && h.tracks==RAT_TRACKS;
    if(ok){
        rat.points=h.points;rat.indices=read_vertices(f,2*total);rat.uv=read_vertices(f,8*h.points);
        ok=rat.indices && rat.uv;
    }
    for(unsigned i=0;ok && i<total;i++)ok=rat.indices[i]<h.points;
    for(unsigned i=0;ok && i<RAT_TRACKS;i++){
        unsigned n;float seconds;
        ok=fread(&n,4,1,f)==1 && fread(&seconds,4,1,f)==1 && n>=2 && n<=256 && seconds>0 && isfinite(seconds);
        if(ok){
            rat.tracks[i].frames=n;rat.tracks[i].seconds=seconds;
            rat.tracks[i].positions=read_vertices(f,n*h.points*8);
            ok=rat.tracks[i].positions!=NULL;
        }
    }
    ok=ok && fgetc(f)==EOF;fclose(f);
    if(!ok){close_rat();return false;}
    // RAT1 pads each position to four shorts. Declare the padding as the
    // fourth component so the PICA fetch layout matches the eight-byte stride.
    AttrInfo_Init(&rat.attributes);AttrInfo_AddLoader(&rat.attributes,0,GPU_SHORT,4);
    AttrInfo_AddLoader(&rat.attributes,1,GPU_FLOAT,2);AttrInfo_AddFixed(&rat.attributes,2);
    return true;
}
float item_rat_duration(RatTrack track){return track<RAT_TRACKS?rat.tracks[track].seconds:0;}

static int find(const char *path) {
    for(unsigned i=0;i<model_count;i++) if(!strcmp(path,models[i].file)) return (int)i;
    return -1;
}
static void release(Model *model) {
    for(unsigned i=0;i<model->loaded;i++) C3D_TexDelete(model->textures+i);
    free(model->runs);for(unsigned i=0;i<3;i++)free(model->tracks[i].poses);
    if(model->vertices) linearFree(model->vertices);
    *model=(Model){0};
}
void item_mesh_close(void) {
    close_rat();
    while(model_count) release(models+--model_count);
}
static bool read_motion(Model *m,const char *name,unsigned total) {
    char fx_path[96];snprintf(fx_path,sizeof fx_path,"weapons/%s.fx",name);
    FILE *fx=campaign_file(fx_path);char magic[4];
    bool valid=fx && fread(magic,1,4,fx)==4 && !memcmp(magic,"WFX1",4) &&
        fread(&m->projectile_offset,sizeof m->projectile_offset,1,fx)==1 &&
        fread(m->muzzle_effect,1,sizeof m->muzzle_effect,fx)==sizeof m->muzzle_effect &&
        memchr(m->muzzle_effect,0,sizeof m->muzzle_effect) && fgetc(fx)==EOF &&
        isfinite(m->projectile_offset.x+m->projectile_offset.y+m->projectile_offset.z);
    if(fx)fclose(fx);
    if(!valid)return false;
    char path[96];snprintf(path,sizeof path,"weapons/%s.motion",name);
    FILE *f=campaign_file(path);if(!f)return false;
    struct {char tag[4];unsigned nodes,runs;} h;
    bool ok=fread(&h,sizeof h,1,f)==1 && !memcmp(h.tag,"WPN1",4) && h.nodes && h.nodes<=64 && h.runs && h.runs<=512;
    if(ok){
        m->nodes=h.nodes;m->run_count=h.runs;m->runs=malloc(h.runs*sizeof(DrawRun));
        ok=m->runs && fread(m->runs,sizeof(DrawRun),h.runs,f)==h.runs;
    }
    unsigned vertices=0;
    for(unsigned i=0;ok && i<m->run_count;i++){
        DrawRun r=m->runs[i];
        ok=r.node<m->nodes && (r.material&~RUN_CULL_BACK_FACES)<m->loaded &&
           r.count && r.count%3==0 && r.count<=total-vertices;
        vertices+=r.count;
    }
    for(unsigned i=0;ok && i<3;i++){
        Track *t=m->tracks+i;
        ok=fread(&t->count,4,1,f)==1 && fread(&t->duration,4,1,f)==1 && t->count>=2 && t->count<=240 && t->duration>0;
        if(ok){
            size_t n=t->count*m->nodes;t->poses=malloc(n*sizeof(NodePose));
            ok=t->poses && fread(t->poses,sizeof(NodePose),n,f)==n;
        }
    }
    ok=ok && vertices==total && fgetc(f)==EOF;fclose(f);return ok;
}
bool item_mesh_load(const char *category,const char *name) {
    char path[96];
    if(snprintf(path,sizeof path,"%s/%s.mesh",category,name)>=(int)sizeof path) return false;
    if(find(path)>=0) return true;
    if(model_count==MODELS) return false;
    FILE *file=campaign_file(path);
    if(!file) return false;
    struct {char magic[4];uint32_t batches,vertices;} h;
    Model *model=models+model_count;
    bool ok=fread(&h,sizeof h,1,file)==1 && !memcmp(h.magic,"OBJ1",4) && h.batches && h.batches<=MATERIALS &&
            h.vertices && h.vertices<=48000;
    unsigned total=0;
    for(unsigned i=0;ok && i<h.batches;i++) {
        unsigned n;
        ok=fread(&n,4,1,file)==1 && n && n%3==0 && n<=h.vertices-total && native_texture_read(file,model->textures+i);
        if(ok) {model->counts[i]=n;model->loaded++;total+=n;}
    }
    if(ok && total==h.vertices) model->vertices=linearAlloc(total*sizeof(Vertex));
    ok=ok && model->vertices && fread(model->vertices,sizeof(Vertex),total,file)==total && fgetc(file)==EOF;
    fclose(file);
    if(ok) {
        BufInfo_Init(&model->binding);
        ok=BufInfo_Add(&model->binding,model->vertices,sizeof(Vertex),2,0x10)>=0;
    }
    if(ok && !strcmp(category,"weapons"))ok=read_motion(model,name,total);
    if(ok && !strcmp(path,"cast/rat.mesh"))ok=open_rat(total);
    if(!ok) {release(model);return false;}
    GSPGPU_FlushDataCache(model->vertices,total*sizeof(Vertex));
    strcpy(model->file,path);model_count++;return true;
}
void item_mesh_draw(const char *category,const char *name,int uniform,const C3D_Mtx *matrix) {
    char path[96];
    if(snprintf(path,sizeof path,"%s/%s.mesh",category,name)>=(int)sizeof path) return;
    int id=find(path);if(id<0) return;
    Model *m=models+id;
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER,uniform,matrix);
    C3D_SetBufInfo(&m->binding);
    unsigned first=0;
    for(unsigned i=0;i<m->loaded;i++) {
        C3D_TexBind(0,m->textures+i);C3D_DrawArrays(GPU_TRIANGLES,first,m->counts[i]);first+=m->counts[i];
    }
}

void item_rat_draw(RatTrack track,float time,int uniform,const C3D_Mtx *matrix){
    int id=find("cast/rat.mesh");
    if(id<0 || track>=RAT_TRACKS || !rat.tracks[track].frames)return;
    Model *m=models+id;
    float t=fmaxf(0,fminf(time/rat.tracks[track].seconds,1));
    unsigned frame=(unsigned)lroundf(t*(rat.tracks[track].frames-1));
    C3D_BufInfo buffers;BufInfo_Init(&buffers);
    if(BufInfo_Add(&buffers,rat.tracks[track].positions+4*frame*rat.points,8,1,0)<0 ||
       BufInfo_Add(&buffers,rat.uv,8,1,1)<0)return;
    C3D_AttrInfo previous=*C3D_GetAttrInfo();
    C3D_SetAttrInfo(&rat.attributes);C3D_SetBufInfo(&buffers);
    C3D_Mtx model=*matrix;Mtx_Scale(&model,1.f/4096,1.f/4096,1.f/4096);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER,uniform,&model);
    unsigned first=0;
    for(unsigned i=0;i<m->loaded;i++){
        C3D_TexBind(0,m->textures+i);C3D_DrawElements(GPU_TRIANGLES,m->counts[i],C3D_UNSIGNED_SHORT,rat.indices+first);
        first+=m->counts[i];
    }
    C3D_SetAttrInfo(&previous);
}

void item_weapon_draw(const char *name,int uniform,const C3D_Mtx *hand,unsigned action,float time) {
    char path[96];snprintf(path,sizeof path,"weapons/%s.mesh",name);
    int id=find(path);if(id<0 || action>=3)return;
    Model *m=models+id;Track *t=m->tracks+action;
    float unit=action?fminf(time/t->duration,1):fmodf(time,t->duration)/t->duration;
    float sample=fmaxf(0,unit)*(t->count-1);
    unsigned frame=(unsigned)sample,after=frame+1<t->count?frame+1:frame;
    float fraction=sample-frame;
    C3D_SetBufInfo(&m->binding);unsigned first=0;
    for(unsigned i=0;i<m->run_count;i++){
        DrawRun run=m->runs[i];
        const NodePose *a=t->poses+frame*m->nodes+run.node,*b=t->poses+after*m->nodes+run.node;
        C3D_Mtx pose,world;Mtx_Identity(&pose);
        for(unsigned k=0;k<3;k++){
            float v[4];for(unsigned j=0;j<4;j++)v[j]=a->row[k][j]+(b->row[k][j]-a->row[k][j])*fraction;
            pose.r[k]=FVec4_New(v[0],v[1],v[2],v[3]);
        }
        Mtx_Multiply(&world,hand,&pose);C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER,uniform,&world);
        C3D_CullFace(run.material&RUN_CULL_BACK_FACES?GPU_CULL_FRONT_CCW:GPU_CULL_NONE);
        C3D_TexBind(0,m->textures+(run.material&~RUN_CULL_BACK_FACES));
        C3D_DrawArrays(GPU_TRIANGLES,first,run.count);first+=run.count;
    }
}
const char *item_weapon_effect(const char *name,Point *offset){
    char path[96];snprintf(path,sizeof path,"weapons/%s.mesh",name);
    int id=find(path);if(id<0)return NULL;
    if(offset)*offset=models[id].projectile_offset;
    return models[id].muzzle_effect;
}
