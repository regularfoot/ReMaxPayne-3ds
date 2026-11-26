// Host regression test for the actual palettes, timing and code used on 3DS.
#include "bone_pose.h"
#include "pose_bank.h"
#include "campaign_file.h"
#include "actor.h"
#include "animation_state.h"
#include "dodge_animation.h"
#include "weapon_timing.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

FILE *campaign_file(const char *name){return fopen(name,"rb");}
// Complete pose crossfades must preserve the arm/leg chains, including when
// the target is masked to the upper body and when the output aliases an input.
static float bone_distance(const Matrix *m,const float pivots[][3],unsigned a,unsigned b){
    float square=0;
    for(unsigned r=0;r<3;r++){
        float delta=m[a].row[r][3]-m[b].row[r][3];
        for(unsigned k=0;k<3;k++)delta+=m[a].row[r][k]*pivots[a][k]-m[b].row[r][k]*pivots[b][k];
        square+=delta*delta;
    }
    return sqrtf(square);
}
static int verify_hierarchy(unsigned bones,const float pivots[][3],unsigned gun){
    FILE *f=campaign_file("bones.tree");struct {char tag[4];uint32_t count;} h;int16_t parents[64];
    if(!f || fread(&h,sizeof h,1,f)!=1 || memcmp(h.tag,"MPT1",4) || h.count!=bones ||
       fread(parents,sizeof *parents,bones,f)!=bones || fgetc(f)!=EOF)return 0;
    fclose(f);
    for(unsigned i=0;i<bones;i++)if(parents[i]<-1 || parents[i]>=(int)i)return 0;
    const unsigned limbs[]={14,15,18,19,21,22,25,26};
    const char *models[]={"max_payne","young_max_payne"};
    Matrix a[64],b[64],mixed[64],inplace[64];uint8_t upper[64];
    for(unsigned i=0;i<bones;i++)upper[i]=i>0 && i<20;
    for(unsigned model=0;model<2;model++){
        unsigned skin=pose_bank_skin(models[model]);
        unsigned stand=pose_motion_action(skin,"stand"),low=pose_motion_action(skin,"crouch"),x,y;float mix;
        if(!pose_bank_frames(stand,.7f,bones,&x,&y,&mix) || !pose_bank_read(stand,x,bones,a) ||
           !pose_bank_frames(low,1.2f,bones,&x,&y,&mix) || !pose_bank_read(low,x,bones,b))return 0;
        for(unsigned mask=0;mask<2;mask++)for(unsigned step=1;step<4;step++){
            float t=step*.25f;const uint8_t *weights=mask?upper:NULL;
            mix_bone_palette(a,b,mixed,t,pivots,parents,bones,gun,weights,pose_bank_rest());
            memcpy(inplace,a,bones*sizeof *a);mix_bone_palette(inplace,b,inplace,t,pivots,parents,bones,gun,weights,pose_bank_rest());
            for(unsigned i=0;i<bones;i++)for(unsigned r=0;r<3;r++)for(unsigned c=0;c<4;c++)
                if(!isfinite(mixed[i].row[r][c]) || fabsf(inplace[i].row[r][c]-mixed[i].row[r][c])>.00001f)return 0;
            for(unsigned i=0;i<sizeof limbs/sizeof *limbs;i++){
                unsigned child=limbs[i],parent=parents[child];
                float first=bone_distance(a,pivots,parent,child),last=bone_distance(b,pivots,parent,child);
                float expected=first+(last-first)*(mask && !upper[child]?0:t);
                float actual=bone_distance(mixed,pivots,parent,child);
                if(fabsf(actual-expected)>.002f){fprintf(stderr,"Limb shortened in %s at bone %u: %.4f vs %.4f\n",models[model],child,actual,expected);return 0;}
            }
        }
    }
    puts("Verified parent-space arm/leg blends, upper-body masks and in-place pose mixing for both Max models");
    return 1;
}

