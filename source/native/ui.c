#include "language.h"
#include "campaign_file.h"
#include "ui.h"
#include "touch.h"
#include "render.h"
#include "game_rules.h"
#include "save.h"
#include "audio_bank.h"
#include "movie.h"
#include "pose_bank.h"
#include <3ds.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { W=320,H=240,FONT_COUNT=161 };
enum { BLACK=0x0862, PANEL=0x18e4, LINE=0x3966, MUTED=0x9c71, GREY=0x736e, PAPER=0xf73a,
       WHITE=0xffff, RED=0xa985, ACCENT=0xf30b, DARKRED=0x5082 };
typedef struct { uint8_t advance,bits[32][4]; } Glyph;
static struct { uint16_t skyline[72][320]; uint8_t silhouette[51][23]; Glyph font[3][FONT_COUNT]; } art;
// Compose in ordinary RAM. Never retain a GSP back-buffer pointer across swaps.
static _Alignas(32) uint16_t canvas[W*H];
static uint16_t *const screen=canvas;
static unsigned update_depth;
static bool update_pending;
static struct {
    uint16_t backgrounds[4][W*H],toggles[2][18*18],slider[112*14],marker[12*18];
} menu_art;
static uint8_t weapon_art[15][24][64];
static WeaponPicker weapon_picker;
static bool new_model,full_training;
enum { MIN_PERCENT=25, GYRO_MAX_PERCENT=200, AIM_MAX_PERCENT=250, STEP_PERCENT=5 };
UiSettings ui_settings={.horizontal=2,.vertical=2,.deadzone=1,.volume=4,.tips=1,.music=3,
                        .gyro_horizontal=100,.gyro_vertical=100,.reticle=1,
                        .stick_horizontal=100,.stick_vertical=100,.touch_horizontal=100,.touch_vertical=100};
static const char *legacy_config="sdmc:/3ds/maxpayne/settings.bin";
static const char *config="sdmc:/3ds/maxpayne/settings-v2.bin";
static const char *config_temp="sdmc:/3ds/maxpayne/settings-v2.tmp";
static bool settings_dirty;

static const unsigned texture_sizes[]={0,128,256,512,1024};

unsigned ui_texture_limit(bool sky){
    if(!new_model)return sky && ui_settings.sky_textures==2?256:128;
    unsigned option=sky?ui_settings.sky_textures:ui_settings.world_textures;
    return option?texture_sizes[option]:512;
}

