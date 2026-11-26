#ifndef MP_PARTICLES_H
#define MP_PARTICLES_H
#include "campaign_world.h"
enum { PARTICLE_SOURCES=160 };
typedef struct {uint16_t owner,effect;float interval;} ParticleSource;
extern ParticleSource particle_sources[PARTICLE_SOURCES];
bool particles_open(void);
void particles_close(void);
void particles_surface_burst(unsigned owner,unsigned room,Point at,Point normal,const char *name);
void particles_burst(unsigned owner,unsigned room,Point at,const char *name);
void particles_weapon_burst(unsigned owner,unsigned room,Point origin,Point forward,Point up,const char *name);
void particles_blood(unsigned owner,unsigned room,Point impact,Point direction);
uint32_t particles_police_color(unsigned room);
void particles_event(unsigned owner,const char *op,const char *name);
void particles_tick(float dt);
void particles_prepare(const Camera *camera);
void particles_light(unsigned room,Point target,IncidentLight *light);
void particles_queue(const C3D_Mtx *view);
bool particles_valid(void);
#endif
