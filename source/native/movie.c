#include "language.h"
#include "movie.h"
#include "campaign_file.h"
#include "audio_bank.h"
#include "ui.h"
#include <3ds.h>
#include <stdlib.h>
#include <string.h>

enum { FRAME_BYTES=400*240*2, MAX_BLOCK=FRAME_BYTES+FRAME_BYTES/255+16,
       SAMPLES=1470, CHANNEL=3, BUFFERS=4 };

static bool length(const uint8_t **p,const uint8_t *end,unsigned nibble,unsigned *result) {
    *result=nibble;
    if(nibble!=15) return true;
    unsigned extra;
    do {
        if(*p==end || *result>FRAME_BYTES) return false;
        extra=*(*p)++;*result+=extra;
    } while(extra==255);
    return true;
}

static bool decode(const uint8_t *data,unsigned size,uint8_t *out) {
    const uint8_t *p=data,*end=data+size;
    unsigned written=0;
    while(p<end) {
        unsigned token=*p++,n;
        if(!length(&p,end,token>>4,&n) || n>(unsigned)(end-p) || n>FRAME_BYTES-written) return false;
        memcpy(out+written,p,n);written+=n;p+=n;
        if(p==end) return written==FRAME_BYTES;
        if(end-p<2) return false;
        unsigned offset=p[0]+(p[1]<<8);p+=2;
        if(!offset || offset>written || !length(&p,end,token&15,&n)) return false;
        n+=4;
        if(n>FRAME_BYTES-written) return false;
        // An LZ4 match can repeat a shorter pattern within the same match.
        while(n) {
            unsigned copy=n<offset?n:offset;
            memcpy(out+written,out+written-offset,copy);written+=copy;n-=copy;
        }
    }
    return false;
}

bool movie_play(const char *name) {
    FILE *f=bank_voice_file(name);
    if(!f) return false;
    struct {char magic[4];uint32_t rate,frames;} header;
    bool ok=fread(&header,sizeof header,1,f)==1 && !memcmp(header.magic,"MPV1",4) &&
            header.rate==15 && header.frames && header.frames<20000;
    uint8_t *block=ok?malloc(MAX_BLOCK):NULL;
    int16_t *pcm=ok?linearAlloc(SAMPLES*BUFFERS*sizeof(*pcm)):NULL;
    if(!block || !pcm) ok=false;
    bool sound=ok && R_SUCCEEDED(ndspInit());
    ndspWaveBuf waves[BUFFERS]={0};
    if(sound) {
        ndspChnReset(CHANNEL);ndspChnSetFormat(CHANNEL,NDSP_FORMAT_MONO_PCM16);ndspChnSetRate(CHANNEL,22050);
        float gain=ui_settings.volume/4.f,mix[12]={gain,gain};ndspChnSetMix(CHANNEL,mix);
    }
    if(ok) {gfxSetScreenFormat(GFX_TOP,GSP_RGB565_OES);gfxSet3D(false);ui_status(MP_TEXT("Видео   /   A пропустить","Video   /   A Skip"));}
    u64 start=svcGetSystemTick();
    for(unsigned i=0;ok && i<header.frames && aptMainLoop();i++) {
        uint32_t n;
        unsigned slot=i%BUFFERS;
        ndspWaveBuf *wave=waves+slot;
        while(sound && (wave->status==NDSP_WBUF_QUEUED || wave->status==NDSP_WBUF_PLAYING)) gspWaitForVBlank();
        int16_t *samples=pcm+slot*SAMPLES;
        ok=fread(&n,sizeof n,1,f)==1 && n && n<=MAX_BLOCK && fread(block,1,n,f)==n &&
           fread(samples,sizeof(*samples),SAMPLES,f)==SAMPLES &&
           decode(block,n,gfxGetFramebuffer(GFX_TOP,GFX_LEFT,NULL,NULL));
        if(!ok) break;
        if(sound) {
            DSP_FlushDataCache(samples,SAMPLES*sizeof(*samples));
            *wave=(ndspWaveBuf){.data_pcm16=samples,.nsamples=SAMPLES};
            ndspChnWaveBufAdd(CHANNEL,wave);
        }
        gfxFlushBuffers();gfxScreenSwapBuffers(GFX_TOP,false);
        do {
            hidScanInput();
            if(hidKeysDown()&(KEY_A|KEY_B|KEY_START|KEY_TOUCH)) goto finish;
            gspWaitForVBlank();
        } while(svcGetSystemTick()-start<(u64)(i+1)*SYSCLOCK_ARM11/header.rate);
    }
finish:
    if(sound) {ndspChnWaveBufClear(CHANNEL);ndspExit();}
    if(pcm) linearFree(pcm);
    free(block);fclose(f);return ok;
}