static unsigned footsteps[2],shots[2],swings;
static unsigned reloads,jumps,protect,unprotect,weapon_animation;
static void message(unsigned actor,const char *target,const char *op,const char *const *args,unsigned n){
    if(actor!=7)abort();
    if(!strcmp(op,"ws_animateshooting")){
        if(strcmp(target,"righthandweapon") && strcmp(target,"lefthandweapon"))abort();
        shots[!strcmp(target,"lefthandweapon")]++;
    }
    if(!strcmp(op,"p_createprojectiletobone") && n && (!strcmp(args[0],"bullet_leadpipe") || !strcmp(args[0],"bullet_baseballbat")))swings++;
    if(!strcmp(op,"p_createprojectiletobone") && n>=3){
        if(!strcmp(args[0],"dummy_footstep_left") && !strcmp(args[2],"foot-l"))footsteps[0]++;
        if(!strcmp(args[0],"dummy_footstep_right") && !strcmp(args[2],"foot-r"))footsteps[1]++;
    }
    if(!strcmp(op,"c_reload"))reloads++;
    if(!strcmp(op,"ws_animate"))weapon_animation++;
    if(!strcmp(op,"c_jump") && n && atof(args[0])>0)jumps++;
    if(!strcmp(op,"c_setdodgeimmortal") && n){
        if(!strcmp(args[0],"true"))protect++;
        if(!strcmp(args[0],"false"))unprotect++;
    }
}
static int verify_cues(void){
    unsigned skin=pose_bank_skin("max_payne");
    unsigned dive=pose_motion_action(skin,"shootdodgeforward");
    float rate=pose_bank_rate(dive);
    if(!dive || rate<=0)return 0;
    pose_events(7,dive,-1,0,message);
    if(reloads!=1 || protect || jumps)return 0;
    pose_events(7,dive,0,16.5f/rate,message);
    if(reloads!=1 || protect!=1 || jumps || unprotect)return 0;
    pose_events(7,dive,16.5f/rate,48.75f/rate,message);
    if(jumps!=1 || unprotect)return 0;
    pose_events(7,dive,48.75f/rate,pose_bank_duration(dive),message);
    pose_events(7,dive,pose_bank_duration(dive),pose_bank_duration(dive),message);
    if(jumps!=1 || protect!=1 || unprotect!=1 || reloads!=1)return 0;
    const char *models[]={"max_payne","young_max_payne"};
    // Iterate individual retail frames, including zero and the last wait before release.
    const struct {const char *name;unsigned n;int frames[8];bool dual,melee;} attacks[]={
        {"shootgrenade",1,{14},false,false},{"shootberettadual",2,{0,3},true,false},
        {"shootingram",4,{2,5,8,11},false,false},{"shootingramdual",8,{0,1,2,3,4,5,6,7},true,false},
        {"shootbaseballbat",2,{4,32},false,true},{"shootleadpipe",2,{7,31},false,true}
    };
    for(unsigned m=0;m<2;m++)for(unsigned i=0;i<sizeof attacks/sizeof attacks[0];i++){
        unsigned clip=pose_motion_action(pose_bank_skin(models[m]),attacks[i].name);
        float fps=pose_bank_rate(clip);if(!clip || !fps)return 0;
        for(int f=0;f<=ceilf(pose_bank_duration(clip)*fps);f++){
            shots[0]=shots[1]=swings=0;
            pose_events(7,clip,(f-.75f)/fps,(f+.25f)/fps,message);
            unsigned expected[3]={0};
            for(unsigned j=0;j<attacks[i].n;j++)if(attacks[i].frames[j]==f)
                expected[attacks[i].melee?2:attacks[i].dual?j%2:0]++;
            if(shots[0]!=expected[0] || shots[1]!=expected[1] || swings!=expected[2]){
                fprintf(stderr,"%s %s frame %d unexpected hand or contact count\n",models[m],attacks[i].name,f);return 0;
            }
        }
    }
    const char *actions[]={"reloadberetta","reloadpumpshotgun"};
    const unsigned start[]={10,2},refill[]={39,17};
    for(unsigned m=0;m<2;m++)for(unsigned w=0;w<2;w++){
        unsigned action=pose_motion_action(pose_bank_skin(models[m]),actions[w]);
        float fps=pose_bank_rate(action);if(!action || fps<=0)return 0;
        float before=(start[w]-.25f)/fps,middle=(refill[w]-.25f)/fps;
        reloads=weapon_animation=0;
        pose_events(7,action,-1,before,message);
        if(weapon_animation || reloads)return 0;
        pose_events(7,action,before,middle,message);
        if(weapon_animation!=1 || reloads)return 0;
        pose_events(7,action,middle,pose_bank_duration(action),message);
        if(reloads!=1)return 0;
    }
    return 1;
}
static int verify_motion_rules(void){
    const char *models[]={"max_payne","young_max_payne"};
    const char *actions[]={"strafeleft","straferight","2_strafeleft","2_straferight",
                           "w_strafeleft","w_straferight","w_2_strafeleft","w_2_straferight"};
    for(unsigned m=0;m<2;m++)for(unsigned i=0;i<8;i++){
        unsigned gait=pose_motion_action(pose_bank_skin(models[m]),actions[i]);
        float stride=pose_stride(gait),duration=pose_bank_duration(gait);
        if(!gait || fabsf(stride-(i<4?4.2f:3.f))>.0001f || duration<=0)return 0;
        footsteps[0]=footsteps[1]=0;
        for(unsigned k=0;k<16;k++)
            pose_events(7,gait,k?k*duration/16:-1,(k+1)*duration/16,message);
        if(footsteps[0]!=1 || footsteps[1]!=1)return 0;
    }
    unsigned johnny=pose_bank_skin("c1_all_johnny"),merc=pose_bank_skin("c5_all_mercenary");
    const SkinRules *j=pose_skin_rules(johnny),*m=pose_skin_rules(merc);
    const FireRate *beretta=pose_fire_rate("beretta");
    if(!j || !m || !beretta || j->visual_radius!=50 || m->visual_radius!=60 ||
       fabsf(j->wounded-10.f/15)>.001f || fabsf(m->wounded-6.f/35)>.001f ||
       fabsf(pose_fire_interval(beretta,j,.5f)-2.5f)>.002f || fabsf(pose_fire_interval(beretta,m,.5f)-.5f)>.002f)return 0;
    return !strcmp(pose_bank_name(pose_motion_action(johnny,"getdamage")),"gethit_1250ms") &&
           !strcmp(pose_bank_name(pose_motion_action(merc,"getdamage")),"gethit_0500ms") &&
           !strcmp(pose_bank_name(pose_motion_action(merc,"getdamagecrouched")),"gethit_crouched_0750ms");
}
// Values are the Xbox PAL shooting.h and per-weapon attributes, not clip lengths.
static int verify_fire_timing(void){
    const struct {const char *name;float normal,maximum,slow;} expected[]={
        {"beretta",4,4.001f,.25f},{"berettadual",4,4.001f,.25f},
        {"deserteagle",3,3.001f,.25f},{"pumpshotgun",1,1.001f,.2f},
        {"sawedshotgun",2,2.001f,.25f},{"jackhammer",3,3.001f,.8f},
        {"ingram",8,8.001f,0},{"ingramdual",8,8.001f,0},{"mp5",6,6.001f,.8f},
        {"sniper",1,1.001f,.8f},{"baseballbat",1.26f,1.27f,0},{"leadpipe",1.46f,1.47f,0},
        {"grenade",1,1.001f,0},{"molotov",1,1.001f,.1f},{"m79",1,1.001f,.8f}
    };
    for(unsigned i=0;i<sizeof expected/sizeof *expected;i++){
        const FireRate *r=pose_fire_rate(expected[i].name);
        if(!r || fabsf(r->normal-expected[i].normal)>.0001f || fabsf(r->maximum-expected[i].maximum)>.0001f ||
           fabsf(r->slow_multiplier-expected[i].slow)>.0001f)return 0;
        // A sustained trigger keeps its cadence at 60/30/20 FPS, also in bullet time.
        for(unsigned fps=20;fps<=60;fps+=10)for(unsigned slow=0;slow<2;slow++){
            float cooldown=0,elapsed=0,interval=1/r->maximum;
            unsigned attacks=0;
            for(unsigned f=0;f<fps*10;f++){
                float real=1.f/fps,dt=weapon_time_step(real*(slow?.2f:1),real,r->slow_multiplier);
                float late=weapon_cooldown_step(&cooldown,dt);
                if(cooldown<=0){weapon_cooldown_start(&cooldown,interval,late);attacks++;}
                elapsed+=dt;
            }
            if(fabsf((float)attacks-elapsed*r->maximum)>1.01f){
                fprintf(stderr,"Fire cadence drift: %s %u FPS slow=%u\n",r->name,fps,slow);return 0;
            }
        }
    }
    // Holding no trigger while ready cannot bank up catch-up shots.
    float cooldown=.1f;
    if(fabsf(weapon_cooldown_step(&cooldown,.15f)-.05f)>.00001f || cooldown!=0 ||
       weapon_cooldown_step(&cooldown,3)!=0 || weapon_cooldown_start(&cooldown,.25f,0)!=0 || cooldown!=.25f)return 0;
    puts("Verified all 15 retail weapon frequencies, bullet-time resistance and frame-rate-independent cadence");
    return 1;
}
static int verify_retail_selection(void){
    const float axes[][2]={{0,1},{1,1},{1,0},{1,-1},{0,-1},{-1,-1},{-1,0},{-1,1}};
    const Clip expected[]={RUN,RUN,STRAFE_RIGHT,BACKPEDAL,BACKPEDAL,BACKPEDAL,STRAFE_LEFT,RUN};
    for(unsigned i=0;i<8;i++)for(unsigned low=0;low<2;low++){
        Clip gait=retail_locomotion(axes[i][1]*.01f,axes[i][0]*.01f,low,.016f,true);
        if(gait!=expected[i] || gait>=CROUCH_DOWN)return 0;
    }
    if(retail_locomotion(0,0,true,.016f,true)!=CROUCH_IDLE || retail_locomotion(0,0,false,.016f,true)!=IDLE)return 0;
    for(Clip retired=CROUCH_DOWN;retired<CLIP_COUNT;retired++)for(unsigned low=0;low<2;low++){
        Animation a={.gait=retired,.previous_gait=retired,.action=retired,.gait_time=42,.transition=.1f};
        retail_restore_animation(&a,low);
        if(a.gait!=(low?CROUCH_IDLE:IDLE) || a.previous_gait!=a.gait || a.action!=IDLE || a.gait_time || a.transition)return 0;
    }
    puts("Verified original movement priority in eight directions and migration of all retired save states");
    return 1;
}

