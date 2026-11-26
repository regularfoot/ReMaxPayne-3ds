#include "touch.h"
#include <math.h>

const TouchRect touch_rects[TOUCH_AIM+1]={
    [TOUCH_HEAL]={4,26,76,65}, [TOUCH_AIM]={84,26,72,65},
    [TOUCH_JOURNAL]={4,98,76,23}, [TOUCH_JUMP]={84,168,72,23},
    [TOUCH_CROUCH]={4,168,76,23}, [TOUCH_SLOW]={84,98,72,23},
    [TOUCH_WEAPON]={4,194,76,42}, [TOUCH_FIRE]={84,194,72,42}
};

TouchAction touch_update(TouchInput *s,bool held,int x,int y,unsigned buttons) {
    TouchAction a={0};
    if (!held) { *s=(TouchInput){0};return a; }
    if (!s->held) {
        s->origin=TOUCH_LOOK;
        for(TouchZone zone=TOUCH_WEAPON;zone<=TOUCH_AIM;zone++){
            TouchRect r=touch_rects[zone];
            if((buttons&TOUCH_BIT(zone)) && x>=r.x && x<r.x+r.w && y>=r.y && y<r.y+r.h){
                s->origin=zone;break;
            }
        }
        a.pressed=s->origin;
    } else if (s->origin==TOUCH_LOOK) {
        a.dx=x-s->x;a.dy=s->y-y;
    }
    s->x=x;s->y=y;s->held=true;a.held=s->origin;return a;
}

float touch_look_angle(float stick,int pixels,float seconds,unsigned setting,bool vertical){
    return (stick*seconds*(vertical?1.8125f:2.5f)+pixels*(vertical?.01125f:.015f))*(.5f+.25f*setting);
}

float cstick_axis(int value,unsigned deadzone_setting){
    // A released C-Stick can report offsets that exceed the lightest Circle Pad setting.
    float dead=fmaxf(24,5+10*deadzone_setting);
    float magnitude=fminf(150,fabsf((float)value));
    return copysignf(fmaxf(0,(magnitude-dead)/(150-dead)),(float)value);
}

// An explicit modal palette uses the whole LCD; gameplay shortcuts stay left.
const int touch_weapon_order[16]={10,11,0,1,2,3,4,5,6,7,8,9,12,13,14,-1};
TouchRect touch_weapon_rect(unsigned slot){return (TouchRect){4+78*(slot%4),50+45*(slot/4),76,42};}
static bool touch_inside(TouchRect r,int x,int y){return x>=r.x && x<r.x+r.w && y>=r.y && y<r.y+r.h;}
WeaponChoice touch_weapon_update(WeaponPicker *p,bool pressed,int x,int y,bool enabled,uint32_t owned,int current){
    WeaponChoice result={.selected=current};
    if(!enabled || !owned){result.changed=p->open;p->open=false;return result;}
    if(!pressed)return result;
    if(p->open){
        result.consumed=true;
        for(unsigned i=0;i<16;i++)if(touch_inside(touch_weapon_rect(i),x,y)){
            int gun=touch_weapon_order[i];
            if(gun<0 || (owned&(1u<<gun))){p->open=false;result.changed=true;if(gun>=0)result.selected=gun;}
            return result;
        }
        p->open=false;result.changed=true;return result;
    }
    if(touch_inside(touch_rects[TOUCH_WEAPON],x,y)){p->open=true;result.consumed=result.changed=true;}
    return result;
}
