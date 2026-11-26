#include "language.h"
#include "mission.h"
#include "phone.h"
#include <math.h>

EnemyGroup mission_group(const Mission *m,RoomId room) {
    if (!m->practice_round) return (EnemyGroup){0,room==STREET?mission_gunmen(m):0};
    switch (room) {
        case STREET: return (EnemyGroup){0,3};
        case SUBWAY: return (EnemyGroup){3,1};
        case SIDE_ALLEY: return (EnemyGroup){4,1};
        case DEAD_END: return (EnemyGroup){5,1};
        default: return (EnemyGroup){0,0};
    }
}

RoomId mission_next_area(const Mission *m) {
    EnemyGroup local=mission_group(m,m->room);
    for (unsigned i=local.first;i<local.first+local.count;i++) if (m->enemies[i].hp) return m->room;
    if (m->room==STREET) {
        if (m->enemies[4].hp) return SIDE_ALLEY;
        if (m->enemies[5].hp) return DEAD_END;
        if (m->enemies[3].hp) return SUBWAY;
    }
    return STREET;
}

Point house_target(const Mission *m) {
    const RoomHeader *h=room_header();
    switch (m->room) {
        case HOME: return !m->house.sign?h->use:!m->house.armed?h->lock:h->exits[0].center;
        case HOME_PHONE: return m->house.call?h->exits[1].center:h->phone;
        case HOME_LIVING: return h->exits[1].center;
        default: return h->use;
    }
}
unsigned house_page(const Mission *m,const Player *p) {
    const RoomHeader *h=room_header();
    if (m->room==HOME && !m->house.sign && room_near(p,h->use,1.6f)) return 6;
    if (m->room==HOME_PHONE && !m->house.call && room_near(p,h->phone,1.6f)) return 7;
    return 0;
}

const char *mission_prompt(const Mission *m,const Player *p) {
    const RoomHeader *h=room_header();
    if (m->room>=HOME) {
        unsigned page=house_page(m,p);
        if (page==6) return MP_TEXT("A   Осмотреть знак на стене","A   Inspect the sign");
        if (page==7) return MP_TEXT("A   Ответить на звонок","A   Answer the phone");
        if (m->room==HOME) return m->house.sign?MP_TEXT("Пройдите через прихожую.","Go through the hallway."):MP_TEXT("Осмотрите знак на стене.","Look at the sign on the wall.");
        if (m->room==HOME_PHONE) return m->house.call?MP_TEXT("Найдите лестницу за гостиной.","Find the stairs past the living room."):MP_TEXT("Найдите звонящий телефон.","Find the ringing phone.");
        if (m->room==HOME_LIVING) return MP_TEXT("Поднимитесь по лестнице.","Go upstairs.");
        return m->house.landing?MP_TEXT("Конец фрагмента. START   Меню","End of demo. START   Menu"):MP_TEXT("Найдите верхнюю площадку.","Find the upstairs landing.");
    }
    if (!m->health) return MP_TEXT("Макс погиб. A   Повторить","Max is dead. A   Retry");
    if (m->rifle && m->defeated==mission_gunmen(m)) {
        if (m->room==STREET && room_near(p,h->use,1.9f))
            return m->practice_round?MP_TEXT("A   Начать новый раунд","A   Start next round"):MP_TEXT("A   Начать тренировку","A   Start training");
        return m->practice_round?MP_TEXT("Раунд окончен. Вернитесь к выключателю.","Round over. Return to the switch."):MP_TEXT("Обучение пройдено. Найдите выключатель.","Tutorial complete. Find the switch.");
    }
    if (m->practice_round) {
        RoomId next=mission_next_area(m);
        if (next==m->room) return MP_TEXT("Устраните противников в этой зоне.","Clear out the enemies here.");
        if (next==SIDE_ALLEY) return MP_TEXT("Проверьте боковой переулок.","Check the side alley.");
        if (next==DEAD_END) return MP_TEXT("Проверьте тупик.","Check the dead end.");
        if (next==SUBWAY) return MP_TEXT("Проверьте вход в метро.","Check the subway entrance.");
        return MP_TEXT("Вернитесь на улицу.","Go back to the street.");
    }
    if (m->room==SIDE_ALLEY || m->room==DEAD_END) return MP_TEXT("Вернитесь на улицу.","Go back to the street.");
    if (m->room==START_ROOM) {
        if (!p->answered) return room_near(p,h->phone,1.5f)?MP_TEXT("A   Ответить на звонок","A   Answer the phone"):MP_TEXT("Найдите звонящий телефон.","Find the ringing phone.");
        if (!m->van) return room_near(p,h->use,2.25f)?MP_TEXT("A   Открыть фургон","A   Open the van"):MP_TEXT("Откройте задние двери фургона.","Open the van's rear doors.");
        if (!m->gate) return MP_TEXT("Сбейте выстрелом замок на воротах.","Shoot the padlock off the gate.");
        return MP_TEXT("Пройдите за ворота и сверните в переулок.","Go through the gate and turn into the alley.");
    }
    if (m->room==ALLEY) return MP_TEXT("L   Перепрыгните ящики и выйдите на улицу.","L   Jump over the crates to the street.");
    if (m->room==SUBWAY) {
        if (m->cabinet) return MP_TEXT("Поднимитесь к фургону на улице.","Go upstairs to the van on the street.");
        return room_near(p,h->use,1.65f)?MP_TEXT("A   Открыть аптечку","A   Open the medicine cabinet"):MP_TEXT("Найдите аптечку.","Find the medicine cabinet.");
    }
    if (!m->street_phone) return room_near(p,h->phone,1.65f)?MP_TEXT("A   Ответить на звонок","A   Answer the phone"):MP_TEXT("Найдите телефон у выключателя.","Find the phone next to the switch.");
    if (!m->engaged) return room_near(p,h->use,1.9f)?MP_TEXT("A   Начать перестрелку","A   Start the firefight"):MP_TEXT("Нажмите выключатель рядом с телефоном.","Press the switch next to the phone.");
    if (m->rifle) return MP_TEXT("Устраните троих противников из переулка.","Take out the three enemies in the alley.");
    if (m->cabinet) return room_near(p,h->rifle_van,2.25f)?MP_TEXT("A   Взять винтовку из фургона","A   Take the rifle from the van"):MP_TEXT("Найдите фургон у входа в метро.","Find the van by the subway entrance.");
    if (m->defeated==STREET_GUNMEN) return MP_TEXT("Спуститесь в метро и найдите аптечку.","Go down to the subway and find the medicine cabinet.");
    return MP_TEXT("Устраните двоих противников.","Take out the two enemies.");
}

