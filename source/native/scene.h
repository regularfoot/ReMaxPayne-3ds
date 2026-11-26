#ifndef MP_NATIVE_SCENE_H
#define MP_NATIVE_SCENE_H
#include "room.h"
typedef struct { Point point,normal; float distance;unsigned face; } Hit;
typedef struct { Point eye,forward,right,up,offset; float distance; int shoulder; } Camera;
typedef struct { Point point,normal; float life; unsigned room,decal;uint16_t support;Point local,tangent,bitangent;char material[32]; } Impact;
enum { SURFACE_MARKS=12, FLOOR_BLOOD_MARKS=16, IMPACT_COUNT=SURFACE_MARKS+FLOOR_BLOOD_MARKS };
typedef struct { Impact marks[IMPACT_COUNT]; unsigned next,next_blood; Point muzzle; float flash; unsigned flash_room; } Effects;

void scene_face_bounds(const Triangle *faces,unsigned count,Point *lo,Point *hi);
typedef struct {Point low,high;} SceneBounds;
void scene_bind(const Triangle *faces,unsigned count,const uint8_t *mask,SceneBounds *bounds);
bool scene_reserve(unsigned faces);
bool scene_open_bounds(const Triangle *faces,unsigned count,const uint8_t *mask,const SceneBounds *baked);
bool scene_open(const Triangle *faces,unsigned count);
enum { SCENE_BODY=1,SCENE_SHOT=2,SCENE_BIG_SHOT=4,SCENE_PIERCE=8,SCENE_PLAYER_OVERLAP=16 };
bool scene_open_masks(const Triangle *faces,unsigned count,const uint8_t *mask);
float scene_panel_distance(const Triangle *face,const Player *body);
bool scene_panel_blocks(const Triangle *face,const Player *body,Point target,bool airborne);
bool scene_wall_overlap(unsigned first,unsigned count,const Player *body,bool airborne);
bool scene_body_touched(unsigned first,unsigned count);
void scene_refit(unsigned first_face,unsigned count);
bool scene_toggle_crouch(Player *p);
bool scene_ground(Point feet,Hit *hit);
void scene_close(void);
bool scene_trace_mask(Point start,Point direction,float length,unsigned mask,Hit *hit);
bool scene_trace(Point start,Point direction,float length,Hit *hit);
typedef bool (*CameraTrace)(Point,Point,float,Hit *);
bool scene_death_camera(Camera *camera,Point pivot,CameraTrace trace);
void scene_clip_camera(Camera *camera,Point pivot,CameraTrace trace);
void scene_camera_sweep(Camera *c,const Player *p,float dt,CameraTrace trace);
void scene_camera_converge(Camera *camera,const Player *player,float distance,float weight);
void scene_camera(Camera *camera,const Player *p,float dt);
// Presentation only: the subject and simulation remain frozen while this runs.
typedef struct {Camera view;Point pivot;float angle,direction,radius,height;} PauseCamera;
void scene_pause_begin(PauseCamera *orbit,const Camera *entry,const Player *player);
void scene_pause_step(PauseCamera *orbit,float dt,CameraTrace trace);
void scene_scope(Camera *camera,const Player *p);
typedef struct { Point feet;bool active,crouched;float bottom,top,radius; } ShotTarget;
bool scene_triangle(const Triangle *face,Point start,Point direction,float *nearest,Point *normal);
bool scene_bodies(Point from,Point direction,float limit,const ShotTarget *targets,unsigned count,Hit *hit,int *actor);
bool scene_cast(Point from,Point direction,float limit,const ShotTarget *targets,unsigned count,Hit *hit,int *actor);
bool scene_fire(Effects *fx,const Camera *c,Point shoulder,Point muzzle,Hit *hit,
                const ShotTarget *targets,unsigned count,int *actor);
bool scene_shot(Effects *effects,const Camera *camera,Point shoulder,Point muzzle,Hit *hit);
typedef struct { float velocity; bool airborne; } Jump;
bool scene_settle_step(Player *player,Jump *jump);
void scene_move_gravity(Player *p,Jump *j,float dx,float dz,float dt,bool takeoff,float gravity,float launch);
void scene_move_player(Player *player,Jump *jump,float dx,float dz,float dt,bool takeoff);
void scene_age(Effects *effects,float dt);
#endif
