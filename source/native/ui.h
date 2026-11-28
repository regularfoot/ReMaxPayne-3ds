#ifndef MP_NATIVE_UI_H
#define MP_NATIVE_UI_H
#include <stdbool.h>
#include <stdint.h>
#include "touch.h"

typedef enum { UI_RESUME, UI_TUTORIAL, UI_CAMPAIGN, UI_TITLE, UI_EXIT, UI_LOAD, UI_SAVE, UI_BONUS, UI_ORIGINAL_TUTORIAL, UI_ANIMATIONS } UiChoice;
typedef struct {
    unsigned char horizontal,vertical,deadzone,volume,inverted,tips,music,scheme,reticle,autoaim,lock,autoswitch,slow_assist,recenter,quickaim,gyro;
    unsigned char gyro_horizontal,gyro_vertical;
    unsigned char stick_horizontal,stick_vertical,touch_horizontal,touch_vertical;
    unsigned char world_textures,sky_textures;
} UiSettings;
extern UiSettings ui_settings;
bool ui_touch_shortcuts(void);
bool ui_touch_slow(void);
unsigned ui_texture_limit(bool sky);
void ui_begin_update(void);
void ui_end_update(void);
uint32_t ui_game_keys(uint32_t keys);
void ui_race_finish(unsigned seconds,unsigned best);
void ui_run_status(int seconds,int saves);
bool ui_init(uint16_t *framebuffer,bool enhanced);
void ui_animation_card(const char *name,unsigned clip,unsigned total);
bool ui_title(void);
UiChoice ui_menu(bool paused,bool can_save);
typedef void (*UiPauseFrame)(float dt,void *context);
UiChoice ui_pause_menu(bool can_save,UiPauseFrame redraw,void *context);
extern unsigned ui_save_slot;
void ui_hud(const char *area,const char *objective,const char *weapon_name,unsigned health,unsigned pills,int weapon,
            unsigned ammo,unsigned reserve,bool reloading,unsigned zoom,bool can_switch,
            unsigned round,unsigned remaining,bool force_prompt);
WeaponChoice ui_weapon_input(bool pressed,int x,int y,bool enabled,uint32_t owned,int current);
void ui_weapon_palette(uint32_t owned,uint32_t loaded,int current);
void ui_weapon_close(void);
void ui_bearing(float angle,float meters,bool alive);
void ui_stance(bool crouched);
void ui_combat_hud(float charge,bool slow,bool crouched,bool allowed);
void ui_journal_link(void);
void ui_journal(const char *objective,const char items[3][96],const char *caption);
void ui_caption(const char *area,const char *caption);
typedef enum { COMIC_PAGE,COMIC_ZOOM_AVAILABLE,COMIC_ZOOM } ComicView;
void ui_comic(bool last,bool single,ComicView view);
void ui_notice(const char *text);
void ui_pause_image(uint16_t *fb);
void ui_loading_progress(unsigned percent);
void ui_loading_begin(void);
void ui_level_loading(const char *stage);
void ui_status(const char *text);
#endif
