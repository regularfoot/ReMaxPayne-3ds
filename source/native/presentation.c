#include "presentation.h"
#include "campaign_file.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
Presentation presentation;
static Tween tween(float a,float b,float seconds) {return (Tween){a,b,0,fmaxf(0,seconds)};}
float presentation_value(Tween t) {
    float progress=t.duration>0?fminf(1,t.time/t.duration):1;
    return t.first+(t.last-t.first)*progress;
}
static uint32_t mix_color(uint32_t a,uint32_t b,float t) {
    uint32_t value=0;
    for(unsigned i=0;i<32;i+=8) {
        float x=(a>>i)&255,y=(b>>i)&255;
        value|=(uint32_t)lroundf(x+(y-x)*t)<<i;
    }
    return value;
}
bool presentation_open(const char *stage,char chapter[64]) {
    FILE *f=campaign_file("stages.idx");if(!f)return false;
    struct {char tag[4];uint32_t count;} h;
    struct {char stage[32],chapter[64];uint32_t enabled,color;float near,far;} row;
    bool found=false;
    if(fread(&h,sizeof h,1,f)==1 && !memcmp(h.tag,"MPSI",4) && h.count<=128) {
        for(unsigned i=0;i<h.count && fread(&row,sizeof row,1,f)==1;i++) {
            if(!memchr(row.stage,0,sizeof row.stage) || !memchr(row.chapter,0,sizeof row.chapter))break;
            if(strcmp(row.stage,stage))continue;
            memcpy(chapter,row.chapter,sizeof row.chapter);
            presentation=(Presentation){.fog=row.enabled,.fog_from=row.color,.fog_to=row.color,
                .fov=tween(1,1,0),.visibility=tween(1,1,0),.fog_near=tween(row.near,row.near,0),.fog_far=tween(row.far,row.far,0)};
            found=true;break;
        }
    }
    fclose(f);return found;
}
uint32_t presentation_fog(void) {return mix_color(presentation.fog_from,presentation.fog_to,presentation_value(presentation.fog_mix));}
uint32_t presentation_fade(void) {return mix_color(presentation.fade_from,presentation.fade_to,presentation_value(presentation.fade));}
uint32_t presentation_flash(void) {
    float t=presentation.flash_time-presentation.flash_in;
    float opacity=t<0?presentation.flash_time/presentation.flash_in:t<presentation.flash_hold?1:
        presentation.flash_out>0?fmaxf(0,1-(t-presentation.flash_hold)/presentation.flash_out):0;
    return (presentation.flash_color&0xffffff00u)|(uint32_t)lroundf((presentation.flash_color&255)*opacity);
}
void presentation_event(const char *op,const char *const *a,unsigned count,bool keep_hud) {
    if(!strcmp(op,"gm_changefov") && count>=2) {
        presentation.fov=tween(presentation_value(presentation.fov),fminf(3,fmaxf(.15f,strtof(a[0],NULL))),strtof(a[1],NULL));
    } else if(!strcmp(op,"gm_changefog") && count==4) {
        float duration=strtof(a[3],NULL);
        presentation.fog_from=presentation_fog();presentation.fog_to=strtoul(a[0],NULL,0);presentation.fog=true;
        presentation.fog_mix=tween(0,1,duration);
        presentation.fog_near=tween(presentation_value(presentation.fog_near),strtof(a[1],NULL),duration);
        presentation.fog_far=tween(presentation_value(presentation.fog_far),strtof(a[2],NULL),duration);
    } else if(!strcmp(op,"mphm_fadetocolor") && count==3) {
        presentation.fade_from=strtoul(a[0],NULL,0);presentation.fade_to=strtoul(a[1],NULL,0);
        presentation.fade=tween(0,1,strtof(a[2],NULL));
    } else if(!strcmp(op,"mphm_flashtocolor") && count==4) {
        presentation.flash_color=strtoul(a[0],NULL,0);presentation.flash_time=0;
        presentation.flash_in=fmaxf(0,strtof(a[1],NULL));presentation.flash_hold=fmaxf(0,strtof(a[2],NULL));presentation.flash_out=fmaxf(0,strtof(a[3],NULL));
    } else if(!keep_hud && !strcmp(op,"mphm_enablehud") && count) {
        float on=strcmp(a[0],"false") && strcmp(a[0],"0");presentation.visibility=tween(on,on,0);
    } else if(!keep_hud && !strcmp(op,"mphm_changehudvisibility") && count>=2) {
        presentation.visibility=tween(presentation_value(presentation.visibility),strtof(a[0],NULL)>0?1:0,strtof(a[1],NULL));
    }
}
void presentation_tick(float dt) {
    Tween *tracks[]={&presentation.fov,&presentation.visibility,&presentation.fog_near,&presentation.fog_far,&presentation.fog_mix,&presentation.fade};
    for(unsigned i=0;i<sizeof tracks/sizeof tracks[0];i++) tracks[i]->time=fminf(tracks[i]->duration,tracks[i]->time+dt);
    presentation.flash_time=fminf(presentation.flash_in+presentation.flash_hold+presentation.flash_out,presentation.flash_time+dt);
}