static void box(int x,int y,int w,int h,uint16_t color) {
    int x1=x+w<W?x+w:W,y1=y+h<H?y+h:H;
    if (x<0) x=0;
    if (y<0) y=0;
    if (y>=y1) return;
    for (;x<x1;x++) {
        uint16_t *p=screen+x*H+H-y1;
        for (int i=y;i<y1;i++) *p++=color;
    }
}
static void line(int x,int y,int x1,int y1,uint16_t c) {
    int dx=abs(x1-x),sx=x<x1?1:-1,dy=-abs(y1-y),sy=y<y1?1:-1,e=dx+dy;
    for (;;) {
        box(x,y,1,1,c);
        if (x==x1 && y==y1) break;
        int d=2*e;
        if (d>=dy) {e+=dy;x+=sx;}
        if (d<=dx) {e+=dx;y+=sy;}
    }
}
static int character(const unsigned char **cursor) {
    const unsigned char *s=*cursor;
    int value=*s++;
    if (value>=0xc0 && value<0xe0 && (*s&0xc0)==0x80) {value=((value&31)<<6)|(*s&63);s++;}
    if(value>=0xe0 && value<=0xef && (s[0]&0xc0)==0x80 && (s[1]&0xc0)==0x80){
        value=((value&15)<<12)|((s[0]&63)<<6)|(s[1]&63);s+=2;
    }
    *cursor=s;
    if(value==171 || value==187 || value==0x201c || value==0x201d)value='"';
    if(value==0x2013 || value==0x2014)value='-';
    if (value>=32 && value<127) return value-32;
    if (value>=1040 && value<=1103) return 95+value-1040;
    if (value==1025 || value==1105) return 159+(value==1105);
    return '?'-32;
}
static void label_to(uint16_t *fb,int x,int y,int size,const char *str,uint16_t color,int right) {
    const unsigned char *s=(const unsigned char *)str;
    int height=size==0?20:size==1?26:30;
    while (*s) {
        const Glyph *g=&art.font[size][character(&s)];
        if (x+g->advance>right) break;
        for (int j=0;j<height && y+j<H;j++) for (int i=0;i<g->advance;i++)
            if (x+i>=0 && y+j>=0 && i<32 && (g->bits[j][i/8]&(128>>(i&7)))) fb[(x+i)*H+H-1-y-j]=color;
        x+=g->advance;
    }
}
static void label(int x,int y,int size,const char *str,uint16_t color,int right){
    label_to(screen,x,y,size,str,color,right);
}
void ui_pause_image(uint16_t *fb){
    for(unsigned i=0;i<400*H;i++)fb[i]=(fb[i]>>1)&0x7bef;
    const char *word=MP_TEXT("ПАУЗА","PAUSE");const unsigned char *s=(const unsigned char*)word;unsigned width=0;
    while(*s)width+=art.font[2][character(&s)].advance;
    int x=(400-width)/2;
    label_to(fb,x+1,106,2,word,BLACK,400);label_to(fb,x,105,2,word,WHITE,400);
}
static int paragraph_step(int x,int y,int size,const char *text,int lines,uint16_t color,int pitch) {
    const unsigned char *line=(const unsigned char *)text;
    while(*line && lines-->0){
        const unsigned char *end=line,*space=NULL;int width=0;
        while(*end){
            const unsigned char *next=end;int glyph=character(&next);
            if(width+art.font[size][glyph].advance>309-x || (size_t)(next-line)>=255)break;
            if(*end==' ')space=end;
            width+=art.font[size][glyph].advance;end=next;
        }
        if(*end && space && space>line)end=space;
        if(end==line)break;
        char buf[256];size_t n=end-line;memcpy(buf,line,n);buf[n]=0;
        label(x,y,size,buf,color,314);y+=pitch;
        line=end;while(*line==' ')line++;
    }
    return y;
}
static int paragraph(int x,int y,int size,const char *text,int lines,uint16_t color) {
    return paragraph_step(x,y,size,text,lines,color,size?25:19);
}
static void flush(void) {
    if(update_depth){update_pending=true;return;}
    // Wait only when the preceding image has not latched yet. Waiting after
    // every HUD update would add a whole frame to weapon/health changes.
    while(gspIsPresentPending(GFX_BOTTOM))gspWaitForVBlank();
    uint16_t *back=(uint16_t*)gfxGetFramebuffer(GFX_BOTTOM,GFX_LEFT,NULL,NULL);
    memcpy(back,screen,sizeof canvas);
    GSPGPU_FlushDataCache(back,sizeof canvas);
    gfxScreenSwapBuffers(GFX_BOTTOM,false);
    update_pending=false;
}
void ui_begin_update(void){update_depth++;}
void ui_end_update(void){if(update_depth && !--update_depth && update_pending)flush();}
static void heading(const char *title) {
    memcpy(screen,menu_art.backgrounds[1],W*H*2);
    label(17,12,2,title,WHITE,310);
}
static void footer(const char *message) {
    box(0,215,W,25,BLACK);box(0,215,W,1,LINE);
    label(12,218,0,message,MUTED,312);
}
static bool load_top(const char *name) {
    FILE *file=fopen(name,"rb");
    if (!file) return false;
    gfxSetScreenFormat(GFX_TOP,GSP_RGB565_OES);
    bool ok=fread(gfxGetFramebuffer(GFX_TOP,GFX_LEFT,NULL,NULL),2,400*240,file)==400*240;
    fclose(file);
    if (ok) {gfxFlushBuffers();gfxScreenSwapBuffers(GFX_TOP,true);gspWaitForVBlank();}
    return ok;
}
bool ui_title(void) {return load_top("romfs:/menu.565");}
static bool valid_percent(unsigned percent,unsigned max){
    return percent>=MIN_PERCENT && percent<=max && percent%STEP_PERCENT==0;
}
static bool valid_settings(const UiSettings *s){
    return valid_percent(s->gyro_horizontal,GYRO_MAX_PERCENT) && valid_percent(s->gyro_vertical,GYRO_MAX_PERCENT) &&
        valid_percent(s->stick_horizontal,AIM_MAX_PERCENT) && valid_percent(s->stick_vertical,AIM_MAX_PERCENT) &&
        valid_percent(s->touch_horizontal,AIM_MAX_PERCENT) && valid_percent(s->touch_vertical,AIM_MAX_PERCENT) &&
        s->world_textures<=3 && s->sky_textures<=4 &&
        s->gyro<2 && s->scheme<4 && s->reticle<3 && s->autoaim<2 && s->lock<2 && s->autoswitch<2 &&
        s->slow_assist<2 && s->recenter<2 && s->quickaim<2 && s->horizontal<=4 && s->vertical<=4 && s->deadzone<=4 &&
        s->volume<=4 && s->inverted<=1 && s->tips<=1 && s->music<=4;
}
static bool read_current_settings(const char *path){
    FILE *file=fopen(path,"rb");
    if(!file)return false;
    UiSettings s;char tag[4];
    bool ok=fread(tag,1,sizeof tag,file)==sizeof tag && !memcmp(tag,"MPS2",4) &&
            fread(&s,1,sizeof s,file)==sizeof s && fgetc(file)==EOF && valid_settings(&s);
    fclose(file);
    if(ok)ui_settings=s;
    return ok;
}
static void read_settings(void){
    if(read_current_settings(config))return;
    // FAT may have removed the previous file before an interrupted rename.
    if(read_current_settings(config_temp)){rename(config_temp,config);return;}
    FILE *file=fopen(legacy_config,"rb");
    if(!file)return;
    UiSettings s=ui_settings;
    size_t legacy_size=offsetof(UiSettings,stick_horizontal);
    size_t bytes=fread(&s,1,legacy_size,file);
    bool shared_sensitivity=bytes==offsetof(UiSettings,gyro_horizontal);
    if(shared_sensitivity){
        s.gyro_horizontal=50+25*s.horizontal;
        s.gyro_vertical=50+25*s.vertical;
    }
    s.stick_horizontal=s.touch_horizontal=50+25*s.horizontal;
    s.stick_vertical=s.touch_vertical=50+25*s.vertical;
    int format=fgetc(file);
    if(format==EOF && !s.reticle)s.reticle=1;
    if((bytes==offsetof(UiSettings,scheme) || bytes==offsetof(UiSettings,gyro) || shared_sensitivity || bytes==legacy_size) &&
       (format==EOF || (bytes==legacy_size && format==1 && fgetc(file)==EOF)) && valid_settings(&s))ui_settings=s;
    fclose(file);
}
bool ui_init(uint16_t *fb,bool enhanced) {
    (void)fb;new_model=enhanced;
    gfxSetDoubleBuffering(GFX_BOTTOM,true);
    FILE *file=fopen("romfs:/lower.ui","rb");
    if (!file) return false;
    char tag[4];
    _Static_assert(sizeof(Glyph)==129,"UI asset layout");
    bool ok=fread(tag,4,1,file)==1 && !memcmp(tag,"UI01",4) &&
        fread(art.skyline,sizeof art.skyline,1,file)==1 && fread(art.silhouette,sizeof art.silhouette,1,file)==1 &&
        fread(art.font,sizeof art.font,1,file)==1 && fgetc(file)==EOF;
    fclose(file);
    file=fopen("romfs:/weapons.ui","rb");
    if(!file)return false;
    struct {char tag[4];uint32_t count,width,height;} weapons;
    ok=ok && fread(&weapons,sizeof weapons,1,file)==1 && !memcmp(weapons.tag,"WPN1",4) &&
       weapons.count==15 && weapons.width==64 && weapons.height==24 &&
       fread(weapon_art,sizeof weapon_art,1,file)==1 && fgetc(file)==EOF;
    fclose(file);
    file=fopen("romfs:/menu.ui","rb");
    if(!file)return false;
    struct {char tag[4];uint32_t count,width,height;} menu;
    ok=ok && fread(&menu,sizeof menu,1,file)==1 && !memcmp(menu.tag,"MNU1",4) &&
       menu.count==4 && menu.width==W && menu.height==H &&
       fread(&menu_art,sizeof menu_art,1,file)==1 && fgetc(file)==EOF;
    fclose(file);
    read_settings();
    FILE *map=campaign_file("tutorial.mpc");full_training=map!=NULL;if(map)fclose(map);
    return ok;
}
static void glyph_icon(int x,int y,int id,uint16_t c) {
    switch (id) {
    case 0:
        for (int i=0;i<10;i++) box(x+i,y+i/2,1,12-i,c);
        break;
    case 1:
        box(x+3,y+1,12,12,c);box(x+5,y+3,8,8,PANEL);box(x,y+5,2,12,c);box(x+1,y+15,10,2,c);break;
    case 2:
        line(x,y+2,x+7,y+5,c);line(x+7,y+5,x+14,y+2,c);
        line(x,y+2,x,y+13,c);line(x+14,y+2,x+14,y+13,c);line(x,y+13,x+7,y+16,c);
        line(x+7,y+16,x+14,y+13,c);line(x+7,y+5,x+7,y+16,c);break;
    case 3:case 4:
        box(x,y+2,15,14,c);box(x+3,y+2,8,5,PANEL);box(x+3,y+10,9,6,PANEL);break;
    case 5:
        for (int i=0;i<3;i++){int w=3+(i*4)%10;box(x,y+i*5+2,16,1,c);box(x+w,y+i*5,3,5,c);}break;
    case 6:
        box(x,y+2,16,13,c);box(x+2,y+5,12,7,PANEL);
        for(int i=0;i<4;i++) {box(x+1+i*4,y+3,2,1,PANEL);box(x+1+i*4,y+13,2,1,PANEL);}break;
    default:
        box(x,y+1,7,15,c);box(x+2,y+3,5,11,PANEL);box(x+5,y+8,11,1,c);
        line(x+12,y+4,x+16,y+8,c);line(x+12,y+12,x+16,y+8,c);break;
    }
}
static void tile(int x,int y,int w,int h,const char *name,bool on,bool current,int icon) {
    uint16_t bg=current?DARKRED:on?PANEL:0x1082, color=on?(current?WHITE:PAPER):GREY;
    box(x,y,w,h,bg);box(x,y+h-1,w,1,current?ACCENT:LINE);
    if(current) box(x,y,3,h,ACCENT);
    if (icon>=0) glyph_icon(x+10,y+(h-16)/2,icon,current?WHITE:on?MUTED:GREY);
    label(x+(icon>=0?34:12),y+(h-26)/2,1,name,color,x+w-3);
}
enum Page { ROOT, DIFFICULTY, OPTIONS, SETS, CONTROLS, MOVE, COMBAT, WEAPONS, GENERAL, SOUND, CREDITS, QUIT, LOAD_SLOTS, SAVE_SLOTS, TRAIN_MENU, GYRO_MENU, AIM_MENU, GRAPHICS };
enum Action { PRESET,RESUME,TRAIN,NEWGAME,LEAVE,BACK,SUB,CHANGE,INFO,BONUS,AUTHORED,ANIMATIONS };
typedef struct {const char *name;int action,argument;} Entry;
typedef struct {const char *name;const Entry *list;int count;enum Page parent;} PageDef;
enum Setting { DEAD,VOLUME,MUSIC,INVERT,TIPS,RETICLE,AUTOAIM,LOCK,AUTOSWITCH,SLOW_ASSIST,RECENTER,QUICKAIM,GYRO_AIM,GYRO_HORIZONTAL,GYRO_VERTICAL,STICK_X,STICK_Y,TOUCH_X,TOUCH_Y,WORLD_TEXTURES,SKY_TEXTURES };
static const Entry root[]={
    {MP_TEXT("Продолжить","Continue"),RESUME,0},{MP_TEXT("Обучение","Tutorial"),SUB,TRAIN_MENU},{MP_TEXT("Новая игра","New Game"),SUB,DIFFICULTY},{MP_TEXT("Загрузка","Load Game"),SUB,LOAD_SLOTS},
    {MP_TEXT("Сохранение","Save Game"),SUB,SAVE_SLOTS},{MP_TEXT("Настройки","Options"),SUB,OPTIONS},{MP_TEXT("Авторы","Credits"),SUB,CREDITS},{MP_TEXT("Выход","Exit"),SUB,QUIT}};
