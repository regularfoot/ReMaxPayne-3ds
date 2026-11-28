#include "language.h"
#include "version.h"
#include "campaign_file.h"
#include "render.h"
#include "phone.h"
#include "touch.h"
#include "gyro.h"
#include "comic.h"
#include "ui.h"
#include "campaign.h"
#include "movie.h"
#include "audio_bank.h"
#include "async_log.h"
#include <citro3d.h>
#include <3ds.h>
#include <malloc.h>
#include <math.h>
#include <stdio.h>
#include <sys/stat.h>
#include <string.h>

u32 __ctru_heap_size=9u<<20;
u32 __ctru_linear_heap_size=18u<<20;

static FILE *journal;
static void memory_report(const char *stage,u64 since) {
    if (!journal) return;
    fprintf(journal,"%s elapsed_ms=%llu heap_used=%lu linear_free=%lu\n",stage,
            (unsigned long long)((svcGetSystemTick()-since)*1000/SYSCLOCK_ARM11),
            (unsigned long)mallinfo().uordblks,(unsigned long)linearSpaceFree());
    fflush(journal);
}

static float axis(int amount) {
    int magnitude=amount<0?-amount:amount;
    int dead=5+10*ui_settings.deadzone;
    if (magnitude<=dead) return 0;
    return copysignf(fminf(1,(magnitude-dead)/(150.f-dead)),(float)amount);
}

static const char *areas[]={"StartRoom","Alley2","Street","Subway","Side alley","Dead end",
                            "Entry hall","Telephone","Living room","Stairs"};
typedef struct { unsigned ammo,reserve,capacity; } Magazine;
static void instructions(const Mission *m,const char *prompt,const Magazine *mag,Weapon weapon,bool loading,unsigned zoom,bool stance,bool crouched) {
    unsigned count=mission_gunmen(m);
    static const char *const map_names[]={MP_TEXT("Двор","Yard"),MP_TEXT("Переулок","Alley"),MP_TEXT("Улица","Street"),MP_TEXT("Метро","Subway"),MP_TEXT("Боковой переулок","Side Alley"),MP_TEXT("Тупик","Dead End"),MP_TEXT("Прихожая","Hallway"),MP_TEXT("Телефон","Phone"),MP_TEXT("Гостиная","Living Room"),MP_TEXT("Лестница","Stairs")};
    ui_begin_update();
    ui_hud(map_names[m->room],prompt,NULL,m->health,m->painkillers,
           m->room>=HOME && !m->house.armed?-1:(int)weapon,mag->ammo,mag->reserve,loading,zoom,m->van,
           m->practice_round,count>m->defeated?count-m->defeated:0,false);
    if(stance)ui_stance(crouched);
    uint32_t owned=m->room>=HOME && !m->house.armed?0:1u;
    if(m->van)owned|=1u<<3;
    if(m->rifle)owned|=1u<<9;
    ui_weapon_palette(owned,owned,weapon==SHOTGUN?3:weapon==RIFLE?9:0);
    ui_end_update();
}
static void guidance(const Mission *m,const Player *p) {
    const RoomHeader *h=room_header();
    Point target;
    if (m->room>=HOME) target=house_target(m);
    else if (m->rifle && m->defeated==mission_gunmen(m) && m->room==STREET) target=h->use;
    else if (m->room==SIDE_ALLEY || m->room==DEAD_END || (m->practice_round && m->room==SUBWAY)) {
        EnemyGroup group=mission_group(m,m->room);
        target=group.count && m->enemies[group.first].hp?m->enemies[group.first].body.position:h->exits[0].center;
    } else if (m->practice_round && m->room==STREET && mission_next_area(m)!=STREET) {
        RoomId next=mission_next_area(m);
        target=h->exits[next==SIDE_ALLEY?2:next==DEAD_END?3:1].center;
        if (next==SUBWAY && p->position.y>-4.25f && !(fabsf(p->position.x-target.x)<1.85f && p->position.z>7.5f))
            target.z=11.6f;
    } else if (m->room==START_ROOM) target=!p->answered?h->phone:!m->van?h->use:!m->gate?h->lock:h->exits[0].center;
    else if (m->room==ALLEY) target=h->exits[1].center;
    else if (m->room==SUBWAY) target=m->cabinet?h->exits[0].center:h->use;
    else if (m->cabinet && !m->rifle) target=h->rifle_van;
    else if (m->defeated==STREET_GUNMEN && !m->rifle) {
        target=h->exits[1].center;
        // The lower landing is directly underneath the street. Lead Max around the railing first.
        if (p->position.y>-4.25f && !(fabsf(p->position.x-target.x)<1.85f && p->position.z>7.5f))
            target.z=11.6f;
    }
    else if (!m->engaged) target=m->street_phone?h->use:h->phone;
    else {
        float best=1e9f;
        target=p->position;
        EnemyGroup group=mission_group(m,m->room);
        for (unsigned i=group.first;i<group.first+group.count;i++) if (m->enemies[i].hp) {
            Point e=m->enemies[i].body.position;
            float d=hypotf(e.x-p->position.x,e.z-p->position.z);
            if (d<best) {best=d;target=e;}
        }
    }
    // From the lower landing the way to any street-level objective is up the stairs.
    if (m->room==STREET && p->position.y<-4.25f && target.y>=-4.25f) target=(Point){0,-4,11.6f};
    float x=target.x-p->position.x,z=target.z-p->position.z;
    float a=remainderf(atan2f(x,z)-p->yaw,6.2831853f);
    ui_bearing(a,hypotf(x,z),m->health>0);
}

