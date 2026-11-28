#ifndef MP_CAMPAIGN_WORLD_H
#define MP_CAMPAIGN_WORLD_H
#include "scene.h"
#ifdef __3DS__
#include <citro3d.h>
#else
typedef struct HostMatrix C3D_Mtx;
#endif

enum { CAMPAIGN_EXITS=8, CAMPAIGN_PROPS=256 };
typedef struct {
    uint32_t id,room,points;
    float transform[12];
    Point normal;
} CampaignExit;
typedef struct {
    bool hidden,solid;uint8_t collision_mask;
    float matrix[12];
    bool moved;
} CampaignProp;
extern CampaignProp campaign_props[CAMPAIGN_PROPS];
void campaign_training_light(RoomId id);
bool campaign_open(const char *stage,bool actor_lights);
void campaign_close(void);
bool campaign_enter(unsigned room);
unsigned campaign_start(Player *player);
const char *campaign_room_name(void);
unsigned campaign_room_id(void);
bool campaign_body_contact(unsigned prop);
bool campaign_collision_relevant(unsigned prop);
unsigned campaign_exit_count(void);
void campaign_camera_room(Point pivot,const Camera *cam,const bool *enabled);
void campaign_update_moving_rooms(void);
unsigned campaign_view_count(void);
unsigned campaign_first_view(unsigned room);
unsigned campaign_next_view(unsigned view);
bool campaign_view_enabled(unsigned view,const bool *enabled);
const C3D_Mtx *campaign_visible_transform(unsigned view);
bool campaign_anchor_mark(Impact *mark,unsigned face);
bool campaign_mark_pose(const Impact *mark,Point *center,Point *normal);
const char *campaign_room_surface(unsigned room,unsigned face);
const char *campaign_surface(unsigned face);
void campaign_visit_surfaces(void (*visit)(const char *material));
typedef struct {Point amount,vector;float weight;} IncidentLight;
void campaign_add_light(IncidentLight *sample,Point target,Point source,float radius,Point rgb);
IncidentLight campaign_incident(unsigned room,Point target);
uint32_t campaign_light(unsigned room,Point position);
Point campaign_from_neighbour(unsigned slot,Point position);
bool campaign_placement(unsigned room,unsigned portal,Player *body);
unsigned campaign_body_view(unsigned view,Point feet,bool crouched);
void campaign_stencil(unsigned portal);
#ifdef __3DS__
void draw_aperture(int uniform,const C3D_Mtx *view,unsigned first,unsigned count,bool crosses_near);
#endif
const CampaignExit *campaign_exit(unsigned slot);
const CampaignExit *campaign_room_exit(unsigned room,unsigned slot);
Point campaign_from_room_neighbour(unsigned room,unsigned slot,Point position);
Point campaign_transform(const CampaignExit *exit,Point point,bool direction);
bool campaign_ray_exit(unsigned room,Point start,Point direction,float *limit,const bool *enabled,CampaignExit *exit);
bool campaign_sound_path(unsigned room,Point listener,unsigned source_room,Point source,const bool *enabled,Point *apparent,float *distance);
bool campaign_camera_trace(Point start,Point direction,float length,const bool *enabled,Hit *hit);
void campaign_pierced_props(unsigned room,Point start,Point dir,float limit,uint32_t *seen,unsigned owner,float damage);
bool campaign_ray_projectile(unsigned room,Point start,Point dir,float limit,bool large,Hit *hit,int *prop);
bool campaign_ray_wall(unsigned room,Point start,Point direction,float limit,Hit *hit,int *prop);
enum { CAMPAIGN_CROSS_BLOCKED=-2 };
bool campaign_npc_passage(uint16_t *room,Point *from,unsigned destination,Point target,
                          bool crouched,float stride,float *yaw,float *travel);
int campaign_crossing(Player *player,Point previous,Point wish,const bool *enabled);
void campaign_cross(Player *player,const CampaignExit *exit);
bool campaign_collision(void);
bool campaign_panel_blocked(unsigned prop,unsigned room,const float *from,const float *to,bool damaging);
bool campaign_contact_frame(unsigned from,unsigned to,Point center,Point *mapped);
void campaign_panel_contact(const Player *player,bool airborne);
int campaign_contact(Point point,float radius);
int campaign_hit_prop(unsigned face);
typedef struct {int prop;Point local,previous;bool placed;float placement[12];} PlatformContact;
PlatformContact campaign_standing_on(Point feet);
void campaign_ride(PlatformContact contact,Point *feet);
void campaign_queue_translucent(int uniform,const C3D_Mtx *view,const bool *enabled);
void campaign_draw_camera_overlay(int uniform,const C3D_Mtx *monoscopic_view);
void campaign_draw(int projection,const C3D_Mtx *combined,const C3D_Mtx *sky,const bool *enabled);
#endif
