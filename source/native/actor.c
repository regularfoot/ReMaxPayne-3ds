#include "campaign_file.h"
#include "actor.h"
#include "animation_state.h"
#include "alpha_pass.h"
#include "bone_pose.h"
#include "aim_pose.h"
#include "pose_bank.h"
#include "texture.h"
#include <3ds.h>
#include <citro3d.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { MAX_BONES=40, MAX_MATERIALS=12 };
typedef struct { Point rest; uint8_t joint[3], count; float weight[3]; } SkinPoint;
typedef struct { uint32_t point; float u,v; } Corner;
static float bone_pivot[MAX_BONES][3];
static unsigned gun_bone;
static unsigned hit_bones[3];
static int16_t bone_parent[MAX_BONES];
static bool has_hierarchy;
static Matrix bone_rest[MAX_BONES],bone_inverse[MAX_BONES];
typedef struct { Point pos; float u,v;uint8_t color[4]; } DrawVertex;
typedef struct { unsigned frames; float duration; Matrix *keys; } Palette;
typedef struct { char tag[4]; uint32_t points,corners,indices,textures,bones,clips,socket; } Mesh;
_Static_assert(sizeof(SkinPoint)==28 && sizeof(Corner)==12 && sizeof(Mesh)==32,"actor file layout");

typedef struct {u64 tick;Clip gait,action;Weapon weapon;uint16_t scripted;bool embedded_weapon;Matrix hand,left_hand;} PoseStamp;
typedef struct {
    Mesh mesh;
    struct { uint32_t points,corners; } ends[WEAPON_COUNT+1];
    SkinPoint *skin;
    Point *posed,*normal;
    uint32_t *colors;
    Corner *corners;
    DrawVertex *buffer[ENEMY_SLOTS];
    uint16_t *indices;
    C3D_BufInfo binding[ENEMY_SLOTS];
    C3D_Tex textures[MAX_MATERIALS];
    unsigned texture_count,owned_textures,counts[MAX_MATERIALS];
    C3D_Tex faces[2];unsigned face_mask,face_part;bool checked_faces;
    uint8_t part[MAX_MATERIALS];
    PoseStamp last_pose[ENEMY_SLOTS];
} Rig;
static bool dynamic_lighting;
static Rig max_rig,enemy_rig,cast_rigs[CAST_SLOTS];
static char cast_names[CAST_SLOTS][64];
typedef struct {Rig *rig;DrawVertex *vertices;unsigned capacity,entity;C3D_BufInfo binding;PoseStamp stamp;} Instance;
static Instance cast_instances[CAST_SLOTS];
static unsigned active_cast;
static struct {unsigned bone;Point position;} torso;
static uint8_t face_variant[CAST_SLOTS+1];
static uint8_t upper[MAX_BONES];
static Matrix left_grip;
static bool has_left_grip;
static struct {unsigned shoulder,hand;Point shoulder_rest,hand_rest;} arms[2];
static Palette clips[CLIP_COUNT];
typedef struct {uint16_t clip;unsigned frame[2];Matrix keys[2][MAX_BONES];} ScriptFrames;
static ScriptFrames script_frames[CAST_SLOTS+1],action_frames[CAST_SLOTS+1],direction_frames;
typedef struct {unsigned gait,action;float time,action_time;bool whole_body,unarmed,two_handed_bones;float exchange_weight;bool valid,turn_torso;unsigned secondary,direction_group;float direction_weight;unsigned aim[3];Point aim_target;ActorTimeline timeline;} RetailMotion;
static RetailMotion motion[CAST_SLOTS+1];
// Retain the displayed pose when a new clip interrupts a transition.
// Raw source endpoints cannot describe that intermediate pose.
typedef struct {
    Matrix displayed[MAX_BONES],from[MAX_BONES];
    Matrix lower_displayed[MAX_BONES],upper_displayed[MAX_BONES],upper_from[MAX_BONES];
    unsigned generation[2];bool channel_ready[2];
    unsigned source,entity;
    float remaining,step;
    bool valid;
} PoseBlend;
static PoseBlend pose_blends[CAST_SLOTS+1];
typedef struct {uint32_t entity,generation[2],flags;Matrix upper[MAX_BONES],lower[MAX_BONES];} SavedTransitionPose;
_Static_assert(sizeof(SavedTransitionPose)==ACTOR_TRANSITION_SNAPSHOT_BYTES,"transition snapshot layout");
unsigned actor_save_transitions(void *data){
    unsigned count=0;uint8_t *out=data;
    for(unsigned slot=0;slot<=active_cast;slot++){
        const ActorTimeline *t=&motion[slot].timeline;const PoseBlend *p=pose_blends+slot;
        bool needs=t->active && ((t->layer[0].stage && t->layer[0].stage!=3) || (t->layer[1].stage && t->layer[1].stage!=3));
        if(!needs || !p->channel_ready[0] || !p->channel_ready[1])continue;
        if(out){
            SavedTransitionPose saved={.entity=p->entity,.generation={p->generation[0],p->generation[1]},.flags=3};
            memcpy(saved.upper,p->upper_from,sizeof saved.upper);memcpy(saved.lower,p->from,sizeof saved.lower);
            memcpy(out,&saved,sizeof saved);out+=sizeof saved;
        }
        count++;
    }
    return count;
}
bool actor_load_transitions(const void *data,unsigned count){
    if(count>CAST_SLOTS+1)return false;
    const uint8_t *in=data;unsigned npc_slot=1;memset(pose_blends,0,sizeof pose_blends);
    for(unsigned i=0;i<count;i++){
        SavedTransitionPose saved;memcpy(&saved,in,sizeof saved);in+=sizeof saved;
        if(saved.flags!=3)return false;
        for(unsigned j=0;j<=CAST_SLOTS;j++)if(pose_blends[j].channel_ready[0] && pose_blends[j].entity==saved.entity)return false;
        for(unsigned b=0;b<MAX_BONES;b++)for(unsigned r=0;r<3;r++)for(unsigned c=0;c<4;c++)
            if(!isfinite(saved.upper[b].row[r][c]) || !isfinite(saved.lower[b].row[r][c]))return false;
        if(saved.entity && npc_slot>CAST_SLOTS)return false;
        PoseBlend *p=pose_blends+(saved.entity?npc_slot++:0);p->entity=saved.entity;
        memcpy(p->generation,saved.generation,sizeof p->generation);
        memcpy(p->upper_from,saved.upper,sizeof saved.upper);memcpy(p->from,saved.lower,sizeof saved.lower);
        p->channel_ready[0]=p->channel_ready[1]=true;
    }
    return true;
}

static ScriptFrames outgoing_frames;
static unsigned exchange_from;
static ScriptFrames aiming_frames;
typedef struct {unsigned neutral,extreme;AimTable table;} AimCache;
static AimCache aim_cache[24];static unsigned aim_cache_next;
static Matrix aim_neutral[MAX_BONES],aim_extreme[MAX_BONES],aim_sample[MAX_BONES];

void actor_set_timeline(unsigned slot,const ActorTimeline *timeline){if(slot<=CAST_SLOTS)motion[slot].timeline=*timeline;}
void actor_exchange_from(unsigned clip){exchange_from=clip;}
void actor_motion(unsigned slot,unsigned gait,float time,unsigned action,float action_time,bool whole_body,bool unarmed,bool two_handed_bones,float exchange_weight,float dt){
    if(slot>CAST_SLOTS)return;
    motion[slot]=(RetailMotion){.gait=gait,.action=action,.time=time,.action_time=action_time,.whole_body=whole_body,.unarmed=unarmed,.two_handed_bones=two_handed_bones,.exchange_weight=exchange_weight,.valid=true};
    pose_blends[slot].remaining=fmaxf(0,pose_blends[slot].remaining-dt);
    pose_blends[slot].step=dt;
}

