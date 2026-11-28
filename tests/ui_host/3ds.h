#ifndef REPLAY_3DS_H
#define REPLAY_3DS_H
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
typedef uint64_t u64;
typedef struct { uint16_t px,py; } touchPosition;
enum { KEY_A=1, KEY_B=2, KEY_X=4, KEY_Y=8, KEY_START=16, KEY_TOUCH=32, KEY_DUP=64,
       KEY_DDOWN=128, KEY_DLEFT=256, KEY_DRIGHT=512, KEY_L=1024, KEY_R=2048, KEY_ZR=4096 };
enum { GFX_TOP, GFX_BOTTOM, GFX_LEFT, GSP_RGB565_OES };
#define SYSCLOCK_ARM11 268123480
bool gspIsPresentPending(int);
void gspWaitForVBlank(void);
void *gfxGetFramebuffer(int,int,void *,void *);
void GSPGPU_FlushDataCache(void *,size_t);
void gfxScreenSwapBuffers(int,bool);
void gfxSetScreenFormat(int,int);
void gfxSet3D(bool);
void gfxFlushBuffers(void);
void gfxSetDoubleBuffering(int,bool);
bool aptMainLoop(void);
void hidScanInput(void);
uint32_t hidKeysDown(void);
uint32_t hidKeysHeld(void);
void hidTouchRead(touchPosition *);
u64 svcGetSystemTick(void);
#endif