static bool enter_area(const Mission *m,RoomId dest,bool sound) {
    if (sound && dest==SUBWAY)medicine_preload();
    bool open=dest==SUBWAY?m->cabinet:dest==STREET?m->rifle:m->van;
    unsigned count=mission_group(m,dest).count;
    return room_switch(dest,open,m->gate) && (!count || actor_enemies(count));
}
// The two halves of the house staircase meet inside a portal. Cross in small
// steps so collision can follow the individual risers in the destination room.
static void house_cross(Player *p,const Portal *exit) {
    float depth=(p->position.x-exit->center.x)*exit->normal.x+(p->position.z-exit->center.z)*exit->normal.z;
    p->position.x+=exit->offset.x-(depth+.08f)*exit->normal.x;
    p->position.y+=exit->offset.y;
    p->position.z+=exit->offset.z-(depth+.08f)*exit->normal.z;
    Point above=p->position;above.y+=.50f;
    Hit floor;
    if (scene_trace(above,(Point){0,-1,0},1.1f,&floor) && fabsf(floor.normal.y)>.62f)
        p->position.y=floor.point.y;
    for (unsigned i=0;i<10;i++) room_move(p,-exit->normal.x*.08f,-exit->normal.z*.08f);
}

static const char *encounter_name(const Mission *m) {
    return m->practice_round?"Practice":m->rifle?"Rifle":"Street";
}