void actor_aim(unsigned slot,const unsigned actions[3],Point target,bool turn_torso){
    if(slot>CAST_SLOTS)return;
    memcpy(motion[slot].aim,actions,sizeof motion[slot].aim);motion[slot].aim_target=target;motion[slot].turn_torso=turn_torso;
}

void actor_blend_gait(unsigned slot,unsigned secondary,float weight,unsigned group){
    if(slot>CAST_SLOTS || !secondary)return;
    motion[slot].secondary=secondary;motion[slot].direction_weight=weight;motion[slot].direction_group=pose_clip_id(group);
    // Both directional channels belong to the full-body shoot-dodge. Keep that
    // ownership with the sampled pose when the camera queries it before the
    // next motion update has replaced it with a get-up or standing pose.
    motion[slot].whole_body=true;
}

void actor_copy_motion(unsigned to,unsigned from){
    if(to<=CAST_SLOTS && from<=CAST_SLOTS){
        motion[to]=motion[from];pose_blends[to]=pose_blends[from];
    }
}

static bool read_palettes(FILE *f,unsigned bones,unsigned first,unsigned count) {
    for (unsigned i=first;i<first+count;i++) {
        Palette *c=&clips[i];
        if (fread(&c->frames,4,1,f)!=1 || c->frames<2 || c->frames>120 ||
            fread(&c->duration,4,1,f)!=1 || !(c->duration>0 && c->duration<10)) return false;
        size_t n=c->frames*bones;
        c->keys=malloc(n*sizeof(Matrix));
        if (!c->keys || fread(c->keys,sizeof(Matrix),n,f)!=n) return false;
    }
    return true;
}

static bool build_normals(Rig *r){
    r->normal=calloc(r->mesh.points,sizeof(Point));r->colors=malloc(r->mesh.points*sizeof(uint32_t));
    if(!r->normal || !r->colors)return false;
    // Accumulate face normals by skin point so texture seams receive the same light.
    // Choose the outside winding from the signed volume of the body.
    float winding=0;
    for(unsigned i=0;i<r->mesh.indices;i+=3){
        unsigned a=r->corners[r->indices[i]].point,b=r->corners[r->indices[i+1]].point,c=r->corners[r->indices[i+2]].point;
        Point p=r->skin[a].rest,q=r->skin[b].rest,t=r->skin[c].rest;
        Point u={q.x-p.x,q.y-p.y,q.z-p.z},v={t.x-p.x,t.y-p.y,t.z-p.z};
        Point n={u.y*v.z-u.z*v.y,u.z*v.x-u.x*v.z,u.x*v.y-u.y*v.x};
        if(a<r->ends[0].points)winding+=p.x*n.x+p.y*n.y+p.z*n.z;
        unsigned ids[3]={a,b,c};
        for(unsigned j=0;j<3;j++){Point *m=r->normal+ids[j];m->x+=n.x;m->y+=n.y;m->z+=n.z;}
    }
    for(unsigned i=0;i<r->mesh.points;i++){
        Point *n=r->normal+i;float scale=(winding<0?-1.f:1.f)/fmaxf(1e-7f,sqrtf(n->x*n->x+n->y*n->y+n->z*n->z));
        n->x*=scale;n->y*=scale;n->z*=scale;
    }
    return true;
}
static bool read_actor(FILE *f,Rig *r,bool enemy) {
    if (fread(&r->mesh,sizeof r->mesh,1,f)!=1 || (memcmp(r->mesh.tag,"MAX4",4) && (!enemy || memcmp(r->mesh.tag,"MAX5",4))) || !r->mesh.points || r->mesh.points>4096 ||
        !r->mesh.corners || r->mesh.corners>8192 || !r->mesh.indices || r->mesh.indices>24000 ||
        !r->mesh.bones || r->mesh.bones>MAX_BONES || r->mesh.socket>=r->mesh.bones ||
        r->mesh.clips!=(enemy?0:RIFLE_IDLE) || !r->mesh.textures || r->mesh.textures>MAX_MATERIALS) return false;
    if (fread(r->ends,sizeof r->ends,1,f)!=1) return false;
    uint32_t last_point=0,last_corner=0;
    for (unsigned i=0;i<=WEAPON_COUNT;i++) {
        if (r->ends[i].points<last_point || r->ends[i].corners<last_corner) return false;
        last_point=r->ends[i].points;last_corner=r->ends[i].corners;
    }
    if (last_point!=r->mesh.points || last_corner!=r->mesh.corners) return false;
    uint8_t mask[MAX_BONES];
    if (fread(mask,1,r->mesh.bones,f)!=r->mesh.bones || fread(r->part,1,r->mesh.textures,f)!=r->mesh.textures) return false;
    if (enemy && (r->mesh.bones!=max_rig.mesh.bones || memcmp(mask,upper,r->mesh.bones))) return false;
    if (!enemy) memcpy(upper,mask,r->mesh.bones);
    for (unsigned i=0;i<r->mesh.textures;i++) {
        if (enemy && r->part[i] && !memcmp(r->mesh.tag,"MAX4",4)) {
            // Match the same material within the shared pistol or shotgun.
            unsigned source=0;
            while (source<max_rig.texture_count && max_rig.part[source]!=r->part[i]) source++;
            unsigned first=i;
            while (first && r->part[first-1]==r->part[i]) first--;
            source+=i-first;
            if (source>=max_rig.texture_count || max_rig.part[source]!=r->part[i]) return false;
            r->textures[i]=max_rig.textures[source];
        } else {
            if (i!=r->owned_textures || !native_texture_read(f,&r->textures[i])) return false;
            r->owned_textures++;
        }
        r->texture_count++;
    }
    r->skin=malloc(r->mesh.points*sizeof(*r->skin));
    r->posed=malloc(r->mesh.points*sizeof(*r->posed));
    r->corners=malloc(r->mesh.corners*sizeof(*r->corners));

    r->indices=linearAlloc(r->mesh.indices*sizeof(*r->indices));
    if (!r->skin || !r->posed || !r->corners || !r->indices) return false;
    if (fread(r->skin,sizeof(*r->skin),r->mesh.points,f)!=r->mesh.points ||
        fread(r->corners,sizeof(*r->corners),r->mesh.corners,f)!=r->mesh.corners) return false;
    for (unsigned i=0;i<r->mesh.points;i++) {
        if (!r->skin[i].count || r->skin[i].count>3) return false;
        for (unsigned j=0;j<r->skin[i].count;j++) if (r->skin[i].joint[j]>=r->mesh.bones) return false;
    }
    for (unsigned i=0;i<r->mesh.corners;i++) {
        if (r->corners[i].point>=r->mesh.points) return false;

    }
    unsigned offset=0;
    for (unsigned i=0;i<r->texture_count;i++) {
        if (fread(&r->counts[i],4,1,f)!=1 || r->counts[i]%3 || r->counts[i]>r->mesh.indices-offset ||
            fread(r->indices+offset,2,r->counts[i],f)!=r->counts[i]) return false;
        offset+=r->counts[i];
    }
    if (offset!=r->mesh.indices) return false;
    for (unsigned i=0;i<offset;i++) if (r->indices[i]>=r->mesh.corners) return false;
    if(dynamic_lighting && !build_normals(r))return false;
    GSPGPU_FlushDataCache(r->indices,r->mesh.indices*sizeof(*r->indices));
    if (!read_palettes(f,r->mesh.bones,0,r->mesh.clips)) return false;
    return fgetc(f)==EOF && !ferror(f);
}

