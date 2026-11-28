// Run from a directory containing romfs:/ with lower.ui, menu.ui and weapons.ui from a personal game build.
#include <assert.h>
#include <stddef.h>
#include "../source/native/ui.c"
static unsigned step,steps;
static uint32_t inputs[64],flips;
static touchPosition taps[64];
static uint16_t fb[400*240];
bool gspIsPresentPending(int dev){(void)dev;return false;}
void gspWaitForVBlank(void){}
void *gfxGetFramebuffer(int s,int e,void *w,void *h){(void)s;(void)e;(void)w;(void)h;return fb;}
void GSPGPU_FlushDataCache(void *p,size_t i){(void)p;(void)i;}
void gfxScreenSwapBuffers(int dev,bool f){(void)dev;(void)f;flips++;}
void gfxSetScreenFormat(int d,int f){(void)d;(void)f;}
void gfxSet3D(bool b){(void)b;}
void gfxFlushBuffers(void){}
void gfxSetDoubleBuffering(int d,bool f){(void)d;(void)f;}
bool aptMainLoop(void){return step<steps;}
void hidScanInput(void){assert(step<steps);step++;}
uint32_t hidKeysDown(void){return inputs[step-1];}
uint32_t hidKeysHeld(void){return hidKeysDown();}
void hidTouchRead(touchPosition *p){*p=taps[step-1];}
u64 svcGetSystemTick(void){return SYSCLOCK_ARM11*(u64)(step+1)/60;}
unsigned pose_bank_count(void){return 0;}
bool movie_play(const char *p){(void)p;return true;}
void bank_tick(void){}
unsigned bank_voice_languages(void){return 3;}
void room_pause_image(bool b){(void)b;}
void room_pause_overlay(bool b){(void)b;}
FILE *campaign_file(const char *p){(void)p;return NULL;}
enum GameMode selected_mode;


static void tap(unsigned x,unsigned y){inputs[steps]=KEY_TOUCH;taps[steps++]=(touchPosition){x,y};inputs[steps++]=0;}
int main(void){
    assert(ui_init(NULL,false));
    unsigned initial_voice=ui_settings.voice;
    tap(230,172); // Main menu -> Options.
    tap(50,65); // Aim sensitivity.
    tap(285,105); // Increase stick X.
    tap(256,105); // Maximum of stick X track.
    tap(260,20); // Back -> Options.
    tap(240,65); // Gyroscope.
    tap(260,60); // Enable gyro.
    tap(280,112); // Increase gyro X.
    tap(260,20); // Back -> Options.
    tap(50,120); // Graphics.
    tap(270,160); // Old 3DS sky: Auto -> 128.
    tap(270,160); // 128 -> 256.
    tap(260,20); // Back -> Options.
    tap(50,170); // Gameplay.
    tap(220,135); // Auto Aim.
    tap(230,221); // Second page.
    tap(200,62); // Auto weapon switch.
    tap(260,20); // Back -> Options.
    tap(240,120); // Sound.
    tap(250,186); // Voice language.
    tap(260,20); // Back -> Options.
    tap(260,20); // Back -> Main menu.
    assert(menu_loop(false,false,NULL,NULL)==UI_EXIT);
    assert(ui_settings.stick_horizontal==250);
    assert(ui_settings.stick_vertical==100);
    assert(ui_settings.gyro==1);
    assert(ui_settings.gyro_horizontal==105 && ui_settings.gyro_vertical==100);
    assert(ui_texture_limit(true)==256 && ui_texture_limit(false)==128);
    assert(ui_settings.autoaim && ui_settings.autoswitch);
    assert(ui_settings.voice!=initial_voice);
    for(enum Page p=ROOT;p<=GRAPHICS;p++){
        for(unsigned paused=0;paused<2;paused++)for(int i=0;i<menu_rows(p);i++){
            TouchRect a=entry_rect(p,i,paused);if(!a.w)continue;
            assert(a.x>=0 && a.y>=0 && a.x+a.w<=320 && a.y+a.h<=240);
            if(p!=ROOT)assert(!inside(a,back_rect.x,back_rect.y));
            for(int j=0;j<i;j++){
                TouchRect b=entry_rect(p,j,paused);if(!b.w)continue;
                assert(a.x>=b.x+b.w || b.x>=a.x+a.w || a.y>=b.y+b.h || b.y>=a.y+a.h);
            }
        }
    }
    // Reload/empty status never changes the actual icon pixels or its position.
    uint16_t normal[76*26];
    for(unsigned variant=0;variant<3;variant++){
        ui_hud("Chapter 5","Find a way out of the hotel. Reach the street through the courtyard.","Pump Shotgun",76,3,0,variant?0:5,26,variant==1,0,true,0,0,false);
        ui_weapon_palette(0x7fff,0x7fff,3);
        unsigned i=0;for(int y=5;y<30;y++)for(int x=174;x<250;x++,i++){
            if(!variant)normal[i]=screen[x*240+239-y];else assert(normal[i]==screen[x*240+239-y]);
        }
    }
    unsigned preferred=ui_settings.voice;
    UiSettings old=ui_settings;old.stick_horizontal=175;
    FILE *migration=fopen("ui-settings-migration.tmp","wb");assert(migration);
    assert(fwrite("MPS2",1,4,migration)==4);
    assert(fwrite(&old,1,offsetof(UiSettings,voice),migration)==offsetof(UiSettings,voice));fclose(migration);
    ui_settings.stick_horizontal=100;
    assert(read_current_settings("ui-settings-migration.tmp"));remove("ui-settings-migration.tmp");
    assert(ui_settings.stick_horizontal==175 && ui_settings.voice==preferred);
    assert(change(VOICE,1) && ui_settings.voice!=preferred);
    puts("Native UI: touch navigation, both settings pages, independent axes, Old texture limits, integer layouts and fixed weapon icon passed");
    return 0;
}
