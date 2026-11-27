/* Exercise each authored event in isolation. Script coverage includes nested sends.
   The ordinary story driver separately checks which sequences reach the next map. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned char *visited;
#define MP_AUDIT_MESSAGE(index) (visited[index]=1)
#include "../source/native/director.c"
#include "campaign_bounds.h"

CampaignProp campaign_props[CAMPAIGN_PROPS];
static unsigned room;
unsigned campaign_room_id(void){return room;}
FILE *campaign_file(const char *name){return fopen(name,"rb");}

static unsigned clock_cues;
static bool restart_clock;
static void clock_message(unsigned id,const char *op,const char *const *args,unsigned count){
    (void)op;(void)args;(void)count;
    clock_cues++;
    if(restart_clock){restart_clock=false;select_pose(id,true);}
}
// Use real Xbox clips and the real director dispatcher. The synthetic frame
// messages count deliveries without starting audio, combat or level changes.
static bool verify_script_clock(void){
    unsigned clip=pose_bank_find(director_text(prog.def[0].model),3);
    unsigned use=pose_bank_action(pose_bank_skin(director_text(prog.def[0].model)),"use");
    float duration=pose_bank_duration(clip),loop=pose_bank_loop_start(clip),rate=pose_bank_rate(clip);
    if(!clip || !use || !pose_bank_loops(clip) || pose_bank_loops(use) || loop<=0 || duration<=loop)return false;
    unsigned sound=0;while(sound<prog.h.messages && strcmp(director_text(prog.messages[sound].op),"a_play3dsound"))sound++;
    if(sound==prog.h.messages)return false;
    Event *saved_events=prog.events;Message saved_message=prog.messages[sound];
    EntityDef def=prog.def[0];EntityState state=director.state[0];DirectorAction external=prog.external;
    Event rows[3]={{EV_POSE,prog.pose_key[3],0,1,sound},
                   {EV_POSE,prog.pose_key[3],(unsigned)lroundf(loop*rate),1,sound},
                   {EV_POSE,prog.pose_key[3],(unsigned)lroundf(duration*rate)-1,1,sound}};
    prog.events=rows;prog.def[0].first_event=0;prog.def[0].events=3;
    prog.messages[sound].target=def.name;prog.external=clock_message;
    director.state[0].dead=false;director.state[0].idle=4;select_pose(0,true);
    clock_cues=0;restart_clock=false;bool ok=true;
    pose_tick(0,.5f/rate);ok=ok && clock_cues==1;
    pose_tick(0,0);ok=ok && clock_cues==1;
    float remaining=duration-director.state[0].pose_seconds+.25f/rate;
    pose_tick(0,remaining);
    ok=ok && clock_cues==4 && fabsf(director.state[0].pose_seconds-loop-.25f/rate)<.00001f;
    unsigned before=clock_cues;
    pose_tick(0,3*(duration-loop));
    ok=ok && clock_cues==before+6 && fabsf(director.state[0].pose_seconds-loop-.25f/rate)<.00001f;
    // A non-looping interaction must finish and hold, including after dt=0.
    director.state[0].custom_pose=use;director.state[0].pose_seconds=0;
    float seconds=pose_bank_duration(use),fps=pose_bank_rate(use);
    rows[1].condition=(unsigned)lroundf(seconds*fps)-1;prog.def[0].events=2;
    before=clock_cues;pose_tick(0,seconds+.1f);pose_tick(0,1);pose_tick(0,0);
    ok=ok && clock_cues==before+2 && director.state[0].pose_seconds==seconds;
    // Restarting this same clip in its first cue cancels the outgoing interval.
    prog.def[0].events=3;rows[1].condition=(unsigned)lroundf(loop*rate);
    select_pose(0,true);before=clock_cues;restart_clock=true;pose_tick(0,duration);
    ok=ok && clock_cues==before+1 && director.state[0].pose_seconds==0;
    // Its replacement starts on the next update and emits frame zero once.
    pose_tick(0,.5f/rate);pose_tick(0,0);ok=ok && clock_cues==before+2;
    prog.events=saved_events;prog.messages[sound]=saved_message;prog.def[0]=def;
    director.state[0].idle=0;select_pose(0,true);
    director.state[0]=state;prog.external=external;restart_clock=false;
    if(ok)fprintf(stderr,"Scripted animation loop starts, holds, overshoot and same-clip restarts verified.\n");
    return ok;
}

static bool verify_saved_objectives(const char *stage){
    if(!strcmp(stage,"part0_level1")){
        const char *keys[3]={0};
        director_recover_objectives(0,3,keys);
        return keys[0] && !strcmp(keys[0],"obj_house_danger0") &&
               keys[1] && !strcmp(keys[1],"obj_house_danger1") && !keys[2];
    }
    if(!strcmp(stage,"p1_l1b-1")){
        const char *keys[3]={0};
        director_recover_objectives(text_id("obj_subway_rats"),5,keys);
        return keys[0] && !strcmp(keys[0],"obj_subway_find_alex") &&
               !keys[1] && keys[2] && !strcmp(keys[2],"obj_subway_rats");
    }
    if(!strcmp(stage,"p2_l2b-2")){
        const char *keys[3]={0};
        director_recover_objectives(0,1,keys);
        return keys[0] && !keys[0][0] && !keys[1] && !keys[2];
    }
    return true;
}

int main(int argc,char **argv){
    bool alternate=argc==3 && !strcmp(argv[2],"--npc");
    if((argc!=2 && !alternate) || !director_open(argv[1],NULL,stderr) || !campaign_bounds_open(argv[1]))return 1;
    if(!verify_saved_objectives(argv[1])){
        fprintf(stderr,"Incorrect legacy objective recovery in %s\n",argv[1]);
        return 6;
    }
    visited=calloc(prog.h.messages+1,1);
    EntityState *initial=malloc(director.count*sizeof *initial);
    if(!visited || !initial)return 2;
    if(!strcmp(argv[1],"tutorial") && !verify_script_clock())return 5;
    memset(visited,0,prog.h.messages+1);
    static CampaignProp base_props[CAMPAIGN_PROPS];
    static PropMotion base_motion[CAMPAIGN_PROPS];
    director_start();director_tick(0);
    Director initial_director=director;
    memcpy(initial,director.state,director.count*sizeof *initial);
    memcpy(base_props,campaign_props,sizeof base_props);memcpy(base_motion,motion,sizeof base_motion);
    unsigned events=0,absent=0;
    for(unsigned actor_index=0;actor_index<director.count;actor_index++){
        unsigned id=alternate?director.count-actor_index-1:actor_index;
        const EntityDef *d=director.def+id;
        for(unsigned row=0;row<d->events;row++){
            unsigned j=d->first_event+(alternate?d->events-1-row:row);
            Event e=prog.events[j];if(!e.count)continue;
            director=initial_director;memcpy(director.state,initial,director.count*sizeof *initial);
            memcpy(campaign_props,base_props,sizeof base_props);memcpy(motion,base_motion,sizeof base_motion);
            if(prog.contacts)memset(prog.contacts,0,(prog.trigger_count*prog.character_count+7)/8+1);
            prog.depth=prog.delivered=0;
            prog.activator=alternate && prog.character_count?prog.characters[row%prog.character_count]:0;
            room=d->room==DIRECTOR_NONE?0:d->room;
            if(e.type==EV_CAMERA){
                camera_events(e.key,(float)e.condition-1,e.condition,e.condition==DIRECTOR_NONE);
            }else if(e.type==EV_POSE){
                unsigned variant=0;while(variant<16 && prog.pose_key[variant]!=e.key)variant++;
                unsigned clip=variant<16?pose_bank_find(director_text(d->model),variant):0;
                if(!clip){absent++;fprintf(stderr,"NOPOSE %s %s %s %s frame %u\n",director_text(d->name),director_text(d->model),director_text(e.key),e.count?director_text(prog.messages[e.first].op):"",e.condition);continue;}
                EntityState *actor=director.state+id;actor->dead=false;actor->custom_pose=clip;actor->pose_variant=variant;
                float rate=pose_bank_rate(clip);
                actor->pose_seconds=fmaxf(0,(e.condition-.25f)/rate);
                pose_tick(id,.5f/rate);
            }else{
                if(e.condition!=DIRECTOR_NONE)director.state[id].state=e.condition;
                director_event(id,e.type,director_text(e.key));
            }
            float dt=alternate?1.f/60:1.f/30;
            director_camera_tick(dt);director_tick(dt);
            if(director.failed || !director_valid_state()){
                fprintf(stderr,"Invalid state in %s entity %s event %u key %s condition %u\n",argv[1],director_text(d->name),e.type,director_text(e.key),e.condition);
                return 3;
            }
            events++;
        }
    }
    unsigned observed=0;
    for(unsigned i=0;i<prog.h.messages;i++)observed+=visited[i]!=0;
    printf("%s\t%u\t%u\t%u\t%u\n",argv[1],events,observed,prog.h.messages,absent);
    bool complete=observed==prog.h.messages && !absent;
    free(initial);free(visited);campaign_bounds_close();director_close();
    return complete?0:4;
}