static unsigned magazine(const Enemy *e){return e->animation.weapon==SHOTGUN?7:18;}

static void begin_encounter(Mission *m,const Point *spawns) {
    m->engaged=true;
    m->defeated=0;
    for (unsigned i=0;i<mission_gunmen(m);i++) {
        Enemy *e=&m->enemies[i];
        *e=(Enemy){.hp=3,.body.position=spawns[i],.attack=2.3f+i*.8f,.approaching=m->rifle};
        e->animation.weapon=(!m->rifle && i==1)?SHOTGUN:BERETTA;
        m->rounds[i]=magazine(e);
    }
}

bool mission_use(Mission *m,Player *p) {
    const RoomHeader *h=room_header();
    if (m->room==START_ROOM && !p->answered && room_near(p,h->phone,1.5f)) p->answered=true;
    else if (m->room==START_ROOM && p->answered && !m->van && room_near(p,h->use,2.25f)) m->van=true;
    else if (m->room==STREET && !m->street_phone && room_near(p,h->phone,1.65f)) m->street_phone=true;
    else if (m->room==STREET && m->street_phone && !m->engaged && room_near(p,h->use,1.9f)) {
        begin_encounter(m,h->enemies);
    } else if (m->room==STREET && m->cabinet && !m->rifle && room_near(p,h->rifle_van,2.25f)) {
        m->rifle=true;
        begin_encounter(m,h->rifle_spawns);
    } else if (m->room==STREET && m->rifle && m->defeated==mission_gunmen(m) && room_near(p,h->use,1.9f)) {
        ++m->practice_round;
        m->defeated=0;
        for (unsigned i=0;i<PRACTICE_GUNMEN;i++) {
            Enemy *e=&m->enemies[i];
            Point spawn=i<2?h->enemies[i]:h->practice_spawns[i-2];
            *e=(Enemy){.hp=3,.body.position=spawn,.attack=2.3f};
            e->animation.weapon=i==1?SHOTGUN:BERETTA;
            m->rounds[i]=magazine(e);
        }
    } else if (m->room==SUBWAY && !m->cabinet && m->defeated==STREET_GUNMEN && room_near(p,h->use,1.65f)) {
        m->cabinet=true;
        m->painkillers+=4;
    } else return false;
    return true;
}

