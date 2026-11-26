/* Replay the locally captured NV2A draws inside the same ARM renderer as the game. */
#include <3ds.h>
#include "nv2a_pb_exec.c"

u32 __ctru_linear_heap_size = 2u << 20;
uint32_t g_xbox_code_hi, g_xbox_code_lo;
static uint8_t *ram;
static unsigned sky_batches, depth_batches;

uint8_t *mp_video_ram(void) { return ram; }
void mp_video_flip(uint32_t va, uint32_t pitch, uint32_t w, uint32_t h, uint32_t bpp, uint64_t pixels) {}
void mp_pica_report(unsigned pixels, int depth) {
    if (depth) ++depth_batches;
    else ++sky_batches;
}

static int run_frame(FILE *in, FILE *log, unsigned pass) {
    rewind(in);
    if (fread(&g_xbox_code_hi,sizeof g_xbox_code_hi,1,in)!=1 ||
        fread(ram,1,MP_XBOX_RAM,in)!=MP_XBOX_RAM) return 1;
    unsigned draws=0;
    uint64_t ticks=0;
    sky_batches=depth_batches=0;
    uint32_t size;
    size_t got;
    while ((got=fread(&size,1,sizeof size,in))!=0) {
        if (got!=sizeof size || size!=sizeof s_gpu || fread(&s_gpu,size,1,in)!=1 || s_gpu.idx_count>NV_MAX_INDICES) return 2;
        for (;;) {
            uint32_t page;
            if (fread(&page,4,1,in)!=1) return 3;
            if (page==UINT32_MAX) break;
            if (page>MP_XBOX_RAM-4096 || fread(ram+page,4096,1,in)!=1) return 4;
        }
        uint64_t before=svcGetSystemTick();
        raster_batch();
        ticks+=svcGetSystemTick()-before;
        ++draws;
    }
    if (ferror(in) || draws!=93 || !surface_begin_batch(ram) || !zeta_pixels() ||
        s_gpu.clip_w!=MP_PAL_W || s_gpu.clip_h!=MP_PAL_H) return 5;
    uint32_t color_hash=2166136261u, depth_hash=2166136261u;
    uint32_t *color=(uint32_t *)s_surface, *depth=zeta_pixels();
    for (unsigned y=0;y<MP_LCD_H;++y) for (unsigned x=0;x<MP_LCD_W;++x) {
        unsigned gx=mp_pal_x(x);
        color_hash=(color_hash^color[2*y*(s_gpu.pitch/4)+gx])*16777619u;
        depth_hash=(depth_hash^depth[2*y*(s_gpu.zpitch/4)+gx])*16777619u;
    }
    fprintf(log,"%s draws=%u sky=%u depth=%u render_ms=%.3f color=%08lx z=%08lx\n",
            pass ? "warm" : "cold",draws,sky_batches,depth_batches,ticks*1000.0/SYSCLOCK_ARM11,
            (unsigned long)color_hash,(unsigned long)depth_hash);
    return color_hash!=0xbf6cf0b9u || depth_hash!=0x22be79c5u || !sky_batches || !depth_batches;
}

int main(void) {
    gfxInit(GSP_BGR8_OES,GSP_RGB565_OES,false);
    consoleInit(GFX_BOTTOM,NULL);
    osSetSpeedupEnable(true);
    const char *base="sdmc:/3ds/maxpayne/tutorial.capture";
    FILE *in=fopen(base,"rb"), *log=fopen("sdmc:/3ds/maxpayne/replay.tmp","w");
    /* newlib's small default stream buffer turns the 64 MiB restore into
     * thousands of emulated SD requests. Restore in large sequential reads. */
    static char file_buffer[256*1024];
    if (in) setvbuf(in,file_buffer,_IOFBF,sizeof file_buffer);
    ram=malloc(MP_XBOX_RAM);
    int rc=1;
    if (in && ram && log) {
        rc=run_frame(in,log,0);
        if (!rc) rc=run_frame(in,log,1);
    }
    if (in) fclose(in);
    if (log) {
        fprintf(log,"result=%d\n",rc);
        fclose(log);
        rename("sdmc:/3ds/maxpayne/replay.tmp","sdmc:/3ds/maxpayne/replay.txt");
    }
    mp_pica_shutdown(); free(ram); gfxExit();
    return rc;
}