static bool prepare_buffers(Rig *r,unsigned count) {
    if (count>ENEMY_SLOTS) return false;
    for (unsigned slot=0;slot<count;slot++) {
        if (r->buffer[slot]) continue;
        DrawVertex *v=linearAlloc(r->mesh.corners*sizeof(*v));
        if (!v) return false;
        for (unsigned i=0;i<r->mesh.corners;i++) {
            v[i].u=r->corners[i].u;
            v[i].v=r->corners[i].v;
            memset(v[i].color,255,sizeof v[i].color);
        }
        BufInfo_Init(&r->binding[slot]);
        if (BufInfo_Add(&r->binding[slot],v,sizeof(*v),3,0x210)<0) { linearFree(v);return false; }
        r->buffer[slot]=v;
    }
    return true;
}

static void close_rig(Rig *r) {
    for(unsigned i=0;i<2;i++)if(r->face_mask&(1u<<i))C3D_TexDelete(r->faces+i);
    for (unsigned i=0;i<r->owned_textures;i++) C3D_TexDelete(&r->textures[i]);
    for (unsigned i=0;i<ENEMY_SLOTS;i++) if (r->buffer[i]) linearFree(r->buffer[i]);
    if (r->indices) linearFree(r->indices);
    free(r->skin);free(r->posed);free(r->normal);free(r->colors);free(r->corners);
    memset(r,0,sizeof(*r));
}
static bool load_rig(Rig *r,const char *path,bool enemy) {
    FILE *f=fopen(path,"rb");
    if (!f) return false;
    bool ok=read_actor(f,r,enemy);
    fclose(f);
    if (!ok) close_rig(r);
    return ok;
}
bool actor_load(bool young,bool actor_lights) {
    dynamic_lighting=actor_lights;
    FILE *rig=fopen("romfs:/aim.bind","rb");
    if(!rig)return false;
    bool ok=fread(arms,sizeof arms,1,rig)==1;
    uint8_t groups[MAX_BONES];
    size_t n=fread(groups,1,sizeof groups,rig);
    ok=ok && n>0 && !ferror(rig) && fgetc(rig)==EOF;
    fclose(rig);
    for(unsigned i=0;i<2;i++)if(arms[i].shoulder>=n || arms[i].hand>=n)ok=false;
    for(unsigned i=0;i<n;i++)if(groups[i]>2)ok=false;
    return ok && load_rig(&max_rig,young?"romfs:/young.actor":"romfs:/max.actor",false) && max_rig.mesh.bones==n && prepare_buffers(&max_rig,1);
}
bool actor_rifle(void) {
    if (clips[RIFLE_IDLE].keys) return true;
    FILE *f=fopen("romfs:/rifle.anim","rb");
    if (!f) return false;
    struct { char tag[4]; uint32_t bones,count; } h;
    bool ok=fread(&h,sizeof h,1,f)==1 && !memcmp(h.tag,"RFA1",4) &&
            h.bones==max_rig.mesh.bones && h.count==CROUCH_IDLE-RIFLE_IDLE &&
            read_palettes(f,h.bones,RIFLE_IDLE,h.count) && fgetc(f)==EOF && !ferror(f);
    fclose(f);
    if (!ok) for (unsigned i=RIFLE_IDLE;i<CROUCH_IDLE;i++) {
        free(clips[i].keys);clips[i]=(Palette){0};
    }
    return ok;
}
bool actor_campaign_moves(void) {
    if(clips[CROUCH_IDLE].keys) return true;
    FILE *pivots=campaign_file("bones.bind");if(!pivots)return false;
    unsigned metadata[2];bool valid=fread(metadata,sizeof metadata,1,pivots)==1 && metadata[0]==max_rig.mesh.bones &&
        metadata[1]<metadata[0] && fread(bone_pivot,sizeof *bone_pivot,metadata[0],pivots)==metadata[0] && fgetc(pivots)==EOF;
    fclose(pivots);if(!valid)return false;gun_bone=metadata[1];
    FILE *tree=campaign_file("bones.tree");has_hierarchy=false;
    if(tree){
        struct {char tag[4];unsigned count;} h;
        valid=fread(&h,sizeof h,1,tree)==1 && !memcmp(h.tag,"MPT1",4) && h.count==metadata[0] &&
            fread(bone_parent,sizeof *bone_parent,h.count,tree)==h.count && fgetc(tree)==EOF;
        fclose(tree);
        for(unsigned i=0;valid && i<h.count;i++)valid=bone_parent[i]>=-1 && bone_parent[i]<(int)i;
        if(!valid)return false;
        has_hierarchy=true;
    }
    FILE *rest=campaign_file("bones.rest");if(!rest)return false;
    struct {char tag[4];uint32_t count;} rh;
    valid=fread(&rh,sizeof rh,1,rest)==1 && !memcmp(rh.tag,"RST1",4) && rh.count==metadata[0] &&
        fread(bone_rest,sizeof *bone_rest,rh.count,rest)==rh.count && fgetc(rest)==EOF;
    fclose(rest);if(!valid)return false;
    for(unsigned i=0;i<rh.count;i++)bone_inverse[i]=bone_inverse_frame(bone_rest+i);
    FILE *hits=campaign_file("hit_bones.bind");if(!hits)return false;
    struct {char tag[4];unsigned count,bone[3];} hb;
    valid=fread(&hb,sizeof hb,1,hits)==1 && !memcmp(hb.tag,"AHB1",4) && hb.count==metadata[0] && fgetc(hits)==EOF;
    fclose(hits);if(!valid)return false;
    for(unsigned i=0;i<3;i++){if(hb.bone[i]>=metadata[0])return false;hit_bones[i]=hb.bone[i];}
    FILE *grip=campaign_file("left_grip.bind");
    has_left_grip=grip && fread(&left_grip,sizeof left_grip,1,grip)==1;
    if(grip)fclose(grip);
    FILE *anchor=campaign_file("torso.bind");if(!anchor)return false;
    bool bound=fread(&torso,sizeof torso,1,anchor)==1 && torso.bone<max_rig.mesh.bones;fclose(anchor);if(!bound)return false;
    FILE *file=campaign_file("moves.anim");if(!file) return false;
    struct {char tag[4];uint32_t bones,count;} h;
    bool ok=fread(&h,sizeof h,1,file)==1 && !memcmp(h.tag,"MOV2",4) && h.bones==max_rig.mesh.bones &&
        h.count==THROW_ITEM-CROUCH_IDLE+1 && read_palettes(file,h.bones,CROUCH_IDLE,h.count) && fgetc(file)==EOF;
    fclose(file);
    if(!ok)for(unsigned i=CROUCH_IDLE;i<CLIP_COUNT;i++){free(clips[i].keys);clips[i]=(Palette){0};}
    return ok;
}
float actor_duration(Clip clip) {return clip<CLIP_COUNT?clips[clip].duration:0;}
static bool oneshot(Clip clip) {return clip==FALL || clip==JUMP || (clip>=DODGE_FORWARD && clip<=DODGE_RIGHT);}
bool actor_enemies(unsigned count) {
    return (enemy_rig.texture_count || load_rig(&enemy_rig,"romfs:/johnny.actor",true)) && prepare_buffers(&enemy_rig,count);
}
static Rig *cast_rig(const char *name){
    if(!*name || strlen(name)>=sizeof cast_names[0] || strspn(name,"abcdefghijklmnopqrstuvwxyz0123456789_-")!=strlen(name))return NULL;
    unsigned empty=CAST_SLOTS;
    for(unsigned i=0;i<CAST_SLOTS;i++){
        if(!strcmp(cast_names[i],name))return cast_rigs+i;
        if(empty==CAST_SLOTS && !cast_names[i][0])empty=i;
    }
    if(empty==CAST_SLOTS)return NULL;
    char path[96];snprintf(path,sizeof path,"cast/%s.actor",name);
    FILE *f=campaign_file(path);if(!f)return NULL;
    Rig *r=cast_rigs+empty;bool ok=read_actor(f,r,true);fclose(f);
    if(!ok){close_rig(r);return NULL;}
    strcpy(cast_names[empty],name);return r;
}
void actor_preload(const char *name){
    if(linearSpaceFree()>3*1024*1024)cast_rig(name);
}
bool actor_cast(unsigned count,const char *const *names,const unsigned *entities) {
    if(count>CAST_SLOTS)return false;
    // Distance sorting and short visits through a doorway can change the draw list
    // without changing any GPU resources. Move the existing bindings with each body.
    unsigned matched=0;
    for(;matched<count;matched++){
        unsigned j=matched;
        while(j<CAST_SLOTS){
            Instance *in=cast_instances+j;
            if(in->rig && in->entity==entities[matched] && !strcmp(cast_names[in->rig-cast_rigs],names[matched]))break;
            j++;
        }
        if(j==CAST_SLOTS)break;
        if(j!=matched){
            Instance swap=cast_instances[matched];cast_instances[matched]=cast_instances[j];cast_instances[j]=swap;
        }
    }
    if(matched==count){active_cast=count;return true;}
    unsigned missing=0,empty=0;
    for(unsigned i=0;i<CAST_SLOTS;i++)empty+=!cast_names[i][0];
    for(unsigned i=0;i<count;i++){
        unsigned j=0;while(j<i && strcmp(names[i],names[j]))j++;
        if(j<i)continue;
        for(j=0;j<CAST_SLOTS && strcmp(cast_names[j],names[i]);j++){}
        missing+=j==CAST_SLOTS;
    }
    bool waited=false;
    for(unsigned i=0;empty<missing && i<CAST_SLOTS;i++){
        if(!cast_names[i][0])continue;
        unsigned j=0;while(j<count && strcmp(names[j],cast_names[i]))j++;
        if(j<count)continue;
        if(!waited){if(C3D_FrameBegin(0))C3D_FrameEnd(0);waited=true;}
        for(j=0;j<CAST_SLOTS;j++)if(cast_instances[j].rig==cast_rigs+i)cast_instances[j].rig=NULL;
        close_rig(cast_rigs+i);cast_names[i][0]=0;empty++;
    }
    active_cast=count;
    for(unsigned i=0;i<count;i++) {
        const char *name=names[i];
        Rig *r=cast_rig(name);if(!r)return false;
        Instance *instance=cast_instances+i;
        if(instance->capacity<r->mesh.corners){
            if(instance->vertices){
                if(!waited){if(C3D_FrameBegin(0))C3D_FrameEnd(0);waited=true;}
                linearFree(instance->vertices);
            }
            instance->capacity=0;instance->vertices=linearAlloc(r->mesh.corners*sizeof(DrawVertex));
            if(!instance->vertices)return false;
            instance->capacity=r->mesh.corners;
            BufInfo_Init(&instance->binding);
            if(BufInfo_Add(&instance->binding,instance->vertices,sizeof(DrawVertex),3,0x210)<0)return false;
        }
        if(instance->entity!=entities[i])instance->stamp.tick=0;
        instance->entity=entities[i];
        if(instance->rig==r && instance->stamp.tick)continue;
        instance->rig=r;instance->stamp.tick=0;
    }
    return true;
}
void actor_leave_enemies(void) { close_rig(&enemy_rig); }
void actor_close(void) {
    memset(face_variant,0,sizeof face_variant);
    memset(pose_blends,0,sizeof pose_blends);
    has_hierarchy=false;exchange_from=0;memset(&outgoing_frames,0,sizeof outgoing_frames);
    memset(&direction_frames,0,sizeof direction_frames);
    memset(aim_cache,0,sizeof aim_cache);aim_cache_next=0;memset(&aiming_frames,0,sizeof aiming_frames);
    memset(script_frames,0,sizeof script_frames);memset(action_frames,0,sizeof action_frames);memset(motion,0,sizeof motion);
    actor_leave_enemies();
    for(unsigned i=0;i<CAST_SLOTS;i++){
        close_rig(cast_rigs+i);cast_names[i][0]=0;
        if(cast_instances[i].vertices)linearFree(cast_instances[i].vertices);
        cast_instances[i]=(Instance){0};
    }
    active_cast=0;
    close_rig(&max_rig);
    for (unsigned i=0;i<CLIP_COUNT;i++) { free(clips[i].keys);clips[i]=(Palette){0}; }
}

