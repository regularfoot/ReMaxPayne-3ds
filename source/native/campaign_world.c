#include "campaign_file.h"
#include "alpha_pass.h"
#include "campaign_world.h"
#include "campaign_bounds.h"
#include "campaign_combat.h"
#include "scene.h"
#include "portal.h"
#include "particles.h"
#include "director.h"
#include "texture.h"
#include "ui.h"
#include "depth_bias.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {Point pos;float uv[2],light[2];} Vertex;
typedef struct {uint16_t base,light;int32_t prop;uint32_t count;} Batch;
enum { TEXTURE_INDEX=0x1fff, DEPTH_DECAL=0x2000, BLEND_SOLID_CORE=0x4000, ALPHA_TEST_127=0x8000 };
enum { CULL_BACK_FACES=0x2000 };
// Remote traces have no joined scene buffer. Encode the original face and its
// prop until the impact has been anchored in the receiving room.
enum { GUEST_FACE_SHIFT=17, GUEST_FACE_MASK=(1u<<GUEST_FACE_SHIFT)-1, GUEST_FACE_TAG=CAMPAIGN_PROPS<<GUEST_FACE_SHIFT };
static bool remote_guest_face(unsigned face){return (face&~(GUEST_FACE_TAG-1u))==GUEST_FACE_TAG;}
static unsigned guest_face_prop(unsigned face){return (face-GUEST_FACE_TAG)>>GUEST_FACE_SHIFT;}
typedef struct {Point lo,hi;} RayBounds;
typedef struct {
    Batch *batches;
    RayBounds *bounds;
    Vertex *vertices;
    unsigned count,total,modes;
    C3D_BufInfo binding;
} Mesh;
typedef struct {
    char magic[4];uint32_t rooms,textures,start,props;Point position;float yaw;
} Header;
typedef struct {char name[64];uint32_t batches,solids,exits,vertices;} RoomInfo;
typedef struct {
    int32_t prop;uint32_t count,first;
    Triangle *original;
    Point lo,hi;
    bool moved,displaced,player_overlap;
    uint8_t mask;bool pierceable;
    float matrix[12];
} Solid;
typedef struct {uint32_t bits[CAMPAIGN_PROPS/32];unsigned count;} PropSet;
typedef struct {
    CampaignExit exit;
    Point verts[PORTAL_VERTICES];
    uint8_t indices[3*(PORTAL_VERTICES-2)];
    unsigned start,count;
    RayBounds box;
    C3D_Mtx inverse;float yaw;
    uint64_t sound_checked;uint32_t sound_revision[2];Point sound_opening;
    bool sound_open,sound_center_open;
} Aperture;
typedef struct GuestMesh {
    unsigned owner;
    C3D_Mtx placement;
    PropSet props;
    struct GuestMesh *next;
} GuestMesh;
typedef struct {const Solid *solid;unsigned owner,first;float placement[12];} GuestSolid;
typedef struct {Point position;float radius;Point rgb;} MapLight;
typedef struct {unsigned room;Point position,axis,rgb;float radius,inner_cos,outer_cos;} ConeLight;
typedef struct {
    GuestMesh *guests;
    MapLight *lamps;unsigned lamp_count;
    ConeLight *cones;unsigned cone_count;
    RoomInfo info;
    Aperture *openings;
    Mesh geometry, masks;
    uint8_t *solid_data,*materials,*masks_live;
    unsigned solid_bytes,faces;bool check_player,pierceable;
    SceneBounds *blocks,*live_blocks;
    Solid *solids;
    float grid[4];
    uint8_t light[16][3];
} StageRoom;
static StageRoom training_light;
typedef struct {unsigned room,tag,first,second;C3D_Mtx inverse;float yaw;} PortalView;
static struct {
    FILE *file;
    Header h;
    uint32_t *offset;
    C3D_Tex *textures;
    bool *loaded;
    Mesh sky;unsigned blend_view;uint32_t beacon_color;
    StageRoom *rooms;
    PortalView views[1+CAMPAIGN_EXITS+CAMPAIGN_EXITS*CAMPAIGN_EXITS];
    unsigned view_count;
    uint8_t first_view[128],next_view[1+CAMPAIGN_EXITS+CAMPAIGN_EXITS*CAMPAIGN_EXITS];
    bool prepared,collision_ready;
    RoomInfo info;
    Solid *solids;
    Triangle *collision;
    uint8_t *collision_masks;
    Triangle *joined;uint8_t *joined_masks;SceneBounds *joined_blocks;
    unsigned joined_capacity,guest_count;
    GuestSolid guest_solids[CAMPAIGN_PROPS];
    Aperture *openings;
    unsigned current,camera_path[2],surface_mask,draw_modes;
    struct { RayBounds box; unsigned room; bool used,moved,placed; float matrix[12]; } prop_bounds[CAMPAIGN_PROPS];
} world;
CampaignProp campaign_props[CAMPAIGN_PROPS];
_Static_assert(sizeof(Header)==36 && sizeof(RoomInfo)==80 && sizeof(Batch)==12 && sizeof(CampaignExit)==72,"Campaign scene layout");

static bool read_data(void *out,size_t size,size_t count) {
    return !count || fread(out,size,count,world.file)==count;
}
static bool seek_to(unsigned index) {return fseek(world.file,world.offset[index],SEEK_SET)==0;}
static void free_mesh(Mesh *mesh) {
    if(mesh->vertices) linearFree(mesh->vertices);
    free(mesh->batches);free(mesh->bounds);*mesh=(Mesh){0};
}
static void clear_room(void) {
    scene_close();
    world.solids=NULL;world.collision=NULL;world.collision_masks=NULL;
    world.collision_ready=false;
    world.guest_count=0;
    memset(&world.info,0,sizeof world.info);
}
void campaign_close(void) {
    if (!world.file) return;
    clear_room();campaign_bounds_close();
    free_mesh(&world.sky);alpha_close();
    if(world.rooms)for(unsigned i=0;i<world.h.rooms;i++){
        StageRoom *r=world.rooms+i;
        if(r->solids)for(unsigned j=0;j<r->info.solids;j++)if(r->solids[j].prop>=0)free(r->solids[j].original);
        while(r->guests){GuestMesh *next=r->guests->next;free(r->guests);r->guests=next;}
        free(r->masks_live);free(r->live_blocks);
        free_mesh(&r->geometry);free_mesh(&r->masks);free(r->openings);free(r->solid_data);free(r->materials);free(r->lamps);free(r->cones);free(r->blocks);free(r->solids);
    }
    free(world.rooms);
    if(world.joined)linearFree(world.joined);
    if(world.joined_masks)linearFree(world.joined_masks);
    if(world.joined_blocks)linearFree(world.joined_blocks);
    if(world.loaded)for(unsigned i=0;i<world.h.textures;i++)if(world.loaded[i])C3D_TexDelete(world.textures+i);
    free(world.textures);free(world.loaded);free(world.offset);
    fclose(world.file);memset(&world,0,sizeof world);
}
static bool prepare_stage(void);
static bool read_surfaces(const char *stage);
static bool read_lamps(const char *stage){
    char path[64];snprintf(path,sizeof path,"%s.lights",stage);
    FILE *f=campaign_file(path);if(!f)return false;
    char tag[4];unsigned rooms;
    bool ok=fread(tag,4,1,f)==1 && !memcmp(tag,"MPL1",4) && fread(&rooms,4,1,f)==1 && rooms==world.h.rooms;
    for(unsigned i=0;ok && i<rooms;i++){
        StageRoom *r=world.rooms+i;
        ok=fread(&r->lamp_count,4,1,f)==1 && r->lamp_count<=4096;
        if(!ok || !r->lamp_count)continue;
        r->lamps=malloc(r->lamp_count*sizeof(MapLight));
        ok=r->lamps && fread(r->lamps,sizeof(MapLight),r->lamp_count,f)==r->lamp_count;
    }
    ok=ok && fgetc(f)==EOF;fclose(f);return ok;
}
static bool read_cones(const char *stage,StageRoom *rooms,unsigned count,int selected){
    char path[64];snprintf(path,sizeof path,"%s.dlg",stage);
    FILE *f=campaign_file(path);if(!f)return true;
    char tag[4];unsigned lights;
    bool ok=fread(tag,4,1,f)==1 && !memcmp(tag,"DLG1",4) && fread(&lights,4,1,f)==1 && lights<=4096;
    for(unsigned i=0;ok && i<lights;i++){
        ConeLight light;
        ok=fread(&light,sizeof light,1,f)==1 && light.room<count && isfinite(light.radius);
        if(!ok || (selected>=0 && light.room!=(unsigned)selected))continue;
        StageRoom *r=selected>=0?rooms:rooms+light.room;
        ConeLight *tmp=realloc(r->cones,(r->cone_count+1)*sizeof light);
        if(!tmp){ok=false;break;}
        r->cones=tmp;r->cones[r->cone_count++]=light;
    }
    ok=ok && fgetc(f)==EOF;fclose(f);return ok;
}
// The standalone training rooms use the same local axes as their campaign originals.
void campaign_training_light(RoomId id){
    static const unsigned room_ids[]={8,1,6,5,4,7,5,7,8,9};
    free(training_light.lamps);free(training_light.cones);training_light=(StageRoom){0};
    if(id>=ROOM_COUNT)return;
    memset(training_light.light,164,sizeof training_light.light);
    unsigned room=room_ids[id],count;char tag[4];
    const char *stage=id>=HOME?"part0_level1":"tutorial";char path[64];
    snprintf(path,sizeof path,"%s.surf",stage);
    FILE *f=campaign_file(path);
    if(f){
        uint8_t samples[16][3];
        if(fread(tag,4,1,f)==1 && (!memcmp(tag,"SRF1",4) || !memcmp(tag,"SRF2",4)) && fread(&count,4,1,f)==1 && room<count &&
           !fseek(f,64L*room,SEEK_CUR) && fread(training_light.grid,16,1,f)==1 && fread(samples,48,1,f)==1)
            memcpy(training_light.light,samples,sizeof samples);
        fclose(f);
    }
    snprintf(path,sizeof path,"%s.lights",stage);f=campaign_file(path);
    if(!f)return;
    if(fread(tag,4,1,f)==1 && !memcmp(tag,"MPL1",4) && fread(&count,4,1,f)==1 && room<count){
        for(unsigned i=0;i<=room;i++){
            unsigned n;if(fread(&n,4,1,f)!=1 || n>4096)break;
            if(i<room){if(fseek(f,(long)n*sizeof(MapLight),SEEK_CUR))break;}
            else if(n){
                training_light.lamps=malloc(n*sizeof(MapLight));
                if(training_light.lamps && fread(training_light.lamps,sizeof(MapLight),n,f)==n)training_light.lamp_count=n;
            }
        }
    }
    fclose(f);
    read_cones(stage,&training_light,128,(int)room);
}
bool campaign_open(const char *stage,bool actor_lights) {
    if(!stage || strspn(stage,"abcdefghijklmnopqrstuvwxyz0123456789_-")!=strlen(stage) || strlen(stage)>31) return false;
    campaign_close();
    char path[96];
    snprintf(path,sizeof path,"%s.mpc",stage);
    world.file=campaign_file(path);
    if(!world.file) return false;
    if(fseek(world.file,0,SEEK_END)!=0) goto fail;
    long length=ftell(world.file);rewind(world.file);
    if(!read_data(&world.h,sizeof world.h,1) || memcmp(world.h.magic,"MPC1",4) ||
        !world.h.rooms || world.h.rooms>128 || world.h.start>=world.h.rooms ||
        world.h.textures<2 || world.h.textures>1024 || world.h.props>CAMPAIGN_PROPS) goto fail;
    unsigned n=world.h.rooms+world.h.textures+2;
    world.offset=malloc(n*sizeof(*world.offset));world.textures=calloc(world.h.textures,sizeof(*world.textures));
    world.loaded=calloc(world.h.textures,sizeof(*world.loaded));
    if(!world.offset || !world.textures || !world.loaded || !read_data(world.offset,4,n) ||
        length<0 || world.offset[n-1]!=(unsigned long)length || world.offset[0]!=sizeof(Header)+4*n) goto fail;
    for(unsigned i=1;i<n;i++) if(world.offset[i]<=world.offset[i-1]) goto fail;
    for(unsigned i=0;i<world.h.props;i++) campaign_props[i]=(CampaignProp){.solid=true};
    if(!campaign_bounds_open(stage) || !prepare_stage() || !read_surfaces(stage) || (actor_lights && (!read_lamps(stage) || !read_cones(stage,world.rooms,world.h.rooms,-1))))goto fail;
    world.prepared=true;
    return true;
fail:campaign_close();return false;
}
unsigned campaign_start(Player *player) {
    *player=(Player){.position=world.h.position,.yaw=world.h.yaw};return world.h.start;
}
const char *campaign_room_name(void) {return world.info.name;}
unsigned campaign_room_id(void) {return world.current;}
unsigned campaign_exit_count(void) {return world.info.exits;}
const CampaignExit *campaign_room_exit(unsigned room,unsigned slot){
    if(room>=world.h.rooms || slot>=world.rooms[room].info.exits)return NULL;
    return &world.rooms[room].openings[slot].exit;
}
const CampaignExit *campaign_exit(unsigned slot) {return slot<world.info.exits?&world.openings[slot].exit:NULL;}