typedef struct {
    const Player *player;
    const Animation *animation;
    const Effects *effects;
    const Mission *mission;
    Camera entry;
    PauseCamera orbit;
    unsigned zoom;
} TutorialPause;
static void tutorial_pause_frame(float dt,void *context){
    TutorialPause *pause=context;
    if(dt>=0)scene_pause_step(&pause->orbit,dt,scene_trace);
    room_render(pause->player,pause->animation,dt<0?&pause->entry:&pause->orbit.view,pause->effects,pause->mission,dt<0?pause->zoom:1);
}
static bool play_level(bool house,UiChoice *next) {
    RoomId first=house?HOME:START_ROOM;
    if (house && !comic_read(3,5,journal)) return false;
    u64 begin=svcGetSystemTick();
    if (!room_load(first)) return false;
    Player p={.position=room_header()->spawn,.yaw=room_header()->yaw};
    bool sound=phone_open(house);
    if (house && journal) fputs("PROLOGUE start P0_L1\n",journal);
    if (journal) fprintf(journal,"PHONE audio=%s\n",sound?"ready":"unavailable");
    memory_report("ROOM",begin);
    Animation animation={0};
    Camera camera={0};
    Jump jump={0};
    Player checkpoint=p;
    Point last_ground=p.position;
    Effects effects={0};
    scene_camera(&camera,&p,0);
    Mission mission={.room=first,.health=100};
    TouchInput touch={0};
    bool last_stereo=false;
    bool stance=actor_campaign_moves();
    Magazine inventory[WEAPON_COUNT]={{18,72,18},{0,0,7},{0,0,5}};
    if (house) inventory[BERETTA]=(Magazine){0,0,18};
    Magazine checkpoint_inventory[WEAPON_COUNT];
    memcpy(checkpoint_inventory,inventory,sizeof inventory);
    Mission checkpoint_mission=mission;
    Weapon checkpoint_weapon=BERETTA;
    unsigned zoom=1;
    bool ok=true;
    const char *prompt=mission_prompt(&mission,&p);
    float cooldown=0;
    instructions(&mission,prompt,inventory,BERETTA,false,zoom,stance,p.crouched);
    gyro_prepare(ui_settings.gyro);
    u64 previous=svcGetSystemTick(),sample=previous;
    unsigned frames=0,total=0,rendered_zoom=1;
    while (aptMainLoop()) {
        gyro_set_active(ui_settings.gyro && mission.health);
        hidScanInput();
        u32 down=hidKeysDown(),held=hidKeysHeld();
        touchPosition point={0};
        if (held&KEY_TOUCH) hidTouchRead(&point);
        TouchAction gesture=touch_update(&touch,held&KEY_TOUCH,point.px,point.py,
            (stance?TOUCH_BIT(TOUCH_CROUCH):0)|TOUCH_BIT(TOUCH_WEAPON)|(ui_touch_shortcuts()?(TOUCH_BIT(TOUCH_WEAPON)|TOUCH_BIT(TOUCH_FIRE)|(stance?TOUCH_BIT(TOUCH_JUMP):0)):0));
        down=ui_game_keys(down);held=ui_game_keys(held);
        uint32_t owned=house && !mission.house.armed?0:1u;
        if(mission.van)owned|=1u<<3;
        if(mission.rifle)owned|=1u<<9;
        int current=animation.weapon==SHOTGUN?3:animation.weapon==RIFLE?9:0;
        WeaponChoice pick=ui_weapon_input(down&KEY_TOUCH,point.px,point.py,mission.health && animation.action!=RELOAD,owned,current);
        if(pick.consumed){touch.origin=TOUCH_WEAPON;gesture=(TouchAction){0};}
        if(pick.selected!=current){animation.weapon=pick.selected==3?SHOTGUN:pick.selected==9?RIFLE:BERETTA;actor_action(&animation,IDLE);zoom=1;}
        if(pick.changed)prompt=NULL;
        if(ui_touch_shortcuts() && gesture.pressed==TOUCH_FIRE)down|=KEY_R;
        if (down&KEY_START) {
            ui_weapon_close();
            gyro_set_active(false);
            if(sound) for(int ch=0;ch<3;ch++) ndspChnSetPaused(ch,true);
            TutorialPause pause={.player=&p,.animation=&animation,.effects=&effects,.mission=&mission,.entry=camera,.zoom=rendered_zoom};
            scene_pause_begin(&pause.orbit,&camera,&p);
            UiChoice action=ui_pause_menu(false,tutorial_pause_frame,&pause);
            if(action==UI_JOURNAL){
                const char items[3][96]={{0}};ui_journal(prompt,items,"");action=UI_RESUME;
            }
            if(action!=UI_RESUME){*next=action;break;}
            if(sound) {for(int ch=0;ch<3;ch++) ndspChnSetPaused(ch,false);phone_volume();}
            gyro_prepare(ui_settings.gyro);
            previous=sample=svcGetSystemTick();frames=0;touch=(TouchInput){0};prompt=NULL;
            instructions(&mission,mission_prompt(&mission,&p),&inventory[animation.weapon],animation.weapon,animation.action==RELOAD,zoom,stance,p.crouched);
            continue;
        }
        if (!mission.health) {
            if (!(down&KEY_A)) {gspWaitForVBlank();continue;}
            if (mission.room!=checkpoint_mission.room &&
                !(ok=enter_area(&checkpoint_mission,checkpoint_mission.room,sound))) break;
            mission=checkpoint_mission;
            mission.health=100;mission.recovery=0;
            memcpy(inventory,checkpoint_inventory,sizeof inventory);
            p=checkpoint;last_ground=p.position;cooldown=0;zoom=1;
            animation=(Animation){.weapon=checkpoint_weapon};camera=(Camera){0};effects=(Effects){0};
            jump=(Jump){0};previous=svcGetSystemTick();scene_camera(&camera,&p,0);
            down&=~KEY_A;
            prompt=NULL;
            if (journal) fprintf(journal,"RETRY %s\nFIGHT %s enemies=%u\n",
                                 encounter_name(&mission),encounter_name(&mission),mission_gunmen(&mission));
        }
        bool switched=mission.van && (down&KEY_Y) && animation.action!=RELOAD;
        if (switched) {
            animation.weapon=(Weapon)((animation.weapon+1)%(mission.rifle?WEAPON_COUNT:2));
            actor_action(&animation,IDLE);zoom=1;
        }
        Magazine *mag=&inventory[animation.weapon];
        circlePosition move={0},look={0};
        hidCircleRead(&move);
        hidCstickRead(&look);
        u64 now=svcGetSystemTick();
        float dt=fminf(.05f,(float)(now-previous)/SYSCLOCK_ARM11);
        previous=now;
        float turn=cstick_axis(look.dx,ui_settings.deadzone)+(!!(held&KEY_DRIGHT)-!!(held&KEY_DLEFT));
        float pitch=cstick_axis(look.dy,ui_settings.deadzone)+(!!(held&KEY_DUP)-!!(held&KEY_DDOWN));
        GyroLook motion=gyro_read(dt,ui_settings.gyro_horizontal,ui_settings.gyro_vertical);
        float manual_yaw=touch_look_angle(turn,gesture.dx,dt,ui_settings.stick_horizontal,ui_settings.touch_horizontal,false);
        float manual_pitch=touch_look_angle(pitch,gesture.dy,dt,ui_settings.stick_vertical,ui_settings.touch_vertical,true);
        p.yaw+=(manual_yaw+motion.yaw)/zoom;
        p.pitch=fminf(1.25f,fmaxf(-1.25f,p.pitch+(manual_pitch+motion.pitch)*(ui_settings.inverted?-1.f:1.f)/zoom));
        bool crouch_tap=gesture.pressed==TOUCH_CROUCH;
        bool jump_tap=gesture.pressed==TOUCH_JUMP;
        if(crouch_tap || jump_tap)touch.origin=TOUCH_NONE;
        bool changed_stance=crouch_tap && scene_toggle_crouch(&p);
        float x=axis(move.dx),z=axis(move.dy),len=sqrtf(x*x+z*z);
        if (len>.0001f) { x/=len; z/=len; }
        float s=sinf(p.yaw),c=cosf(p.yaw);
        Point before=p.position;
        if(p.crouched && hypotf(x,z)>.04f)scene_toggle_crouch(&p);
        float stride=p.crouched?0:jump.airborne?2.5f:5.25f;
        scene_move_player(&p,&jump,(c*x+s*z)*stride*dt,(c*z-s*x)*stride*dt,dt,((down&KEY_B) || jump_tap) && !p.crouched);
        if (!jump.airborne) last_ground=p.position;
        else if (p.position.y<last_ground.y-12.f) {
            p.position=last_ground;jump=(Jump){0};scene_camera(&camera,&p,0);
        }
        room_hint(p.position,p.position.x-before.x,p.position.z-before.z);
        float distance=hypotf(p.position.x-before.x,p.position.z-before.z);
        Clip gait=IDLE;
        if (distance>.0001f) {
            if (fabsf(x)>fabsf(z)) gait=x<0?STRAFE_LEFT:STRAFE_RIGHT;
            else if (z<0) gait=BACKPEDAL;
            else gait=len>.78f?RUN:WALK;
        }
        if (jump.airborne) gait=JUMP;
        Clip old_gait=animation.gait;
        bool was_loading=animation.action==RELOAD;
        float speed=gait==IDLE?1:fminf(1.4f,fmaxf(.45f,distance/fmaxf(.001f,dt)/3.1f));
        if(stance){
            float dx=p.position.x-before.x,dz=p.position.z-before.z;
            actor_locomotion_style(&animation,dx*s+dz*c,dx*c-dz*s,p.crouched,jump.airborne?JUMP:IDLE,dt,true);
            gait=animation.gait;
        }else actor_step(&animation,gait,speed,dt);
        if (journal && old_gait!=gait) fprintf(journal,"GAIT %u\n",(unsigned)gait);
        scene_age(&effects,dt);
        if (zoom>1) scene_scope(&camera,&p);
        else scene_camera(&camera,&p,dt);
        cooldown=fmaxf(0,cooldown-dt);
        bool hud_changed=switched || changed_stance;
        if ((down&KEY_SELECT) && mission_painkiller(&mission)) {
            hud_changed=true;
            if (sound) medicine_sound(true);
            if (journal) fprintf(journal,"PAINKILLER remaining=%u hp=%u\n",mission.painkillers,mission.health);
        }
        if (was_loading && animation.action!=RELOAD) {
            unsigned count=mag->capacity-mag->ammo;
            if (count>mag->reserve) count=mag->reserve;
            mag->ammo+=count;
            mag->reserve-=count;
            hud_changed=true;
            if (journal) fprintf(journal,"RELOADED ammo=%u reserve=%u\n",mag->ammo,mag->reserve);
        }
        bool wants_reload=(down&KEY_X) || ((down&KEY_R) && !mag->ammo);
        if (wants_reload && animation.action!=RELOAD && mag->ammo<mag->capacity && mag->reserve) {
            actor_action(&animation,RELOAD);
            if (sound) weapon_sound(true,animation.weapon);
            hud_changed=true;
        } else if ((down&KEY_R) && !cooldown && mag->ammo && animation.action!=RELOAD) {
            actor_action(&animation,FIRE);
            --mag->ammo;
            static const float shot_delay[]={.16f,.70f,1.f};
            cooldown=shot_delay[animation.weapon];
            Point muzzle=actor_muzzle(&p,&animation);
            Hit hit={0};
            Point shoulder=p.position;
            shoulder.y+=p.crouched?.91f:1.42f;
            ShotTarget targets[ENEMY_SLOTS];
            EnemyGroup group=mission_group(&mission,mission.room);
            for (unsigned slot=0;slot<group.count;slot++) {
                Enemy *foe=&mission.enemies[group.first+slot];
                targets[slot]=(ShotTarget){.feet=foe->body.position,.active=foe->hp>0,.crouched=foe->body.crouched};
            }
            int enemy=-1;
            bool struck=scene_fire(&effects,&camera,shoulder,muzzle,&hit,targets,group.count,&enemy);
            if (enemy>=0) {
                unsigned id=group.first+(unsigned)enemy;
                bool dead=mission_hurt_enemy(&mission,id,animation.weapon);
                if (journal) fprintf(journal,"ENEMY %u hp=%u down=%u\n",id+1,mission.enemies[id].hp,dead);
                if (dead && mission.rifle && mission.defeated==mission_gunmen(&mission) && journal) {
                    if (mission.practice_round) fprintf(journal,"PRACTICE clear round=%u\n",mission.practice_round);
                    else fputs("TRAINING complete\n",journal);
                }
            } else if (struck && mission_lock(&mission,&hit)) {
                if (!(ok=room_geometry(mission.van,true))) break;
                if (sound) door_sound();
                if (journal) fputs("GATE open\n",journal);
            }
            if (sound) weapon_sound(false,animation.weapon);
            if (journal) fprintf(journal,"SHOT ammo=%u hit=%u distance=%.2f moving=%u\n",mag->ammo,struck,hit.distance,gait!=IDLE);
            hud_changed=true;
        }
        unsigned page=house && (down&KEY_A)?house_page(&mission,&p):0;
        if (page) {
            if (page==7 && sound) phone_ring(false);
            // Finish the last GPU transfer before writing the comic into the LCD buffer.
            if (C3D_FrameBegin(0)) C3D_FrameEnd(0);
            gyro_set_active(false);
            if (!(ok=comic_read(page,page,journal))) break;
            if (page==6) mission.house.sign=true;
            else mission.house.call=true;
            now=previous=sample=svcGetSystemTick();frames=0;
            hud_changed=true;
        } else if (!house && (down&KEY_A) && mission_use(&mission,&p)) {
            hud_changed=true;
            if (mission.room==START_ROOM && mission.van) {
                if (!(ok=room_geometry(true,false))) break;
                inventory[1]=(Magazine){7,14,7};
                animation.weapon=SHOTGUN;actor_action(&animation,IDLE);
                if (sound) door_sound();
                if (journal) fputs("VAN open pickup=pumpshotgun shells=21\n",journal);
            } else if (mission.room==SUBWAY) {
                if (!(ok=room_geometry(true,false))) break;
                if (sound) medicine_sound(false);
                if (journal) fputs("CABINET open painkillers=4\n",journal);
            } else if (mission.room==STREET && mission.engaged) {
                unsigned count=mission_gunmen(&mission);
                if (sound) phone_finish_calls();
                if (!(ok=actor_enemies(mission_group(&mission,STREET).count))) break;
                if (mission.practice_round) {
                    Magazine *pistol=&inventory[BERETTA];
                    if (pistol->ammo+pistol->reserve<pistol->capacity)
                        pistol->reserve=pistol->capacity-pistol->ammo;
                    if (sound) switch_sound();
                    if (journal) fprintf(journal,"PRACTICE begin round=%u\n",mission.practice_round);
                } else if (mission.rifle) {
                    if (!(ok=actor_rifle() && room_geometry(true,false))) break;
                    inventory[RIFLE]=(Magazine){5,7,5};
                    animation.weapon=RIFLE;actor_action(&animation,IDLE);
                    cooldown=0;
                    if (sound) door_sound();
                    if (journal) fputs("VAN Street pickup=sniper ammo=5 reserve=7\n",journal);
                }
                if (sound && !mission.rifle) switch_sound();
                checkpoint=p;checkpoint_mission=mission;checkpoint_weapon=animation.weapon;
                memcpy(checkpoint_inventory,inventory,sizeof inventory);
                if (journal) fprintf(journal,"FIGHT %s enemies=%u\n",encounter_name(&mission),count);
                memory_report("ENCOUNTER",now);
            } else {
                if (sound) phone_answer();
                if (journal) fprintf(journal,"USE %s::%s\n",areas[mission.room],mission.room==STREET?"Use_04":"Use_01");
            }
        } else if ((down&KEY_A) && animation.weapon==RIFLE) {
            zoom=zoom==4?1:2*zoom;
            if (zoom>1) scene_scope(&camera,&p);
            else scene_camera(&camera,&p,0);
            hud_changed=true;
            if (journal) fprintf(journal,"SCOPE %ux\n",zoom);
        }
        int portal=mission_exit(&mission,&p);
        if (portal>=0) {
            Portal exit=room_header()->exits[portal];
            RoomId dest=(RoomId)exit.destination;
            if (!(ok=enter_area(&mission,dest,sound))) break;
            // The paired room has its own local coordinate system. Step clear of the seam.
            if (house && !jump.airborne) {
                house_cross(&p,&exit);
                last_ground=p.position;
            } else {
                p.position.x+=exit.offset.x-exit.normal.x*1.1f;
                p.position.y+=exit.offset.y;
                last_ground.y+=exit.offset.y;
                last_ground.x+=exit.offset.x;last_ground.z+=exit.offset.z;
                p.position.z+=exit.offset.z-exit.normal.z*1.1f;
                room_move(&p,.001f,0);
            }
            if (journal) fprintf(journal,"ENTER %s from=%s\n",areas[dest],areas[mission.room]);
            memory_report(areas[dest],now);
            zoom=1;mission.room=dest;effects=(Effects){0};
            scene_camera(&camera,&p,0);
            previous=svcGetSystemTick();
            if (sound) phone_ring((dest==STREET && !mission.street_phone) || (dest==HOME_PHONE && !mission.house.call));
            hud_changed=true;
        }
        if (!mission.house.armed && (mission.room==HOME_PHONE ||
            (mission.room==HOME && room_near(&p,room_header()->lock,1.65f)))) {
            mission.house.armed=true;
            inventory[BERETTA]=(Magazine){18,81,18};
            hud_changed=true;
            if (journal) fputs("PROLOGUE Beretta rounds=99\n",journal);
        }
        if (mission.room==HOME_STAIRS && !mission.house.landing && room_near(&p,room_header()->use,1.45f)) {
            mission.house.landing=true;hud_changed=true;
            if (journal) {fputs("PROLOGUE upstairs landing\n",journal);fflush(journal);}
        }
        unsigned previous_hp=mission.health;
        if (mission_update(&mission,&p,dt,sound)) {
            hud_changed=true;
            if (journal) { fprintf(journal,"%s player=%u\n",mission.health<previous_hp?"DAMAGE":"HEAL",mission.health);fflush(journal); }
        }
        const char *next_prompt=mission_prompt(&mission,&p);
        if (hud_changed || next_prompt!=prompt)
            instructions(&mission,next_prompt,&inventory[animation.weapon],animation.weapon,animation.action==RELOAD,zoom,stance,p.crouched);
        prompt=next_prompt;
        if (hud_changed || !(total%15)) guidance(&mission,&p);
        if (sound && ((mission.room==START_ROOM && !p.answered) || (mission.room==STREET && !mission.street_phone) ||
                      (mission.room==HOME_PHONE && !mission.house.call)) && !(total&7)) {
            Point phone=room_header()->phone;
            phone_mix(phone.x-p.position.x,phone.z-p.position.z,p.yaw);
        }
        rendered_zoom=zoom;
        room_render(&p,&animation,&camera,&effects,&mission,rendered_zoom);
        if (last_stereo!=room_stereo() || !total) {
            last_stereo=room_stereo();
            if (journal) fprintf(journal,"DISPLAY eyes=%u slider=%.2f\n",last_stereo?2:1,(double)osGet3DSliderState());
        }
        ++frames; ++total;
        if (total<=2 && journal) {
            fprintf(journal,"DRAW frame=%u elapsed_ms=%llu\n",total,
                    (unsigned long long)((svcGetSystemTick()-begin)*1000/SYSCLOCK_ARM11));
            fflush(journal);
        }
        if (journal && now-sample>=2ULL*SYSCLOCK_ARM11) {
            fprintf(journal,"FRAME %u fps=%.1f pos=%.2f,%.2f,%.2f yaw=%.2f camera=%.2f\n",total,
                    (double)frames*SYSCLOCK_ARM11/(now-sample),p.position.x,p.position.y,p.position.z,p.yaw,camera.distance);
            fflush(journal);sample=now;frames=0;
        }
    }
    if (!ok && journal) fprintf(journal,"%s resource load failed\n",house?"Prologue":"Tutorial");
    gyro_prepare(false);
    if (sound) phone_close();
    room_close();
    return ok;
}

