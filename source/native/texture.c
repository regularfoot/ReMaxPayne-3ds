#include "texture.h"
#include <3ds.h>
#include <stdint.h>

typedef struct {
    uint16_t width,height;
    uint8_t format,max_level,clamp,reserved;
} TextureHeader;
_Static_assert(sizeof(TextureHeader)==8,"texture file layout");

static bool valid_size(unsigned size) {
    return size>=8 && size<=1024 && !(size&(size-1));
}

bool native_texture_read(FILE *file,C3D_Tex *tex) {
    TextureHeader h;
    if (fread(&h,sizeof h,1,file)!=1 || !valid_size(h.width) || !valid_size(h.height) ||
        (h.format!=GPU_RGBA8 && h.format!=GPU_RGB8 && h.format!=GPU_RGB565 && h.format!=GPU_RGBA4) || h.clamp>1 || h.reserved ||
        h.max_level>C3D_TexCalcMaxLevel(h.width,h.height)) return false;
    unsigned bytes_per_texel=h.format==GPU_RGBA8?4:h.format==GPU_RGB8?3:2;
    // Both models share a package; only New 3DS allocates the larger base.
    static bool checked,enhanced;
    if(!checked){APT_CheckNew3DS(&enhanced);checked=true;}
    while(!enhanced && (h.width>128 || h.height>128)){
        if(!h.max_level || fseek(file,(long)h.width*h.height*bytes_per_texel,SEEK_CUR))return false;
        h.width>>=1;h.height>>=1;h.max_level--;
    }
    C3D_TexInitParams params={.width=h.width,.height=h.height,.format=h.format,
                              .maxLevel=h.max_level,.type=GPU_TEX_2D};
    if (!C3D_TexInitWithParams(tex,NULL,params)) return false;
    uint32_t bytes=C3D_TexCalcTotalSize(tex->size,h.max_level);
    if (fread(tex->data,1,bytes,file)!=bytes) {
        C3D_TexDelete(tex);
        return false;
    }
    C3D_TexFlush(tex);
    C3D_TexSetFilter(tex,GPU_LINEAR,GPU_LINEAR);
    C3D_TexSetFilterMipmap(tex,GPU_LINEAR);
    GPU_TEXTURE_WRAP_PARAM wrap=h.clamp?GPU_CLAMP_TO_EDGE:GPU_REPEAT;
    C3D_TexSetWrap(tex,wrap,wrap);
    return true;
}