static Clip equipped(const Animation *a,Clip clip);
void actor_step(Animation *a,Clip gait,float speed,float dt) {
    if (gait!=a->gait) {
        a->previous_gait=a->gait;
        a->previous_time=a->gait_time;
        a->transition=.13f;
        a->gait=gait;
        a->gait_time=0;
    }
    float duration=clips[equipped(a,gait)].duration;
    if(duration<=0)duration=1;
    // Death clips vary by skin. Keep their simulation time even while the body is off screen.
    if(gait==FALL)a->gait_time+=dt;
    else if(oneshot(gait))a->gait_time=fminf(duration,a->gait_time+dt*speed);
    else a->gait_time=fmodf(a->gait_time+dt*speed,duration);
    a->transition=fmaxf(0,a->transition-dt);
    a->action_time+=dt;
    if (a->action!=IDLE && a->action_time>=clips[equipped(a,a->action)].duration) a->action=IDLE;
}

int actor_locomotion_style(Animation *a,float forward,float side,bool crouched,Clip override,float dt,bool run_walk) {
    retail_restore_animation(a,crouched);
    Clip gait=override==IDLE?retail_locomotion(forward,side,crouched,dt,run_walk):override;
    if(gait!=IDLE)a->scripted=0;
    actor_step(a,gait,1,dt);
    return -1; // Footsteps come only from the original per-frame messages.
}
int actor_locomotion(Animation *a,float forward,float side,bool crouched,Clip override,float dt){
    return actor_locomotion_style(a,forward,side,crouched,override,dt,false);
}

void actor_action(Animation *a,Clip action) { a->action=action; a->action_time=0; }

