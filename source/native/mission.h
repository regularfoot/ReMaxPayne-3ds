#ifndef MP_NATIVE_MISSION_H
#define MP_NATIVE_MISSION_H
#include "actor.h"
#include "scene.h"
enum { STREET_GUNMEN=2, PRACTICE_GUNMEN=6 };
typedef struct {
    Player body;
    Animation animation;
    unsigned hp;
    float attack,think;
    uint8_t portal;
    bool sees,approaching,unarmed;
} Enemy;
typedef struct {
    RoomId room;
    bool van,gate,street_phone,engaged;
    unsigned health,defeated;
    Enemy enemies[PRACTICE_GUNMEN];
    uint8_t rounds[PRACTICE_GUNMEN];
    bool cabinet,rifle;
    unsigned painkillers;
    float recovery;
    unsigned practice_round;
    struct { bool sign,armed,call,landing; } house;
} Mission;
static inline unsigned mission_gunmen(const Mission *m) {
    if (m->practice_round) return PRACTICE_GUNMEN;
    return m->engaged?(m->rifle?ENEMY_SLOTS:STREET_GUNMEN):0;
}
typedef struct { unsigned first,count; } EnemyGroup;
EnemyGroup mission_group(const Mission *m,RoomId room);
RoomId mission_next_area(const Mission *m);
Point house_target(const Mission *m);
unsigned house_page(const Mission *m,const Player *p);
const char *mission_prompt(const Mission *m,const Player *player);
bool mission_use(Mission *m,Player *player);
bool mission_painkiller(Mission *m);
int mission_exit(const Mission *m,const Player *player);
bool mission_lock(Mission *m,const Hit *wall);
bool mission_hurt_enemy(Mission *m,unsigned index,Weapon weapon);
bool mission_update(Mission *m,const Player *player,float dt,bool sound);
#endif
