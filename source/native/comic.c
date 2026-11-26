#include "campaign_file.h"
#include "comic.h"
#include "ui.h"
#include "audio_bank.h"
#include <3ds.h>
#include <stdlib.h>
#include <string.h>

enum { CHANNEL=3, BUFFERS=3, SAMPLES=2048, PAGE_WIDTH=400, PAGE_HEIGHT=240 };
typedef struct {
    FILE *file;
    int16_t *pcm;
    ndspWaveBuf wave[BUFFERS];
    unsigned next;
} Narration;

static void fill_audio(Narration *voice) {
    while (voice->file && !feof(voice->file) && !ferror(voice->file)) {
        unsigned i=voice->next;
        ndspWaveBuf *wave=&voice->wave[i];
        if (wave->status==NDSP_WBUF_QUEUED || wave->status==NDSP_WBUF_PLAYING) break;
        int16_t *samples=voice->pcm+i*SAMPLES;
        size_t count=fread(samples,sizeof(*samples),SAMPLES,voice->file);
        if (!count) break;
        DSP_FlushDataCache(samples,count*sizeof(*samples));
        *wave=(ndspWaveBuf){.data_pcm16=samples,.nsamples=count};
        ndspChnWaveBufAdd(CHANNEL,wave);
        voice->next=(i+1)%BUFFERS;
    }
}

static bool show_page(FILE *file) {
    return fread(gfxGetFramebuffer(GFX_TOP,GFX_LEFT,NULL,NULL),2,PAGE_WIDTH*PAGE_HEIGHT,file)==PAGE_WIDTH*PAGE_HEIGHT;
}
static void show_zoom(const uint16_t *pixels,int x,int y) {
    uint16_t *screen=(uint16_t *)gfxGetFramebuffer(GFX_TOP,GFX_LEFT,NULL,NULL);
    // Both assets store pixels in bottom-up columns, just like the 3DS LCD.
    const uint16_t *first=pixels+x*(2*PAGE_HEIGHT)+PAGE_HEIGHT-y;
    for(unsigned column=0;column<PAGE_WIDTH;column++)
        memcpy(screen+column*PAGE_HEIGHT,first+column*(2*PAGE_HEIGHT),PAGE_HEIGHT*2);
}
static int pan(int position,int change,int end) {
    position+=change;
    return position<0?0:position>end?end:position;
}

static FILE *page_asset(const char *folder,const char *name) {
    char path[144];
    snprintf(path,sizeof path,"%s/%s.565",folder,name);
    return campaign_file(path);
}

