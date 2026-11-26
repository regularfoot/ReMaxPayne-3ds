#ifndef MP_NATIVE_ROOM_H
#define MP_NATIVE_ROOM_H
#include <stdbool.h>
#include <stdint.h>
typedef struct { float x,y,z; } Point;
typedef struct { Point a,b,c; float normal_y; } Triangle;
typedef enum { START_ROOM, ALLEY, STREET, SUBWAY, SIDE_ALLEY, DEAD_END, HOME, HOME_PHONE, HOME_LIVING, HOME_STAIRS, ROOM_COUNT } RoomId;
typedef struct { Point center,normal,offset; float width; int32_t destination; } Portal;
typedef struct {
    char magic[4]; uint32_t textures,batches,vertices,collision;
    Point spawn;float yaw;
    Point phone,use,lock,enemies[2];
    Portal exits[4];
    Point rifle_van,rifle_spawns[3],rifle_destinations[3],practice_spawns[4];
    uint32_t group_counts[5];
} RoomHeader;
typedef struct { Point position; float yaw,pitch; bool answered,crouched; } Player;
bool room_load(RoomId first);
void room_hint(Point position,float dx,float dz);
bool room_switch(RoomId id,bool doors_open,bool gate);
bool room_geometry(bool doors_open,bool gate);
void room_close(void);
const RoomHeader *room_header(void);
void room_move(Player *player,float dx,float dz);
Point room_pursue(Point from,Point to,bool visible);
bool room_near(const Player *player,Point marker,float radius);
bool room_portal(const Player *player,const Portal *exit);
#endif