static const Entry training[]={{MP_TEXT("Обучение","Tutorial"),AUTHORED,0},{MP_TEXT("Тренировочные раунды","Practice Rounds"),TRAIN,0}};
static const Entry difficulty[]={{MP_TEXT("Беглец","Fugitive"),NEWGAME,FUGITIVE},{MP_TEXT("Крутой коп","Hard-Boiled"),NEWGAME,HARD_BOILED},{MP_TEXT("Смерть по прибытии","Dead on Arrival"),NEWGAME,DEAD_ON_ARRIVAL},{MP_TEXT("Минута Нью-Йорка","New York Minute"),NEWGAME,NEW_YORK_MINUTE},{MP_TEXT("Последняя схватка","Last Challenge"),BONUS,0}};
static const Entry options[]={{MP_TEXT("Схемы управления","Control Presets"),SUB,SETS},{MP_TEXT("Управление","Controls"),SUB,CONTROLS},{MP_TEXT("Графика","Graphics"),SUB,GRAPHICS},{MP_TEXT("Игра","Gameplay"),SUB,GENERAL},{MP_TEXT("Звук","Sound"),SUB,SOUND},{MP_TEXT("Анимации персонажа","Character Animations"),ANIMATIONS,0}};
static const Entry sets[]={{MP_TEXT("1   Стандарт","1   Standard"),PRESET,0},{MP_TEXT("2   Левша","2   Left-Handed"),PRESET,1},{MP_TEXT("3   Кнопки X / Y","3   Swap X / Y"),PRESET,2},{MP_TEXT("4   Левша и X / Y","4   Left-Handed + X / Y"),PRESET,3}};
static const Entry controls[]={{MP_TEXT("Движение","Movement"),SUB,MOVE},{MP_TEXT("Бой и камера","Combat and Camera"),SUB,COMBAT},{MP_TEXT("Оружие","Weapons"),SUB,WEAPONS},{MP_TEXT("Гироскоп","Gyroscope"),SUB,GYRO_MENU},{MP_TEXT("Чувствительность","Aim Sensitivity"),SUB,AIM_MENU}};
static const Entry gyro_options[]={{MP_TEXT("Управление наклоном","Tilt to Aim"),CHANGE,GYRO_AIM},{MP_TEXT("По горизонтали","Horizontal"),CHANGE,GYRO_HORIZONTAL},{MP_TEXT("По вертикали","Vertical"),CHANGE,GYRO_VERTICAL}};
static const Entry aim_options[]={
    {MP_TEXT("Стик по горизонтали","Stick Horizontal"),CHANGE,STICK_X},{MP_TEXT("Стик по вертикали","Stick Vertical"),CHANGE,STICK_Y},
    {MP_TEXT("Экран по горизонтали","Touch Horizontal"),CHANGE,TOUCH_X},{MP_TEXT("Экран по вертикали","Touch Vertical"),CHANGE,TOUCH_Y}};
static const Entry graphics[]={
    {MP_TEXT("Текстуры окружения","World Textures"),CHANGE,WORLD_TEXTURES},{MP_TEXT("Текстуры неба","Sky Textures"),CHANGE,SKY_TEXTURES}};
static const Entry movement[]={{MP_TEXT("Движение   /   Circle Pad","Move   /   Circle Pad"),INFO,0},{MP_TEXT("Присесть","Crouch"),INFO,1},{MP_TEXT("Прыжок   /   B","Jump   /   B"),INFO,0},{MP_TEXT("Действие   /   A","Interact   /   A"),INFO,0},{"Bullet Time   /   ZL",INFO,2},{MP_TEXT("Прыжок с уклонением","Shoot-Dodge"),INFO,3}};
static const Entry combat[]={{MP_TEXT("Выстрел   /   R","Fire   /   R"),INFO,0},{MP_TEXT("Перезарядка   /   X","Reload   /   X"),INFO,0},{MP_TEXT("Болеутоляющее   /   SELECT","Painkiller   /   SELECT"),INFO,0},{MP_TEXT("Быстрый прицел","Quick Aim"),CHANGE,QUICKAIM},{MP_TEXT("Инверсия камеры","Invert Camera"),CHANGE,INVERT}};
static const Entry weapons[]={{MP_TEXT("Иконка оружия / Y","Weapon Icon / Y"),INFO,0},{MP_TEXT("Холодное оружие","Melee Weapons"),INFO,0},{MP_TEXT("Пистолеты","Pistols"),INFO,0},{MP_TEXT("Дробовики","Shotguns"),INFO,0},{MP_TEXT("Автоматы","Automatic Weapons"),INFO,0},{MP_TEXT("Метательное оружие","Throwables"),INFO,0},{MP_TEXT("Снайперская винтовка","Sniper Rifle"),INFO,4}};
static const Entry general[]={{MP_TEXT("Настройка прицела","Reticle"),CHANGE,RETICLE},{MP_TEXT("Мёртвая зона стика","Stick Dead Zone"),CHANGE,DEAD},{MP_TEXT("Автоприцел","Auto Aim"),CHANGE,AUTOAIM},{MP_TEXT("Захват цели","Target Lock"),CHANGE,LOCK},{MP_TEXT("Автосмена оружия","Auto Weapon Switch"),CHANGE,AUTOSWITCH},{MP_TEXT("Подсказки","Hints"),CHANGE,TIPS},{MP_TEXT("Помощь Bullet Time","Bullet Time Assist"),CHANGE,SLOW_ASSIST},{MP_TEXT("Возврат камеры","Recenter Camera"),CHANGE,RECENTER}};
static const Entry sound[]={{MP_TEXT("Громкость музыки","Music Volume"),CHANGE,MUSIC},{MP_TEXT("Громкость звука","Sound Volume"),CHANGE,VOLUME}};
static const Entry quit[]={{MP_TEXT("Назад","Back"),BACK,0},{MP_TEXT("Выйти","Quit"),LEAVE,0}};
#define PAGE(title,items,up) {title,items,sizeof(items)/sizeof(*items),up}
static const PageDef pages[]={PAGE(MP_TEXT("Главное меню","Main Menu"),root,ROOT),PAGE(MP_TEXT("Сложность","Difficulty"),difficulty,ROOT),PAGE(MP_TEXT("Настройки","Options"),options,ROOT),
    PAGE(MP_TEXT("Схемы управления","Control Presets"),sets,OPTIONS),PAGE(MP_TEXT("Управление","Controls"),controls,OPTIONS),PAGE(MP_TEXT("Движение","Movement"),movement,CONTROLS),
    PAGE(MP_TEXT("Бой и камера","Combat and Camera"),combat,CONTROLS),PAGE(MP_TEXT("Оружие","Weapons"),weapons,CONTROLS),PAGE(MP_TEXT("Игра","Gameplay"),general,OPTIONS),
    PAGE(MP_TEXT("Звук","Sound"),sound,OPTIONS),{MP_TEXT("Авторы","Credits"),NULL,0,ROOT},PAGE(MP_TEXT("Выход","Exit"),quit,ROOT),{MP_TEXT("Загрузка","Load Game"),NULL,SAVE_COUNT,ROOT},{MP_TEXT("Сохранение","Save Game"),NULL,SAVE_COUNT-2,ROOT},PAGE(MP_TEXT("Обучение","Tutorial"),training,ROOT),
    PAGE(MP_TEXT("Гироскоп","Gyroscope"),gyro_options,CONTROLS),
    PAGE(MP_TEXT("Чувствительность","Aim Sensitivity"),aim_options,CONTROLS),PAGE(MP_TEXT("Графика","Graphics"),graphics,OPTIONS)};
