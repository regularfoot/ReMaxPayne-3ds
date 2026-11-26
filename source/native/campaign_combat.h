#ifndef MP_CAMPAIGN_COMBAT_H
#define MP_CAMPAIGN_COMBAT_H
#include "scene.h"
enum { PROJECTILE_LIMIT=64 };
typedef struct {
    Point position,velocity;float age;
    uint16_t type,owner,room,burst;
    bool active,exploding,resting,dropped;
} Projectile;
extern Projectile campaign_projectiles[PROJECTILE_LIMIT];
void combat_slow_rounds(bool enabled);
void combat_set_health_scales(float player,float enemy);
bool combat_open(void);
void combat_close(void);
bool combat_valid(void);
bool combat_launch(const char *type,unsigned owner,unsigned room,Point origin,Point forward);
bool combat_launch_attached(const char *name,unsigned owner,unsigned room,Point origin,const Point axes[3]);
bool combat_has_weapon_drops(void);
bool combat_drop_on_death(unsigned owner,const char *weapon,Point origin,Point forward);
bool combat_footstep(const char *effect,unsigned owner,unsigned room,Point feet,Effects *effects);
void combat_tick(float dt,Effects *effects);
typedef struct {unsigned timed;float health,slow;} KillRewards;
KillRewards combat_take_rewards(void);
bool combat_sight(unsigned room,Point eye,unsigned target_room,Point target,float range,bool activation,Point *direction);
bool campaign_actor_crouched(unsigned id);
unsigned combat_aim_target(const Player *player,unsigned previous,float cone,Point *direction,float *focus_distance);
bool combat_waits_for_target(unsigned actor);
void combat_camera_bodies(unsigned subject,unsigned room);
bool combat_camera_blocked(Point start,Point direction,float limit,Hit *hit);
void combat_hitscan(unsigned owner,unsigned room,const Camera *camera,Point shoulder,Point muzzle,
                    unsigned pellets,float damage,float range,const char *round,Effects *effects);
typedef struct { char weapons[2][16],ammo[16],sound[32],caption[64];uint32_t amount; } DropGrant;
const DropGrant *combat_drop_grant(const Projectile *drop,bool first);
const char *combat_mesh(const Projectile *p,bool *pickup);
#endif