static bool bind_mesh(Mesh *m) {
    GSPGPU_FlushDataCache(m->vertices,m->total*sizeof(Vertex));
    BufInfo_Init(&m->binding);
    return BufInfo_Add(&m->binding,m->vertices,sizeof(Vertex),3,0x210)>=0;
}
static bool paired_glass_quad(const Vertex *vertices) {
    Point corners[4];
    unsigned count=0,edges[2]={0},reverse[2]={0},used[2]={0},faces[2]={0};
    C3D_FVec first_normal={0};
    for(unsigned t=0;t<4;t++){
        unsigned ids[3];
        C3D_FVec positions[3];
        for(unsigned j=0;j<3;j++){
            Point p=vertices[3*t+j].pos;
            unsigned i=0;
            while(i<count && (corners[i].x!=p.x || corners[i].y!=p.y || corners[i].z!=p.z))i++;
            if(i==count){
                if(count==4)return false;
                corners[count++]=p;
            }
            ids[j]=i;
            positions[j]=FVec3_New(p.x,p.y,p.z);
        }
        C3D_FVec normal=FVec3_Cross(FVec3_Subtract(positions[1],positions[0]),FVec3_Subtract(positions[2],positions[0]));
        if(!t)first_normal=normal;
        float facing=FVec3_Dot(first_normal,normal);
        if(!isfinite(facing) || facing==0)return false;
        unsigned side=facing<0;
        if(++faces[side]>2)return false;
        for(unsigned j=0;j<3;j++){
            unsigned a=ids[j],b=ids[(j+1)%3],edge=1u<<(4*a+b);
            if(a==b || (edges[side]&edge))return false;
            used[side]|=1u<<a;
            edges[side]|=edge;reverse[side]|=1u<<(4*b+a);
        }
    }
    unsigned inner0=edges[0]&reverse[0],inner1=edges[1]&reverse[1];
    if(used[0]!=15 || used[1]!=15 || __builtin_popcount(inner0)!=2 || __builtin_popcount(inner1)!=2 ||
       (edges[0]^inner0)!=(reverse[1]^inner1))return false;
    Point a=corners[0],b=corners[3];
    return fabsf(FVec3_Dot(first_normal,FVec3_New(b.x-a.x,b.y-a.y,b.z-a.z)))<=
           .00001f*FVec3_Magnitude(first_normal);
}
static bool read_mesh(Mesh *m,unsigned count,unsigned expected) {
    if(count>4096) return false;
    if(!count) return expected==0 || expected==UINT32_MAX;
    m->count=count;m->batches=malloc(count*sizeof(Batch));
    if(!m->batches || !read_data(m->batches,sizeof(Batch),count)) return false;
    for(unsigned i=0;i<count;i++) {
        const Batch b=m->batches[i];
        if((b.base&TEXTURE_INDEX)>=world.h.textures || (b.light&TEXTURE_INDEX)>=world.h.textures || b.prop< -1 || b.prop>=(int)world.h.props ||
            !b.count || b.count%3 || b.count>240000-m->total) return false;
        if(b.base&BLEND_SOLID_CORE)m->modes|=1;
        m->modes|=1u<<(b.light>>14);m->total+=b.count;
    }
    if(expected!=UINT32_MAX && m->total!=expected) return false;
    m->vertices=linearAlloc(m->total*sizeof(Vertex));
    if(!m->vertices || !read_data(m->vertices,sizeof(Vertex),m->total) || !bind_mesh(m))return false;
    // The worldsphere follows the camera. Only room geometry needs bounds.
    if(expected==UINT32_MAX)return true;
    m->bounds=malloc(count*sizeof(*m->bounds));
    if(!m->bounds)return false;
    unsigned first=0;
    for(unsigned i=0;i<count;i++){
        Batch *batch=m->batches+i;
        unsigned end=first+batch->count;
        // KF2 panes have two opposed faces, sometimes with different diagonals.
        // Draw the facing side alone to avoid blending/writing depth twice.
        if(batch->count==12 && (batch->light>>14)==1 &&
           (batch->base&BLEND_SOLID_CORE) && !(batch->base&DEPTH_DECAL) && paired_glass_quad(m->vertices+first))
            batch->light|=CULL_BACK_FACES;
        RayBounds box={m->vertices[first].pos,m->vertices[first].pos};
        for(unsigned j=first+1;j<end;j++){
            Point p=m->vertices[j].pos;
            if(p.x<box.lo.x)box.lo.x=p.x;
            if(p.y<box.lo.y)box.lo.y=p.y;
            if(p.z<box.lo.z)box.lo.z=p.z;
            if(p.x>box.hi.x)box.hi.x=p.x;
            if(p.y>box.hi.y)box.hi.y=p.y;
            if(p.z>box.hi.z)box.hi.z=p.z;
        }
        m->bounds[i]=box;first=end;
    }
    return true;
}
static bool read_room_header(unsigned room,RoomInfo *info,Aperture *openings) {
    if(room>=world.h.rooms || !seek_to(1+room)) return false;
    if(!read_data(info,sizeof *info,1) || !memchr(info->name,0,sizeof info->name) || info->batches>4096 ||
        info->exits>CAMPAIGN_EXITS || info->solids>world.h.props+1 || info->vertices>240000) return false;
    for(unsigned i=0;i<info->exits;i++) {
        Aperture tmp={0};
        if(!read_data(&tmp.exit,sizeof tmp.exit,1) || tmp.exit.points<3 || tmp.exit.points>32 ||
            tmp.exit.id>=256 || tmp.exit.room>=world.h.rooms || !read_data(tmp.verts,sizeof(Point),tmp.exit.points)) return false;
        if(openings)openings[i]=tmp;
    }
    return true;
}
static bool index_solids(StageRoom *room) {
    RoomInfo info=room->info;
    room->solids=calloc(info.solids,sizeof(Solid));
    if(info.solids && !room->solids)return false;
    const uint8_t *src=room->solid_data;
    unsigned total=0,used=0;
    for(unsigned i=0;i<info.solids;i++){
        Solid *solid=room->solids+i;
        if(room->solid_bytes-used<8)return false;
        memcpy(&solid->prop,src+used,4);memcpy(&solid->count,src+used+4,4);used+=8;
        if(!solid->count || solid->count>room->faces-total || solid->prop< -1 || solid->prop>=(int)world.h.props ||
           solid->count>(room->solid_bytes-used)/sizeof(Triangle))return false;
        Triangle *live=(Triangle*)room->solid_data+total;
        solid->first=total;
        if(solid->prop>=0){
            solid->original=malloc(solid->count*sizeof(Triangle));
            if(!solid->original)return false;
            memcpy(solid->original,src+used,solid->count*sizeof(Triangle));
            scene_face_bounds(solid->original,solid->count,&solid->lo,&solid->hi);
        }else solid->original=live;
        memmove(live,src+used,solid->count*sizeof(Triangle));
        solid->mask=SCENE_BODY|SCENE_SHOT;
        total+=solid->count;used+=solid->count*sizeof(Triangle);
    }
    return total==room->faces && used==room->solid_bytes;
}
static Point apply(const float *m,Point p) {
    return (Point){m[9]+p.x*m[0]+p.y*m[3]+p.z*m[6],m[10]+p.x*m[1]+p.y*m[4]+p.z*m[7],
                   m[11]+p.x*m[2]+p.y*m[5]+p.z*m[8]};
}
static unsigned prop_channels(const CampaignProp *prop){
    if(!prop)return 15;
    if(prop->hidden || !prop->solid)return 0;
    unsigned cfg=prop->collision_mask?prop->collision_mask:SCENE_BODY|SCENE_SHOT;
    return (cfg&SCENE_BODY) | ((cfg&SCENE_SHOT)?SCENE_SHOT|SCENE_BIG_SHOT|SCENE_PIERCE:0);
}
static void update_face_masks(StageRoom *room,const Solid *solid){
    for(unsigned j=solid->first;j<solid->first+solid->count;j++)
        room->masks_live[j]=((room->materials[j]>>4)&solid->mask) | (solid->player_overlap?SCENE_PLAYER_OVERLAP:0);
}
static bool join_guest_collision(void);
bool campaign_collision(void) {
    StageRoom *room=world.rooms+world.current;
    // Refit the authored room cache first. Rendering visitors and collision
    // visitors must then use the same portal placement and current DO matrix.
    scene_bind(world.collision,room->faces,room->masks_live,room->live_blocks);
    for(unsigned i=0;i<world.info.solids;i++) {
        Solid *s=&world.solids[i];
        CampaignProp *p=s->prop<0?NULL:&campaign_props[s->prop];
        unsigned mask=prop_channels(p);
        if(s->mask!=mask){
            s->mask=mask;update_face_masks(&world.rooms[world.current],s);
        }
        if(!p || (s->moved==p->moved &&
           (!p->moved || !memcmp(s->matrix,p->matrix,sizeof s->matrix))))continue;
        if(!p->moved) memcpy(world.collision+s->first,s->original,s->count*sizeof(Triangle));
        else {
            const float *m=p->matrix;
            bool level=m[1]==0 && m[3]==0 && m[4]==1 && m[5]==0 && m[7]==0 &&
                       fabsf(m[0]*m[0]+m[2]*m[2]-1)<.0001f && fabsf(m[6]*m[6]+m[8]*m[8]-1)<.0001f &&
                       fabsf(m[0]*m[6]+m[2]*m[8])<.0001f;
            for(unsigned j=0;j<s->count;j++) {
                const Triangle *t=s->original+j;
                Point a=apply(m,t->a),b=apply(m,t->b),c=apply(m,t->c);
                float slope=t->normal_y;
                if(!level){
                    Point u={b.x-a.x,b.y-a.y,b.z-a.z},v={c.x-a.x,c.y-a.y,c.z-a.z};
                    float nx=u.y*v.z-u.z*v.y,ny=u.z*v.x-u.x*v.z,nz=u.x*v.y-u.y*v.x;
                    slope=fabsf(ny)/fmaxf(.000001f,sqrtf(nx*nx+ny*ny+nz*nz));
                }
                world.collision[s->first+j]=(Triangle){a,b,c,slope};
            }
        }
        s->displaced=true;world.rooms[world.current].check_player=true;
        s->moved=p->moved;
        if(s->moved)memcpy(s->matrix,p->matrix,sizeof s->matrix);
        scene_refit(s->first,s->count);
    }
    campaign_update_moving_rooms();
    return world.collision_ready && join_guest_collision();
}
// Measure the clearance to actual panel faces in both poses. A panel which
// is already touching the capsule may still move away from it.
bool campaign_contact_frame(unsigned from,unsigned to,Point center,Point *mapped){
    if(from>=world.h.rooms || to>=world.h.rooms)return false;
    const StageRoom *r=world.rooms+from;
    for(unsigned i=0;i<r->info.exits;i++){
        const Aperture *a=r->openings+i;
        if(a->exit.room!=to)continue;
        if(!portal_nearby(a->verts,a->indices,a->count,a->exit.normal,center))continue;
        // A closed portal is intentional: its sensor must be able to open it.
        *mapped=apply(a->exit.transform,center);return true;
    }
    // A moving train keeps its authored room and child sensors after crossing
    // a portal. Its cabin may now be far beyond that fixed aperture. Resolve
    // contacts in the same receiving-room frame as its visible collision mesh.
    for(const GuestMesh *g=r->guests;g;g=g->next){
        if(g->owner!=to || !g->props.count)continue;
        C3D_Mtx inverse=g->placement;Mtx_Inverse(&inverse);
        C3D_FVec q=Mtx_MultiplyFVecH(&inverse,FVec3_New(center.x,center.y,center.z));
        Point local={q.x,q.y,q.z};
        for(unsigned prop=0;prop<world.h.props;prop++)
            if((g->props.bits[prop/32]&(1u<<(prop%32))) && campaign_bounds_overlap(prop,local,.6f)){
                *mapped=local;return true;
            }
    }
    return false;
}
bool campaign_panel_blocked(unsigned prop,unsigned room,const float *from,const float *to){
    if(room>=world.h.rooms || director.state[0].room>=world.h.rooms || prop>=world.h.props || director.state[0].dead || campaign_props[prop].hidden ||
       !(prop_channels(campaign_props+prop)&SCENE_BODY))return false;
    if(!memcmp(from,to,12*sizeof(float)))return false;
    const StageRoom *r=world.rooms+room;
    // Reserve standing headroom too: a drawer must not trap stationary kneeling
    // Max underneath it when the original skin has no crouch locomotion.
    Player body={.position=director.state[0].position};
    if(room!=director.state[0].room){
        Point center=body.position;center.y+=.9f;
        const StageRoom *receiving=world.rooms+director.state[0].room;
        const GuestMesh *guest=receiving->guests;
        while(guest && (guest->owner!=room || !(guest->props.bits[prop/32]&(1u<<(prop%32)))))guest=guest->next;
        if(guest){
            C3D_Mtx inverse=guest->placement;Mtx_Inverse(&inverse);
            C3D_FVec local=Mtx_MultiplyFVecH(&inverse,FVec3_New(center.x,center.y,center.z));
            center=(Point){local.x,local.y,local.z};
        }else if(!campaign_contact_frame(director.state[0].room,room,center,&center))return false;
        body.position=center;body.position.y-=.9f;
    }
    const float radius=.225f;
    Point feet=body.position;
    for(unsigned i=0;i<r->info.solids;i++){
        const Solid *s=r->solids+i;if(s->prop!=(int)prop)continue;
        Point mid={(s->lo.x+s->hi.x)*.5f,(s->lo.y+s->hi.y)*.5f,(s->lo.z+s->hi.z)*.5f};
        Point half={s->hi.x-mid.x,s->hi.y-mid.y,s->hi.z-mid.z};
        mid=apply(to,mid);
        if(fabsf(mid.x-feet.x)>radius+fabsf(to[0])*half.x+fabsf(to[3])*half.y+fabsf(to[6])*half.z ||
           fabsf(mid.z-feet.z)>radius+fabsf(to[2])*half.x+fabsf(to[5])*half.y+fabsf(to[8])*half.z)continue;
        float before=INFINITY,after=INFINITY;
        for(unsigned j=0;j<s->count;j++){
            if(!((r->materials[s->first+j]>>4)&SCENE_BODY))continue;
            const Triangle *t=s->original+j;
            Triangle next={apply(to,t->a),apply(to,t->b),apply(to,t->c),0};
            after=fminf(after,scene_panel_distance(&next,&body));
            Triangle old={apply(from,t->a),apply(from,t->b),apply(from,t->c),0};
            before=fminf(before,scene_panel_distance(&old,&body));
        }
        if(after<radius*radius && after+.000001f<before)return true;
    }
    return false;
}
// An animated panel may sweep into a stationary capsule. Let just that
// player walk clear of its sides; the floor, shots and camera stay solid.
void campaign_panel_contact(const Player *p,bool airborne){
    StageRoom *r=world.rooms+world.current;
    if(!r->check_player)return;
    r->check_player=false;
    for(unsigned i=0;i<r->info.solids;i++){
        Solid *s=r->solids+i;
        if(!s->displaced && !s->player_overlap)continue;
        s->displaced=false;
        bool touching=(s->mask&SCENE_BODY) && scene_wall_overlap(s->first,s->count,p,airborne);
        if(touching!=s->player_overlap){
            s->player_overlap=touching;
            update_face_masks(r,s);
        }
        if(touching)r->check_player=true;
    }
    if(world.guest_count)memcpy(world.joined_masks,r->masks_live,r->faces);
}
static void affine(float out[12],const C3D_Mtx *m);
static bool anchor_prop_mark(Impact *mark,unsigned prop,const float *placement){
    if(campaign_props[prop].hidden)return false;
    // Three points preserve the contact plane under rotation and scale.
    Point n=mark->normal,axis=fabsf(n.y)<.9f?(Point){0,1,0}:(Point){1,0,0};
    Point t={n.y*axis.z-n.z*axis.y,n.z*axis.x-n.x*axis.z,n.x*axis.y-n.y*axis.x};
    Point b={n.y*t.z-n.z*t.y,n.z*t.x-n.x*t.z,n.x*t.y-n.y*t.x};
    Point c=mark->point,ct={c.x+t.x,c.y+t.y,c.z+t.z},cb={c.x+b.x,c.y+b.y,c.z+b.z};
    PlatformContact a,u,v;
    if(placement){
        a=campaign_platform_placed(prop,c,placement);
        u=campaign_platform_placed(prop,ct,placement);
        v=campaign_platform_placed(prop,cb,placement);
    }else{
        a=campaign_platform_contact(prop,c);
        u=campaign_platform_contact(prop,ct);
        v=campaign_platform_contact(prop,cb);
    }
    if(a.prop<0 || u.prop<0 || v.prop<0)return false;
    mark->support=prop+1;mark->local=a.local;mark->tangent=u.local;mark->bitangent=v.local;
    return true;
}
bool campaign_anchor_mark(Impact *mark,unsigned face){
    if(mark->room>=world.h.rooms)return false;
    const StageRoom *r=world.rooms+mark->room;
    if(remote_guest_face(face)){
        unsigned prop=guest_face_prop(face);
        for(const GuestMesh *g=r->guests;g;g=g->next){
            if(g->owner!=world.prop_bounds[prop].room || !(g->props.bits[prop/32]&(1u<<(prop%32))))continue;
            float placement[12];affine(placement,&g->placement);
            return anchor_prop_mark(mark,prop,placement);
        }
        return false;
    }
    if(face>=r->faces){
        if(mark->room!=world.current)return false;
        for(unsigned i=0;i<world.guest_count;i++){
            const GuestSolid *g=world.guest_solids+i;const Solid *s=g->solid;
            if(face<g->first || face-g->first>=s->count)continue;
            return anchor_prop_mark(mark,s->prop,g->placement);
        }
        return false;
    }
    for(unsigned i=0;i<r->info.solids;i++){
        const Solid *s=r->solids+i;
        if(face<s->first || face-s->first>=s->count)continue;
        return s->prop<0 || anchor_prop_mark(mark,s->prop,NULL);
    }
    return false;
}
bool campaign_mark_pose(const Impact *mark,Point *center,Point *normal){
    *center=mark->point;*normal=mark->normal;
    if(!mark->support)return true;
    const CampaignProp *p=campaign_props+mark->support-1;
    if(p->hidden || !p->solid)return false;
    Point c=mark->local,u=mark->tangent,v=mark->bitangent;
    if(p->moved){c=apply(p->matrix,c);u=apply(p->matrix,u);v=apply(p->matrix,v);}
    unsigned home=world.prop_bounds[mark->support-1].room;
    if(home!=mark->room){
        bool found=false;
        if(mark->room>=world.h.rooms)return false;
        for(const GuestMesh *g=world.rooms[mark->room].guests;g;g=g->next){
            unsigned prop=mark->support-1;
            if(g->owner!=home || !((g->props.bits[prop/32]>>(prop%32))&1u))continue;
            C3D_FVec a=Mtx_MultiplyFVecH(&g->placement,FVec3_New(c.x,c.y,c.z));c=(Point){a.x,a.y,a.z};
            a=Mtx_MultiplyFVecH(&g->placement,FVec3_New(u.x,u.y,u.z));u=(Point){a.x,a.y,a.z};
            a=Mtx_MultiplyFVecH(&g->placement,FVec3_New(v.x,v.y,v.z));v=(Point){a.x,a.y,a.z};found=true;break;
        }
        if(!found)return false;
    }
    u=(Point){u.x-c.x,u.y-c.y,u.z-c.z};v=(Point){v.x-c.x,v.y-c.y,v.z-c.z};
    Point axis={u.y*v.z-u.z*v.y,u.z*v.x-u.x*v.z,u.x*v.y-u.y*v.x};
    float length=sqrtf(axis.x*axis.x+axis.y*axis.y+axis.z*axis.z);
    if(length<.0001f)return false;
    *normal=(Point){axis.x/length,axis.y/length,axis.z/length};*center=c;return true;
}
int campaign_hit_prop(unsigned face) {
    for(unsigned i=0;i<world.info.solids;i++) {
        const Solid *s=world.solids+i;
        if(s->first!=UINT32_MAX && face>=s->first && face-s->first<s->count) return s->prop;
    }
    for(unsigned i=0;i<world.guest_count;i++){
        const GuestSolid *g=world.guest_solids+i;
        if(face>=g->first && face-g->first<g->solid->count)return g->solid->prop;
    }
    return -1;
}
bool campaign_body_contact(unsigned prop){
    for(unsigned i=0;i<world.info.solids;i++){
        const Solid *s=world.solids+i;
        if(s->prop==(int)prop && scene_body_touched(s->first,s->count))return true;
    }
    for(unsigned i=0;i<world.guest_count;i++){
        const GuestSolid *g=world.guest_solids+i;
        if(g->solid->prop==(int)prop && scene_body_touched(g->first,g->solid->count))return true;
    }
    return false;
}
bool campaign_collision_relevant(unsigned prop){
    if(!world.prepared || prop>=world.h.props)return false;
    unsigned home=world.prop_bounds[prop].room;
    if(home==world.current)return true;
    const StageRoom *r=world.rooms+world.current;
    for(const GuestMesh *g=r->guests;g;g=g->next)
        if(g->props.bits[prop/32]&(1u<<(prop%32)))return true;
    for(unsigned i=0;i<r->info.exits;i++)if(r->openings[i].exit.room==home)return true;
    return false;
}
PlatformContact campaign_standing_on(Point feet) {
    Hit floor;
    int prop=scene_ground(feet,&floor)?campaign_hit_prop(floor.face):-1;
    if(prop>=0)for(unsigned i=0;i<world.guest_count;i++){
        const GuestSolid *g=world.guest_solids+i;
        if(floor.face>=g->first && floor.face-g->first<g->solid->count)
            return campaign_platform_placed(prop,feet,g->placement);
    }
    return campaign_platform_contact(prop,feet);
}
static bool solid_overlaps(const Solid *s,Point lower,Point upper) {
    if(s->prop<0 || campaign_props[s->prop].hidden || !campaign_props[s->prop].solid) return false;
    Point lo=s->lo,hi=s->hi;
    if(campaign_props[s->prop].moved) {
        const float *m=campaign_props[s->prop].matrix;
        Point half={(hi.x-lo.x)*.5f,(hi.y-lo.y)*.5f,(hi.z-lo.z)*.5f};
        Point mid=apply(m,(Point){lo.x+half.x,lo.y+half.y,lo.z+half.z});
        Point ext={fabsf(m[0])*half.x+fabsf(m[3])*half.y+fabsf(m[6])*half.z,
                   fabsf(m[1])*half.x+fabsf(m[4])*half.y+fabsf(m[7])*half.z,
                   fabsf(m[2])*half.x+fabsf(m[5])*half.y+fabsf(m[8])*half.z};
        lo=(Point){mid.x-ext.x,mid.y-ext.y,mid.z-ext.z};
        hi=(Point){mid.x+ext.x,mid.y+ext.y,mid.z+ext.z};
    }
    return upper.x>=lo.x && lower.x<=hi.x && upper.y>=lo.y && lower.y<=hi.y && upper.z>=lo.z && lower.z<=hi.z;
}
static bool in_solid(const Solid *s,Point point,float radius){
    return solid_overlaps(s,(Point){point.x-radius,point.y-radius,point.z-radius},
                            (Point){point.x+radius,point.y+radius,point.z+radius});
}
int campaign_contact(Point point,float radius) {
    for(unsigned i=0;i<world.info.solids;i++) if(in_solid(world.solids+i,point,radius)) return world.solids[i].prop;
    return -1;
}
static bool load_textures(const Mesh *m) {
    for(unsigned i=0;i<m->count;i++) {
        unsigned ids[]={m->batches[i].base&TEXTURE_INDEX,m->batches[i].light&TEXTURE_INDEX};
        for(unsigned j=0;j<2;j++) {
            unsigned id=ids[j];
            if(world.loaded[id]) continue;
            unsigned index=1+world.h.rooms+id;
            if(!seek_to(index) || !native_texture_read(world.file,&world.textures[id])) return false;
            world.loaded[id]=true;
            if(ftell(world.file)!=(long)world.offset[index+1]) return false;
        }
    }
    return true;
}
static void matrix(C3D_Mtx *out,const float *m);
static void extend_box(RayBounds *box,Point p){
    box->lo=(Point){fminf(box->lo.x,p.x),fminf(box->lo.y,p.y),fminf(box->lo.z,p.z)};
    box->hi=(Point){fmaxf(box->hi.x,p.x),fmaxf(box->hi.y,p.y),fmaxf(box->hi.z,p.z)};
}
static bool prepare_stage(void) {
    unsigned sky_batches;
    if(!seek_to(0) || !read_data(&sky_batches,4,1) || !read_mesh(&world.sky,sky_batches,UINT32_MAX) || !load_textures(&world.sky))return false;
    world.rooms=calloc(world.h.rooms,sizeof(StageRoom));if(!world.rooms)return false;
    for(unsigned id=0;id<world.h.rooms;id++){
        ui_loading_progress(5+65*id/world.h.rooms);
        StageRoom *r=world.rooms+id;Aperture openings[CAMPAIGN_EXITS];
        if(!read_room_header(id,&r->info,openings) || !read_mesh(&r->geometry,r->info.batches,r->info.vertices))return false;
        world.draw_modes|=r->geometry.modes;
        for(unsigned i=0;i<r->geometry.count;i++){
            int prop=r->geometry.batches[i].prop;
            if(prop<0 || !r->geometry.bounds)continue;
            RayBounds b=r->geometry.bounds[i];
            if(!world.prop_bounds[prop].used){
                world.prop_bounds[prop].box=b;world.prop_bounds[prop].room=id;world.prop_bounds[prop].used=true;
            }else{extend_box(&world.prop_bounds[prop].box,b.lo);extend_box(&world.prop_bounds[prop].box,b.hi);}
        }
        long start=ftell(world.file);
        if(start<0 || (unsigned long)start>world.offset[id+2])return false;
        r->solid_bytes=world.offset[id+2]-start;
        if(r->solid_bytes<8*r->info.solids)return false;
        unsigned bytes=r->solid_bytes-8*r->info.solids;
        if(bytes%sizeof(Triangle) || bytes/sizeof(Triangle)>96000)return false;
        r->faces=bytes/sizeof(Triangle);
        r->masks_live=malloc(r->faces?r->faces:1);
        if(!r->masks_live)return false;
        memset(r->masks_live,SCENE_BODY|SCENE_SHOT,r->faces);
        r->solid_data=malloc(r->solid_bytes?r->solid_bytes:1);
        if(!r->solid_data || !read_data(r->solid_data,1,r->solid_bytes) || !index_solids(r) || !load_textures(&r->geometry))return false;
        unsigned count=r->info.exits;if(!count)continue;
        r->openings=malloc(count*sizeof(Aperture));if(!r->openings)return false;
        memcpy(r->openings,openings,count*sizeof(Aperture));
        Mesh *mask=&r->masks;
        for(unsigned j=0;j<count;j++){
            Aperture *a=r->openings+j;matrix(&a->inverse,a->exit.transform);
            Mtx_Inverse(&a->inverse);a->yaw=atan2f(a->inverse.r[0].z,a->inverse.r[2].z);
            a->box=(RayBounds){a->verts[0],a->verts[0]};
            for(unsigned k=1;k<a->exit.points;k++)extend_box(&a->box,a->verts[k]);
            a->start=mask->total;a->count=portal_triangles(a->verts,a->exit.points,a->exit.normal,a->indices);
            if(!a->count)return false;
            mask->total+=a->count;
        }
        mask->total+=3;
        mask->vertices=linearAlloc(mask->total*sizeof(Vertex));if(!mask->vertices)return false;
        Vertex *v=mask->vertices;
        for(unsigned j=0;j<count;j++){
            Aperture *a=r->openings+j;
            for(unsigned k=0;k<a->count;k++)*v++=(Vertex){.pos=a->verts[a->indices[k]]};
        }
        *v++=(Vertex){.pos={-1,-1,-.5f}};
        *v++=(Vertex){.pos={3,-1,-.5f}};
        *v=(Vertex){.pos={-1,3,-.5f}};
        if(!bind_mesh(mask))return false;
    }
    for(unsigned i=0;i<2;i++)if(!world.loaded[i]){
        if(!seek_to(1+world.h.rooms+i) || !native_texture_read(world.file,&world.textures[i]))return false;
        world.loaded[i]=true;
    }
    return true;
}
static const char *const surface_names[]={"default","carpet","cardboard","couch","gravel","metal","metal_hollow","metal_outside","metal_solid","paper","pipes","pipes_steam","snow","water_bottom","wood","wireframe"};
static bool read_surfaces(const char *stage){
    char path[72];snprintf(path,sizeof path,"%s.surf",stage);
    FILE *f=campaign_file(path);if(!f)return false;
    struct {char tag[4];unsigned rooms;} h;
    bool ok=fread(&h,sizeof h,1,f)==1 && (!memcmp(h.tag,"SRF1",4) || !memcmp(h.tag,"SRF2",4)) && h.rooms==world.h.rooms;
    for(unsigned i=0;ok && i<h.rooms;i++){
        StageRoom *r=world.rooms+i;
        ok=fread(r->grid,sizeof r->grid,1,f)==1 && fread(r->light,sizeof r->light,1,f)==1;
    }
    uint32_t offsets[129];
    ok=ok && fread(offsets,4,h.rooms+1,f)==h.rooms+1;
    for(unsigned i=0;ok && i<h.rooms;i++){
        StageRoom *r=world.rooms+i;unsigned n,blocks=(r->faces+15)/16;
        ok=ftell(f)==(long)offsets[i] && fread(&n,4,1,f)==1 && n==r->faces;
        if(!ok)break;
        r->materials=malloc(n?n:1);r->blocks=malloc((blocks?blocks:1)*sizeof(SceneBounds));
        r->live_blocks=malloc((blocks?blocks:1)*sizeof(SceneBounds));
        ok=r->live_blocks && r->materials && r->blocks && fread(r->materials,1,n,f)==n && fread(r->blocks,sizeof(SceneBounds),blocks,f)==blocks;
        if(ok){
            memcpy(r->live_blocks,r->blocks,blocks*sizeof(SceneBounds));
            for(unsigned j=0;j<n;j++){
                if(h.tag[3]=='1')r->materials[j]|=(SCENE_BODY|SCENE_SHOT|SCENE_BIG_SHOT)<<4;
                world.surface_mask|=1u<<(r->materials[j]&15);
            }
            for(unsigned j=0;j<r->info.solids;j++){
                Solid *solid=r->solids+j;solid->mask=prop_channels(solid->prop<0?NULL:campaign_props+solid->prop);
                update_face_masks(r,solid);
                if(solid->prop>=0)for(unsigned k=solid->first;k<solid->first+solid->count;k++)
                    if((r->materials[k]>>4)&SCENE_PIERCE){solid->pierceable=r->pierceable=true;break;}
            }
        }
    }
    ok=ok && ftell(f)==(long)offsets[h.rooms] && fgetc(f)==EOF;fclose(f);return ok;
}
static void build_views(const unsigned path[2]){
    unsigned room=world.current;PortalView *root=world.views;
    *root=(PortalView){0};Mtx_Identity(&root->inverse);
    memcpy(world.camera_path,path,sizeof world.camera_path);
    for(unsigned i=0;i<2 && path[i]!=UINT32_MAX;i++){
        const Aperture *a=world.rooms[room].openings+path[i];
        C3D_Mtx transform;Mtx_Multiply(&transform,&root->inverse,&a->inverse);
        root->inverse=transform;root->yaw+=a->yaw;room=a->exit.room;
    }
    root->room=room;world.view_count=1;
    StageRoom *r=world.rooms+room;
    for(unsigned i=0;i<r->info.exits;i++){
        Aperture *a=r->openings+i;
        PortalView *v=world.views+world.view_count++;
        *v=(PortalView){.room=a->exit.room,.tag=i+1,.first=i,.yaw=root->yaw+a->yaw};
        Mtx_Multiply(&v->inverse,&root->inverse,&a->inverse);
    }
    for(unsigned i=0;i<r->info.exits;i++){
        Aperture *a=r->openings+i;StageRoom *next=world.rooms+a->exit.room;
        for(unsigned j=0;j<next->info.exits;j++){
            Aperture *b=next->openings+j;if(b->exit.room==room || b->exit.room==a->exit.room)continue;
            PortalView *v=world.views+world.view_count++;
            *v=(PortalView){.room=b->exit.room,.tag=(i+1)|((j+1)<<4),.first=i,.second=j,.yaw=world.views[i+1].yaw+b->yaw};
            Mtx_Multiply(&v->inverse,&world.views[i+1].inverse,&b->inverse);
        }
    }
    memset(world.first_view,255,sizeof world.first_view);
    for(unsigned v=world.view_count;v--;){
        unsigned id=world.views[v].room;
        world.next_view[v]=world.first_view[id];world.first_view[id]=v;
    }
}