int mission_exit(const Mission *m,const Player *p) {
    if (m->room<HOME && (!m->gate || (!m->practice_round && m->room==STREET && m->defeated<mission_gunmen(m)))) return -1;
    for (unsigned i=0;i<sizeof room_header()->exits/sizeof(Portal);i++) {
        const Portal *exit=&room_header()->exits[i];
        if (exit->destination==SUBWAY && !m->cabinet && m->defeated<STREET_GUNMEN) continue;
        if (room_portal(p,exit)) return i;
    }
    return -1;
}
bool mission_lock(Mission *m,const Hit *hit) {
    if (m->room!=START_ROOM || !m->van || m->gate) return false;
    Point k=room_header()->lock;
    float x=hit->point.x-k.x,y=hit->point.y-k.y,z=hit->point.z-k.z;
    if (fabsf(z)>.48f || x*x+y*y>.38f*.38f) return false;
    m->gate=true;
    return true;
}
bool mission_hurt_enemy(Mission *m,unsigned i,Weapon weapon) {
    Enemy *e=&m->enemies[i];
    unsigned damage=weapon==BERETTA?1:3;
    e->hp=e->hp>damage?e->hp-damage:0;
    if (e->hp) return false;
    m->defeated++;
    actor_action(&e->animation,IDLE);
    actor_step(&e->animation,FALL,1,0);
    return true;
}
bool mission_painkiller(Mission *m) {
    if (!m->painkillers || !m->health || m->health==100 || m->recovery>0) return false;
    m->painkillers--;
    m->recovery=2.f;
    return true;
}
bool mission_update(Mission *m,const Player *p,float dt,bool sound) {
    unsigned before=m->health;
    if (m->recovery>0 && m->health) {
        float remaining=fmaxf(0,m->recovery-dt);
        unsigned gain=(unsigned)ceilf(m->recovery*12.5f)-(unsigned)ceilf(remaining*12.5f);
        m->health+=gain;
        if (m->health>=100) {m->health=100;remaining=0;}
        m->recovery=remaining;
    }
    if (!m->health) return m->health!=before;
    EnemyGroup group=mission_group(m,m->room);
    for (unsigned i=group.first;i<group.first+group.count;i++) {
        Enemy *e=&m->enemies[i];
        if (!e->hp) { actor_step(&e->animation,FALL,1,dt);continue; }
        Point a=e->body.position,b=p->position;
        float dx=b.x-a.x,dz=b.z-a.z,dy=b.y-a.y;
        float dist=hypotf(dx,dz),len=hypotf(dist,dy);
        if (len<.01f) continue;
        e->body.yaw=atan2f(dx,dz);
        e->think-=dt;
        if (e->think<=0) {
            Hit wall;
            Point eye=a;eye.y+=1.32f;
            e->sees=!scene_trace(eye,(Point){dx/len,dy/len,dz/len},len,&wall);
            e->think=.12f;
        }
        bool moving=false;
        if (e->approaching) {
            Point goal=room_header()->rifle_destinations[i];
            if (hypotf(goal.x-a.x,goal.z-a.z)<1.f || (e->sees && dist<18)) e->approaching=false;
        }
        if (e->approaching || !e->sees || dist>10) {
            Point goal=e->approaching?room_header()->rifle_destinations[i]:b;
            Point step=room_pursue(a,goal,e->approaching || e->sees);
            e->body.yaw=atan2f(step.x,step.z);
            room_move(&e->body,step.x*1.9f*dt,step.z*1.9f*dt);
            moving=hypotf(e->body.position.x-a.x,e->body.position.z-a.z)>.001f;
            if (!moving) e->approaching=false;
        }
        bool loading=e->animation.action==RELOAD;
        actor_step(&e->animation,moving?RUN:IDLE,1,dt);
        if(loading && e->animation.action!=RELOAD)m->rounds[i]=magazine(e);
        if(!m->rounds[i]){
            if(e->animation.action==IDLE){
                actor_action(&e->animation,RELOAD);
                if(sound)enemy_sound(e->animation.weapon==SHOTGUN,true);
            }
            continue;
        }
        e->attack-=dt;
        if (e->sees && dist<23 && e->attack<=0 && m->health) {
            e->body.yaw=atan2f(dx,dz);
            // A visible head is not enough if the muzzle is behind cover.
            Point muzzle=actor_muzzle(&e->body,&e->animation);
            Point target=p->position;target.y+=1.32f;
            Point ray={target.x-muzzle.x,target.y-muzzle.y,target.z-muzzle.z};
            float range=sqrtf(ray.x*ray.x+ray.y*ray.y+ray.z*ray.z);
            Hit cover;
            if (range<.001f || scene_trace(muzzle,(Point){ray.x/range,ray.y/range,ray.z/range},range,&cover)) {
                e->attack=.18f;
                continue;
            }
            m->rounds[i]--;actor_action(&e->animation,FIRE);
            e->attack=e->animation.weapon==SHOTGUN?2.7f:1.9f;
            unsigned damage=e->animation.weapon==SHOTGUN && dist<8?12:6;
            m->health=m->health>damage?m->health-damage:0;
            if (sound) enemy_sound(e->animation.weapon==SHOTGUN,false);
        }
    }
    return m->health!=before;
}
