#ifndef MP_GAME_RULES_H
#define MP_GAME_RULES_H
#include <stdbool.h>
#include <stdint.h>
enum GameMode { FUGITIVE,HARD_BOILED,DEAD_ON_ARRIVAL,NEW_YORK_MINUTE,GAME_MODES };
typedef struct {
    float enemy_health,ammo,player_health,aim_radius,aim_height,regenerate,immediate,bullet_gain;
} GameTuning;
typedef struct {
    uint32_t mode,saves,deaths;
    int32_t tier;
    float remaining,elapsed,chapter_time;
    char chapter[48];
} GameRun;
extern GameRun game_run;
extern enum GameMode selected_mode;
const GameTuning *game_tuning(void);
float game_gravity(void);
bool game_rules_enter(const char *stage);
bool game_run_valid(const GameRun *run);
void game_rules_finish(float health_fraction);
unsigned game_save_limit(void);
float game_pill_fraction(void);
unsigned game_kill_bonus(void);
#endif