typedef struct { const Matrix *a,*b; float mix;bool local;const uint8_t *normalization;const float (*scaling)[3]; } Keys;
static Keys keys(Clip id,float seconds,bool looping) {
    const Palette *c=&clips[id];
    if(!c->keys)c=&clips[IDLE];
    float frame=seconds/c->duration*(looping?c->frames:c->frames-1);
    frame=fminf(frame,c->frames-.0001f);
    unsigned i=(unsigned)frame;
    unsigned j=(i+1<c->frames)?i+1:(looping?0:i);
    return (Keys){.a=c->keys+i*max_rig.mesh.bones,.b=c->keys+j*max_rig.mesh.bones,.mix=frame-i};
}
typedef struct { Keys gait,directional,previous,action,outgoing;float direction_weight; Matrix action_root,outgoing_root;float outgoing_weight;const Matrix *blend_from;float blend;float transition,overlay;const uint8_t *upper;Keys aimed;Matrix aimed_root;float aim_weight;Point aim_target;bool aim_yaw;unsigned slot;bool retail; } Pose;
static Clip equipped(const Animation *a,Clip clip) {
    static const Clip idle[]={IDLE,PUMP_IDLE,RIFLE_IDLE};
    static const Clip fire[]={FIRE,PUMP_FIRE,RIFLE_FIRE};
    static const Clip reload[]={RELOAD,PUMP_RELOAD,RIFLE_RELOAD};
    if (clip==IDLE) return idle[a->weapon];
    if (clip==FIRE) return fire[a->weapon];
    if (clip==RELOAD) return reload[a->weapon];
    return clip;
}
float actor_equipped_duration(const Animation *a,Clip clip){return actor_duration(equipped(a,clip));}
static bool bank_keys(ScriptFrames *cache,unsigned clip,float seconds,bool looping,Keys *result) {
    unsigned index[2],bones=max_rig.mesh.bones;float mix;
    bool found=looping?pose_bank_loop_frames(clip,seconds,bones,index,index+1,&mix):
                       pose_bank_frames(clip,seconds,bones,index,index+1,&mix);
    if(!found)return false;
    if(clip!=cache->clip){cache->clip=clip;cache->frame[0]=cache->frame[1]=UINT32_MAX;}
    unsigned left=cache->frame[1]==index[0]?1:0;
    for(unsigned j=0;j<2;j++) {
        unsigned k=left^j;
        if(cache->frame[k]==index[j])continue;
        if(!pose_bank_read_local(clip,index[j],bones,cache->keys[k]))return false;
        cache->frame[k]=index[j];
    }
    *result=(Keys){cache->keys[left],cache->keys[left^1],mix,true,pose_bank_normalization(clip),pose_bank_scaling(clip)};return true;
}
// Calibrate once per actual clip pair. Skin overrides therefore receive their
// own table, and the frame loop only performs the sixteen-ray lookup.
static const AimTable *aim_table(unsigned neutral,unsigned extreme){
    unsigned a=pose_clip_id(neutral),b=pose_clip_id(extreme);
    if(!has_hierarchy || !a || !b)return NULL;
    for(unsigned i=0;i<24;i++)if(aim_cache[i].neutral==a && aim_cache[i].extreme==b)return &aim_cache[i].table;
    unsigned count=max_rig.mesh.bones;
    if(!pose_bank_read_local(neutral,0,count,aim_neutral) || !pose_bank_read_local(extreme,0,count,aim_extreme))return NULL;
    AimCache *cache=aim_cache+aim_cache_next;aim_cache_next=(aim_cache_next+1)%24;
    if(!aim_calibrate(aim_neutral,aim_extreme,aim_sample,bone_inverse,bone_parent,count,max_rig.mesh.socket,&cache->table))return NULL;
    cache->neutral=a;cache->extreme=b;return &cache->table;
}
static bool palette_ready;
static Pose base_pose(const Animation *a,float pitch) {
    (void)pitch; // Campaign aiming supplies a model-space target shared by both layers.
    palette_ready=false;
    Pose p={.gait=keys(equipped(a,a->gait),a->gait_time,!oneshot(a->gait)),
            .transition=a->transition/.13f};
    RetailMotion m=a->slot<=CAST_SLOTS?motion[a->slot]:(RetailMotion){0};
    p.upper=m.valid?pose_bank_upper(m.two_handed_bones):NULL;
    if(!p.upper)p.upper=upper; // Standalone rooms retain their resident palette mask.
    if(m.valid && m.timeline.active){
        PoseBlend *blend=pose_blends+a->slot;
        unsigned entity=a->slot && a->slot<=active_cast?cast_instances[a->slot-1].entity:0;
        if(blend->entity!=entity){
            unsigned found=CAST_SLOTS+1;
            for(unsigned j=0;j<=CAST_SLOTS;j++)if(pose_blends[j].entity==entity && pose_blends[j].channel_ready[0]){found=j;break;}
            if(found<=CAST_SLOTS){PoseBlend saved=*blend;*blend=pose_blends[found];pose_blends[found]=saved;}
            else {memset(blend,0,sizeof *blend);blend->entity=entity;}
        }
        p.retail=true;p.slot=a->slot;p.transition=0;
        p.overlay=m.timeline.source[0]!=m.timeline.source[1]?1:0;
        p.aim_yaw=m.timeline.aiming && m.turn_torso;return p;
    }
    if(m.valid){
        PoseBlend *blend=pose_blends+a->slot;
        unsigned entity=a->slot && a->slot<=active_cast?cast_instances[a->slot-1].entity:0;
        unsigned source=a->scripted?a->scripted:m.secondary?0x20000u+m.direction_group:
            m.gait?pose_clip_id(m.gait):65536u+equipped(a,a->gait);
        if(blend->entity!=entity){blend->valid=false;blend->remaining=0;blend->entity=entity;}
        if(blend->valid && blend->source!=source){
            memcpy(blend->from,blend->displayed,sizeof blend->from);
            blend->remaining=fmaxf(0,.13f-blend->step);
        }
        blend->source=source;
        if(blend->valid && blend->remaining>0){p.blend_from=blend->from;p.blend=blend->remaining/.13f;}
        // The pose snapshot already includes an interrupted gait and upper-body action.
        // Standalone tutorial rendering keeps its original clip transition fallback.
        p.transition=0;
    }
    if(a->scripted && a->slot<=CAST_SLOTS && bank_keys(script_frames+a->slot,a->scripted,a->script_time,a->gait!=FALL,&p.gait)){
        p.transition=0;return p;
    }
    if(m.gait)bank_keys(script_frames+a->slot,m.gait,m.time,!oneshot(a->gait) && !m.whole_body,&p.gait);
    if(m.secondary && m.direction_weight>0 && bank_keys(&direction_frames,m.secondary,m.time,false,&p.directional))
        p.direction_weight=m.direction_weight;
    if(m.whole_body || a->gait==FALL || a->gait==JUMP || (a->gait>=DODGE_FORWARD && a->gait<=DODGE_RIGHT)){
        p.transition=0;return p;
    }
    if(p.transition>0){
        p.previous=keys(equipped(a,a->previous_gait),a->previous_time,!oneshot(a->previous_gait));
    }
    if (a->action!=IDLE) {
        p.action=keys(equipped(a,a->action),a->action_time,false);
        float remaining=clips[equipped(a,a->action)].duration-a->action_time;
        if(m.action && bank_keys(action_frames+a->slot,m.action,m.action_time,false,&p.action))
            remaining=pose_bank_duration(m.action)-m.action_time;
        p.overlay=fmaxf(0,fminf(1,remaining/.1f));
        if (a->action==RELOAD) p.overlay*=fminf(1,a->action_time/.08f);
    }else if(m.action && bank_keys(action_frames+a->slot,m.action,m.action_time,false,&p.action)){
        float remaining=pose_bank_duration(m.action)-m.action_time;
        p.overlay=m.exchange_weight>=0?m.exchange_weight:fmaxf(0,fminf(1,fminf(m.action_time,remaining)/.08f));
    }
    if(p.overlay>0 && m.aim[0] && m.action==m.aim[0]){
        const float target[3]={m.aim_target.x,m.aim_target.y,m.aim_target.z};
        const AimTable *neutral=aim_table(m.aim[0],m.aim[1]?m.aim[1]:m.aim[0]);
        if(neutral){
            unsigned selected=aim_high(neutral,target)?2:1;
            unsigned clip=m.aim[selected]?m.aim[selected]:m.aim[0];
            const AimTable *calibration=aim_table(m.aim[0],clip);
            if(calibration && pose_clip_id(clip)!=pose_clip_id(m.aim[0]) &&
               bank_keys(&aiming_frames,clip,m.action_time,false,&p.aimed)){
                p.aim_weight=aim_extreme_weight(calibration,target,selected==2);

            }
            p.aim_target=m.aim_target;p.aim_yaw=m.turn_torso;
        }
    }

    if(!a->slot && exchange_from && p.overlay>0 && m.action_time<.12f){
        Pose from=p;
        if(bank_keys(&outgoing_frames,exchange_from,pose_bank_duration(exchange_from),false,&from.action)){
            p.outgoing=from.action;p.outgoing_root=from.action_root;p.outgoing_weight=1-m.action_time/.12f;
        }
    }
    return p;
}
// One resolved palette is shared by the joint queries of the current pose.
// Pose evaluation is synchronous; weapon sockets finish before another pose starts.
static Matrix resolved_palette[MAX_BONES],overlay_palette[MAX_BONES],outgoing_palette[MAX_BONES];
// Keep animation channels in the authored parent coordinate systems. The Xbox
// finalizes the summed local matrices, then rebuilds the world skeleton once.
static void blend_palette(Matrix *a,const Matrix *b,float weight,const uint8_t *mask){
    for(unsigned i=0;i<max_rig.mesh.bones;i++)if(!mask || mask[i])a[i]=bone_mix_local(a+i,b+i,weight);
}
static void finalize_palette(Matrix *local,const uint8_t *mask){
    for(unsigned i=0;i<max_rig.mesh.bones;i++)if(!mask || mask[i]){
        Matrix before=local[i];if(!bone_normalize(local+i,2))local[i]=before;
    }
}
static void sample_palette(Keys keys,Matrix *result){
    if(keys.local){
        for(unsigned i=0;i<max_rig.mesh.bones;i++){
            result[i]=bone_mix_local(keys.a+i,keys.b+i,keys.mix);
            if(keys.normalization)bone_temporal_finish(result+i,keys.normalization[i],keys.scaling?keys.scaling[i]:NULL);
        }
    }else if(has_hierarchy){
        Matrix first[MAX_BONES],last[MAX_BONES];
        bone_skin_to_local(keys.a,first,bone_rest,bone_parent,max_rig.mesh.bones);
        bone_skin_to_local(keys.b,last,bone_rest,bone_parent,max_rig.mesh.bones);
        for(unsigned i=0;i<max_rig.mesh.bones;i++)result[i]=bone_mix_local(first+i,last+i,keys.mix);
    }else{
        for(unsigned i=0;i<max_rig.mesh.bones;i++)result[i]=bone_mix_local(keys.a+i,keys.b+i,keys.mix);
    }
}
static ScriptFrames timeline_previous;
static Matrix timeline_current[MAX_BONES],timeline_old[MAX_BONES],timeline_channels[2][MAX_BONES];
static void timeline_sample(unsigned slot,unsigned layer,unsigned clip,float time,Keys fallback,Matrix *out){
    Keys frame=fallback;ScriptFrames *cache=layer?script_frames+slot:action_frames+slot;
    if(layer>1)cache=&timeline_previous;
    bank_keys(cache,clip,time,true,&frame);sample_palette(frame,out);
}
static void timeline_channel(const Pose *p,unsigned layer,Matrix *out){
    const RetailMotion *m=motion+p->slot;const ActorTimeline *timeline=&m->timeline;
    const AnimationEvaluation *e=timeline->sample+layer;PoseBlend *history=pose_blends+p->slot;
    Matrix *snapshot=layer?history->from:history->upper_from;
    Matrix *displayed=layer?history->lower_displayed:history->upper_displayed;
    unsigned current=e->clip;
    if(m->secondary && current==timeline->source[1])current=m->gait;
    timeline_sample(p->slot,layer,current,e->time,p->gait,timeline_current);
    if(m->secondary && e->clip==timeline->source[1] && m->direction_weight>0){
        timeline_sample(p->slot,2,m->secondary,e->time,p->gait,timeline_old);
        blend_palette(timeline_current,timeline_old,m->direction_weight,NULL);
    }
    if(!layer && current==m->aim[0] && timeline->aiming){
        const float target[3]={m->aim_target.x,m->aim_target.y,m->aim_target.z};
        const AimTable *neutral=aim_table(m->aim[0],m->aim[1]?m->aim[1]:m->aim[0]);
        if(neutral){
            unsigned selected=aim_high(neutral,target)?2:1,clip=m->aim[selected]?m->aim[selected]:m->aim[0];
            const AimTable *calibration=aim_table(m->aim[0],clip);Keys frame;
            if(calibration && pose_clip_id(clip)!=pose_clip_id(m->aim[0]) && bank_keys(&aiming_frames,clip,e->time,false,&frame)){
                sample_palette(frame,timeline_old);blend_palette(timeline_current,timeline_old,aim_extreme_weight(calibration,target,selected==2),NULL);
            }
        }
    }
    if(!history->channel_ready[layer]){
        memcpy(snapshot,timeline_current,sizeof(Matrix)*max_rig.mesh.bones);
        history->channel_ready[layer]=true;
    }else if(history->generation[layer]!=timeline->layer[layer].generation){
        memcpy(snapshot,displayed,sizeof(Matrix)*max_rig.mesh.bones);
    }
    history->generation[layer]=timeline->layer[layer].generation;
    if(e->previous>0)timeline_sample(p->slot,2,e->previous_clip,e->previous_time,p->gait,timeline_old);
    for(unsigned i=0;i<max_rig.mesh.bones;i++)for(unsigned r=0;r<3;r++)for(unsigned c=0;c<4;c++){
        out[i].row[r][c]=timeline_current[i].row[r][c]*e->current+snapshot[i].row[r][c]*e->snapshot;
        if(e->previous>0)out[i].row[r][c]+=timeline_old[i].row[r][c]*e->previous;
    }
    finalize_palette(out,NULL);memcpy(displayed,out,sizeof(Matrix)*max_rig.mesh.bones);
}
static void resolve_timeline(const Pose *p){
    const RetailMotion *m=motion+p->slot;
    timeline_channel(p,1,timeline_channels[1]);timeline_channel(p,0,timeline_channels[0]);
    for(unsigned i=0;i<max_rig.mesh.bones;i++)resolved_palette[i]=timeline_channels[p->upper[i]?0:1][i];
    if(has_hierarchy)bone_local_to_skin(resolved_palette,resolved_palette,bone_inverse,bone_parent,max_rig.mesh.bones);
    // A pipe/bat swing has its own authored arc. Keep its high/low source
    // poses, while avoiding a firearm correction that fights the swing.
    if(m->turn_torso && m->timeline.aiming && m->timeline.aim_weight>0){
        Matrix pivot=bone_joint_frame(resolved_palette[torso.bone],bone_pivot[torso.bone]);
        const Matrix *gun=resolved_palette+max_rig.mesh.socket;
        float position[3]={pivot.row[0][3],pivot.row[1][3],pivot.row[2][3]},direction[3]={gun->row[0][2],gun->row[1][2],gun->row[2][2]};
        float target[3]={m->aim_target.x,m->aim_target.y,m->aim_target.z};
        aim_turn_torso(resolved_palette,max_rig.mesh.bones,torso.bone,bone_parent,position,
                       aim_torso_yaw(direction,position,target,m->timeline.aim_weight));
    }
    palette_ready=true;
}
static void resolve_palette(const Pose *p){
    if(p->retail){resolve_timeline(p);return;}
    sample_palette(p->gait,resolved_palette);
    if(p->direction_weight>0){
        sample_palette(p->directional,overlay_palette);
        blend_palette(resolved_palette,overlay_palette,p->direction_weight,NULL);
    }
    if(p->transition>0){
        sample_palette(p->previous,overlay_palette);
        blend_palette(resolved_palette,overlay_palette,p->transition,NULL);
    }
    finalize_palette(resolved_palette,NULL);
    if(p->overlay>0){
        sample_palette(p->action,overlay_palette);
        if(p->aim_weight>0){
            sample_palette(p->aimed,outgoing_palette);
            blend_palette(overlay_palette,outgoing_palette,p->aim_weight,NULL);
        }
        if(p->outgoing_weight>0){
            sample_palette(p->outgoing,outgoing_palette);
            blend_palette(overlay_palette,outgoing_palette,p->outgoing_weight,NULL);
        }
        blend_palette(resolved_palette,overlay_palette,p->overlay,p->upper);
        finalize_palette(resolved_palette,p->upper);
    }
    if(p->blend>0){
        bone_skin_to_local(p->blend_from,outgoing_palette,bone_rest,bone_parent,max_rig.mesh.bones);
        blend_palette(resolved_palette,outgoing_palette,p->blend,NULL);
        finalize_palette(resolved_palette,NULL);
    }
    if(has_hierarchy)bone_local_to_skin(resolved_palette,resolved_palette,bone_inverse,bone_parent,max_rig.mesh.bones);
    if(p->aim_yaw && p->overlay>0){
        Matrix pivot=bone_joint_frame(resolved_palette[torso.bone],bone_pivot[torso.bone]);
        const Matrix *gun=resolved_palette+max_rig.mesh.socket;
        float position[3]={pivot.row[0][3],pivot.row[1][3],pivot.row[2][3]};
        float direction[3]={gun->row[0][2],gun->row[1][2],gun->row[2][2]};
        float target[3]={p->aim_target.x,p->aim_target.y,p->aim_target.z};
        aim_turn_torso(resolved_palette,max_rig.mesh.bones,torso.bone,bone_parent,position,
                       aim_torso_yaw(direction,position,target,p->overlay));
    }
    palette_ready=true;
}
static Matrix animated_bone(const Pose *p,unsigned id) {
    if(!palette_ready)resolve_palette(p);
    return resolved_palette[id];
}
static Point transform(const Matrix *m,Point p) {
    Point q;
    const float (*r)[4]=m->row;
    q.x=r[0][0]*p.x+r[0][1]*p.y+r[0][2]*p.z+r[0][3];
    q.y=r[1][0]*p.x+r[1][1]*p.y+r[1][2]*p.z+r[1][3];
    q.z=r[2][0]*p.x+r[2][1]*p.y+r[2][2]*p.z+r[2][3];
    return q;
}
static Pose pose(const Animation *a,float pitch){return base_pose(a,pitch);}
static Matrix bone(const Pose *p,unsigned id,Matrix *remember){
    Matrix m=animated_bone(p,id);
    if(remember)*remember=m;
    return m;
}
static Matrix offhand_socket(const Matrix *wrist,Matrix right){
    if(!has_left_grip){right.row[0][3]=-right.row[0][3];return right;}
    Matrix socket;
    for(unsigned i=0;i<3;i++)for(unsigned j=0;j<4;j++){
        const float *w=wrist->row[i];
        socket.row[i][j]=w[0]*left_grip.row[0][j]+w[1]*left_grip.row[1][j]+w[2]*left_grip.row[2][j]+(j==3?w[3]:0);
    }
    return socket;
}
void actor_draw_hand(unsigned instance,bool left,float result[3][4]) {
    const PoseStamp *pose=instance?&cast_instances[instance-1].stamp:&max_rig.last_pose[0];
    memcpy(result,left?pose->left_hand.row:pose->hand.row,sizeof pose->hand.row);
}
// The follow camera must descend with the authored torso during a shoot-dodge.
// This queries the source pose; it does not rotate or reposition any bone.
float actor_camera_offset(const Animation *a){
    Pose p=pose(a,0);Matrix m=bone(&p,torso.bone,NULL);
    return transform(&m,torso.position).y-torso.position.y;
}
Point actor_attachment(const Player *p,const Animation *a,bool hand) {
    Pose posed=pose(a,p->pitch);Matrix m=bone(&posed,hand?max_rig.mesh.socket:torso.bone,NULL);
    Point local=transform(&m,hand?(Point){0}:torso.position);
    float c=cosf(p->yaw),s=sinf(p->yaw);
    return (Point){p->position.x+c*local.x+s*local.z,p->position.y+local.y,p->position.z-s*local.x+c*local.z};
}
bool actor_projectile_frame(const Player *p,const Animation *a,const char *name,Point *origin,Point axes[3]){
    const char *const names[]={"neck","head","pelvis"};unsigned i=0;
    while(i<3 && strcmp(name,names[i]))i++;
    if(i==3)return false;
    unsigned joint=hit_bones[i];Pose posed=pose(a,p->pitch);Matrix skin=bone(&posed,joint,NULL);
    Matrix frame=bone_compose_frame(&skin,bone_rest+joint);
    float c=cosf(p->yaw),s=sinf(p->yaw);
    for(unsigned k=0;k<3;k++)axes[k]=(Point){c*frame.row[0][k]+s*frame.row[2][k],frame.row[1][k],-s*frame.row[0][k]+c*frame.row[2][k]};
    *origin=(Point){p->position.x+c*frame.row[0][3]+s*frame.row[2][3],p->position.y+frame.row[1][3],p->position.z-s*frame.row[0][3]+c*frame.row[2][3]};
    return true;
}
Point actor_hand_point(const Player *p,const Animation *a,bool left,Point offset) {
    Pose pose_data=pose(a,p->pitch);
    Matrix gun=bone(&pose_data,max_rig.mesh.socket,NULL);
    if(left){Matrix wrist=bone(&pose_data,arms[1].hand,NULL);gun=offhand_socket(&wrist,gun);}
    Point local=transform(&gun,offset);
    float c=cosf(p->yaw),s=sinf(p->yaw);
    return (Point){p->position.x+c*local.x+s*local.z,p->position.y+local.y,p->position.z-s*local.x+c*local.z};
}
Point actor_hand_muzzle(const Player *p,const Animation *a,bool left) {
    static const float barrel[]={.20f,.62f,.92f};
    return actor_hand_point(p,a,left,(Point){0,.08f,barrel[a->weapon]});
}
Point actor_muzzle(const Player *p,const Animation *a){return actor_hand_muzzle(p,a,false);}
static ActorLight illumination[CAST_SLOTS+1];
void actor_set_light(unsigned slot,ActorLight light){illumination[slot]=light;}
static unsigned channel(float ambient,float diffuse,float shade){return (unsigned)fminf(255,.5f*(ambient+diffuse*shade));}
void actor_prepare(const Animation *a,float pitch,unsigned instance,bool embedded_weapon,u64 now) {
    Instance *cast=instance && instance<=active_cast?cast_instances+instance-1:NULL;
    Rig *r=cast?cast->rig:instance?&enemy_rig:&max_rig;
    unsigned slot=cast || !instance?0:instance-1;
    PoseStamp *stamp=cast?&cast->stamp:&r->last_pose[slot];
    if (!dynamic_lighting && instance && stamp->tick && stamp->embedded_weapon==embedded_weapon &&
        stamp->gait==a->gait && stamp->action==a->action &&
        stamp->weapon==a->weapon && stamp->scripted==a->scripted && now-stamp->tick<SYSCLOCK_ARM11/32) return;
    if(cast && !stamp->tick){
        for(unsigned i=0;i<r->mesh.corners;i++){
            cast->vertices[i].u=r->corners[i].u;cast->vertices[i].v=r->corners[i].v;
            memset(cast->vertices[i].color,255,sizeof cast->vertices[i].color);
        }
    }
    stamp->embedded_weapon=embedded_weapon;stamp->tick=now;stamp->gait=a->gait;
    stamp->action=a->action;stamp->weapon=a->weapon;stamp->scripted=a->scripted;
    DrawVertex *buffer=cast?cast->vertices:r->buffer[slot];
    Pose pose_data=pose(a,pitch);
    Matrix palette[MAX_BONES];
    for (unsigned i=0;i<r->mesh.bones;i++){
        palette[i]=bone(&pose_data,i,pose_blends[a->slot].displayed+i);
        if(i==r->mesh.socket)stamp->hand=palette[i];
    }
    pose_blends[a->slot].valid=true;
    stamp->left_hand=offhand_socket(palette+arms[1].hand,stamp->hand);
    ActorLight light=illumination[instance];
    Point incoming[MAX_BONES];
    if(dynamic_lighting)for(unsigned j=0;j<r->mesh.bones;j++){
        const Matrix *m=palette+j;Point d=light.direction;
        incoming[j]=(Point){m->row[0][0]*d.x+m->row[1][0]*d.y+m->row[2][0]*d.z,
                            m->row[0][1]*d.x+m->row[1][1]*d.y+m->row[2][1]*d.z,
                            m->row[0][2]*d.x+m->row[1][2]*d.y+m->row[2][2]*d.z};
    }
    // The packed mesh stores the body first and one contiguous range per weapon.
    unsigned body_and_gun[]={0,a->weapon+1};
    for (unsigned range=0;range<1u+embedded_weapon;range++) {
        unsigned part=body_and_gun[range];
        unsigned first_point=part?r->ends[part-1].points:0;
        unsigned first_corner=part?r->ends[part-1].corners:0;
        for (unsigned i=first_point;i<r->ends[part].points;i++) {
            const SkinPoint *v=&r->skin[i];
            Point sum;
            if(v->count==1)sum=transform(&palette[v->joint[0]],v->rest);
            else {
                sum=(Point){0};
                for (unsigned j=0;j<v->count;j++) {
                    Point q=transform(&palette[v->joint[j]],v->rest);
                    float w=v->weight[j];
                    sum.x+=q.x*w;sum.y+=q.y*w;sum.z+=q.z*w;
                }
            }
            r->posed[i]=sum;
            if(!dynamic_lighting)continue;
            Point n=r->normal[i];float dot=0;
            for(unsigned j=0;j<v->count;j++){
                Point d=incoming[v->joint[j]];dot+=v->weight[j]*(n.x*d.x+n.y*d.y+n.z*d.z);
            }
            float shade=fminf(1,fmaxf(0,(dot+.2f)/1.2f));
            r->colors[i]=0xff000000 | channel(light.ambient.x,light.diffuse.x,shade) |
                (channel(light.ambient.y,light.diffuse.y,shade)<<8) | (channel(light.ambient.z,light.diffuse.z,shade)<<16);
        }
        for(unsigned i=first_corner;i<r->ends[part].corners;i++){
            unsigned point=r->corners[i].point;buffer[i].pos=r->posed[point];
            if(dynamic_lighting)memcpy(buffer[i].color,r->colors+point,4);
        }
    }
    GSPGPU_FlushDataCache(buffer,r->ends[embedded_weapon?a->weapon+1:0].corners*sizeof(*buffer));
}
bool actor_face(unsigned instance,const char *skin,unsigned variant) {
    if(instance>active_cast || variant>2)return false;
    Rig *r=instance?cast_instances[instance-1].rig:&max_rig;
    face_variant[instance]=variant;
    if(!variant || r->checked_faces)return true;
    char path[96];snprintf(path,sizeof path,"faces/%s.face",skin);
    FILE *f=campaign_file(path);r->checked_faces=true;
    if(!f)return true;
    struct {char tag[4];unsigned material,mask;} h;
    bool ok=fread(&h,sizeof h,1,f)==1 && !memcmp(h.tag,"MPF1",4) && h.material<r->texture_count && h.mask<=3;
    if(ok){
        r->face_part=h.material;
        for(unsigned i=0;ok && i<2;i++)if(h.mask&(1u<<i)){
            ok=native_texture_read(f,r->faces+i);if(ok)r->face_mask|=1u<<i;
        }
    }
    fclose(f);return ok;
}
Point actor_center(void){
    Point lo=max_rig.posed[0],hi=lo;
    for(unsigned i=1;i<max_rig.ends[0].points;i++){
        Point p=max_rig.posed[i];
        lo.x=fminf(lo.x,p.x);lo.y=fminf(lo.y,p.y);lo.z=fminf(lo.z,p.z);
        hi.x=fmaxf(hi.x,p.x);hi.y=fmaxf(hi.y,p.y);hi.z=fmaxf(hi.z,p.z);
    }
    return (Point){(lo.x+hi.x)*.5f,(lo.y+hi.y)*.5f,(lo.z+hi.z)*.5f};
}
void actor_draw(const Animation *a,unsigned instance,bool unarmed,const C3D_Mtx *transform,unsigned portal,bool tint) {
    Instance *cast=instance && instance<=active_cast?cast_instances+instance-1:NULL;
    Rig *r=cast?cast->rig:instance?&enemy_rig:&max_rig;
    unsigned slot=cast || !instance?0:instance-1;
    C3D_BufInfo *binding=cast?&cast->binding:&r->binding[slot];
    C3D_SetBufInfo(binding);
    unsigned first=0,soft=0;
    C3D_Tex *selected[MAX_MATERIALS];
    unsigned face=face_variant[instance];
    for(unsigned i=0;i<r->texture_count;i++){
        bool alternate=i==r->face_part && face && (r->face_mask&(1u<<(face-1)));
        selected[i]=alternate?r->faces+face-1:r->textures+i;
        if(!r->part[i] || (!unarmed && r->part[i]==a->weapon+1)){
            bool alpha=selected[i]->fmt==GPU_RGBA4;
            if(alpha && r->counts[i]){
                if(transform){
                    const DrawVertex *vertices=cast?cast->vertices:r->buffer[slot];
                    Point center=vertices[r->indices[first]].pos;
                    alpha_skin(transform,center,portal,binding,r->indices+first,r->counts[i],selected[i],tint,C3D_GetTexEnv(0)->color);
                }else soft|=1u<<i;
                C3D_AlphaTest(true,GPU_EQUAL,255);
            }
            C3D_TexBind(0,selected[i]);C3D_DrawElements(GPU_TRIANGLES,r->counts[i],C3D_UNSIGNED_SHORT,r->indices+first);
            if(alpha)C3D_AlphaTest(true,GPU_GREATER,63);
        }
        first+=r->counts[i];
    }
    if(!soft)return;
    C3D_DepthTest(true,GPU_GEQUAL,GPU_WRITE_COLOR);C3D_AlphaTest(true,GPU_GREATER,0);
    C3D_AlphaBlend(GPU_BLEND_ADD,GPU_BLEND_ADD,GPU_SRC_ALPHA,GPU_ONE_MINUS_SRC_ALPHA,GPU_ONE,GPU_ZERO);
    first=0;
    for(unsigned i=0;i<r->texture_count;i++){
        if(soft&(1u<<i)){C3D_TexBind(0,selected[i]);C3D_DrawElements(GPU_TRIANGLES,r->counts[i],C3D_UNSIGNED_SHORT,r->indices+first);}
        first+=r->counts[i];
    }
    C3D_AlphaBlend(GPU_BLEND_ADD,GPU_BLEND_ADD,GPU_ONE,GPU_ZERO,GPU_ONE,GPU_ZERO);
    C3D_DepthTest(true,GPU_GEQUAL,GPU_WRITE_ALL);C3D_AlphaTest(true,GPU_GREATER,63);
}
