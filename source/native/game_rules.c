#include "game_rules.h"
#include "campaign_file.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
typedef struct {uint32_t saves;float pill;GameTuning tiers[3];} Profile;
typedef struct {
    char stage[32],chapter[48];
    float seconds,bonus,low_health,high_health,few_deaths,many_deaths,fast,slow,hard_health,hard_deaths,hard_time,gravity;
} LevelRules;
static Profile profiles[GAME_MODES];
static LevelRules level;
GameRun game_run;
enum GameMode selected_mode;
float game_gravity(void){return level.gravity;}
const GameTuning *game_tuning(void){return &profiles[game_run.mode].tiers[game_run.tier+1];}
unsigned game_save_limit(void){return profiles[game_run.mode].saves;}
float game_pill_fraction(void){return profiles[game_run.mode].pill;}
unsigned game_kill_bonus(void){return (unsigned)level.bonus;}
bool game_run_valid(const GameRun *s) {
    return s->mode<GAME_MODES && s->tier>=-1 && s->tier<=1 && isfinite(s->remaining) &&
           isfinite(s->elapsed) && isfinite(s->chapter_time) && s->elapsed>=0 && s->chapter_time>=0 &&
           memchr(s->chapter,0,sizeof s->chapter) && !strcmp(s->chapter,level.chapter);
}
bool game_rules_enter(const char *stage) {
    FILE *f=campaign_file("difficulty.bin");if(!f)return false;
    struct {char tag[4];unsigned count;} h;
    bool found=false;
    if(fread(&h,sizeof h,1,f)==1 && (!memcmp(h.tag,"MPR1",4) || !memcmp(h.tag,"MPR2",4)) && h.count<=128 &&
       fread(profiles,sizeof(Profile),GAME_MODES,f)==GAME_MODES) {
        for(unsigned i=0;i<h.count;i++) {
            size_t bytes=h.tag[3]=='2'?sizeof level:sizeof level-sizeof(float);
            level.gravity=20;
            if(fread(&level,bytes,1,f)!=1 || !isfinite(level.gravity) || level.gravity<=0 || !memchr(level.stage,0,sizeof level.stage) ||
               !memchr(level.chapter,0,sizeof level.chapter))break;
            if(!strcmp(level.stage,stage)){found=true;break;}
        }
    }
    fclose(f);if(!found)return false;
    if(strcmp(game_run.chapter,level.chapter)) {
        strcpy(game_run.chapter,level.chapter);
        game_run.saves=game_run.deaths=0;game_run.chapter_time=0;game_run.remaining=level.seconds;
    }
    return true;
}
void game_rules_finish(float health) {
    if(game_run.mode>HARD_BOILED)return;
    if(game_run.deaths>=level.many_deaths || (health<level.low_health && game_run.chapter_time>level.slow))
        game_run.tier=game_run.tier>-1?game_run.tier-1:-1;
    else if(game_run.deaths<=level.hard_deaths && (health>=level.hard_health || game_run.chapter_time<level.hard_time))
        game_run.tier=game_run.tier<1?game_run.tier+1:1;
}
