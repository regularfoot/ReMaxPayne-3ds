#include "phone.h"
#include "ui.h"
#include <3ds.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static ndspWaveBuf ring,answer,shot,reload,pump,pump_reload,door,npc,cabinet,dose,rifle,rifle_reload,button;

static bool read_sample(ndspWaveBuf *out,const char *path) {
    FILE *f=fopen(path,"rb");
    if (!f) return false;
    bool ok=false;
    if (fseek(f,0,SEEK_END)==0) {
        long size=ftell(f);
        if (size>0 && size<=256*1024 && !(size&1) && fseek(f,0,SEEK_SET)==0) {
            int16_t *data=linearAlloc(size);
            if (data && fread(data,1,size,f)==(size_t)size) {
                DSP_FlushDataCache(data,size);
                out->data_pcm16=data;
                out->nsamples=(unsigned)size/2;
                ok=true;
            } else if (data) linearFree(data);
        }
    }
    fclose(f);
    return ok;
}

void phone_mix(float dx,float dz,float yaw) {
    float distance=hypotf(dx,dz);
    float pan=(cosf(yaw)*dx-sinf(yaw)*dz)/fmaxf(1,distance);
    float gain=.85f*(ui_settings.volume/4.f)/(1+.10f*distance);
    float mix[12]={gain*(1-.55f*pan),gain*(1+.55f*pan)};
    ndspChnSetMix(0,mix);
}
void phone_volume(void) {
    float gain=ui_settings.volume/4.f;
    float center[12]={.78f*gain,.78f*gain},far_mix[12]={.35f*gain,.35f*gain};
    ndspChnSetMix(1,center);ndspChnSetMix(2,far_mix);
}

bool phone_open(bool house) {
    if (R_FAILED(ndspInit())) return false;
    if (!read_sample(&ring,"romfs:/ring.pcm") || (!house && !read_sample(&answer,"romfs:/answer.pcm")) ||
        !read_sample(&shot,"romfs:/shot.pcm") || !read_sample(&reload,"romfs:/reload.pcm") ||
        (!house && (!read_sample(&pump_reload,"romfs:/pump_reload.pcm") ||
        !read_sample(&pump,"romfs:/pump.pcm") || !read_sample(&door,"romfs:/door.pcm")))) {
        phone_close();
        return false;
    }
    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    ndspChnReset(0);
    ndspChnSetFormat(0,NDSP_FORMAT_MONO_PCM16);
    ndspChnSetRate(0,22050);
    ndspChnSetInterp(0,NDSP_INTERP_LINEAR);
    ndspChnReset(1);
    ndspChnSetFormat(1,NDSP_FORMAT_MONO_PCM16);
    ndspChnSetRate(1,22050);
    ndspChnSetInterp(1,NDSP_INTERP_LINEAR);
    ndspChnReset(2);ndspChnSetFormat(2,NDSP_FORMAT_MONO_PCM16);ndspChnSetRate(2,22050);
    phone_volume();
    phone_mix(0,0,0);
    ring.looping=true;
    if (!house) ndspChnWaveBufAdd(0,&ring);
    return true;
}

void phone_answer(void) {
    ndspChnWaveBufClear(0);
    phone_mix(0,0,0);
    ndspChnWaveBufAdd(0,&answer);
}

void phone_ring(bool enabled) {
    ndspChnWaveBufClear(0);
    if (enabled) ndspChnWaveBufAdd(0,&ring);
}
void door_sound(void) {
    ndspChnWaveBufClear(0);
    phone_mix(0,0,0);
    ndspChnWaveBufAdd(0,&door);
}
void switch_sound(void) {
    if (!button.data_vaddr && !read_sample(&button,"romfs:/switch.pcm")) return;
    ndspChnWaveBufClear(0);
    phone_mix(0,0,0);
    ndspChnWaveBufAdd(0,&button);
}
void medicine_preload(void){
    if(!cabinet.data_vaddr)read_sample(&cabinet,"romfs:/cabinet.pcm");
    if(!dose.data_vaddr)read_sample(&dose,"romfs:/dose.pcm");
}
void medicine_sound(bool taking) {
    ndspWaveBuf *wave=taking?&dose:&cabinet;
    if (!wave->data_vaddr && !read_sample(wave,taking?"romfs:/dose.pcm":"romfs:/cabinet.pcm")) return;
    ndspChnWaveBufClear(0);
    phone_mix(0,0,0);
    ndspChnWaveBufAdd(0,wave);
}
void enemy_sound(bool shotgun,bool loading) {
    ndspChnWaveBufClear(2);
    memset(&npc,0,sizeof npc);
    const ndspWaveBuf *sample=loading?(shotgun?&pump_reload:&reload):(shotgun?&pump:&shot);
    npc.data_vaddr=sample->data_vaddr;npc.nsamples=sample->nsamples;
    ndspChnWaveBufAdd(2,&npc);
}
static void free_sample(ndspWaveBuf *wave) {
    if (wave->data_vaddr) linearFree((void *)wave->data_vaddr);
    *wave=(ndspWaveBuf){0};
}
void phone_finish_calls(void) {
    ndspChnWaveBufClear(0);
    free_sample(&ring);
    free_sample(&answer);
}
void weapon_sound(bool loading,Weapon weapon) {
    static ndspWaveBuf *const shots[]={&shot,&pump,&rifle};
    static ndspWaveBuf *const reloads[]={&reload,&pump_reload,&rifle_reload};
    ndspWaveBuf *wave=loading?reloads[weapon]:shots[weapon];
    if (weapon==RIFLE && !wave->data_vaddr &&
        !read_sample(wave,loading?"romfs:/sniper_reload.pcm":"romfs:/sniper.pcm")) return;
    ndspChnWaveBufClear(1);
    ndspChnWaveBufAdd(1,wave);
}

static void medicine_close(void) {
    ndspChnWaveBufClear(0);
    free_sample(&cabinet);
    free_sample(&dose);
}
void phone_close(void) {
    medicine_close();
    ndspChnWaveBufClear(2);
    ndspChnWaveBufClear(1);
    ndspChnWaveBufClear(0);
    ndspExit();
    free_sample(&ring);free_sample(&answer);free_sample(&shot);free_sample(&reload);
    free_sample(&pump_reload);free_sample(&pump);free_sample(&door);
    free_sample(&rifle);free_sample(&rifle_reload);free_sample(&button);
}