void ui_animation_card(const char *name,unsigned clip,unsigned total){
    char caption[64];heading(MP_TEXT("Анимации персонажа","Character Animations"));snprintf(caption,sizeof caption,"%u / %u",clip,total);
    label(12,57,0,caption,MUTED,310);paragraph(12,83,0,name,3,WHITE);
    tile(8,153,148,37,MP_TEXT("Предыдущая","Previous"),true,false,-1);tile(164,153,148,37,MP_TEXT("Следующая","Next"),true,false,-1);
    footer(MP_TEXT("L / R   Через 10     •     B   Назад","L / R   Skip 10     /     B   Back"));flush();
}
static uint8_t *setting(int id) {
    switch(id){case DEAD:return &ui_settings.deadzone;case VOLUME:return &ui_settings.volume;case MUSIC:return &ui_settings.music;case RETICLE:return &ui_settings.reticle;
    case AUTOAIM:return &ui_settings.autoaim;case LOCK:return &ui_settings.lock;case AUTOSWITCH:return &ui_settings.autoswitch;
    case SLOW_ASSIST:return &ui_settings.slow_assist;case RECENTER:return &ui_settings.recenter;case QUICKAIM:return &ui_settings.quickaim;case INVERT:return &ui_settings.inverted;
    case GYRO_AIM:return &ui_settings.gyro;
    case GYRO_HORIZONTAL:return &ui_settings.gyro_horizontal;
    case GYRO_VERTICAL:return &ui_settings.gyro_vertical;
    case STICK_X:return &ui_settings.stick_horizontal;
    case STICK_Y:return &ui_settings.stick_vertical;
    case TOUCH_X:return &ui_settings.touch_horizontal;
    case TOUCH_Y:return &ui_settings.touch_vertical;
    case WORLD_TEXTURES:return &ui_settings.world_textures;
    case SKY_TEXTURES:return &ui_settings.sky_textures;
    default:return &ui_settings.tips;}
}
static void write_settings(void){
    if(!settings_dirty)return;
    FILE *file=fopen(config_temp,"wb");
    if(!file)return;
    bool ok=fwrite("MPS2",1,4,file)==4 && fwrite(&ui_settings,sizeof ui_settings,1,file)==1;
    if(fclose(file))ok=false;
    if(ok && !rename(config_temp,config))settings_dirty=false;
}
static unsigned percent_max(int id){
    if(id==GYRO_HORIZONTAL || id==GYRO_VERTICAL)return GYRO_MAX_PERCENT;
    if(id>=STICK_X && id<=TOUCH_Y)return AIM_MAX_PERCENT;
    return 0;
}
static bool change(int id,int step){
    uint8_t *value=setting(id);
    unsigned maximum=percent_max(id);
    int next;
    if(maximum){
        next=*value+step*STEP_PERCENT;
        if(next<MIN_PERCENT || next>(int)maximum)return false;
    }else{
        int limit;
        if(id==WORLD_TEXTURES)limit=new_model?4:1;
        else if(id==SKY_TEXTURES)limit=new_model?5:3;
        else if(id==RETICLE)limit=3;
        else limit=(id>=INVERT)?2:5;
        if(limit==1)return false;
        int current=*value<limit?*value:0;
        next=(current+step+limit)%limit;
    }
    *value=next;
    settings_dirty=true;
    return true;
}
static uint32_t swap_keys(uint32_t keys,uint32_t a,uint32_t b) {
    return (keys&~(a|b))|((keys&a)?b:0)|((keys&b)?a:0);
}
bool ui_touch_shortcuts(void) {return false;}
bool ui_touch_slow(void){return !new_model;}
uint32_t ui_game_keys(uint32_t keys) {
    if(ui_settings.scheme&1)keys=swap_keys(keys,KEY_L,KEY_R);
    if(ui_settings.scheme&2)keys=swap_keys(keys,KEY_X,KEY_Y);
    if(keys&KEY_ZR)keys|=KEY_R;
    return keys;
}
unsigned ui_save_slot;
static SaveHeader slot_info[SAVE_BONUS+1];
static unsigned modes;
static bool slot_valid[SAVE_BONUS+1],any_save,allow_save,cleared;
static void refresh_slots(void) {
    any_save=false;cleared=save_completed();modes=save_unlocked_modes();
    for(unsigned i=0;i<=SAVE_BONUS;i++) {
        slot_valid[i]=save_info(i,slot_info+i);any_save|=slot_valid[i];
    }
}
static int menu_entries(enum Page page){
    if(page==LOAD_SLOTS)return SAVE_COUNT+slot_valid[SAVE_TRAINING]+slot_valid[SAVE_BONUS];
    return pages[page].count;
}
static unsigned menu_slot(enum Page page,int row){
    if(page==SAVE_SLOTS)return row+2;
    if(row==SAVE_TRAINING && !slot_valid[SAVE_TRAINING])return SAVE_BONUS;
    return row;
}
static bool select_save(unsigned slot){
    // Validate only the requested save. Checking every payload would stall the pause menu.
    if(!save_loadable(slot)){
        ui_notice(MP_TEXT("Ошибка чтения сохранения","Could not read save"));
        return false;
    }
    ui_save_slot=slot;return true;
}
static const char *binding_label(const Entry *e) {
    if(!strcmp(e->name,"Bullet Time   /   ZL") && !new_model)return MP_TEXT("Bullet Time   /   экран","Bullet Time   /   Touch Screen");
    if(!strcmp(e->name,MP_TEXT("Выстрел   /   R","Fire   /   R")) && (ui_settings.scheme&1))return MP_TEXT("Выстрел   /   L","Fire   /   L");
    if(!strcmp(e->name,MP_TEXT("Перезарядка   /   X","Reload   /   X")) && (ui_settings.scheme&2))return MP_TEXT("Перезарядка   /   Y","Reload   /   Y");
    if(!strcmp(e->name,MP_TEXT("Иконка оружия / Y","Weapon Icon / Y")) && (ui_settings.scheme&2))return MP_TEXT("Иконка оружия / X","Weapon Icon / X");
    return e->name;
}
static void control_help(enum Page page,const Entry *entry) {
    const char *detail=binding_label(entry);
    if(page==SETS) detail=MP_TEXT("Circle Pad   /   C-Stick   /   стилус","Circle Pad   /   C-Stick   /   Stylus");
    if(page==WEAPONS) detail=(ui_settings.scheme&2)?MP_TEXT("X   Сменить оружие","X   Change Weapon"):MP_TEXT("Y   Сменить оружие","Y   Change Weapon");
    if(entry->argument==1) detail=MP_TEXT("Коснитесь кнопки «Присесть»","Tap Crouch on the touch screen");
    if(entry->argument==2) detail=new_model?MP_TEXT("ZL: вкл./выкл. без прыжка. Или шкала BT","ZL toggles slow motion"):MP_TEXT("Коснитесь шкалы Bullet Time","Tap the Bullet Time meter");
    if(entry->argument==3) detail=(ui_settings.scheme&1)?MP_TEXT("Наклоните Circle Pad и нажмите R","Move and press R"):MP_TEXT("Наклоните Circle Pad и нажмите L","Move and press L");
    if(entry->argument==4) detail=MP_TEXT("A   Сменить увеличение прицела","A   Change scope magnification");
    heading(pages[page].name);label(32,79,1,binding_label(entry),WHITE,316);
    label(32,117,0,detail,PAPER,316);label(32,173,1,MP_TEXT("Назад","Back"),WHITE,310);flush();
    while(aptMainLoop()) {
        bank_tick();hidScanInput();
        if(hidKeysDown()&(KEY_A|KEY_B|KEY_TOUCH|KEY_START)) break;
        gspWaitForVBlank();
    }
}
static bool enabled(const Entry *e,bool paused) {return (e->action!=ANIMATIONS || (paused && pose_bank_count())) && (e->action!=NEWGAME || (modes&(1u<<e->argument))) && (e->action!=RESUME || paused || slot_valid[0]) && (e->action!=BONUS || cleared) &&
        (e->action!=SUB || e->argument!=LOAD_SLOTS || any_save) && (e->action!=SUB || e->argument!=SAVE_SLOTS || allow_save);}
