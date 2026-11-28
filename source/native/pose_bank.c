#include "pose_bank.h"
#include "campaign_file.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#ifdef __3DS__
#include <3ds.h>
#define resident_alloc linearAlloc
#define resident_free linearFree
#else
#define resident_alloc malloc
#define resident_free free
#endif
typedef struct {char name[64];uint16_t slot[16];} Skin;
typedef struct {uint32_t offset,frames;float seconds,rate;} ClipInfo;
static FILE *file;
static PoseTransitionRule *transition_rules;static uint16_t *transition_actions,(*transition_pairs)[2];
static unsigned transition_count,transition_copies,transition_phases;

static Matrix rest[64],inverse_rest[64];static int16_t parents[64];static uint8_t *normalization;
typedef struct {unsigned clip;float values[64][3];} Scaling;
static Scaling *scaling;static unsigned scaling_count;
const float (*pose_bank_scaling(unsigned clip))[3]{
    clip=pose_clip_id(clip);for(unsigned i=0;i<scaling_count;i++)if(scaling[i].clip==clip)return scaling[i].values;return NULL;
}
const Matrix *pose_bank_rest(void){return rest;}

static Skin *skins;
static ClipInfo *clips;
typedef struct {uint32_t frames,loop_start,flags;} ClipMeta;
static ClipMeta *metadata;
static char (*actions)[64],(*names)[64];
static uint16_t *bindings;
static SkinRules *skin_rules;
static unsigned action_count,skin_count,clip_count,bone_count;
const uint8_t *pose_bank_normalization(unsigned clip){clip=pose_clip_id(clip);return normalization && clip && clip<=clip_count?normalization+(clip-1)*bone_count:NULL;}
static FireRate fire_rates[16];
typedef struct {char name[24];uint8_t bones,animation,multiple,reserved;} WeaponPose;
static WeaponPose weapon_poses[32];static unsigned weapon_pose_count;
static uint8_t upper_body[2][64];
const uint8_t *pose_bank_upper(bool two_handed){return weapon_pose_count?upper_body[two_handed]:NULL;}
static const WeaponPose *weapon_pose(const char *name){
    if(!strcmp(name,"coltcommando"))name="mp5";
    for(unsigned i=0;i<weapon_pose_count;i++)if(!strcmp(weapon_poses[i].name,name))return weapon_poses+i;
    return NULL;
}
static bool read_body_sets(void){
    FILE *f=campaign_file("bone_sets.bind");if(!f)return false;
    struct {char tag[4];uint32_t bones,weapons;} h;
    bool ok=fread(&h,sizeof h,1,f)==1 && !memcmp(h.tag,"BMS1",4) && h.bones==bone_count && h.weapons && h.weapons<=32;
    for(unsigned i=0;ok && i<2;i++){
        ok=fread(upper_body[i],1,bone_count,f)==bone_count;
        for(unsigned j=0;ok && j<bone_count;j++)ok=upper_body[i][j]<=1;
    }
    if(ok){
        ok=fread(weapon_poses,sizeof *weapon_poses,h.weapons,f)==h.weapons && fgetc(f)==EOF;
        for(unsigned i=0;ok && i<h.weapons;i++){
            const WeaponPose *p=weapon_poses+i;
            ok=memchr(p->name,0,sizeof p->name) && p->bones<=1 && p->animation<=1 && p->multiple<=1 && !p->reserved &&
                (!i || strcmp(weapon_poses[i-1].name,p->name)<0);
        }
    }
    fclose(f);if(ok)weapon_pose_count=h.weapons;return ok;
}
const FireRate *pose_fire_rate(const char *name){
    for(unsigned i=0;i<16;i++)if(!strcmp(fire_rates[i].name,name))return fire_rates+i;
    return NULL;
}
static float *travel;
typedef struct {uint16_t first,count;} CueRange;
typedef struct {uint16_t frame,target,op,count,arg[4];} FrameCue;
static CueRange *cue_ranges;
static FrameCue *cue_rows;
static char *cue_text,**cue_strings;
enum {ACTION_HANDLE=0x8000};
unsigned pose_clip_id(unsigned handle){
    if(handle<ACTION_HANDLE)return handle;
    unsigned n=handle-ACTION_HANDLE;
    return bindings && n<action_count*skin_count?bindings[n]:0;
}