bool campaign_enter(unsigned room) {
    if(!world.prepared || room>=world.h.rooms)return false;
    world.collision_ready=false;world.guest_count=0;world.current=room;StageRoom *r=world.rooms+room;
    world.info=r->info;world.openings=r->openings;
    world.solids=r->solids;world.collision=(Triangle*)r->solid_data;world.collision_masks=r->masks_live;
    scene_bind(world.collision,r->faces,r->masks_live,r->live_blocks);
    world.collision_ready=true;
    if(!campaign_collision())return false;
    build_views((unsigned[2]){UINT32_MAX,UINT32_MAX});
    return true;
}

// World coordinates for gameplay stay in the player's room. Portal masks must
// start in the room occupied by the camera at the far end of its boom.
void campaign_camera_room(Point pivot,const Camera *cam,const bool *enabled){
    unsigned path[2]={UINT32_MAX,UINT32_MAX},room=world.current;Point eye=cam->eye;
    for(unsigned step=0;enabled && step<2;step++){
        Point d={eye.x-pivot.x,eye.y-pivot.y,eye.z-pivot.z};float end=1;CampaignExit hit;
        if(!campaign_ray_exit(room,pivot,d,&end,enabled,&hit))break;
        const StageRoom *r=world.rooms+room;
        for(unsigned i=0;i<r->info.exits;i++)if(r->openings[i].exit.id==hit.id){path[step]=i;break;}
        // Continue from just beyond this opening, in the next room's coordinate system.
        end=fminf(1,end+.0001f);
        pivot=campaign_transform(&hit,(Point){pivot.x+d.x*end,pivot.y+d.y*end,pivot.z+d.z*end},false);
        eye=campaign_transform(&hit,eye,false);room=hit.room;
    }
    if(memcmp(path,world.camera_path,sizeof path))build_views(path);
}
unsigned campaign_view_count(void){return world.view_count;}
unsigned campaign_first_view(unsigned room){return room<world.h.rooms?world.first_view[room]:255;}
unsigned campaign_next_view(unsigned view){return world.next_view[view];}
bool campaign_view_enabled(unsigned view,const bool *enabled){
    if(view>=world.view_count)return false;
    if(!view || !enabled)return true;
    const PortalView *v=world.views+view;
    const Aperture *a=world.rooms[world.views[0].room].openings+v->first;
    return enabled[a->exit.id] && (v->tag<16 || enabled[world.rooms[a->exit.room].openings[v->second].exit.id]);
}
const char *campaign_surface(unsigned face){return campaign_room_surface(world.current,face);}
const char *campaign_room_surface(unsigned room,unsigned face){
    if(remote_guest_face(face)){
        room=world.prop_bounds[guest_face_prop(face)].room;face&=GUEST_FACE_MASK;
    }else if(room==world.current)for(unsigned i=0;i<world.guest_count;i++){
        const GuestSolid *g=world.guest_solids+i;
        if(face>=g->first && face-g->first<g->solid->count){room=g->owner;face=g->solid->first+face-g->first;break;}
    }
    if(room>=world.h.rooms)return "default";
    StageRoom *r=world.rooms+room;
    unsigned m=face<r->faces?(r->materials[face]&15):0;
    return surface_names[m<sizeof surface_names/sizeof *surface_names?m:0];
}
void campaign_visit_surfaces(void (*visit)(const char *material)){
    visit(surface_names[0]);
    for(unsigned i=1;i<sizeof surface_names/sizeof *surface_names;i++)
        if(world.surface_mask&(1u<<i))visit(surface_names[i]);
}
void campaign_add_light(IncidentLight *s,Point target,Point source,float radius,Point rgb){
    Point delta={source.x-target.x,source.y-target.y,source.z-target.z};
    float squared=delta.x*delta.x+delta.y*delta.y+delta.z*delta.z;
    if(squared>=radius*radius || radius<=0)return;
    float distance=sqrtf(squared),attenuation=1.f-distance/radius;
    attenuation*=attenuation;
    s->amount.x+=rgb.x*attenuation;s->amount.y+=rgb.y*attenuation;s->amount.z+=rgb.z*attenuation;
    float weight=(rgb.x*.299f+rgb.y*.587f+rgb.z*.114f)*attenuation;
    float scale=weight/fmaxf(.02f,distance);
    s->vector.x+=delta.x*scale;s->vector.y+=delta.y*scale;s->vector.z+=delta.z*scale;s->weight+=weight;
}
static void room_incident(const StageRoom *r,Point target,IncidentLight *out){
    IncidentLight result={0};
    for(unsigned i=0;i<r->lamp_count;i++){
        const MapLight *l=r->lamps+i;campaign_add_light(&result,target,l->position,l->radius,l->rgb);
    }
    for(unsigned i=0;i<r->cone_count;i++){
        const ConeLight *l=r->cones+i;
        Point d={target.x-l->position.x,target.y-l->position.y,target.z-l->position.z};
        float sq=d.x*d.x+d.y*d.y+d.z*d.z;
        if(sq>=l->radius*l->radius)continue;
        float cos_angle=(d.x*l->axis.x+d.y*l->axis.y+d.z*l->axis.z)/sqrtf(fmaxf(.000001f,sq));
        float gain=fminf(1,(cos_angle-l->outer_cos)/fmaxf(.001f,l->inner_cos-l->outer_cos));
        if(gain>0)campaign_add_light(&result,target,l->position,l->radius,(Point){gain*l->rgb.x,gain*l->rgb.y,gain*l->rgb.z});
    }
    *out=result;
}
static bool sound_opening(unsigned room,Aperture *a,uint64_t now);
IncidentLight campaign_incident(unsigned room,Point target){
    IncidentLight result={0};
    if(world.file && room>=world.h.rooms)return result;
    StageRoom *r=world.file?world.rooms+room:&training_light;
    room_incident(r,target,&result);
    if(!world.file)return result;
    for(unsigned i=0;i<r->info.exits;i++){
        Aperture *a=r->openings+i;
        if(!director.exits[a->exit.id])continue;
        StageRoom *other=world.rooms+a->exit.room;
        Point remote=apply(a->exit.transform,target);IncidentLight incoming={0};
        room_incident(other,remote,&incoming);
        if(incoming.weight<=0)continue;
        // Range-check before consulting the shared physical-door cache.
        if(!sound_opening(room,a,svcGetSystemTick()) || !a->sound_center_open)continue;
        for(unsigned kind=0;kind<2;kind++)for(unsigned j=0;j<(kind?other->cone_count:other->lamp_count);j++){
            Point source=kind?other->cones[j].position:other->lamps[j].position;
            C3D_FVec v=Mtx_MultiplyFVecH(&a->inverse,FVec3_New(source.x,source.y,source.z));
            Point local={v.x,v.y,v.z},delta={local.x-target.x,local.y-target.y,local.z-target.z};
            float distance=sqrtf(delta.x*delta.x+delta.y*delta.y+delta.z*delta.z);
            float radius=kind?other->cones[j].radius:other->lamps[j].radius;
            if(distance>=radius || distance<.0001f)continue;
            Point dir={delta.x/distance,delta.y/distance,delta.z/distance};
            float boundary=distance;CampaignExit crossed;
            if(!campaign_ray_exit(room,target,dir,&boundary,director.exits,&crossed) || crossed.id!=a->exit.id)continue;
            Point rgb=kind?other->cones[j].rgb:other->lamps[j].rgb;
            if(kind){
                const ConeLight *l=other->cones+j;
                Point d={remote.x-source.x,remote.y-source.y,remote.z-source.z};
                float cosine=(d.x*l->axis.x+d.y*l->axis.y+d.z*l->axis.z)/distance;
                float gain=fminf(1,fmaxf(0,(cosine-l->outer_cos)/fmaxf(.001f,l->inner_cos-l->outer_cos)));
                rgb.x*=gain;rgb.y*=gain;rgb.z*=gain;
            }
            campaign_add_light(&result,target,local,radius,rgb);
        }
    }
    return result;
}
uint32_t campaign_light(unsigned room,Point pos){
    const StageRoom *r=world.file?world.rooms+room:&training_light;
    float x=fminf(3,fmaxf(0,(pos.x-r->grid[0])*r->grid[2])),z=fminf(3,fmaxf(0,(pos.z-r->grid[1])*r->grid[3]));
    unsigned ix=(unsigned)x,iz=(unsigned)z,jx=ix<3?ix+1:ix,jz=iz<3?iz+1:iz;
    const uint8_t *a=r->light[iz*4+ix],*b=r->light[iz*4+jx],*c=r->light[jz*4+ix],*d=r->light[jz*4+jx];
    x-=ix;z-=iz;uint32_t color=0xff000000;
    for(unsigned i=0;i<3;i++){
        float above=a[i]+(b[i]-a[i])*x,below=c[i]+(d[i]-c[i])*x;
        color|=(uint32_t)(above+(below-above)*z)<<(8*i);
    }
    return color;
}
static void matrix(C3D_Mtx *out,const float *m) {
    Mtx_Identity(out);
    for(unsigned i=0;i<3;i++) out->r[i]=FVec4_New(m[i],m[3+i],m[6+i],m[9+i]);
}
// Determine which neighbouring room also contains a moving mesh. Use the
// entrance in the receiving room so multiple exits to one room stay distinct.
static bool through_exit(const Aperture *a,const C3D_Mtx *from,RayBounds home,const float *motion){
    C3D_FVec center=FVec3_New((home.lo.x+home.hi.x)*.5f,(home.lo.y+home.hi.y)*.5f,(home.lo.z+home.hi.z)*.5f);
    C3D_Mtx moved,local;matrix(&local,motion);Mtx_Multiply(&moved,from,&local);
    C3D_FVec start=Mtx_MultiplyFVecH(from,center),end=Mtx_MultiplyFVecH(&moved,center);
    float half[3]={(home.hi.x-home.lo.x)*.5f,(home.hi.y-home.lo.y)*.5f,(home.hi.z-home.lo.z)*.5f};
    Point n=a->exit.normal,o=a->verts[0];
    float d0=(start.x-o.x)*n.x+(start.y-o.y)*n.y+(start.z-o.z)*n.z;
    float d1=(end.x-o.x)*n.x+(end.y-o.y)*n.y+(end.z-o.z)*n.z;
    float t=fabsf(d1-d0)>.0001f?fminf(1,fmaxf(0,-d0/(d1-d0))):1;
    C3D_FVec hit=FVec3_Add(start,FVec3_Scale(FVec3_Subtract(end,start),t));
    float extent[3];
    for(unsigned i=0;i<3;i++){
        C3D_FVec r=moved.r[i],h=from->r[i];
        extent[i]=fmaxf(fabsf(r.x)*half[0]+fabsf(r.y)*half[1]+fabsf(r.z)*half[2],
                        fabsf(h.x)*half[0]+fabsf(h.y)*half[1]+fabsf(h.z)*half[2]);
    }
    float radius=fabsf(n.x)*extent[0]+fabsf(n.y)*extent[1]+fabsf(n.z)*extent[2];
    // Portal normals point into the source room. Trace the swept hull out through it.
    return d0>=-radius && d1<=radius &&
        hit.x+extent[0]>=a->box.lo.x && hit.x-extent[0]<=a->box.hi.x &&
        hit.y+extent[1]>=a->box.lo.y && hit.y-extent[1]<=a->box.hi.y &&
        hit.z+extent[2]>=a->box.lo.z && hit.z-extent[2]<=a->box.hi.z;
}
static bool set_contains(const PropSet *set,unsigned id){return (set->bits[id/32]>>(id%32))&1u;}
static void affine(float out[12],const C3D_Mtx *m){
    for(unsigned i=0;i<3;i++){
        out[i]=m->r[i].x;out[3+i]=m->r[i].y;out[6+i]=m->r[i].z;out[9+i]=m->r[i].w;
    }
}
static bool join_guest_collision(void){
    StageRoom *r=world.rooms+world.current;
    unsigned total=r->faces;world.guest_count=0;
    for(const GuestMesh *g=r->guests;g;g=g->next){
        const StageRoom *home=world.rooms+g->owner;
        for(unsigned i=0;i<home->info.solids;i++){
            const Solid *s=home->solids+i;
            if(s->prop<0 || !set_contains(&g->props,s->prop) || !prop_channels(campaign_props+s->prop))continue;
            if(world.guest_count>=CAMPAIGN_PROPS)return false;
            GuestSolid *to=world.guest_solids+world.guest_count++;
            *to=(GuestSolid){.solid=s,.owner=g->owner,.first=total};affine(to->placement,&g->placement);
            total+=s->count;
        }
    }
    if(!world.guest_count)return true;
    if(total>world.joined_capacity){
        // Leave the small normal heap to poses, scripts and cast models. The
        // linear arena has room for this contiguous CPU collision cache too.
        unsigned capacity=(total+1023u)&~1023u;
        Triangle *triangles=linearAlloc(capacity*sizeof *triangles);
        uint8_t *masks=linearAlloc(capacity);
        SceneBounds *blocks=linearAlloc(((capacity+15)/16)*sizeof *blocks);
        if(!triangles || !masks || !blocks){
            if(triangles)linearFree(triangles);
            if(masks)linearFree(masks);
            if(blocks)linearFree(blocks);
            return false;
        }
        if(world.joined)linearFree(world.joined);
        if(world.joined_masks)linearFree(world.joined_masks);
        if(world.joined_blocks)linearFree(world.joined_blocks);
        world.joined=triangles;world.joined_masks=masks;world.joined_blocks=blocks;
        world.joined_capacity=capacity;
    }
    memcpy(world.joined,world.collision,r->faces*sizeof(Triangle));memcpy(world.joined_masks,r->masks_live,r->faces);
    for(unsigned i=0;i<world.guest_count;i++){
        const GuestSolid *g=world.guest_solids+i;const Solid *s=g->solid;
        const CampaignProp *p=campaign_props+s->prop;const StageRoom *home=world.rooms+g->owner;
        C3D_Mtx placed,local,combined;matrix(&placed,g->placement);
        if(p->moved){matrix(&local,p->matrix);Mtx_Multiply(&combined,&placed,&local);}else combined=placed;
        float m[12];affine(m,&combined);
        bool level=fabsf(m[1])+fabsf(m[3])+fabsf(m[5])+fabsf(m[7])+fabsf(m[4]-1)<.0001f;
        unsigned mask=prop_channels(p);
        for(unsigned j=0;j<s->count;j++){
            const Triangle *t=s->original+j;
            Point a=apply(m,t->a),b=apply(m,t->b),c=apply(m,t->c);float slope=t->normal_y;
            if(!level){
                Point u={b.x-a.x,b.y-a.y,b.z-a.z},v={c.x-a.x,c.y-a.y,c.z-a.z};
                float nx=u.y*v.z-u.z*v.y,ny=u.z*v.x-u.x*v.z,nz=u.x*v.y-u.y*v.x;
                slope=fabsf(ny)/fmaxf(.000001f,sqrtf(nx*nx+ny*ny+nz*nz));
            }
            world.joined[g->first+j]=(Triangle){a,b,c,slope};
            world.joined_masks[g->first+j]=(home->materials[s->first+j]>>4)&mask;
        }
    }
    unsigned full_blocks=r->faces/16;
    memcpy(world.joined_blocks,r->live_blocks,full_blocks*sizeof(SceneBounds));
    scene_bind(world.joined,total,world.joined_masks,world.joined_blocks);
    scene_refit(full_blocks*16,total-full_blocks*16);return true;
}
static void set_membership(PropSet *set,unsigned id,bool present){
    if(set_contains(set,id)==present)return;
    set->bits[id/32]^=1u<<(id%32);
    if(present)set->count++;else set->count--;
}
void campaign_update_moving_rooms(void){
    for(unsigned id=0;id<world.h.props;id++){
        CampaignProp *p=campaign_props+id;
        if(!world.prop_bounds[id].used || (world.prop_bounds[id].placed && world.prop_bounds[id].moved==p->moved &&
           (!p->moved || !memcmp(world.prop_bounds[id].matrix,p->matrix,sizeof p->matrix))))continue;
        world.prop_bounds[id].placed=true;world.prop_bounds[id].moved=p->moved;memcpy(world.prop_bounds[id].matrix,p->matrix,sizeof p->matrix);
        unsigned home=world.prop_bounds[id].room;
        // Each portal maps between the coordinate frames of its two rooms. A train can
        // cross several before its authored mesh changes owner (or without changing owner).
        static struct {unsigned room;C3D_Mtx from;} route[128];
        bool visited[128]={0};
        unsigned tail=1;
        visited[home]=true;route[0].room=home;Mtx_Identity(&route[0].from);
        static const float identity[12]={1,0,0,0,1,0,0,0,1,0,0,0};
        const float *motion=p->moved?p->matrix:identity;
        for(unsigned q=0;q<tail;q++){
            StageRoom *src=world.rooms+route[q].room;
            for(unsigned k=0;k<src->info.exits;k++){
                Aperture *a=src->openings+k;unsigned dest=a->exit.room;
                if(visited[dest] || !through_exit(a,&route[q].from,world.prop_bounds[id].box,motion))continue;
                C3D_Mtx next;matrix(&next,a->exit.transform);
                route[tail].room=dest;Mtx_Multiply(&route[tail].from,&next,&route[q].from);
                GuestMesh **ptr=&world.rooms[dest].guests;
                while(*ptr && (*ptr)->owner!=home)ptr=&(*ptr)->next;
                if(!*ptr){
                    *ptr=calloc(1,sizeof(GuestMesh));if(!*ptr)continue;
                    (*ptr)->owner=home;(*ptr)->placement=route[tail].from;
                }
                set_membership(&(*ptr)->props,id,true);
                visited[dest]=true;tail++;
            }
        }
        for(unsigned r=0;r<world.h.rooms;r++)if(!visited[r]){
            for(GuestMesh *g=world.rooms[r].guests;g;g=g->next)
                if(g->owner==home)set_membership(&g->props,id,false);
        }
    }
}
static bool outside_view(const RayBounds *bounds,const C3D_FVec planes[4]) {
    for(unsigned i=0;i<4;i++){
        C3D_FVec plane=planes[i];
        float x=plane.x>=0?bounds->hi.x:bounds->lo.x;
        float y=plane.y>=0?bounds->hi.y:bounds->lo.y;
        float z=plane.z>=0?bounds->hi.z:bounds->lo.z;
        if(plane.x*x+plane.y*y+plane.z*z+plane.w<-.01f)return true;
    }
    return false;
}
typedef struct {float l,r,b,t;} ScreenRect;
static ScreenRect eye_rects[256];
static bool eye_views[256];
const C3D_Mtx *campaign_visible_transform(unsigned view){
    const PortalView *v=world.views+view;
    return eye_views[v->tag]?&v->inverse:NULL;
}
static const ScreenRect full_view={-1,1,-1,1};