int main(void) {
    u64 begin=svcGetSystemTick();
    // Reserve the larger top buffers before any UI/GPU output is queued.
    // 565 artwork changes the format temporarily without reallocating them.
    gfxInit(GSP_BGR8_OES,GSP_RGB565_OES,false);
    gfxSetDoubleBuffering(GFX_BOTTOM,false);
    gyro_init();
    bool enhanced=false;
    APT_CheckNew3DS(&enhanced);
    if (enhanced) osSetSpeedupEnable(true);
    mkdir("sdmc:/3ds/maxpayne",0777);
    journal=async_log_open("sdmc:/3ds/maxpayne/native.log");
    if(journal)fputs("BUILD " MP_VERSION " touch_ui=1 selectable_voice=1 async_io=1\n",journal);
    bool mounted=R_SUCCEEDED(romfsInit());
    bool ready=mounted && ui_init((u16*)gfxGetFramebuffer(GFX_BOTTOM,GFX_LEFT,NULL,NULL),enhanced) && ui_title();
    if (ready) {
        memory_report("MENU",begin);
        if (journal) fprintf(journal,"PROFILE %s\n",enhanced?"New 3DS":"Old 3DS");
        bank_open();
        UiChoice next=UI_TITLE;
        while(aptMainLoop()) {
            if(next==UI_TITLE) {bank_music("max_payne");next=ui_menu(false,false);}
            if(next==UI_EXIT) break;
            int load_slot=next==UI_LOAD?(int)ui_save_slot:-1;
            bool bonus=next==UI_BONUS,training=next==UI_ORIGINAL_TUTORIAL;
            bool house=next==UI_CAMPAIGN || bonus || training || load_slot>=0;
            next=UI_TITLE;
            bank_clear();ui_loading_begin();
            FILE *map=house?campaign_file("part0_level1.mpc"):NULL;
            bool installed=map!=NULL;
            if(map) fclose(map);
            if(installed && !bonus && !training && load_slot<0) movie_play("intro.mpv");
            bool ok=installed?campaign_play(training?"tutorial":bonus?"end_combat":"startup_level",load_slot,&next,journal):load_slot<0 && play_level(house,&next);
            if(next==UI_EXIT) break;
            if(!ui_title()) break;
            if(!ok) {
                if(journal) {fputs("ROOM load failed\n",journal);fflush(journal);}
                ui_notice(MP_TEXT("Ошибка загрузки уровня","Could not load level"));
            } else memory_report("MENU_RETURN",begin);
        }
    } else {
        consoleInit(GFX_BOTTOM,NULL);
        puts("Could not load the menu artwork. START exits.");
        while(aptMainLoop()){hidScanInput();if(hidKeysDown()&KEY_START) break;gspWaitForVBlank();}
    }
    gyro_close();
    bank_close();
    if (journal) fclose(journal);
    if (mounted) romfsExit();
    gfxExit();
    return 0;
}