static int verify_shootdodge_blend(unsigned bones,const float pivots[][3],unsigned gun){
    const char *suffix[]={"forward","backward","left","right","backwardleft","backwardright","forwardleft","forwardright"};
    const unsigned ring[]={0,6,2,4,1,5,3,7};
    const char *models[]={"max_payne","young_max_payne"};
    FILE *tree=campaign_file("bones.tree");int16_t parents[64];
    if(!tree || fseek(tree,8,SEEK_SET) || fread(parents,2,bones,tree)!=bones)return 0;
    fclose(tree);
    const unsigned limbs[]={14,15,18,19,21,22,25,26};
    Matrix a[64],b[64],out[64];
    for(unsigned model=0;model<2;model++)for(unsigned heavy=0;heavy<2;heavy++){
        unsigned clips[8];float duration=0;
        for(unsigned direction=0;direction<8;direction++){
            char action[64];snprintf(action,sizeof action,"%sshootdodge%s",heavy?"2_":"",suffix[direction]);
            clips[direction]=pose_motion_action(pose_bank_skin(models[model]),action);
            if(!clips[direction])return 0;
            if(!direction)duration=pose_bank_duration(clips[0]);
            if(fabsf(duration-pose_bank_duration(clips[direction]))>.00001f || pose_bank_rate(clips[direction])!=40)return 0;
        }
        for(unsigned sector=0;sector<8;sector++){
            const float quarter=.7853981633974483f;
            for(unsigned step=0;step<=4;step++){
                float weight=step*.25f,angle=(sector+weight)*quarter;
                DodgeBlend mix=dodge_animation_blend(-angle,0);
                unsigned expected=step==4?ring[(sector+1)&7]:ring[sector];
                if(mix.first!=expected || (step<4 && (mix.second!=ring[(sector+1)&7] || fabsf(mix.weight-weight)>.00001f)))return 0;
            }
            // Exercise the actual source palettes throughout the fall. Both legs
            // and arms retain their authored length between adjacent directions.
            for(unsigned frame=0;frame<61;frame++){
                if(!pose_bank_read(clips[ring[sector]],frame,bones,a) || !pose_bank_read(clips[ring[(sector+1)&7]],frame,bones,b))return 0;
                for(unsigned step=1;step<4;step++){
                    float t=step*.25f;mix_bone_palette(a,b,out,t,pivots,parents,bones,gun,NULL,pose_bank_rest());
                    for(unsigned i=0;i<sizeof limbs/sizeof *limbs;i++){
                        unsigned child=limbs[i],parent=parents[child];
                        float expected=bone_distance(a,pivots,parent,child)*(1-t)+bone_distance(b,pivots,parent,child)*t;
                        if(fabsf(bone_distance(out,pivots,parent,child)-expected)>.002f)return 0;
                    }
                    for(unsigned i=0;i<bones;i++)for(unsigned row=0;row<3;row++)for(unsigned col=0;col<4;col++)if(!isfinite(out[i].row[row][col]))return 0;
                }
            }
        }
    }
    // Crossing +/-pi and a complete camera turn must not change the sector.
    for(int k=-32;k<=32;k++){
        float yaw=k*.173f;DodgeBlend a=dodge_animation_blend(.7f,yaw),b=dodge_animation_blend(.7f,yaw+6.283185307179586f);
        if(a.first!=b.first || a.second!=b.second || fabsf(a.weight-b.weight)>.00001f)return 0;
    }
    const unsigned gets[]={0,2,2,1,1,3,3,0};
    for(unsigned i=0;i<8;i++)if(dodge_animation_getup((i+.25f)*.7853981633974483f)!=gets[i])return 0;
    puts("Verified Xbox eight-way shoot-dodge selection, wrap, four get-ups and every directional arm/leg blend for both Max models and weapon grips");
    return 1;
}