static float decal_bias(float depth,float slope){
    const float rounding=fmaxf(8.f/16777215.f,depth*(4.f/65536.f));
    const float slope_limit=fmaxf(rounding,depth/1024.f);
    return rounding+fminf(.25f*slope,slope_limit);
}

static float decal_depth_offset(const C3D_Mtx *clip,const Vertex *verts,unsigned count){
    float bias=decal_bias(0,0);
    for(unsigned i=0;i<count;i+=3){
        C3D_FVec p[3];
        float depth=0;
        bool projectable=true;
        for(unsigned j=0;j<3;j++){
            Point v=verts[i+j].pos;
            p[j]=Mtx_MultiplyFVecH(clip,FVec3_New(v.x,v.y,v.z));
            if(p[j].w<=.001f){projectable=false;break;}
            p[j]=FVec4_Scale(p[j],1/p[j].w);
            depth=fmaxf(depth,fminf(1,-p[j].z));
        }
        if(!projectable){bias=fmaxf(bias,decal_bias(1,0));continue;}
        C3D_FVec a=FVec4_Subtract(p[1],p[0]),b=FVec4_Subtract(p[2],p[0]);
        float area=fabsf(a.x*b.y-a.y*b.x);
        float slope=0;
        if(area>1e-9f){
            // The PICA render target is rotated, 240 pixels along X, 400 along Y.
            float dx=fabsf(a.z*b.y-a.y*b.z)/(area*120.f);
            float dy=fabsf(a.x*b.z-a.z*b.x)/(area*200.f);
            slope=dx+dy;
        }
        bias=fmaxf(bias,decal_bias(depth,slope));
    }
    return bias;
}