static bool read_pages(unsigned first,unsigned last,const char *const *names,FILE *journal) {
    gfxSetScreenFormat(GFX_TOP,GSP_RGB565_OES);
    bool audio=R_SUCCEEDED(ndspInit());
    Narration voice={0};
    if (audio) {
        voice.pcm=linearAlloc(BUFFERS*SAMPLES*sizeof(*voice.pcm));
        ndspChnReset(CHANNEL);
        ndspChnSetFormat(CHANNEL,NDSP_FORMAT_MONO_PCM16);
        ndspChnSetRate(CHANNEL,22050);
        float gain=.85f*ui_settings.volume/4.f,mix[12]={gain,gain};
        ndspChnSetMix(CHANNEL,mix);
    }
    bool ok=true,closed=false;
    gfxSet3D(false);
    for (unsigned page=first;page<=last && !closed;page++) {
        char path[144];
        if(names) {
            const char *name=names[page];
            if(strlen(name)>50 || strspn(name,"abcdefghijklmnopqrstuvwxyz0123456789_-")!=strlen(name)){ok=false;break;}
            snprintf(path,sizeof path,"pages/%s.565",name);
        } else snprintf(path,sizeof path,"romfs:/page%u.565",page);
        FILE *image=names?campaign_file(path):fopen(path,"rb");
        ok=image && show_page(image);
        if (image) fclose(image);
        if (!ok) break;
        FILE *zoom=names?page_asset("pages-zoom",names[page]):NULL;
        uint16_t *pixels=NULL;
        int x=0,y=0;
        ComicView view=zoom?COMIC_ZOOM_AVAILABLE:COMIC_PAGE;
        if (voice.pcm) {
            strcpy(strrchr(path,'.'),".pcm");
            voice.file=names?campaign_file(path):fopen(path,"rb");
            voice.next=0;
            for (unsigned i=0;i<BUFFERS;i++) voice.wave[i]=(ndspWaveBuf){0};
            fill_audio(&voice);
        }
        ui_comic(page==last,first==last,view);
        gfxFlushBuffers();gfxScreenSwapBuffers(GFX_TOP,false);gspWaitForVBlank();
        if (journal) {fprintf(journal,"COMIC page=%u voice=%u\n",page,voice.file!=NULL);fflush(journal);}
        while (ok) {
            if (!aptMainLoop()) {closed=true;break;}
            fill_audio(&voice);bank_tick();
            hidScanInput();
            u32 down=hidKeysDown();
            if(down&KEY_TOUCH) {
                touchPosition tap;hidTouchRead(&tap);
                if(tap.px>=8 && tap.px<312 && tap.py>=77 && tap.py<133) down|=KEY_A;
                if(tap.px>=8 && tap.px<312 && tap.py>=144 && tap.py<198) down|=KEY_B;
                if(zoom && tap.py>=215)down|=KEY_X;
            }
            if(zoom && (down&KEY_X)){
                if(!pixels){
                    const size_t count=4*PAGE_WIDTH*PAGE_HEIGHT;
                    pixels=malloc(count*sizeof(*pixels));
                    if(!pixels || fread(pixels,sizeof(*pixels),count,zoom)!=count){
                        free(pixels);pixels=NULL;
                        fclose(zoom);zoom=NULL;
                    }
                }
                if(view==COMIC_ZOOM){
                    FILE *base=page_asset("pages",names[page]);
                    ok=base && show_page(base);
                    if(base)fclose(base);
                    view=COMIC_ZOOM_AVAILABLE;
                }else if(pixels){
                    show_zoom(pixels,x,y);
                    view=COMIC_ZOOM;
                }else{
                    view=COMIC_PAGE;
                }
                if(!ok)break;
                ui_comic(page==last,first==last,view);
                if(pixels){gfxFlushBuffers();gfxScreenSwapBuffers(GFX_TOP,false);}
                down&=~KEY_A;
            }else if(view==COMIC_ZOOM){
                u32 held=hidKeysHeld();
                int nx=pan(x,6*(!!(held&KEY_DRIGHT)-!!(held&KEY_DLEFT)),PAGE_WIDTH);
                int ny=pan(y,6*(!!(held&KEY_DDOWN)-!!(held&KEY_DUP)),PAGE_HEIGHT);
                if(nx!=x || ny!=y){
                    x=nx;y=ny;show_zoom(pixels,x,y);
                    gfxFlushBuffers();gfxScreenSwapBuffers(GFX_TOP,false);
                }
            }
            if (down&(KEY_A|KEY_B)) {closed=(down&KEY_B)!=0;break;}
            gspWaitForVBlank();
        }
        if (zoom) fclose(zoom);
        free(pixels);
        if (audio) ndspChnWaveBufClear(CHANNEL);
        if (voice.file) {fclose(voice.file);voice.file=NULL;}
        if (!ok) break;
    }
    if (audio) ndspExit();
    if (voice.pcm) linearFree(voice.pcm);
    return ok;
}

bool comic_read(unsigned first,unsigned last,FILE *journal) {return read_pages(first,last,NULL,journal);}
bool comic_campaign(const char *const *names,unsigned count,FILE *journal) {
    return !count || read_pages(0,count-1,names,journal);
}