enum {FRAME_PAGES=48,FRAMES_PER_PAGE=4};
typedef struct {unsigned clip,first,stamp;unsigned char *data;} FramePage;
static FramePage pages[FRAME_PAGES];static unsigned page_clock;
// Keep the complete player locomotion set resident. Missing diagonals otherwise
// seek the SD from inside skinning on the first step after a cinematic.
// Use the linear arena on hardware; the small normal heap also holds scripts.
enum {RESIDENT_LIMIT=2*1024*1024};
static unsigned char *resident[1025];static size_t resident_bytes;


void pose_bank_close(void) {
    free(transition_rules);free(transition_actions);free(transition_pairs);
    transition_rules=NULL;transition_actions=NULL;transition_pairs=NULL;
    transition_count=transition_copies=transition_phases=0;

    memset(fire_rates,0,sizeof fire_rates);weapon_pose_count=0;memset(upper_body,0,sizeof upper_body);
    free(scaling);scaling=NULL;scaling_count=0;
    free(normalization);normalization=NULL;
    free(metadata);metadata=NULL;
    free(skin_rules);skin_rules=NULL;free(travel);travel=NULL;
    free(cue_ranges);free(cue_rows);free(cue_text);free(cue_strings);
    cue_ranges=NULL;cue_rows=NULL;cue_text=NULL;cue_strings=NULL;
    for(unsigned i=1;i<=clip_count;i++){resident_free(resident[i]);resident[i]=NULL;}resident_bytes=0;
    if(file)fclose(file);
    for(unsigned i=0;i<FRAME_PAGES;i++){free(pages[i].data);pages[i]=(FramePage){0};}
    page_clock=0;
    free(actions);actions=NULL;free(names);names=NULL;free(bindings);bindings=NULL;action_count=0;
    file=NULL;free(skins);free(clips);skins=NULL;clips=NULL;skin_count=clip_count=bone_count=0;
}
static bool read_index(void){
    FILE *index=campaign_file("poses.idx");if(!index)return false;
    struct {char tag[4];unsigned clips,actions,skins;} h;
    bool ok=fread(&h,sizeof h,1,index)==1 && !memcmp(h.tag,"MPI1",4) && h.clips==clip_count &&
        h.skins==skin_count && h.actions && h.actions<=1024;
    if(ok){
        action_count=h.actions;
        actions=calloc(h.actions,sizeof(*actions));names=calloc(h.clips,sizeof(*names));
        bindings=calloc(h.actions*h.skins,sizeof(*bindings));
        ok=actions && names && bindings && fread(actions,64,h.actions,index)==h.actions &&
            fread(names,64,h.clips,index)==h.clips && fread(bindings,2,h.actions*h.skins,index)==h.actions*h.skins;
        for(unsigned i=0;ok && i<h.actions;i++)
            ok=memchr(actions[i],0,64) && (!i || strcmp(actions[i-1],actions[i])<0);
        for(unsigned i=0;ok && i<h.clips;i++)ok=memchr(names[i],0,64)!=NULL;
        for(unsigned i=0;ok && i<h.actions*h.skins;i++)ok=bindings[i]<=clip_count;
    }
    ok=ok && fgetc(index)==EOF;fclose(index);return ok;
}
static bool read_metadata(void){
    FILE *f=campaign_file("poses.meta");if(!f)return false;
    struct {char tag[4];uint32_t count;} h;
    bool ok=fread(&h,sizeof h,1,f)==1 && !memcmp(h.tag,"MPM1",4) && h.count==clip_count;
    if(ok){
        metadata=calloc(clip_count+1,sizeof *metadata);
        ok=metadata && fread(metadata+1,sizeof *metadata,clip_count,f)==clip_count && fgetc(f)==EOF;
        for(unsigned i=1;ok && i<=clip_count;i++){
            ClipMeta m=metadata[i];ClipInfo c=clips[i];
            ok=m.frames==c.frames && (!(m.flags&1) || m.loop_start<m.frames) && m.flags<=3 &&
                fabsf(c.seconds*c.rate-m.frames)<.002f;
        }
    }
    fclose(f);return ok;
}
static bool read_skin_rules(void){
    FILE *f=campaign_file("skin_rules.bin");
    if(!f)return true; // Older external campaigns use the resident movement defaults.
    struct {char tag[4];uint32_t skins,actions;} h;
    bool ok=fread(&h,sizeof h,1,f)==1 && (!memcmp(h.tag,"SKN1",4) || !memcmp(h.tag,"SKN2",4)) && h.skins==skin_count && h.actions==action_count;
    if(ok){
        unsigned n=skin_count*action_count;
        skin_rules=calloc(skin_count,sizeof *skin_rules);travel=calloc(n,sizeof *travel);
        ok=skin_rules && travel && fread(skin_rules,sizeof *skin_rules,skin_count,f)==skin_count &&
           fread(travel,sizeof *travel,n,f)==n;
        uint32_t rates;
        ok=ok && fread(&rates,sizeof rates,1,f)==1 && rates==16;
        for(unsigned i=0;ok && i<rates;i++){
            FireRate *r=fire_rates+i;
            size_t bytes=h.tag[3]=='2'?sizeof *r:sizeof *r-sizeof(float);
            ok=fread(r,bytes,1,f)==1 && memchr(r->name,0,20) &&
                isfinite(r->normal) && r->normal>0 && isfinite(r->maximum) && r->maximum>0 &&
                isfinite(r->slow_multiplier) && r->slow_multiplier>=0 && r->slow_multiplier<=1;
        }
        ok=ok && fgetc(f)==EOF;
        for(unsigned i=0;ok && i<skin_count;i++){
            const SkinRules *s=skin_rules+i;
            ok=s->wounded>=0 && s->wounded<=1 && s->visual_radius>0 && s->general_radius>=0 && s->reaction_time>=0 &&
               s->shooting_cone>=0 && s->shooting_cone<=180 && s->turn_speed>0 && s->aim_cone>=0 && s->aim_speed>0 && s->shoot_frequency>0;
        }
        for(unsigned i=0;ok && i<n;i++)ok=isfinite(travel[i]) && travel[i]>=0;
    }
    fclose(f);return ok;
}
const SkinRules *pose_skin_rules(unsigned skin){return skin_rules && skin<skin_count?skin_rules+skin:NULL;}
float pose_fire_interval(const FireRate *rate,const SkinRules *rules,float fraction){
    float first=1.f/(rate->maximum*rules->shoot_frequency),last=1.f/(rate->normal*rules->shoot_frequency);
    return first+(last-first)*fraction;
}
float pose_stride(unsigned handle){
    unsigned at=handle-ACTION_HANDLE;
    return travel && at<skin_count*action_count?travel[at]:0;
}
static bool read_cues(void){
    FILE *f=campaign_file("cues.bin");if(!f)return false;
    struct {char tag[4];unsigned skins,actions,rows,strings,bytes;} h;
    bool ok=fread(&h,sizeof h,1,f)==1 && !memcmp(h.tag,"CUE1",4) && h.skins==skin_count &&
        h.actions==action_count && h.skins*h.actions<ACTION_HANDLE && h.rows<=65535 && h.strings && h.strings<=65535 && h.bytes<1024*1024;
    if(ok){
        cue_ranges=calloc(h.skins*h.actions,sizeof *cue_ranges);cue_rows=calloc(h.rows+1,sizeof *cue_rows);
        cue_text=malloc(h.bytes);cue_strings=calloc(h.strings,sizeof *cue_strings);
        ok=cue_ranges && cue_rows && cue_text && cue_strings &&
            fread(cue_ranges,sizeof *cue_ranges,h.skins*h.actions,f)==h.skins*h.actions &&
            fread(cue_rows,sizeof *cue_rows,h.rows,f)==h.rows && fread(cue_text,1,h.bytes,f)==h.bytes && fgetc(f)==EOF;
        unsigned pos=0;
        for(unsigned i=0;ok && i<h.strings;i++){
            char *end=memchr(cue_text+pos,0,h.bytes-pos);
            if(!end){ok=false;break;}
            cue_strings[i]=cue_text+pos;pos=(unsigned)(end+1-cue_text);
        }
        ok=ok && pos==h.bytes;
        for(unsigned i=0;ok && i<h.skins*h.actions;i++)ok=cue_ranges[i].first+cue_ranges[i].count<=h.rows;
        for(unsigned i=0;ok && i<h.rows;i++){
            FrameCue c=cue_rows[i];ok=c.op<h.strings && c.target<h.strings && c.count<=4;
            for(unsigned a=0;ok && a<c.count;a++)ok=c.arg[a]<h.strings;
        }
    }
    fclose(f);return ok;
}
static CueRange range_for(unsigned action){
    unsigned n=action-ACTION_HANDLE;
    return cue_ranges && n<action_count*skin_count?cue_ranges[n]:(CueRange){0};
}
bool pose_message_matches(unsigned action,const char *op,const char *first,const char *second){
    CueRange range=range_for(action);
    for(unsigned i=0;i<range.count;i++){
        FrameCue c=cue_rows[range.first+i];
        if(strcmp(cue_strings[c.op],op) || !c.count || strncmp(cue_strings[c.arg[0]],first,strlen(first)))continue;
        if(!second || (c.count>1 && !strcmp(cue_strings[c.arg[1]],second)))return true;
    }
    return false;
}
bool pose_has_event(unsigned action,const char *op){
    return pose_has_target_event(action,NULL,op);
}
bool pose_has_target_event(unsigned action,const char *target,const char *op){
    CueRange range=range_for(action);
    for(unsigned i=0;i<range.count;i++){
        FrameCue c=cue_rows[range.first+i];
        if(!strcmp(cue_strings[c.op],op) && (!target || !strcmp(cue_strings[c.target],target)))return true;
    }
    return false;
}
void pose_events(unsigned actor,unsigned action,float after,float through,PoseMessage send){
    CueRange range=range_for(action);float rate=pose_bank_rate(action);
    for(unsigned i=0;i<range.count;i++){
        FrameCue c=cue_rows[range.first+i];if(c.frame>through*rate)break;
        if(c.frame<=after*rate)continue;
        const char *args[4];for(unsigned j=0;j<4;j++)args[j]=cue_strings[c.arg[j]];
        send(actor,cue_strings[c.target],cue_strings[c.op],args,c.count);
    }
}
void pose_events_advance(unsigned actor,unsigned action,float after,float through,bool looping,PoseMessage send){
    if(through+.00001f<after){
        if(looping){
            pose_events(actor,action,after,pose_bank_duration(action),send);
            after=pose_bank_loop_start(action)-.0001f;
        }else after=-1;
    }
    pose_events(actor,action,after,through,send);
}
bool pose_bank_two_handed(const char *weapon){
    const WeaponPose *p=weapon_pose(weapon);return p && p->animation;
}
bool pose_bank_two_handed_bones(const char *weapon){
    const WeaponPose *p=weapon_pose(weapon);return p && p->bones;
}
unsigned pose_bank_count(void){return clip_count;}
const char *pose_bank_name(unsigned clip){clip=pose_clip_id(clip);return clip && clip<=clip_count?names[clip-1]:"";}
unsigned pose_bank_skin(const char *name){
    for(unsigned i=0;i<skin_count;i++)if(!strcmp(skins[i].name,name))return i;
    return skin_count;
}
unsigned pose_motion_action(unsigned skin,const char *name){
    if(skin>=skin_count)return 0;
    unsigned a=0,b=action_count;
    while(a<b){unsigned m=(a+b)/2;if(strcmp(actions[m],name)<0)a=m+1;else b=m;}
    return a<action_count && !strcmp(name,actions[a])&& bindings[skin*action_count+a]?ACTION_HANDLE+skin*action_count+a:0;
}
unsigned pose_bank_action(unsigned skin,const char *name){return pose_clip_id(pose_motion_action(skin,name));}