static void draw_layer(const Mesh *mesh,int loc,const C3D_Mtx *view,unsigned mode,const ScreenRect *rect,const PropSet *visitors) {
    if(!(mesh->modes&(1u<<mode)))return;
    C3D_FVec planes[4];
    if(mesh->bounds){
        planes[0]=FVec4_Subtract(view->r[0],FVec4_Scale(view->r[3],rect->l));
        planes[1]=FVec4_Subtract(FVec4_Scale(view->r[3],rect->r),view->r[0]);
        planes[2]=FVec4_Subtract(view->r[1],FVec4_Scale(view->r[3],rect->b));
        planes[3]=FVec4_Subtract(FVec4_Scale(view->r[3],rect->t),view->r[1]);
    }
    C3D_SetBufInfo((C3D_BufInfo*)&mesh->binding);
    int last=-2,alpha=-1;unsigned start=0;bool cull_back=false;
    for(unsigned i=0;i<mesh->count;i++) {
        Batch b=mesh->batches[i];unsigned at=start;start+=b.count;
        bool solid_core=mode==0 && (b.base&BLEND_SOLID_CORE);
        if(((b.light>>14)!=mode && !solid_core) || (visitors && (b.prop<0 || !set_contains(visitors,b.prop))))continue;
        bool front_only=b.light&CULL_BACK_FACES;
        b.light&=TEXTURE_INDEX;
        CampaignProp *prop=b.prop<0?NULL:&campaign_props[b.prop];
        if(prop && (prop->hidden || director_camera_overlay(b.prop))) continue;
        int transform=prop && prop->moved?b.prop:-1;
        if(transform<0 && mesh->bounds && outside_view(mesh->bounds+i,planes))continue;
        if(mode){
            C3D_Mtx placed;const C3D_Mtx *matrix_ptr=view;
            if(transform>=0){C3D_Mtx m;matrix(&m,prop->matrix);Mtx_Multiply(&placed,view,&m);matrix_ptr=&placed;}
            const RayBounds *box=mesh->bounds+i;
            Point center={(box->lo.x+box->hi.x)*.5f,(box->lo.y+box->hi.y)*.5f,(box->lo.z+box->hi.z)*.5f};
            C3D_Tex *diffuse=world.textures+(b.base&TEXTURE_INDEX);
            C3D_Tex *light=world.textures+(b.light?b.light:1);
            if(mode==1 && (b.base&DEPTH_DECAL)){
                float bias=decal_depth_offset(matrix_ptr,mesh->vertices+at,b.count);
                alpha_decal(matrix_ptr,center,world.blend_view,(C3D_BufInfo*)&mesh->binding,at,b.count,diffuse,light,bias);
            }else{
                AlphaStyle style=mode==2?ALPHA_BEAM:mode==3?ALPHA_BEACON:ALPHA_WORLD;
                if(mode==1 && (b.base&BLEND_SOLID_CORE))style=front_only?ALPHA_WORLD_DEPTH_CULLED:ALPHA_WORLD_DEPTH;
                alpha_submit(style,matrix_ptr,center,world.blend_view,(C3D_BufInfo*)&mesh->binding,at,b.count,
                             diffuse,light,mode==3?world.beacon_color:0);
            }
            continue;
        }
        if(transform!=last) {
            if(transform<0) C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER,loc,view);
            else {C3D_Mtx m,combined;matrix(&m,prop->matrix);Mtx_Multiply(&combined,view,&m);
                C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER,loc,&combined);}
            last=transform;
        }
        if(front_only!=cull_back){
            C3D_CullFace(front_only?GPU_CULL_FRONT_CCW:GPU_CULL_NONE);
            cull_back=front_only;
        }
        int cutoff=solid_core?254:(b.base&ALPHA_TEST_127)?127:15;
        if(cutoff!=alpha){C3D_AlphaTest(true,GPU_GREATER,cutoff);alpha=cutoff;}
        C3D_Tex *diffuse=world.textures+(b.base&TEXTURE_INDEX);
        if(mesh==&world.sky){
            // A solid backdrop covers the color buffer directly. Keep blending
            // for sky textures that include a transparent skyline.
            if(diffuse->fmt==GPU_RGB8 || diffuse->fmt==GPU_RGB565)C3D_ColorLogicOp(GPU_LOGICOP_COPY);
            else C3D_AlphaBlend(GPU_BLEND_ADD,GPU_BLEND_ADD,GPU_SRC_ALPHA,GPU_ONE_MINUS_SRC_ALPHA,GPU_ONE,GPU_ZERO);
        }
        C3D_TexBind(0,diffuse);C3D_TexBind(1,&world.textures[b.light?b.light:1]);
        C3D_DrawArrays(GPU_TRIANGLES,at,b.count);
    }
    if(cull_back)C3D_CullFace(GPU_CULL_NONE);
    if(alpha!=-1)C3D_AlphaTest(true,GPU_GREATER,63);
}
void campaign_draw_camera_overlay(int loc,const C3D_Mtx *view){
    if(director.camera>=director.count)return;
    const Mesh *mesh=&world.rooms[world.current].geometry;
    C3D_Mtx root;Mtx_Multiply(&root,view,&world.views[0].inverse);
    C3D_DepthTest(false,GPU_ALWAYS,GPU_WRITE_COLOR);
    C3D_StencilTest(false,GPU_ALWAYS,0,0xff,0);
    C3D_AlphaTest(true,GPU_GREATER,0);
    C3D_AlphaBlend(GPU_BLEND_ADD,GPU_BLEND_ADD,GPU_SRC_ALPHA,GPU_ONE_MINUS_SRC_ALPHA,GPU_ONE,GPU_ZERO);
    C3D_SetBufInfo((C3D_BufInfo*)&mesh->binding);
    unsigned start=0;
    for(unsigned i=0;i<mesh->count;i++){
        const Batch *b=mesh->batches+i;unsigned at=start;start+=b->count;
        if(b->prop<0 || campaign_props[b->prop].hidden || !director_camera_overlay(b->prop))continue;
        const CampaignProp *prop=campaign_props+b->prop;
        C3D_Mtx placed=root;
        if(prop->moved){C3D_Mtx m;matrix(&m,prop->matrix);Mtx_Multiply(&placed,&root,&m);}
        C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER,loc,&placed);
        unsigned light=b->light&TEXTURE_INDEX;
        C3D_TexBind(0,world.textures+(b->base&TEXTURE_INDEX));C3D_TexBind(1,world.textures+(light?light:1));
        C3D_DrawArrays(GPU_TRIANGLES,at,b->count);
    }
    C3D_AlphaBlend(GPU_BLEND_ADD,GPU_BLEND_ADD,GPU_ONE,GPU_ZERO,GPU_ONE,GPU_ZERO);
    C3D_AlphaTest(true,GPU_GREATER,63);
    C3D_DepthTest(true,GPU_GEQUAL,GPU_WRITE_ALL);
    campaign_stencil(0);
}
static void draw_room(const StageRoom *room,int loc,const C3D_Mtx *view,unsigned mode,const ScreenRect *rect){
    draw_layer(&room->geometry,loc,view,mode,rect,NULL);
    for(const GuestMesh *g=room->guests;g;g=g->next){
        if(!g->props.count)continue;
        C3D_Mtx placed;Mtx_Multiply(&placed,view,&g->placement);
        draw_layer(&world.rooms[g->owner].geometry,loc,&placed,mode,rect,&g->props);
    }
}
void campaign_queue_translucent(int loc,const C3D_Mtx *view,const bool *enabled){
    for(unsigned mode=1;mode<=3;mode++){
        if(!(world.draw_modes&(1u<<mode)))continue;
        for(unsigned i=0;i<world.view_count;i++){
            const PortalView *v=world.views+i;const StageRoom *r=world.rooms+v->room;
            if(!eye_views[v->tag] || !campaign_view_enabled(i,enabled))continue;
            bool any=r->geometry.modes&(1u<<mode);
            for(GuestMesh *g=r->guests;!any && g;g=g->next)any=g->props.count && (world.rooms[g->owner].geometry.modes&(1u<<mode));
            if(!any)continue;
            if(mode==3 && !(world.beacon_color=particles_police_color(v->room)))continue;
            world.blend_view=i;
            C3D_Mtx m;Mtx_Multiply(&m,view,&v->inverse);
            draw_room(r,loc,&m,mode,&eye_rects[v->tag]);
        }
    }
}
void draw_aperture(int uniform,const C3D_Mtx *view,unsigned first,unsigned count,bool crosses_near){
    C3D_DepthTest(true,GPU_GEQUAL,0);
    C3D_DrawArrays(GPU_TRIANGLES,first,count);
    if(!crosses_near)return;
    // The world clips at z = -w. Mark the rest of the opening between that
    // plane and the eye without changing its projected outline or writing depth.
    C3D_Mtx near=*view;
    near.r[2]=FVec4_Scale(FVec4_Add(view->r[2],view->r[3]),.0001f);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER,uniform,&near);
    C3D_DepthTest(true,GPU_ALWAYS,0);
    C3D_DrawArrays(GPU_TRIANGLES,first,count);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER,uniform,view);
}
static bool portal_mask(const StageRoom *room,const Aperture *a,int loc,const C3D_Mtx *view,Point eye,unsigned tag,unsigned parent_mask,
                        const ScreenRect *parent,ScreenRect *clip){
    unsigned outside=31;bool crosses_eye=false;ScreenRect box={1,-1,1,-1};
    Point n=a->exit.normal,p0=a->verts[0];
    float distance=(eye.x-p0.x)*n.x+(eye.y-p0.y)*n.y+(eye.z-p0.z)*n.z;
    // On a stereo threshold one eye can already be inside the adjoining room.
    // Its portal is behind that eye. The parent stencil and opaque world depth
    // still apply while rendering the room occupied by that eye.
    bool inside=distance<=.0001f && distance>=-.07f &&
        portal_contains(a->verts,a->indices,a->count,n,
                        (Point){eye.x-distance*n.x,eye.y-distance*n.y,eye.z-distance*n.z});
    for(unsigned j=0;j<a->exit.points;j++){
        Point p=a->verts[j];C3D_FVec v=Mtx_MultiplyFVecH(view,FVec3_New(p.x,p.y,p.z));
        outside&=(v.x<parent->l*v.w)|((v.x>parent->r*v.w)<<1)|((v.y<parent->b*v.w)<<2)|((v.y>parent->t*v.w)<<3)|((v.w<=0)<<4);
        if(v.z<=-v.w || v.w<=0){crosses_eye=true;continue;}
        float inv=1/v.w,x=v.x*inv,y=v.y*inv;
        box.l=fminf(box.l,x);box.r=fmaxf(box.r,x);box.b=fminf(box.b,y);box.t=fmaxf(box.t,y);
    }
    if(outside && !inside)return false;
    // If the eye cuts the polygon, projection can be unbounded on either side.
    *clip=inside || crosses_eye?*parent:(ScreenRect){fmaxf(parent->l,box.l-.005f),fminf(parent->r,box.r+.005f),
                                          fmaxf(parent->b,box.b-.005f),fminf(parent->t,box.t+.005f)};
    if(clip->l>clip->r || clip->b>clip->t)return false;
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER,loc,view);C3D_SetBufInfo((C3D_BufInfo*)&room->masks.binding);
    C3D_TexBind(0,world.textures);C3D_TexBind(1,world.textures);
    C3D_StencilTest(true,parent_mask?GPU_EQUAL:GPU_ALWAYS,tag,parent_mask,parent_mask?0xf0:0xff);
    C3D_StencilOp(GPU_STENCIL_KEEP,GPU_STENCIL_KEEP,GPU_STENCIL_REPLACE);
    // Keep the mask on the correct side of quantization for each eye.
    C3D_FVec center=Mtx_MultiplyFVecH(view,FVec3_New(a->verts[0].x,a->verts[0].y,a->verts[0].z));
    C3D_DepthMap(true,-1,depth_surface_bias(center.z,center.w));
    if(inside){
        C3D_Mtx screen;Mtx_Identity(&screen);C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER,loc,&screen);
        C3D_DepthTest(true,GPU_ALWAYS,0);
        C3D_DrawArrays(GPU_TRIANGLES,room->masks.total-3,3);
    }else draw_aperture(loc,view,a->start,a->count,crosses_eye);
    C3D_DepthMap(true,-1,0);
    C3D_StencilTest(true,GPU_EQUAL,tag,0xff,0);C3D_StencilOp(GPU_STENCIL_KEEP,GPU_STENCIL_KEEP,GPU_STENCIL_KEEP);
    C3D_DepthTest(true,GPU_GEQUAL,GPU_WRITE_ALL);return true;
}
void campaign_draw(int loc,const C3D_Mtx *view,const C3D_Mtx *sky,const bool *enabled) {
    C3D_StencilTest(false,GPU_ALWAYS,0,255,0);
    C3D_StencilOp(GPU_STENCIL_KEEP,GPU_STENCIL_KEEP,GPU_STENCIL_KEEP);
    C3D_DepthMap(true,-1,0);
    C3D_CullFace(GPU_CULL_NONE);
    if(world.sky.count){
        // KF2 orders the opaque sky before its skyline silhouettes. Preserve their filtered edges.
        C3D_DepthTest(false,GPU_ALWAYS,GPU_WRITE_COLOR);
        draw_layer(&world.sky,loc,sky,0,&full_view,NULL);
        C3D_AlphaBlend(GPU_BLEND_ADD,GPU_BLEND_ADD,GPU_ONE,GPU_ZERO,GPU_ONE,GPU_ZERO);
    }
    memset(eye_views,0,sizeof eye_views);eye_views[0]=true;eye_rects[0]=full_view;
    StageRoom *current=world.rooms+world.views[0].room;
    C3D_Mtx root;Mtx_Multiply(&root,view,&world.views[0].inverse);view=&root;
    C3D_Mtx inverse=root;Mtx_Inverse(&inverse);
    C3D_FVec origin=Mtx_MultiplyFVec4(&inverse,FVec4_New(0,0,1,0));
    Point eye={origin.x/origin.w,origin.y/origin.w,origin.z/origin.w};
    C3D_DepthTest(true,GPU_GEQUAL,GPU_WRITE_ALL);draw_room(current,loc,view,0,&full_view);
    for(unsigned i=0;i<current->info.exits;i++){
        const Aperture *a=current->openings+i;unsigned tag=i+1;ScreenRect first;
        if((enabled && !enabled[a->exit.id]) || !portal_mask(current,a,loc,view,eye,tag,0,&full_view,&first))continue;
        eye_views[tag]=true;eye_rects[tag]=first;
        C3D_Mtx parent;Mtx_Multiply(&parent,view,&a->inverse);
        Point next_eye=apply(a->exit.transform,eye);
        StageRoom *next=world.rooms+a->exit.room;draw_room(next,loc,&parent,0,&first);
        for(unsigned j=0;j<next->info.exits;j++){
            const Aperture *b=next->openings+j;
            if(b->exit.room==world.views[0].room || b->exit.room==a->exit.room || (enabled && !enabled[b->exit.id]))continue;
            ScreenRect second;
            if(!portal_mask(next,b,loc,&parent,next_eye,tag|((j+1)<<4),15,&first,&second))continue;
            unsigned child_tag=tag|((j+1)<<4);eye_views[child_tag]=true;eye_rects[child_tag]=second;
            C3D_Mtx child;Mtx_Multiply(&child,&parent,&b->inverse);
            draw_room(world.rooms+b->exit.room,loc,&child,0,&second);
        }
    }
    C3D_StencilTest(false,GPU_ALWAYS,0,0xff,0xff);
}
Point campaign_from_room_neighbour(unsigned room,unsigned slot,Point position){
    C3D_FVec p=Mtx_MultiplyFVecH(&world.rooms[room].openings[slot].inverse,FVec3_New(position.x,position.y,position.z));
    return (Point){p.x,p.y,p.z};
}
Point campaign_from_neighbour(unsigned slot,Point position){
    return campaign_from_room_neighbour(world.current,slot,position);
}
bool campaign_placement(unsigned room,unsigned portal,Player *body) {
    if(!portal && world.camera_path[0]==UINT32_MAX)return room==world.current;
    if(portal>=world.view_count || world.views[portal].room!=room)return false;
    const PortalView *v=world.views+portal;
    C3D_FVec p=Mtx_MultiplyFVecH(&v->inverse,FVec3_New(body->position.x,body->position.y,body->position.z));
    body->position=(Point){p.x,p.y,p.z};body->yaw+=v->yaw;return true;
}
static unsigned cinematic_rider_view(unsigned view,Point feet){
    unsigned room=world.views[view].room;
    PlatformContact support=campaign_remote_floor(room,feet);
    if(support.prop<0 || !campaign_props[support.prop].moved)return view;
    // Scripts keep riders in their authored room. Once the floor under a
    // rider has crossed an aperture, use the receiving room's stencil.
    Point start=support.local;start.y+=.65f;feet.y+=.65f;
    while(view){
        const PortalView *v=world.views+view;
        unsigned parent=v->tag<16?0:v->first+1;
        unsigned slot=parent?v->second:v->first;
        unsigned parent_room=world.views[parent].room;
        const Aperture *a=world.rooms[parent_room].openings+slot;
        Point from=campaign_from_room_neighbour(parent_room,slot,start);
        Point to=campaign_from_room_neighbour(parent_room,slot,feet);
        Point n=a->exit.normal,o=a->verts[0];
        float before=(from.x-o.x)*n.x+(from.y-o.y)*n.y+(from.z-o.z)*n.z;
        float after=(to.x-o.x)*n.x+(to.y-o.y)*n.y+(to.z-o.z)*n.z;
        if(before>=0 || after<=0)break;
        float t=before/(before-after);
        Point crossing={from.x+(to.x-from.x)*t,from.y+(to.y-from.y)*t,from.z+(to.z-from.z)*t};
        if(!portal_contains(a->verts,a->indices,a->count,n,crossing))break;
        view=parent;start=from;feet=to;
    }
    return view;
}
unsigned campaign_body_view(unsigned view,Point feet,bool crouched){
    if(!view || view>=world.view_count)return view;
    if(!director.controls){
        unsigned rider=cinematic_rider_view(view,feet);
        if(rider!=view)return rider;
    }
    const PortalView *v=world.views+view;
    unsigned parent=v->tag<16?0:v->first+1;
    const Aperture *a=world.rooms[world.views[parent].room].openings+(parent?v->second:v->first);
    C3D_FVec pos=Mtx_MultiplyFVecH(&a->inverse,FVec3_New(feet.x,feet.y,feet.z));
    Point n=a->exit.normal,o=a->verts[0];
    for(unsigned k=0;k<3;k++){
        Point sample={pos.x,pos.y+.2f+k*(crouched?.35f:.62f),pos.z};
        float d=(sample.x-o.x)*n.x+(sample.y-o.y)*n.y+(sample.z-o.z)*n.z;
        if(fabsf(d)<=.46f && portal_contains(a->verts,a->indices,a->count,n,
           (Point){sample.x-d*n.x,sample.y-d*n.y,sample.z-d*n.z}))return parent;
    }
    return view;
}
void campaign_stencil(unsigned portal) {
    unsigned tag=portal<world.view_count?world.views[portal].tag:0;
    C3D_StencilTest(tag!=0,GPU_EQUAL,tag,tag<16?15:255,0);
    C3D_StencilOp(GPU_STENCIL_KEEP,GPU_STENCIL_KEEP,GPU_STENCIL_KEEP);
}
static bool inside_aperture(const Aperture *a,Point at) {
    return portal_contains(a->verts,a->indices,a->count,a->exit.normal,at);
}