enum { MINUS_X=174, PLUS_X=257, ADJUST_BUTTON_WIDTH=38 };
static void draw_percent(int y,unsigned value,unsigned maximum) {
    char text[8];snprintf(text,sizeof text,"%u%%",value);
    box(MINUS_X,y+2,ADJUST_BUTTON_WIDTH,29,PANEL);
    box(PLUS_X,y+2,ADJUST_BUTTON_WIDTH,29,PANEL);
    uint16_t minus=value==MIN_PERCENT?GREY:WHITE,plus=value==maximum?GREY:WHITE;
    box(MINUS_X+13,y+16,12,2,minus);
    box(PLUS_X+13,y+16,12,2,plus);box(PLUS_X+18,y+11,2,12,plus);
    label(MINUS_X+ADJUST_BUTTON_WIDTH+3,y+7,0,text,PAPER,PLUS_X);
}
enum { ROOT_Y=8, ROOT_PITCH=25, MENU_Y=48, MENU_PITCH=39, SETTING_PITCH=53 };
static bool settings_page(enum Page page){return page==COMBAT || page==GENERAL || page==SOUND || page==GYRO_MENU || page==AIM_MENU || page==GRAPHICS || page==LOAD_SLOTS || page==SAVE_SLOTS;}
static int menu_rows(enum Page page){return page==ROOT?8:settings_page(page)?3:4;}
static int menu_pitch(enum Page page){return page==ROOT?ROOT_PITCH:settings_page(page)?SETTING_PITCH:MENU_PITCH;}
static void menu_sprite(const uint16_t *pixels,int width,int height,int x,int y){
    for(int col=0;col<width;col++)for(int row=0;row<height;row++)
        if(x+col>=0 && x+col<W && y+row>=0 && y+row<H)
            screen[(x+col)*H+H-1-y-row]=pixels[col*height+height-1-row];
}
static void menu_line(int x,int y,int size,const char *name,bool on,bool selected,int right){
    uint16_t color=!on?GREY:selected?WHITE:MUTED;
    label(x+1,y+1,size,name,0,right);label(x,y,size,name,color,right);
}
static void draw_menu(enum Page page,int selected,int top,bool paused,int card) {
    const PageDef *p=&pages[page];
    int entries=menu_entries(page);
    unsigned background=page==ROOT || page==DIFFICULTY?0:page==LOAD_SLOTS || page==SAVE_SLOTS?2:page==QUIT?3:1;
    memcpy(screen,menu_art.backgrounds[background],W*H*2);
    if(page!=ROOT)label(17,12,2,p->name,WHITE,310);
    if(page==CREDITS) {
        label(18,78,1,"Remedy Entertainment",WHITE,311);
        label(18,106,0,"2001",MUTED,311);
        menu_line(18,170,0,card?MP_TEXT("Предыдущая страница","Previous Page"):MP_TEXT("Следующая страница","Next Page"),true,true,310);
    } else for(int k=0;k<menu_rows(page) && k+top<entries;k++) {
        int i=k+top,y=(page==ROOT?ROOT_Y:MENU_Y)+menu_pitch(page)*k;
        if(page==LOAD_SLOTS || page==SAVE_SLOTS) {
            unsigned slot=menu_slot(page,i);
            char prefix[40];
            if(!slot)strcpy(prefix,MP_TEXT("Автосохранение","Autosave"));
            else if(slot==1)strcpy(prefix,MP_TEXT("Быстрое сохранение","Quicksave"));
            else if(slot==SAVE_TRAINING)strcpy(prefix,MP_TEXT("Обучение","Tutorial Autosave"));
            else if(slot==SAVE_BONUS)strcpy(prefix,MP_TEXT("Последняя схватка","Last Challenge Autosave"));
            else snprintf(prefix,sizeof prefix,MP_TEXT("Ячейка %u","Slot %u"),slot-1);
            menu_line(32,y,1,prefix,page==SAVE_SLOTS || slot_valid[slot],i==selected,296);
            label(33,y+24,0,slot_valid[slot]?slot_info[slot].room:MP_TEXT("Пусто","Empty"),MUTED,296);continue;
        }
        const Entry *e=&p->list[i];const char *name=binding_label(e);
        int size=1,x=page==ROOT?18:32;
        menu_line(x,y,size,name,enabled(e,paused),i==selected,296);
        if(e->action==PRESET && e->argument==ui_settings.scheme)menu_sprite(menu_art.toggles[1],18,18,275,y+3);
        if(e->action==CHANGE) {
            y+=23;
            unsigned value=*setting(e->argument);
            unsigned maximum=percent_max(e->argument);
            if(maximum)draw_percent(y,value,maximum);
            else if(page==GRAPHICS){
                bool sky=e->argument==SKY_TEXTURES;
                bool automatic=!value || (!new_model && sky && value>2);
                char resolution[48];
                if(!new_model && !sky)snprintf(resolution,sizeof resolution,"128");
                else if(automatic)snprintf(resolution,sizeof resolution,MP_TEXT("< Авто (%u) >","< Auto (%u) >"),ui_texture_limit(sky));
                else snprintf(resolution,sizeof resolution,"< %u >",ui_texture_limit(sky));
                label(177,y,0,resolution,PAPER,301);
            }
            else if(e->argument==RETICLE){const char *styles[]={MP_TEXT("Крест","Crosshair"),MP_TEXT("Точка","Dot"),MP_TEXT("Круг","Circle")};label(227,y,0,styles[value],PAPER,303);}
            else if(e->argument>=INVERT)menu_sprite(menu_art.toggles[value!=0],18,18,275,y+2);
            else{
                menu_sprite(menu_art.slider,112,14,182,y+9);
                menu_sprite(menu_art.marker,12,18,182+25*value,y+7);
            }
        }
    }
    if(page!=ROOT && entries>menu_rows(page)){
        if(top)label(303,58,1,"^",PAPER,319);
        if(top+menu_rows(page)<entries)label(303,185,1,"v",PAPER,319);
    }
    const char *hint=page==GRAPHICS?MP_TEXT("При следующей загрузке уровня","Applies on next level load"):
        (page==GYRO_MENU || page==AIM_MENU)?MP_TEXT("Влево / вправо   Изменить     B   Назад","Left / Right   Adjust    B   Back"):page==ROOT?
        (paused && allow_save?MP_TEXT("X   Сохранить   /   Y   Загрузить","X   Quicksave   /   Y   Quickload"):MP_TEXT("A   Выбрать   /   Касание","A   Select   /   Touch")):
        MP_TEXT("B   Назад     /     A   Выбрать","B   Back     /     A   Select");
    label(10,220,0,hint,MUTED,314);flush();
}
static UiChoice menu_loop(bool paused,bool can_save,UiPauseFrame redraw,void *context) {
    allow_save=can_save;refresh_slots();
    enum Page page=ROOT;int positions[sizeof pages/sizeof *pages]={0};positions[ROOT]=paused?0:1;
    int top=0,card=0;bool dirty=true;
    static unsigned attract;
    u64 last_input=svcGetSystemTick(),last_frame=last_input-SYSCLOCK_ARM11/15;
    u64 repeat_at=0;
    uint32_t repeat_key=0;
    while(aptMainLoop()) {
        u64 tick=svcGetSystemTick();
        int *position=&positions[page];
        const PageDef *p=&pages[page];
        int entries=menu_entries(page);
        if(*position<top) top=*position;
        if(page!=ROOT && *position>=top+menu_rows(page)) top=*position-menu_rows(page)+1;
        if(dirty) {draw_menu(page,*position,top,paused,card);dirty=false;}
        bank_tick();hidScanInput();
        uint32_t down=hidKeysDown();
        uint32_t held=hidKeysHeld();
        if(held)last_input=tick;
        uint32_t adjust=held&(KEY_DLEFT|KEY_DRIGHT);
        bool percent_row=(page==GYRO_MENU || page==AIM_MENU) && percent_max(p->list[*position].argument);
        if(!percent_row || (adjust!=KEY_DLEFT && adjust!=KEY_DRIGHT))repeat_key=0;
        else if(adjust!=repeat_key || (down&adjust)){
            repeat_key=adjust;repeat_at=tick+SYSCLOCK_ARM11/4;
        }else if(!down && tick>=repeat_at){
            down=repeat_key;repeat_at=tick+SYSCLOCK_ARM11/15;
        }
        if(!paused && page==ROOT && !down && svcGetSystemTick()-last_input>60ULL*SYSCLOCK_ARM11){
            movie_play(attract++%2?"attract01.mpv":"attract00.mpv");ui_title();dirty=true;last_input=svcGetSystemTick();
        }
        if(!down){
            bool adjusting=page==OPTIONS || page==CONTROLS || page==GYRO_MENU || page==AIM_MENU || page==GRAPHICS;
            if(redraw && page!=CREDITS && (!adjusting || tick-last_frame>=SYSCLOCK_ARM11/15)){
                redraw(fminf(.075f,(tick-last_frame)/(float)SYSCLOCK_ARM11),context);
                last_frame=tick;
            }
            gspWaitForVBlank();
            continue;
        }
        bool accept=down&KEY_A,back=down&KEY_B;
        if(page==ROOT && paused && allow_save && (down&KEY_X)){ui_save_slot=1;return UI_SAVE;}
        if(page==ROOT && paused && slot_valid[1] && (down&KEY_Y)){
            if(select_save(1))return UI_LOAD;
            dirty=true;continue;
        }
        if(page==ROOT && !paused && (down&KEY_X)){selected_mode=FUGITIVE;return UI_CAMPAIGN;}
        if(down&KEY_START) {
            if(paused && page==ROOT) return UI_RESUME;
            if(!paused && page==ROOT){page=QUIT;top=0;dirty=true;continue;}
        }
        if(down&KEY_TOUCH) {
            touchPosition pt;hidTouchRead(&pt);
            if(pt.py>=215 && page!=ROOT) back=true;
            else if(page==CREDITS) accept=pt.py>=162 && pt.py<202;
            else if(page!=ROOT && pt.px>=298 && entries>menu_rows(page)) down|=pt.py<127?KEY_DUP:KEY_DDOWN;
            else {
                int start=page==ROOT?ROOT_Y:MENU_Y,pitch=menu_pitch(page);
                int row=(pt.py-start)/pitch,i=top+row;
                bool hit=pt.py>=start && row<menu_rows(page) && (pt.py-start)%pitch<pitch-1 &&
                         pt.px>=12 && pt.px<296;
                if(hit && i<entries && (page==SAVE_SLOTS ||
                    (page==LOAD_SLOTS?slot_valid[menu_slot(page,i)]:enabled(&p->list[i],paused)))){
                    *position=i;dirty=true;accept=true;
                    if(p->list && p->list[i].action==CHANGE && percent_max(p->list[i].argument)){
                        accept=false;
                        if(pt.px>=MINUS_X && pt.px<MINUS_X+ADJUST_BUTTON_WIDTH)down|=KEY_DLEFT;
                        if(pt.px>=PLUS_X && pt.px<PLUS_X+ADJUST_BUTTON_WIDTH)down|=KEY_DRIGHT;
                    }
                }
            }
        }

        if(back) {
            if(page==ROOT && paused) return UI_RESUME;
            if(page==CREDITS){if(paused){if(redraw)redraw(0,context);else room_pause_image(true);}else ui_title();}
            if(page!=ROOT){page=p->parent;top=0;dirty=true;}
            continue;
        }
        if(page==CREDITS) {
            if(accept || (down&(KEY_DRIGHT|KEY_DLEFT))) {
                card^=1;load_top(card?"romfs:/credits1.565":"romfs:/credits0.565");dirty=true;
            }
            continue;
        }
        int delta=0;
        if(down&KEY_DUP) delta=-1;
        if(down&KEY_DDOWN) delta=1;
        if(page==ROOT && (down&(KEY_DLEFT|KEY_DRIGHT))) delta=(down&KEY_DLEFT)?-1:1;
        if(delta) {*position=(*position+delta+entries)%entries;dirty=true;continue;}
        if(page==LOAD_SLOTS || page==SAVE_SLOTS) {
            unsigned slot=menu_slot(page,*position);
            if(accept && page==SAVE_SLOTS){ui_save_slot=slot;return UI_SAVE;}
            if(accept && slot_valid[slot]){
                if(select_save(slot))return UI_LOAD;
                dirty=true;
            }
            continue;
        }
        const Entry *e=&p->list[*position];
        if(e->action==CHANGE && (accept || (down&(KEY_DLEFT|KEY_DRIGHT)))) {
            dirty|=change(e->argument,(down&KEY_DLEFT)?-1:1);continue;
        }
        if(!accept || !enabled(e,paused)) continue;
        switch(e->action) {
        case RESUME:if(paused)return UI_RESUME;if(select_save(0))return UI_LOAD;break;
        case ANIMATIONS:return UI_ANIMATIONS;
        case BONUS:return UI_BONUS;
        case TRAIN:return UI_TUTORIAL;
        case AUTHORED:return UI_ORIGINAL_TUTORIAL;
        case NEWGAME:selected_mode=(enum GameMode)e->argument;return UI_CAMPAIGN;
        case PRESET:if(ui_settings.scheme!=e->argument){ui_settings.scheme=e->argument;settings_dirty=true;}break;
        case LEAVE:return paused?UI_TITLE:UI_EXIT;
        case BACK:page=p->parent;break;
        case INFO:control_help(page,e);break;
        case SUB:if(e->argument==TRAIN_MENU && !full_training)return UI_TUTORIAL;
            page=(enum Page)e->argument;
            if(page==CREDITS){
                gfxSet3D(false);card=0;
                movie_play("credits.mpv");load_top("romfs:/credits0.565");
            }
            break;
        default:continue;
        }
        top=0;dirty=true;
    }
    return UI_EXIT;
}
static UiChoice menu(bool paused,bool can_save,UiPauseFrame redraw,void *context){
    if(paused && !redraw)room_pause_image(true);

    room_pause_overlay(paused && redraw);
    UiChoice result=menu_loop(paused,can_save,redraw,context);
    write_settings();
    room_pause_overlay(false);

    if(paused && (result==UI_RESUME || result==UI_SAVE)){
        if(redraw)redraw(-1,context);else room_pause_image(false);
    }
    return result;
}
UiChoice ui_menu(bool paused,bool can_save){return menu(paused,can_save,NULL,NULL);}
UiChoice ui_pause_menu(bool can_save,UiPauseFrame redraw,void *context){return menu(true,can_save,redraw,context);}
static int last_dir=-1,last_distance=-1;
void ui_hud(const char *area,const char *objective,const char *weapon_name,unsigned health,unsigned pills,int weapon,
            unsigned ammo,unsigned reserve,bool reloading,unsigned zoom,bool can_switch,unsigned round,unsigned remaining,bool force_prompt) {
    static const char *names[]={MP_TEXT("Беретта","Beretta"),MP_TEXT("Дробовик","Pump Shotgun"),MP_TEXT("Винтовка","Rifle")};
    char text[80];
    box(0,0,W,H,BLACK);
    box(0,0,320,25,PANEL);box(0,24,320,1,RED);
    label(9,2,1,area,PAPER,200);
    label(233,4,0,MP_TEXT("START   Пауза","START Pause"),MUTED,316);
    for(int y=0;y<51;y++) for(int x=0;x<23;x++) if(art.silhouette[y][x]>85)
        box(9+x,32+y,1,1,y<51-(int)(health*51/100)?RED:PAPER);
    snprintf(text,sizeof text,"%u%%",health);
    label(39,34,1,text,WHITE,90);
    snprintf(text,sizeof text,"+ %u",pills);
    label(39,59,0,text,pills?PAPER:GREY,89);
    box(88,34,1,49,LINE);
    label(99,31,0,weapon<0?MP_TEXT("Без оружия","Unarmed"):weapon_name?weapon_name:names[weapon],MUTED,211);
    if(weapon>=0) {
        snprintf(text,sizeof text,"%u / %u",ammo,reserve);label(98,49,2,text,WHITE,224);
    }
    if (reloading) label(219,55,0,MP_TEXT("Перезарядка","Reloading"),ACCENT,316);
    else if (weapon==2 && zoom) {snprintf(text,sizeof text,MP_TEXT("Прицел  %ux","Scope  %ux"),zoom);label(226,55,0,text,PAPER,317);}
    if (round || remaining) {
        if(round) snprintf(text,sizeof text,MP_TEXT("Раунд %u   /   Целей %u","Round %u   /   Targets %u"),round,remaining);
        else snprintf(text,sizeof text,MP_TEXT("Целей %u","Targets %u"),remaining);
        label(169,28,0,text,ACCENT,316);
    }
    box(8,94,304,1,LINE);
    if (ui_settings.tips || force_prompt || !health) {
        label(164,103,0,MP_TEXT("ЗАДАЧА","OBJECTIVE"),ACCENT,245);
        paragraph_step(10,118,1,objective,2,PAPER,20);
    }
    (void)can_switch;
    if(ui_touch_shortcuts()){
        TouchRect f=touch_rects[TOUCH_FIRE];
        tile(f.x,f.y,f.w,f.h,MP_TEXT("Огонь","Fire"),weapon>=0,false,-1);
    }
    last_dir=last_distance=-1;flush();
}
static uint16_t mix565(uint16_t back,uint16_t color,unsigned alpha){
    unsigned red=(((back>>11)&31)*(255-alpha)+((color>>11)&31)*alpha)/255;
    unsigned green=(((back>>5)&63)*(255-alpha)+((color>>5)&63)*alpha)/255;
    unsigned blue=((back&31)*(255-alpha)+(color&31)*alpha)/255;
    return (red<<11)|(green<<5)|blue;
}
static void weapon_icon(int x,int y,int gun,uint16_t color){
    if(gun<0 || gun>=15)return;
    for(int row=0;row<24;row++)for(int col=0;col<64;col++){
        unsigned alpha=weapon_art[gun][row][col];
        if(alpha && x+col>=0 && x+col<W && y+row>=0 && y+row<H){
            uint16_t *pixel=screen+(x+col)*H+H-1-y-row;*pixel=mix565(*pixel,color,alpha);
        }
    }
}
WeaponChoice ui_weapon_input(bool pressed,int x,int y,bool enabled,uint32_t owned,int current){
    return touch_weapon_update(&weapon_picker,pressed,x,y,enabled,owned,current);
}
void ui_weapon_close(void){weapon_picker.open=false;}
void ui_weapon_palette(uint32_t owned,uint32_t loaded,int current){
    TouchRect r=touch_rects[TOUCH_WEAPON];box(r.x,r.y,r.w,r.h,PANEL);
    weapon_icon(r.x+6,r.y+6,current,current>=0?PAPER:GREY);
    if(current<0)label(r.x+7,r.y+10,0,MP_TEXT("Оружие","Weapons"),owned?PAPER:GREY,r.x+r.w);
    if(weapon_picker.open){
        box(0,25,320,215,BLACK);
        label(8,27,0,MP_TEXT("ОРУЖИЕ","WEAPONS"),PAPER,108);
        label(113,27,0,MP_TEXT("Коснись значка для выбора","Tap a weapon to select it"),MUTED,315);
        for(unsigned i=0;i<16;i++){
            int gun=touch_weapon_order[i];TouchRect cell=touch_weapon_rect(i);
            bool selected=gun==current,available=gun<0 || (owned&(1u<<gun));
            box(cell.x,cell.y,cell.w,cell.h,selected?LINE:PANEL);
            if(gun<0)label(cell.x+8,cell.y+13,0,MP_TEXT("Назад","Back"),MUTED,cell.x+cell.w);
            else if(gun<TOUCH_WEAPON_COUNT){
                static const char *short_name[]={MP_TEXT("Беретта","Beretta"),MP_TEXT("2 Беретты","2 Berettas"),"D. Eagle",MP_TEXT("Помпа","Pump"),MP_TEXT("Обрез","Sawed-Off"),"Jackhammer",
                    "Ingram","2 Ingram","Commando",MP_TEXT("Снайперка","Sniper"),MP_TEXT("Бита","Baseball Bat"),MP_TEXT("Труба","Lead Pipe"),MP_TEXT("Граната","Grenade"),MP_TEXT("Молотов","Molotov"),"M79"};
                weapon_icon(cell.x+6,cell.y+2,gun,!available?GREY:selected?WHITE:PAPER);
                label(cell.x+4,cell.y+24,0,short_name[gun],!available?GREY:(loaded&(1u<<gun)) || gun==10 || gun==11?PAPER:RED,cell.x+cell.w-2);
            }
        }
    }
    flush();
}
void ui_bearing(float angle,float meters,bool alive) {
    if(!ui_settings.tips || !alive) return;
    static const char *const dir[]={MP_TEXT("Впереди","Ahead"),MP_TEXT("Справа","Right"),MP_TEXT("Сзади","Behind"),MP_TEXT("Слева","Left")};
    int d=fabsf(angle)<.48f?0:fabsf(angle)>2.65f?2:angle>0?1:3,dist=(int)lroundf(meters);
    if(d==last_dir && dist==last_distance) return;
    last_dir=d;last_distance=dist;
    char text[64];snprintf(text,sizeof text,MP_TEXT("%s   %d м","%s   %d m"),dir[d],dist);
    box(220,101,98,19,BLACK);label(222,102,0,text,MUTED,318);
    flush();
}
void ui_combat_hud(float charge,bool slow,bool crouched,bool allowed) {
    TouchRect r=touch_rects[TOUCH_SLOW];
    box(r.x,r.y,r.w,r.h,new_model?BLACK:PANEL);label(r.x+3,r.y,0,"Bullet Time",allowed?(slow?WHITE:MUTED):GREY,r.x+r.w);
    box(r.x+4,r.y+18,r.w-8,3,GREY);
    box(r.x+4,r.y+18,(int)((r.w-8)*fminf(1,fmaxf(0,charge))),3,allowed?(slow?ACCENT:PAPER):GREY);
    ui_stance(crouched);
}
void ui_stance(bool crouched){
    TouchRect r=touch_rects[TOUCH_CROUCH];
    box(r.x,r.y,r.w,r.h,PANEL);label(r.x+3,r.y+2,0,crouched?MP_TEXT("Встать","Stand"):MP_TEXT("Присесть","Crouch"),PAPER,r.x+r.w);
    if(ui_touch_shortcuts()){
        r=touch_rects[TOUCH_JUMP];
        box(r.x,r.y,r.w,r.h,PANEL);label(r.x+3,r.y+2,0,MP_TEXT("Прыжок","Jump"),PAPER,r.x+r.w);
    }
    label(168,177,0,MP_TEXT("Обзор стилусом","Stylus Look"),MUTED,318);
    flush();
}
void ui_race_finish(unsigned seconds,unsigned best) {
    char result[72];heading(MP_TEXT("Минута Нью-Йорка","New York Minute"));
    snprintf(result,sizeof result,MP_TEXT("Ваше время   %u:%02u:%02u","Your Time   %u:%02u:%02u"),seconds/3600,seconds/60%60,seconds%60);
    label(12,75,1,result,PAPER,312);
    if(best){snprintf(result,sizeof result,MP_TEXT("Рекорд   %u:%02u:%02u","Record   %u:%02u:%02u"),best/3600,best/60%60,best%60);
        label(12,112,1,result,ACCENT,312);}
    tile(8,162,304,40,MP_TEXT("Дальше","Next"),true,true,-1);flush();
    while(aptMainLoop()){bank_tick();hidScanInput();if(hidKeysDown()&(KEY_A|KEY_B|KEY_TOUCH))break;gspWaitForVBlank();}
}
void ui_run_status(int seconds,int saves) {
    if(seconds<0 && saves<0)return;
    char text[48];
    if(seconds>=0)snprintf(text,sizeof text,"%d:%02d",seconds/60,seconds%60);
    else snprintf(text,sizeof text,MP_TEXT("Сохр. %d","Saves %d"),saves);
    box(226,65,90,22,PANEL);label(232,66,0,text,seconds>=0 && seconds<=10?ACCENT:PAPER,315);flush();
}
void ui_journal_link(void) {
    TouchRect r=touch_rects[TOUCH_JOURNAL];
    box(r.x,r.y,r.w,r.h,PANEL);label(r.x+3,r.y+2,0,MP_TEXT("Дневник","Journal"),PAPER,r.x+r.w);flush();
}
void ui_journal(const char *objective,const char items[3][96],const char *caption) {
    heading(MP_TEXT("Дневник Макса","Max's Journal"));
    label(10,56,0,MP_TEXT("ЗАДАЧА","OBJECTIVE"),ACCENT,310);
    int y=paragraph(10,74,0,*objective?objective:MP_TEXT("Задача не указана","No objective"),4,PAPER);
    if(*caption && strcmp(caption,objective))y=paragraph(10,y+2,0,caption,2,MUTED);
    bool inventory=false;
    for(unsigned i=0;i<3;i++)if(items[i][0]){
        if(!inventory){y+=4;box(8,y,304,1,LINE);label(10,y+2,0,MP_TEXT("ПРЕДМЕТЫ","ITEMS"),ACCENT,310);y+=20;inventory=true;}
        if(y<=196)y=paragraph(10,y,0,items[i],1,PAPER);
    }
    footer(MP_TEXT("A  /  B   Назад","A  /  B   Back"));flush();
    while(aptMainLoop()){
        hidScanInput();if(hidKeysDown()&(KEY_A|KEY_B|KEY_START|KEY_TOUCH))break;
        gspWaitForVBlank();
    }
}
void ui_caption(const char *area,const char *caption) {
    heading(area);paragraph(12,90,1,caption,4,PAPER);flush();
}
void ui_comic(bool last,bool single,ComicView view) {
    static const char *const controls[]={
        [COMIC_PAGE]=MP_TEXT("A   Дальше     /     B   Закрыть","A   Next     /     B   Close"),
        [COMIC_ZOOM_AVAILABLE]=MP_TEXT("X   Увеличить   /   A   Дальше","X   Zoom   /   A   Next"),
        [COMIC_ZOOM]=MP_TEXT("X   Вся страница   /   Крестовина","X   Full Page   /   D-Pad")
    };
    heading(MP_TEXT("Графический роман","Graphic Novel"));
    tile(8,77,304,56,last?MP_TEXT("Дальше","Next"):MP_TEXT("Следующая страница","Next Page"),true,true,-1);
    tile(8,144,304,54,single?MP_TEXT("Закрыть","Close"):MP_TEXT("Пропустить вступление","Skip Intro"),true,false,-1);
    footer(controls[view]);flush();
}
static int loading_percent=-1;
void ui_loading_progress(unsigned percent){
    if(percent>100)percent=100;
    if((int)percent==loading_percent)return;
    if(loading_percent<0 || percent<(unsigned)loading_percent){
        heading(MP_TEXT("Загрузка","Loading"));box(20,210,280,12,LINE);
    }
    loading_percent=percent;
    box(22,212,276,8,PANEL);box(22,212,276*percent/100,8,ACCENT);
    char value[12];snprintf(value,sizeof value,"%u%%",percent);
    // Restore the artwork under the previous digits; no opaque text backing.
    for(unsigned x=20;x<110;x++)memcpy(screen+x*H+H-210,menu_art.backgrounds[1]+x*H+H-210,28*2);
    label(20,184,1,value,PAPER,300);flush();
}
void ui_loading_begin(void){loading_percent=-1;ui_loading_progress(0);}
void ui_level_loading(const char *stage) {
    ui_loading_begin();
    char path[80];snprintf(path,sizeof path,"loading/%s.565",stage);
    FILE *f=campaign_file(path);if(!f)return;
    gfxSetScreenFormat(GFX_TOP,GSP_RGB565_OES);
    gfxSet3D(false);
    bool ok=fread(gfxGetFramebuffer(GFX_TOP,GFX_LEFT,NULL,NULL),2,400*240,f)==400*240;fclose(f);
    if(ok){gfxFlushBuffers();gfxScreenSwapBuffers(GFX_TOP,true);gspWaitForVBlank();}
}
void ui_status(const char *text){heading(text);flush();}
void ui_notice(const char *text) {
    heading(MP_TEXT("Сообщение","Notice"));label(12,88,1,text,PAPER,317);tile(8,159,304,42,MP_TEXT("Назад","Back"),true,true,-1);flush();
    while(aptMainLoop()){hidScanInput();if(hidKeysDown()&(KEY_A|KEY_B|KEY_TOUCH|KEY_START)) break;gspWaitForVBlank();}
}
