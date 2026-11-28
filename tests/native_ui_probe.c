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
static void navigation_checks(void){
    memset(slot_valid,0,sizeof slot_valid);slot_valid[0]=true;
    any_save=allow_save=cleared=true;modes=15;
    int column=0,at=menu_first(ROOT,false);assert(at==0);
    const uint32_t directions[]={KEY_DDOWN,KEY_DRIGHT,KEY_DDOWN,KEY_DLEFT,KEY_DDOWN,KEY_DRIGHT,KEY_DUP,KEY_DUP,KEY_DUP,KEY_DDOWN};
    const int expected[]={2,3,5,1,6,7,5,3,0,3};
    for(unsigned i=0;i<sizeof expected/sizeof *expected;i++){
        at=menu_move(ROOT,at,directions[i],false,&column);assert(at==expected[i]);
    }
    assert(menu_move(ROOT,0,KEY_DLEFT,false,&column)==0);
    column=0;assert(menu_move(ROOT,0,KEY_DDOWN,true,&column)==4);
    assert(menu_move(ROOT,4,KEY_DRIGHT,true,&column)==3);
    assert(menu_move(ROOT,3,KEY_DDOWN,true,&column)==7);
    assert(menu_move(ROOT,7,KEY_DLEFT,true,&column)==5);
    assert(menu_move(ROOT,5,KEY_DDOWN,true,&column)==6);
    assert(menu_move(ROOT,6,KEY_DRIGHT,true,&column)==2);
    allow_save=false;column=0;assert(menu_move(ROOT,0,KEY_DDOWN,true,&column)==3);
    column=0;at=0;
    at=menu_move(OPTIONS,at,KEY_DRIGHT,false,&column);assert(at==1);
    at=menu_move(OPTIONS,at,KEY_DDOWN,false,&column);assert(at==3);
    at=menu_move(OPTIONS,at,KEY_DDOWN,false,&column);assert(at==5);
    at=menu_move(OPTIONS,at,KEY_DDOWN,false,&column);assert(at==6); // Next page; animation gallery is disabled.
    at=menu_move(OPTIONS,at,KEY_DUP,false,&column);assert(at==4);
    column=0;assert(menu_move(SETS,0,KEY_DDOWN,false,&column)==2);
    assert(menu_move(SETS,2,KEY_DRIGHT,false,&column)==3);
    assert(menu_move(SETS,3,KEY_DUP,false,&column)==1);
    column=0;assert(menu_move(SAVE_SLOTS,3,KEY_DDOWN,false,&column)==4);
    assert(menu_move(SAVE_SLOTS,4,KEY_DUP,false,&column)==3);
    assert(menu_move(SAVE_SLOTS,0,KEY_DUP,false,&column)==0);
    assert(menu_move(SAVE_SLOTS,menu_entries(SAVE_SLOTS)-1,KEY_DDOWN,false,&column)==menu_entries(SAVE_SLOTS)-1);
    memset(slot_valid,0,sizeof slot_valid);slot_valid[2]=slot_valid[5]=true;
    column=0;assert(menu_move(LOAD_SLOTS,2,KEY_DDOWN,false,&column)==5);
    assert(menu_valid_position(LOAD_SLOTS,4,false)==5);
    assert(menu_first(ROOT,false)==2); // No autosave: start on New Game, not Tutorial.
    assert(menu_keys(KEY_CPAD_UP)==KEY_DUP && menu_keys(KEY_CPAD_RIGHT|KEY_A)==(KEY_DRIGHT|KEY_A));
    MenuRepeat repeat={0};
    assert(menu_repeat(&repeat,KEY_DDOWN,KEY_DDOWN,0,true,false)==KEY_DDOWN);
    assert(!menu_repeat(&repeat,0,KEY_DDOWN,SYSCLOCK_ARM11/4,true,false));
    assert(menu_repeat(&repeat,0,KEY_DDOWN,SYSCLOCK_ARM11/3,true,false)==KEY_DDOWN);
    assert(!menu_repeat(&repeat,0,0,SYSCLOCK_ARM11,true,false) && !repeat.key);
    assert(menu_repeat(&repeat,KEY_DRIGHT,KEY_DRIGHT,0,false,false)==KEY_DRIGHT);
    assert(!menu_repeat(&repeat,0,KEY_DRIGHT,SYSCLOCK_ARM11,false,false)); // Toggles do not oscillate while held.
    assert(menu_repeat(&repeat,KEY_DRIGHT,KEY_DRIGHT,0,true,true)==KEY_DRIGHT);
    assert(menu_repeat(&repeat,0,KEY_DRIGHT,SYSCLOCK_ARM11/4,true,true)==KEY_DRIGHT);
    repeat=(MenuRepeat){0};
    assert(menu_repeat(&repeat,KEY_DUP,KEY_DUP,0,true,false)==KEY_DUP);
    assert(!menu_repeat(&repeat,KEY_DRIGHT,KEY_DUP,1,true,false));
    assert(menu_repeat(&repeat,0,KEY_DRIGHT,2,true,false)==KEY_DRIGHT);
    // Every available entry remains reachable, including incomplete grid rows.
    for(unsigned i=0;i<=SAVE_BONUS;i++)slot_valid[i]=true;
    for(unsigned paused=0;paused<2;paused++)for(enum Page page=ROOT;page<=GRAPHICS;page++){
        if(!menu_entries(page))continue;
        bool reached[64]={0};int queue[64],head=0,tail=0;
        int first=menu_first(page,paused);reached[first]=true;queue[tail++]=first;
        while(head<tail){
            int from=queue[head++];
            const uint32_t arrows[]={KEY_DUP,KEY_DDOWN,KEY_DLEFT,KEY_DRIGHT};
            for(unsigned d=0;d<4;d++){
                int lane=0,to=menu_move(page,from,arrows[d],paused,&lane);
                if(!reached[to]){reached[to]=true;queue[tail++]=to;}
            }
        }
        for(int i=0;i<menu_entries(page);i++)if(menu_selectable(page,i,paused))assert(reached[i]);
    }
}
int main(void){
    assert(ui_init(NULL,false));
    // Both alphabets retain partial edge coverage at every native font size.
    for(int px=FONT_FIRST;px<=FONT_LAST;px++){
        box(0,0,W,H,BLACK);text_px(3,3,px,"Menu / Меню 123",WHITE,200);
        unsigned softened=0,solid=0;
        for(int x=0;x<W;x++)for(int y=0;y<H;y++){
            uint16_t pixel=screen[x*H+H-1-y];
            softened+=pixel!=BLACK && pixel!=WHITE;solid+=pixel==WHITE;
        }
        assert(softened && solid);
        box(0,0,W,H,BLACK);text_px(-3,-5,px,"Clipping / Обрезка",WHITE,40);
        for(int x=40;x<W;x++)for(int y=0;y<H;y++)assert(screen[x*H+H-1-y]==BLACK);
    }
    // A damaged or stale packed font must fail validation, never read beyond its bitmap.
    FILE *bad_font=tmpfile();assert(bad_font);
    uint32_t header[]={FONT_FIRST,FONT_LAST,FONT_COUNT,1};
    Glyph saved=font[0][0];font[0][0].offset=2;
    assert(fwrite(header,sizeof header,1,bad_font)==1 && fwrite(font,sizeof font,1,bad_font)==1);
    rewind(bad_font);assert(!load_font(bad_font));fclose(bad_font);font[0][0]=saved;
    bad_font=tmpfile();assert(bad_font);header[3]=1024*1024;
    assert(fwrite(header,sizeof header,1,bad_font)==1 && fwrite(font,sizeof font,1,bad_font)==1);
    rewind(bad_font);assert(!load_font(bad_font));fclose(bad_font);
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
    navigation_checks();
    step=steps=0;memset(inputs,0,sizeof inputs);full_training=false;
    inputs[steps++]=KEY_CPAD_DOWN;inputs[steps++]=0;inputs[steps++]=KEY_A;
    assert(menu_loop(false,false,NULL,NULL)==UI_TUTORIAL);
    puts("Native UI: spatial button navigation, paging, disabled entries, Circle Pad, repeat, antialiased text, touch navigation and settings passed");
    return 0;
}