static int verify_loops(unsigned bones){
    const char *const models[]={"max_payne","young_max_payne"};
    const char *const names[]={"stand","run","walkbackward","turnleft","2_run","2_strafeleft","crouch"};
    for(unsigned m=0;m<2;m++)for(unsigned n=0;n<sizeof names/sizeof *names;n++){
        unsigned clip=pose_motion_action(pose_bank_skin(models[m]),names[n]);
        float duration=pose_bank_duration(clip),mix,rate=pose_bank_rate(clip),start=pose_bank_loop_start(clip);
        unsigned first,last,loop=(unsigned)lroundf(start*rate),frames=(unsigned)lroundf(duration*rate);
        if(!clip || !pose_bank_loop_frames(clip,duration-.0001f,bones,&first,&last,&mix) ||
           last!=loop || first!=frames-1 || mix<.9f)return 0;
        if(!pose_bank_loop_frames(clip,duration,bones,&first,&last,&mix) || first!=loop || last!=loop+1 || fabsf(mix)>.0001f)return 0;
        if(!pose_bank_loop_frames(clip,0,bones,&first,&last,&mix) || first || last!=1 || mix)return 0;
        if(!pose_bank_frames(clip,duration,bones,&first,&last,&mix) || first!=last || first!=frames-1 || mix)return 0;
        // Frame cue f must coincide with palette f, including sparse source clips.
        if(!pose_bank_frames(clip,5.25f/rate,bones,&first,&last,&mix) || first!=5 || last!=6 || fabsf(mix-.25f)>.0001f)return 0;
        if(n==6 && loop!=10)return 0;
        if(m==1 && n==0 && loop!=20)return 0;
    }
    unsigned dodge=pose_motion_action(pose_bank_skin(models[0]),"shootdodgeforward"),first,last;float mix;
    if(!pose_bank_loop_frames(dodge,20,bones,&first,&last,&mix) || first!=last || first!=60 || mix)return 0;
    return 1;
}

