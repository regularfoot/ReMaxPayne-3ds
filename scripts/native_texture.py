"""Encode textures in the mip and 8×8 tile order used by the 3DS GPU."""
import struct

import numpy as np
from PIL import Image

RGB565 = 3
RGBA4 = 4
RGB8 = 1
RGBA8 = 0
TEXEL_BYTES = {RGBA8: 4, RGB8: 3, RGB565: 2, RGBA4: 2}


def _tile(image, fmt):
    pixels = np.asarray(image, dtype=np.uint16)
    r, g, b, a = pixels.transpose(2, 0, 1)
    if fmt in (RGB8, RGBA8):
        h,w=r.shape
        y,x=np.indices((h,w))
        morton=((x&1)|((y&1)<<1)|((x&2)<<1)|((y&2)<<2)|((x&4)<<2)|((y&4)<<3))
        channels=(a,b,g,r) if fmt==RGBA8 else (b,g,r)
        tiled=np.empty((h*w,len(channels)),dtype='u1')
        tiled[(y//8)*w*8+(x//8)*64+morton]=np.stack(channels,axis=-1)[::-1]
        return tiled.tobytes()
    if fmt == RGB565:
        words = (((r * 31 + 127) // 255) << 11 |
                 ((g * 63 + 127) // 255) << 5 |
                 ((b * 31 + 127) // 255))
    else:
        words = (((r + 8) // 17) << 12 | ((g + 8) // 17) << 8 |
                 ((b + 8) // 17) << 4 | (a + 8) // 17)
    h, w = words.shape
    y, x = np.indices((h, w))
    morton = ((x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2)
              | ((x & 4) << 2) | ((y & 4) << 3))
    tiled = np.empty(h * w, dtype='<u2')
    # LDB and KF2 use Direct3D UVs. PICA's first tile row is the bottom of the image.
    tiled[(y // 8) * w * 8 + (x // 8) * 64 + morton] = words[::-1]
    return tiled.tobytes()


def texture(image, *, lightmap=False, max_size=128, full_color=False):
    if max_size not in (128,256,512,1024):
        raise ValueError('Native texture limit must be 128, 256, 512 or 1024')
    image = image.convert('RGBA')
    original=image.copy()
    image.thumbnail((max_size, max_size), Image.Resampling.LANCZOS)
    if max(image.size)>128 and min(image.size)*128<8*max(image.size):
        image=original.copy()
        image.thumbnail((128,128),Image.Resampling.LANCZOS)
    w, h = (max(8, n) for n in image.size)
    if w & (w - 1) or h & (h - 1):
        raise ValueError(f'Unexpected texture size {image.size}')
    if image.size != (w, h):
        image = image.resize((w, h), Image.Resampling.BILINEAR)
    opaque=image.getchannel('A').getextrema()[0] == 255
    # Baked lighting must retain its gradients before multiplication by diffuse.
    # Beam masks also use clamping but still require their authored alpha.
    if opaque:
        fmt = RGB8 if lightmap or full_color else RGB565
    else:
        fmt = RGBA8 if full_color else RGBA4
    # Independent lighting charts share each atlas. Filtering across their edges
    # would give one wall its neighbour's light at a distance.
    max_level = 0 if lightmap else min(w, h).bit_length() - 4
    levels = [struct.pack('<HHBBBB', w, h, fmt, max_level, int(lightmap), 0)]
    for level in range(max_level + 1):
        if level:
            if max(w,h)>128 and max(w,h)>>level>=128:
                # Build the large levels directly from the source. A device
                # that skips the top levels receives the same 128-pixel base.
                image=original.copy()
                limit=max(w,h)>>level
                image.thumbnail((limit,limit),Image.Resampling.LANCZOS)
            else:
                image = image.resize((w >> level, h >> level), Image.Resampling.BOX)
        levels.append(_tile(image, fmt))
    return b''.join(levels)