const PoseTransitionRule *pose_transition_rule(unsigned clip){
    static const PoseTransitionRule fallback={12,1,.25f};
    unsigned action=clip-ACTION_HANDLE;
    if(!transition_rules || !transition_actions || action>=action_count*skin_count)return &fallback;
    return transition_rules+transition_actions[action%action_count];
}
unsigned pose_transition_mode(unsigned previous,unsigned next){
    const PoseTransitionRule *a=pose_transition_rule(previous),*b=pose_transition_rule(next);
    unsigned x=a->id<b->id?a->id:b->id,y=a->id<b->id?b->id:a->id;
    for(unsigned i=0;i<transition_copies+transition_phases;i++)
        if(transition_pairs[i][0]==x && transition_pairs[i][1]==y)return i<transition_copies?2:3;
    return b->mode;
}
static bool read_transition_rules(void){
    FILE *f=campaign_file("transitions.bin");if(!f)return false;
    struct {char tag[4];uint32_t rules,actions,copies,phases;} h;
    bool ok=fread(&h,sizeof h,1,f)==1 && !memcmp(h.tag,"TRN1",4) && h.rules==447 && h.actions==action_count && h.copies==32 && h.phases==144;
    if(ok){
        transition_rules=calloc(h.rules,sizeof *transition_rules);transition_actions=calloc(action_count,sizeof *transition_actions);
        transition_pairs=calloc(h.copies+h.phases,sizeof *transition_pairs);
        ok=transition_rules && transition_actions && transition_pairs;
    }
    for(unsigned i=0;ok && i<h.rules;i++){
        unsigned char raw[24];ok=fread(raw,1,24,f)==24;
        if(ok){
            PoseTransitionRule *r=transition_rules+i;r->id=i;
            memcpy(&r->mode,raw+1,4);memcpy(&r->seconds,raw+5,4);
            ok=r->mode<=4 && isfinite(r->seconds) && r->seconds>=0 && r->seconds<=1;
        }
    }
    ok=ok && fread(transition_actions,2,action_count,f)==action_count &&
        fread(transition_pairs,sizeof *transition_pairs,h.copies+h.phases,f)==h.copies+h.phases && fgetc(f)==EOF;
    for(unsigned i=0;ok && i<action_count;i++)ok=transition_actions[i]<h.rules;
    for(unsigned i=0;ok && i<h.copies+h.phases;i++)ok=transition_pairs[i][0]<transition_pairs[i][1] && transition_pairs[i][1]<h.rules;
    fclose(f);
    if(ok){transition_count=h.rules;transition_copies=h.copies;transition_phases=h.phases;}
    return ok;
}
static bool read_local_skeleton(void){
    struct {char tag[4];uint32_t count;} h;FILE *f=campaign_file("bones.rest");
    if(!f)return false;
    bool ok=fread(&h,sizeof h,1,f)==1 && !memcmp(h.tag,"RST1",4) && h.count==bone_count &&
        fread(rest,sizeof *rest,bone_count,f)==bone_count && fgetc(f)==EOF;
    fclose(f);if(!ok)return false;
    f=campaign_file("bones.tree");if(!f)return false;
    ok=fread(&h,sizeof h,1,f)==1 && !memcmp(h.tag,"MPT1",4) && h.count==bone_count &&
        fread(parents,sizeof *parents,bone_count,f)==bone_count && fgetc(f)==EOF;
    fclose(f);if(!ok)return false;
    for(unsigned i=0;i<bone_count;i++){
        if(parents[i]<-1 || parents[i]>=(int)i)return false;
        for(unsigned r=0;r<3;r++)for(unsigned c=0;c<4;c++)if(!isfinite(rest[i].row[r][c]))return false;
        inverse_rest[i]=bone_inverse_frame(rest+i);
    }
    struct {char tag[4];uint32_t clips,bones,scales;} n;f=campaign_file("poses.norm");if(!f)return false;
    normalization=malloc(clip_count*bone_count);
    ok=normalization && fread(&n,sizeof n,1,f)==1 && !memcmp(n.tag,"MPN2",4) && n.clips==clip_count && n.bones==bone_count && n.scales<=clip_count &&
        fread(normalization,bone_count,clip_count,f)==clip_count;
    if(ok && n.scales){scaling=calloc(n.scales,sizeof *scaling);ok=scaling!=NULL;}
    for(unsigned i=0;ok && i<n.scales;i++){
        ok=fread(&scaling[i].clip,4,1,f)==1 && scaling[i].clip && scaling[i].clip<=clip_count &&
            (!i || scaling[i-1].clip<scaling[i].clip) && fread(scaling[i].values,12,bone_count,f)==bone_count;
        for(unsigned b=0;ok && b<bone_count;b++)for(unsigned axis=0;ok && axis<3;axis++)ok=isfinite(scaling[i].values[b][axis]);
    }
    if(ok)scaling_count=n.scales;
    ok=ok && fgetc(f)==EOF;fclose(f);
    for(unsigned i=0;ok && i<clip_count*bone_count;i++)ok=(normalization[i]&3)<=2 && normalization[i]<=6 &&
        (!(normalization[i]&4) || pose_bank_scaling(i/bone_count+1));
    return ok;
}
bool pose_bank_open(void) {
    pose_bank_close();file=campaign_file("poses.pak");if(!file)return false;
    struct {char tag[4];uint32_t skins,clips,bones;} h;
    if(fread(&h,sizeof h,1,file)!=1 || memcmp(h.tag,"MPP2",4) || h.skins>128 || h.clips>1024 || !h.bones || h.bones>64)goto fail;
    skins=calloc(h.skins+1,sizeof(*skins));clips=calloc(h.clips+1,sizeof(*clips));
    if(!skins || !clips || fread(skins,sizeof(*skins),h.skins,file)!=h.skins || fread(clips+1,sizeof(*clips),h.clips,file)!=h.clips)goto fail;
    long first=ftell(file);if(first<0 || fseek(file,0,SEEK_END))goto fail;
    long length=ftell(file);
    for(unsigned i=1;i<=h.clips;i++) {
        ClipInfo c=clips[i];
        if(c.offset!=(unsigned long)first || c.frames<2 || c.frames>8192 || !(c.seconds>0) || !(c.rate>0))goto fail;
        first+=(long)c.frames*h.bones*48;if(first>length)goto fail;
    }
    if(first!=length)goto fail;
    for(unsigned i=0;i<h.skins;i++) {
        if(!memchr(skins[i].name,0,64))goto fail;
        for(unsigned j=0;j<16;j++)if(skins[i].slot[j]>h.clips)goto fail;
    }
    skin_count=h.skins;clip_count=h.clips;bone_count=h.bones;if(!read_local_skeleton() || !read_index() || !read_transition_rules() || !read_metadata() || !read_cues() || !read_skin_rules() || !read_body_sets())goto fail;return true;
fail:pose_bank_close();return false;
}
uint16_t pose_bank_find(const char *name,unsigned variant) {
    if(variant>=16)return 0;
    for(unsigned i=0;i<skin_count;i++)if(!strcmp(skins[i].name,name))return skins[i].slot[variant];
    return 0;
}
float pose_bank_duration(unsigned clip){clip=pose_clip_id(clip);return clip && clip<=clip_count?clips[clip].seconds:0;}
float pose_bank_rate(unsigned clip){clip=pose_clip_id(clip);return clip && clip<=clip_count?clips[clip].rate:0;}
bool pose_bank_loops(unsigned clip){
    clip=pose_clip_id(clip);
    return clip && clip<=clip_count && (metadata[clip].flags&1);
}
float pose_bank_loop_start(unsigned clip){
    clip=pose_clip_id(clip);
    return clip && clip<=clip_count && (metadata[clip].flags&1)?metadata[clip].loop_start/clips[clip].rate:0;
}
float pose_bank_wrap_time(unsigned clip,float seconds){
    clip=pose_clip_id(clip);
    if(!clip || clip>clip_count || !isfinite(seconds))return 0;
    ClipInfo c=clips[clip];ClipMeta m=metadata[clip];
    float t=fmaxf(0,seconds);
    if(!(m.flags&1))return fminf(t,c.seconds);
    if(t<c.seconds)return t;
    float start=m.loop_start/c.rate;
    return start+fmodf(t-start,c.seconds-start);
}
bool pose_bank_frames(unsigned clip,float seconds,unsigned bones,unsigned *first,unsigned *last,float *mix) {
    clip=pose_clip_id(clip);
    if(!clip || clip>clip_count || bone_count!=bones || !isfinite(seconds))return false;
    ClipInfo c=clips[clip];float frame=fminf(c.frames-1,fmaxf(0,seconds*c.rate));
    *first=(unsigned)frame;*last=*first<c.frames-1?*first+1:*first;*mix=frame-*first;return true;
}
bool pose_bank_loop_frames(unsigned clip,float seconds,unsigned bones,unsigned *first,unsigned *last,float *mix){
    clip=pose_clip_id(clip);
    if(!clip || clip>clip_count || bone_count!=bones || !isfinite(seconds))return false;
    ClipInfo c=clips[clip];ClipMeta m=metadata[clip];
    if(!(m.flags&1))return pose_bank_frames(clip,seconds,bones,first,last,mix);
    float frame=pose_bank_wrap_time(clip,seconds)*c.rate;
    if(frame>=c.frames)frame=m.loop_start; // Float rounding at the exact wrap.
    *first=(unsigned)frame;*last=*first+1;*mix=frame-*first;
    if(*last==c.frames)*last=(m.flags&2)?m.loop_start:*first;
    return true;
}
void pose_bank_preload(unsigned clip){clip=pose_clip_id(clip);
    if(!clip || clip>clip_count || resident[clip])return;
    size_t bytes=clips[clip].frames*bone_count*48;
    if(bytes>RESIDENT_LIMIT-resident_bytes)return;
    unsigned char *data=resident_alloc(bytes);if(!data)return;
    if(fseek(file,clips[clip].offset,SEEK_SET) || fread(data,1,bytes,file)!=bytes){resident_free(data);return;}
    resident[clip]=data;resident_bytes+=bytes;
}
bool pose_bank_preload_player(unsigned skin){
    const char *const actions[]={"stand","use","run","walkbackward","crouch",
        "strafeleft","straferight","forwardleft","forwardright","backwardleft","backwardright"};
    for(unsigned i=0;i<sizeof actions/sizeof *actions;i++)for(unsigned heavy=0;heavy<2;heavy++){
        char name[64];snprintf(name,sizeof name,"%s%s",heavy?"2_":"",actions[i]);
        unsigned clip=pose_clip_id(pose_motion_action(skin,name));
        pose_bank_preload(clip);
        if(clip && !resident[clip])return false;
    }
    const char *const exchange[]={"holsterweapon","getweapon"};
    for(unsigned i=0;i<2;i++){
        unsigned clip=pose_clip_id(pose_motion_action(skin,exchange[i]));pose_bank_preload(clip);
        if(clip && !resident[clip])return false;
    }
    return true;
}
bool pose_bank_read_local(unsigned clip,unsigned frame,unsigned bones,void *matrix) {clip=pose_clip_id(clip);
    if(!clip || clip>clip_count || frame>=clips[clip].frames || bones!=bone_count)return false;
    size_t stride=bones*48;unsigned first=frame/FRAMES_PER_PAGE*FRAMES_PER_PAGE;
    if(resident[clip]){memcpy(matrix,resident[clip]+frame*stride,stride);return true;}
    FramePage *page=pages;
    for(unsigned i=0;i<FRAME_PAGES;i++){
        FramePage *candidate=pages+i;
        if(candidate->clip==clip && candidate->first==first){page=candidate;break;}
        if(candidate->stamp<page->stamp)page=candidate;
    }
    if(page->clip!=clip || page->first!=first){
        if(!page->data)page->data=malloc(FRAMES_PER_PAGE*stride);
        if(!page->data)return !fseek(file,clips[clip].offset+frame*stride,SEEK_SET) && fread(matrix,1,stride,file)==stride;
        unsigned n=clips[clip].frames-first;if(n>FRAMES_PER_PAGE)n=FRAMES_PER_PAGE;
        page->clip=page->stamp=0;
        if(fseek(file,clips[clip].offset+first*stride,SEEK_SET) || fread(page->data,stride,n,file)!=n)return false;
        page->clip=clip;page->first=first;
    }
    page->stamp=++page_clock;memcpy(matrix,page->data+(frame-first)*stride,stride);return true;
}

// Compatibility query for mesh-space tools. The renderer consumes local keys
// directly and finalizes only after all active animation layers are combined.
bool pose_bank_read(unsigned clip,unsigned frame,unsigned bones,void *matrix){
    Matrix local[64];if(!pose_bank_read_local(clip,frame,bones,local))return false;
    const uint8_t *modes=pose_bank_normalization(clip);const float (*scale)[3]=pose_bank_scaling(clip);
    for(unsigned i=0;i<bones;i++)if(!bone_temporal_finish(local+i,modes[i],scale?scale[i]:NULL) || !bone_normalize(local+i,2))return false;
    bone_local_to_skin(local,matrix,inverse_rest,parents,bones);return true;
}