Point campaign_transform(const CampaignExit *exit,Point p,bool direction) {
    Point q=apply(exit->transform,p);
    if(direction){q.x-=exit->transform[9];q.y-=exit->transform[10];q.z-=exit->transform[11];}
    return q;
}
bool campaign_ray_exit(unsigned room,Point start,Point direction,float *limit,const bool *enabled,CampaignExit *out) {
    if(room>=world.h.rooms)return false;
    StageRoom *r=world.rooms+room;
    const Aperture *openings=r->openings;
    unsigned count=r->info.exits;bool found=false;
    for(unsigned i=0;i<count;i++){
        const Aperture *a=openings+i;Point n=a->exit.normal,o=a->verts[0];
        if(enabled && !enabled[a->exit.id])continue;
        float dot=n.x*direction.x+n.y*direction.y+n.z*direction.z;
        if(dot>=-.00001f)continue;
        float t=((o.x-start.x)*n.x+(o.y-start.y)*n.y+(o.z-start.z)*n.z)/dot;
        if(t<0 || t>=*limit)continue;
        Point at={start.x+t*direction.x,start.y+t*direction.y,start.z+t*direction.z};
        if(inside_aperture(a,at)){*limit=t;*out=a->exit;found=true;}
    }
    return found;
}
static bool ray_solids(const StageRoom *r,const PropSet *guests,Point start,Point dir,float limit,unsigned mask,Hit *hit,int *prop){
    bool found=false;Point normal={0};unsigned face_id=UINT32_MAX;
    Point end={start.x+limit*dir.x,start.y+limit*dir.y,start.z+limit*dir.z};
    Point lo={fminf(start.x,end.x),fminf(start.y,end.y),fminf(start.z,end.z)};
    Point hi={fmaxf(start.x,end.x),fmaxf(start.y,end.y),fmaxf(start.z,end.z)};
    for(unsigned i=0;i<r->info.solids;i++){
        const Solid *g=r->solids+i;
        if(guests && (g->prop<0 || !set_contains(guests,g->prop)))continue;
        const CampaignProp *p=g->prop<0?NULL:campaign_props+g->prop;
        if(p && (!(prop_channels(p)&mask) || !solid_overlaps(g,lo,hi)))continue;
        for(unsigned first=0;first<g->count;){
            unsigned face=g->first+first,n=16-face%16;
            if(n>g->count-first)n=g->count-first;
            const Triangle *t=g->original+first;first+=n;
            if(!p || !p->moved){
                const SceneBounds *b=r->blocks+face/16;
                if(hi.x<b->low.x || lo.x>b->high.x || hi.y<b->low.y || lo.y>b->high.y || hi.z<b->low.z || lo.z>b->high.z)continue;
            }
            for(unsigned k=0;k<n;k++){
                if(!((r->materials[face+k]>>4)&mask))continue;
                Triangle v=t[k];
                if(p && p->moved){v.a=apply(p->matrix,v.a);v.b=apply(p->matrix,v.b);v.c=apply(p->matrix,v.c);}
                if(scene_triangle(&v,start,dir,&limit,&normal)){found=true;face_id=face+k;*prop=g->prop;}
            }
        }
    }
    if(found){
        float scale=1.f/fmaxf(.000001f,hypotf(normal.x,hypotf(normal.y,normal.z)));
        if(normal.x*dir.x+normal.y*dir.y+normal.z*dir.z>0)scale=-scale;
        *hit=(Hit){{start.x+dir.x*limit,start.y+dir.y*limit,start.z+dir.z*limit},
                   {normal.x*scale,normal.y*scale,normal.z*scale},limit,face_id};
    }
    return found;
}
static void map_ray(const C3D_Mtx *matrix,Point *start,Point *dir){
    C3D_FVec a=Mtx_MultiplyFVecH(matrix,FVec3_New(start->x,start->y,start->z));
    C3D_FVec v=Mtx_MultiplyFVec4(matrix,FVec4_New(dir->x,dir->y,dir->z,0));
    *start=(Point){a.x,a.y,a.z};*dir=(Point){v.x,v.y,v.z};
}
static bool ray_geometry(unsigned room,Point start,Point dir,float limit,unsigned mask,Hit *hit,int *prop) {
    *prop=-1;
    if(room==world.current){
        if(!scene_trace_mask(start,dir,limit,mask,hit))return false;
        *prop=campaign_hit_prop(hit->face);return true;
    }
    if(room>=world.h.rooms)return false;
    const StageRoom *r=world.rooms+room;
    bool found=ray_solids(r,NULL,start,dir,limit,mask,hit,prop);
    if(found)limit=hit->distance;
    for(const GuestMesh *g=r->guests;g;g=g->next){
        if(!g->props.count)continue;
        C3D_Mtx inverse=g->placement;Mtx_Inverse(&inverse);
        Point local_start=start,local_dir=dir;
        map_ray(&inverse,&local_start,&local_dir);
        Hit local;int owner;
        if(!ray_solids(world.rooms+g->owner,&g->props,local_start,local_dir,limit,mask,&local,&owner))continue;
        Point n=local.normal;
        Point normal={inverse.r[0].x*n.x+inverse.r[1].x*n.y+inverse.r[2].x*n.z,
                      inverse.r[0].y*n.x+inverse.r[1].y*n.y+inverse.r[2].y*n.z,
                      inverse.r[0].z*n.x+inverse.r[1].z*n.y+inverse.r[2].z*n.z};
        float length=sqrtf(normal.x*normal.x+normal.y*normal.y+normal.z*normal.z);
        if(length<=.000001f)continue;
        limit=local.distance;*prop=owner;found=true;
        *hit=(Hit){{start.x+dir.x*limit,start.y+dir.y*limit,start.z+dir.z*limit},
                   {normal.x/length,normal.y/length,normal.z/length},limit,
                   GUEST_FACE_TAG|((unsigned)owner<<GUEST_FACE_SHIFT)|local.face};
    }
    return found;
}
// A penetrable prop can receive its script event while the damaging round continues.
static void pierce_solids(const StageRoom *r,const PropSet *guests,Point start,Point dir,float limit,
                          uint32_t *seen,unsigned owner,float damage){
    if(!r->pierceable)return;
    Point end={start.x+dir.x*limit,start.y+dir.y*limit,start.z+dir.z*limit};
    Point lo={fminf(start.x,end.x),fminf(start.y,end.y),fminf(start.z,end.z)};
    Point hi={fmaxf(start.x,end.x),fmaxf(start.y,end.y),fmaxf(start.z,end.z)};
    for(unsigned i=0;i<r->info.solids;i++){
        const Solid *g=r->solids+i;
        if(!g->pierceable || (guests && !set_contains(guests,g->prop)))continue;
        unsigned id=g->prop,bit=1u<<(id%32);
        const CampaignProp *p=campaign_props+id;
        if((seen[id/32]&bit) || !(prop_channels(p)&SCENE_PIERCE) || !solid_overlaps(g,lo,hi))continue;
        for(unsigned j=0;j<g->count;j++){
            if(!((r->materials[g->first+j]>>4)&SCENE_PIERCE))continue;
            Triangle face=g->original[j];Point normal;float distance=limit;
            if(p->moved){face.a=apply(p->matrix,face.a);face.b=apply(p->matrix,face.b);face.c=apply(p->matrix,face.c);}
            if(scene_triangle(&face,start,dir,&distance,&normal)){
                seen[id/32]|=bit;director_pierce_prop(id,owner,damage);break;
            }
        }
    }
}
void campaign_pierced_props(unsigned room,Point start,Point dir,float limit,uint32_t *seen,unsigned owner,float damage){
    if(room>=world.h.rooms)return;
    const StageRoom *r=world.rooms+room;
    pierce_solids(r,NULL,start,dir,limit,seen,owner,damage);
    for(const GuestMesh *g=r->guests;g;g=g->next){
        const StageRoom *home=world.rooms+g->owner;
        if(!g->props.count || !home->pierceable)continue;
        C3D_Mtx inverse=g->placement;Mtx_Inverse(&inverse);
        Point local_start=start,local_dir=dir;
        map_ray(&inverse,&local_start,&local_dir);
        pierce_solids(home,&g->props,local_start,local_dir,limit,seen,owner,damage);
    }
}
bool campaign_ray_projectile(unsigned room,Point start,Point dir,float limit,bool large,Hit *hit,int *prop){
    return ray_geometry(room,start,dir,limit,large?SCENE_BIG_SHOT:SCENE_SHOT,hit,prop);
}
bool campaign_ray_wall(unsigned room,Point start,Point dir,float limit,Hit *hit,int *prop){
    return ray_geometry(room,start,dir,limit,SCENE_BODY,hit,prop);
}
// Portals can remain enabled while an animated door still fills the opening.
// Probe a short segment on each side in that room's own coordinate system.
static bool door_panel(unsigned room,Point at,Point inward){
    const StageRoom *r=world.rooms+room;
    Point start={at.x-.45f*inward.x,at.y-.45f*inward.y,at.z-.45f*inward.z};
    for(unsigned i=0;i<r->info.solids;i++){
        const Solid *s=r->solids+i;
        if(!in_solid(s,at,.46f))continue;
        const CampaignProp *p=campaign_props+s->prop;
        if(p->collision_mask && !(p->collision_mask&SCENE_BODY))continue;
        for(unsigned j=0;j<s->count;j++){
            Triangle tri=s->original[j];
            if(p->moved){tri.a=apply(p->matrix,tri.a);tri.b=apply(p->matrix,tri.b);tri.c=apply(p->matrix,tri.c);}
            float length=.9f;Point normal;
            if(scene_triangle(&tri,start,inward,&length,&normal))return true;
        }
    }
    return false;
}
static bool sound_opening(unsigned room,Aperture *a,uint64_t now){
    uint32_t home=director.acoustic_revision[room],other_room=director.acoustic_revision[a->exit.room];
    if(a->sound_checked && (now-a->sound_checked<SYSCLOCK_ARM11/12 ||
       (a->sound_revision[0]==home && a->sound_revision[1]==other_room)))return a->sound_open;
    a->sound_revision[0]=home;a->sound_revision[1]=other_room;
    a->sound_checked=now;a->sound_center_open=a->sound_open=false;
    Point center={(a->box.lo.x+a->box.hi.x)*.5f,(a->box.lo.y+a->box.hi.y)*.5f,(a->box.lo.z+a->box.hi.z)*.5f};
    Point n=a->exit.normal,other=campaign_transform(&a->exit,n,true);
    // A partially opened or double door may leave only part of the aperture free.
    for(unsigned k=0;k<=a->count/3;k++){
        Point at=center;
        if(k){
            unsigned j=(k-1)*3;
            Point p=a->verts[a->indices[j]],q=a->verts[a->indices[j+1]],r=a->verts[a->indices[j+2]];
            at=(Point){(p.x+q.x+r.x)/3,(p.y+q.y+r.y)/3,(p.z+q.z+r.z)/3};
        }else if(!inside_aperture(a,at))continue;
        if(!door_panel(room,at,n) && !door_panel(a->exit.room,campaign_transform(&a->exit,at,false),other)){
            a->sound_opening=at;a->sound_center_open=k==0;a->sound_open=true;break;
        }
    }
    return a->sound_open;
}
static float sound_distance(Point a,Point b){
    float x=a.x-b.x,y=a.y-b.y,z=a.z-b.z;return sqrtf(x*x+y*y+z*z);
}
bool campaign_sound_path(unsigned room,Point listener,unsigned source_room,Point source,const bool *enabled,Point *apparent,float *distance){
    if(room>=world.h.rooms || source_room>=world.h.rooms || !world.rooms)return false;
    if(room==source_room){*apparent=source;*distance=sound_distance(listener,source);return true;}
    StageRoom *r=world.rooms+room;
    float best=INFINITY;
    for(unsigned i=0;i<r->info.exits;i++){
        Aperture *a=r->openings+i;
        if(a->exit.room!=source_room || !enabled[a->exit.id] || !sound_opening(room,a,svcGetSystemTick()))continue;
        C3D_FVec v=Mtx_MultiplyFVecH(&a->inverse,FVec3_New(source.x,source.y,source.z));
        Point pos={v.x,v.y,v.z},via=a->sound_opening;
        Point delta={pos.x-listener.x,pos.y-listener.y,pos.z-listener.z},n=a->exit.normal;
        float dot=n.x*delta.x+n.y*delta.y+n.z*delta.z;
        float path=sound_distance(listener,via)+sound_distance(via,pos);
        if(a->sound_center_open && fabsf(dot)>.00001f){
            Point o=a->verts[0];
            float t=(n.x*(o.x-listener.x)+n.y*(o.y-listener.y)+n.z*(o.z-listener.z))/dot;
            Point hit={listener.x+t*delta.x,listener.y+t*delta.y,listener.z+t*delta.z};
            if(t>=0 && t<=1 && inside_aperture(a,hit)){via=pos;path=sound_distance(listener,pos);}
        }
        if(path<best){best=path;*apparent=via;}
    }
    if(!isfinite(best))return false;
    *distance=best;return true;
}
bool campaign_camera_trace(Point start,Point direction,float length,const bool *enabled,Hit *hit){
    unsigned room=world.current;
    float travelled=0;
    for(unsigned depth=0;;depth++){
        float segment=length-travelled;
        CampaignExit exit;int prop;
        bool crossing=campaign_ray_exit(room,start,direction,&segment,NULL,&exit);
        if(ray_geometry(room,start,direction,segment,SCENE_BODY,hit,&prop)){
            hit->distance+=travelled;return true;
        }
        if(!crossing)return false;
        if(!enabled[exit.id] || depth==2){
            hit->distance=travelled+segment;return true;
        }
        segment=fminf(length-travelled,segment+.0002f);
        travelled+=segment;
        if(travelled>=length)return false;
        Point next={start.x+segment*direction.x,start.y+segment*direction.y,start.z+segment*direction.z};
        start=campaign_transform(&exit,next,false);
        direction=campaign_transform(&exit,direction,true);room=exit.room;
    }
}
static bool panel_path_clear(unsigned room,const Player *body,Point target){
    const StageRoom *r=world.rooms+room;
    Point from=body->position;
    Point lo={fminf(from.x,target.x)-.22f,fminf(from.y,target.y)+.03f,fminf(from.z,target.z)-.22f};
    Point hi={fmaxf(from.x,target.x)+.22f,fmaxf(from.y,target.y)+1.70f,fmaxf(from.z,target.z)+.22f};
    Player end=*body;end.position=target;
    for(unsigned i=0;i<r->info.solids;i++){
        const Solid *s=r->solids+i;
        if(s->prop<0 || !(prop_channels(campaign_props+s->prop)&SCENE_BODY) || !solid_overlaps(s,lo,hi))continue;
        const CampaignProp *p=campaign_props+s->prop;
        for(unsigned j=0;j<s->count;j++){
            if(!((r->materials[s->first+j]>>4)&SCENE_BODY))continue;
            Triangle t=s->original[j];
            if(p->moved){t.a=apply(p->matrix,t.a);t.b=apply(p->matrix,t.b);t.c=apply(p->matrix,t.c);}
            if(scene_panel_blocks(&t,body,target,false) || scene_panel_distance(&t,&end)<.22f*.22f-1.e-7f)return false;
        }
    }
    return true;
}
// An enabled BSP exit does not imply an open physical door. The AI graph
// can place its next node on the far side of a still-moving or closed panel.
bool campaign_npc_passage(uint16_t *id,Point *position,unsigned destination,Point target,
                          bool crouched,float stride,float *yaw,float *travel){
    unsigned room=*id;Point from=*position;
    if(room>=world.h.rooms || destination>=world.h.rooms)return false;
    const StageRoom *r=world.rooms+room;
    for(unsigned i=0;i<r->info.exits;i++){
        const Aperture *a=r->openings+i;
        if(a->exit.room!=destination || !director.exits[a->exit.id])continue;
        Point goal=campaign_from_room_neighbour(room,i,target),n=a->exit.normal,o=a->verts[0];
        Point start=from;start.y+=.70f;
        Point delta={goal.x-from.x,goal.y-from.y,goal.z-from.z};
        float dot=n.x*delta.x+n.y*delta.y+n.z*delta.z;
        float distance=(o.x-start.x)*n.x+(o.y-start.y)*n.y+(o.z-start.z)*n.z;
        float t=fabsf(dot)>.00001f?distance/dot:0;
        if((fabsf(dot)<=.00001f && fabsf(distance)>.05f) || t<-.001f || t>1.001f)continue;
        Point at={start.x+delta.x*t,start.y+delta.y*t,start.z+delta.z*t};
        if(!inside_aperture(a,at))continue;
        Player body={.position=from,.crouched=crouched};
        if(!panel_path_clear(room,&body,goal))continue;
        body.position=campaign_transform(&a->exit,from,false);
        if(!panel_path_clear(destination,&body,target))continue;
        float length=hypotf(delta.x,delta.z),step=fminf(1,stride/fmaxf(length,.001f));
        Point next={from.x+delta.x*step,from.y+delta.y*step,from.z+delta.z*step};
        *yaw=atan2f(delta.x,delta.z);
        *travel=length*step;
        if(step+1.e-5f<t){
            if(room==world.current){
                body.position=from;room_move(&body,next.x-from.x,next.z-from.z);
                *travel=hypotf(body.position.x-from.x,body.position.z-from.z);
                if(*travel<.0001f)continue;
                next=body.position;
            }
        }else{
            next.x-=n.x*.008f;next.z-=n.z*.008f;
            next=campaign_transform(&a->exit,next,false);
            *yaw+=atan2f(a->exit.transform[6],a->exit.transform[8]);
            *id=destination;
        }
        Point above=next;above.y+=.321f;
        Hit floor;int prop;
        if(ray_geometry(*id,above,(Point){0,-1,0},.921f,SCENE_BODY,&floor,&prop) && floor.normal.y>.62f)
            next.y=floor.point.y;
        *position=next;return true;
    }
    return false;
}
static bool doorway_panels_clear(const Aperture *a,const Player *p,Point previous,float distance){
    Point n=a->exit.normal;
    // Fit the full capsule on the far side before moving between BSP rooms.
    Point target={p->position.x-(distance+.23f)*n.x,p->position.y-(distance+.23f)*n.y,
                  p->position.z-(distance+.23f)*n.z};
    Player body=*p;body.position=previous;
    if(!panel_path_clear(world.current,&body,target))return false;
    body.position=apply(a->exit.transform,previous);target=apply(a->exit.transform,target);
    return panel_path_clear(a->exit.room,&body,target);
}
int campaign_crossing(Player *p,Point previous,Point wish,const bool *enabled) {
    Point at=p->position;at.y+=.70f;
    Point from=previous;from.y+=.70f;
    int first=-1;float nearest=INFINITY;bool jamb=false;
    for(unsigned i=0;i<world.info.exits;i++) {
        const Aperture *a=world.openings+i;Point n=a->exit.normal;
        float fraction;
        bool center=portal_departure(a->verts,a->indices,a->count,n,from,at,wish,&fraction);
        bool clear=center;
        if(fabsf(n.y)<.5f && (center ||
           (at.x-a->verts[0].x)*n.x+(at.y-a->verts[0].y)*n.y+(at.z-a->verts[0].z)*n.z<-.025f)){
            float width=.20f/hypotf(n.x,n.z);
            Point side={-n.z*width,0,n.x*width};
            if(center){
                Point contact={from.x+(at.x-from.x)*fraction,from.y+(at.y-from.y)*fraction,from.z+(at.z-from.z)*fraction};
                clear=inside_aperture(a,(Point){contact.x+side.x,contact.y,contact.z+side.z}) &&
                      inside_aperture(a,(Point){contact.x-side.x,contact.y,contact.z-side.z});
            }else{
                // The centre can miss the opening while the side of the
                // capsule still slips past the last floor triangle.
                for(int sign=-1;sign<=1;sign+=2){
                    Point start={from.x+side.x*sign,from.y,from.z+side.z*sign};
                    Point end={at.x+side.x*sign,at.y,at.z+side.z*sign};
                    if(portal_departure(a->verts,a->indices,a->count,n,start,end,wish,&fraction)){
                        center=true;break;
                    }
                }
            }
        }
        if(!center || fraction>=nearest)continue;
        first=i;nearest=fraction;jamb=!clear;
    }
    if(first<0)return -1;
    const Aperture *a=world.openings+first;Point n=a->exit.normal,o=a->verts[0];
    float distance=(at.x-o.x)*n.x+(at.y-o.y)*n.y+(at.z-o.z)*n.z;
    // Disabled exits have no receiving BSP collision bound to the player. Treat
    // them as closed even during the gap between a panel and its scripted toggle.
    if(jamb || (enabled && !enabled[a->exit.id]) || !doorway_panels_clear(a,p,previous,distance)){
        p->position=previous;return CAMPAIGN_CROSS_BLOCKED;
    }
    return first;
}
void campaign_cross(Player *p,const CampaignExit *exit) {
    // Place the feet just inside the adjoining BSP when a wall edge stopped the capsule.
    const Aperture *a=NULL;
    for(unsigned i=0;i<world.info.exits;i++)if(world.openings[i].exit.id==exit->id){a=world.openings+i;break;}
    if(a){
        Point delta={p->position.x-a->verts[0].x,p->position.y+.70f-a->verts[0].y,p->position.z-a->verts[0].z};
        Point n=exit->normal;float inset=fmaxf(0,delta.x*n.x+delta.y*n.y+delta.z*n.z+.008f);
        p->position.x-=inset*n.x;p->position.y-=inset*n.y;p->position.z-=inset*n.z;
    }
    p->position=apply(exit->transform,p->position);
    p->yaw+=atan2f(exit->transform[6],exit->transform[8]);
}