static int verify_body_sets(unsigned bones,unsigned gun){
    const uint8_t *one=pose_bank_upper(false),*two=pose_bank_upper(true);
    if(!one || !two || bones!=29)return 0;
    unsigned counts[2]={0};
    for(unsigned i=0;i<bones;i++){
        counts[0]+=one[i];counts[1]+=two[i];
        if(i>=4 && i<=12 && (one[i] || two[i]))return 0; // Authored jacket lower body.
        if(i>=17 && i<=19 && (one[i] || !two[i]))return 0; // Left arm differs by set.
    }
    if(counts[0]!=8 || counts[1]!=11 || !one[gun] || !two[gun])return 0;
    if(pose_bank_two_handed("berettadual") || !pose_bank_two_handed_bones("berettadual") ||
       pose_bank_two_handed("deserteagle") || !pose_bank_two_handed_bones("deserteagle") ||
       pose_bank_two_handed("beretta") || pose_bank_two_handed_bones("beretta") ||
       !pose_bank_two_handed("coltcommando") || !pose_bank_two_handed_bones("coltcommando"))return 0;
    puts("Original one/two-handed body masks and separate dual-pistol posing flags verified");
    return 1;
}

int main(void){
    if(!verify_retail_selection())return 1;
    if(!pose_bank_open() || !verify_cues()){fprintf(stderr,"Invalid dodge or reload frame events\n");return 1;}
    if(!verify_fire_timing()){fprintf(stderr,"Invalid retail weapon timing\n");return 1;}
    if(!verify_motion_rules()){fprintf(stderr,"Invalid lateral foot contacts or combat rules\n");return 1;}
    FILE *bind=campaign_file("bones.bind");
    uint32_t h[2];float pivots[64][3];
    if(!bind || fread(h,sizeof h,1,bind)!=1 || !h[0] || h[0]>64 || h[1]>=h[0] ||
       fread(pivots,sizeof(*pivots),h[0],bind)!=h[0] || fgetc(bind)!=EOF)return 1;
    fclose(bind);
    if(!verify_body_sets(h[0],h[1])){fprintf(stderr,"Invalid authored body bone sets\n");return 1;}
    if(!verify_hierarchy(h[0],pivots,h[1])){fprintf(stderr,"Invalid arm/leg hierarchy blend\n");return 1;}
    if(!verify_loops(h[0])){fprintf(stderr,"Invalid looping pose boundary\n");return 1;}
    if(!verify_shootdodge_blend(h[0],pivots,h[1])){fprintf(stderr,"Invalid retail directional shoot-dodge blend\n");return 1;}
    FILE *tree=campaign_file("bones.tree");uint32_t tree_header[2];int16_t parents[64];
    if(!tree || fread(tree_header,sizeof tree_header,1,tree)!=1 || tree_header[1]!=h[0] ||
       fread(parents,sizeof *parents,h[0],tree)!=h[0] || fgetc(tree)!=EOF)return 1;
    fclose(tree);
    Matrix a[64],b[64],out[64];float limb_error=0;
    const char *const players[]={"max_payne","young_max_payne"};
    for(unsigned m=0;m<2;m++){
        unsigned skin=pose_bank_skin(players[m]),put=pose_motion_action(skin,"holsterweapon"),get=pose_motion_action(skin,"getweapon");
        unsigned first,last;float mix;
        if(!pose_bank_frames(put,pose_bank_duration(put),h[0],&first,&last,&mix) ||
           !pose_bank_read(put,last,h[0],a) || !pose_bank_read(get,0,h[0],b))return 1;
        for(unsigned bone=0;bone<h[0];bone++)for(unsigned row=0;row<3;row++)for(unsigned col=0;col<4;col++)
            if(fabsf(a[bone].row[row][col]-b[bone].row[row][col])>.001f){
                fprintf(stderr,"Holster/draw pose discontinuity for %s at bone %u\n",players[m],bone);return 1;
            }
        reloads=weapon_animation=0;
        pose_events(7,get,-1,0,message);
        pose_events(7,get,0,pose_bank_duration(get),message);
        if(reloads!=1 || weapon_animation!=1){fprintf(stderr,"Incorrect draw frame cues for %s\n",players[m]);return 1;}
    }
    FILE *pak=campaign_file("poses.pak");uint32_t header[4];
    if(!pak || fread(header,sizeof header,1,pak)!=1 || header[3]!=h[0])return 1;
    unsigned long pairs=0;
    for(unsigned clip=1;clip<=pose_bank_count();clip++){
        uint32_t frames;
        if(fseek(pak,16+96L*header[1]+16L*(clip-1)+4,SEEK_SET) || fread(&frames,4,1,pak)!=1 ||
           !pose_bank_read(clip,0,h[0],a))return 1;
        for(unsigned frame=1;frame<frames;frame++){
            if(!pose_bank_read(clip,frame,h[0],b))return 1;
            for(unsigned step=1;step<=3;step++){
                float t=step*.25f;
                mix_bone_palette(a,b,out,t,pivots,parents,h[0],h[1],NULL,pose_bank_rest());
                const unsigned limbs[]={14,15,18,19,21,22,25,26};
                for(unsigned i=0;i<sizeof limbs/sizeof *limbs;i++){
                    unsigned child=limbs[i],parent=parents[child];
                    Matrix pa=bone_joint_frame(a[parent],pivots[parent]),pb=bone_joint_frame(b[parent],pivots[parent]);
                    Matrix ca=bone_joint_frame(a[child],pivots[child]),cb=bone_joint_frame(b[child],pivots[child]);
                    Matrix first=bone_relative_frame(&pa,&ca),last=bone_relative_frame(&pb,&cb);
                    // Some authored clips translate joints (for example hanging
                    // at a bar). Preserve that local motion, not a fixed length.
                    float square=0;
                    for(unsigned axis=0;axis<3;axis++){
                        float position=(1-t)*first.row[axis][3]+t*last.row[axis][3];square+=position*position;
                    }
                    float expected=sqrtf(square);
                    float error=fabsf(bone_distance(out,pivots,parent,child)-expected);
                    if(!isfinite(error) || error>.002f){
                        fprintf(stderr,"Temporal limb shortened in %s frame %u bone %u: %.6f m\n",pose_bank_name(clip),frame,child,error);return 1;
                    }
                    if(error>limb_error)limb_error=error;
                }
            }
            pairs++;memcpy(a,b,h[0]*sizeof(Matrix));
        }
    }
    printf("%u clips, %lu adjacent frame pairs; Xbox local-matrix blending, dodge and reload events verified\n",pose_bank_count(),pairs);
    printf("Parent-space temporal sampling: all adjacent frames, maximum limb-length error %.8f m\n",limb_error);
    fclose(pak);pose_bank_close();return 0;
}
