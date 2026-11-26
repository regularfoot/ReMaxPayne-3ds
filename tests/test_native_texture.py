"""Regression checks for PICA texture row order, format and mip layout."""
from pathlib import Path
import struct
import sys
import unittest

from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from native_texture import RGB565, RGBA4, RGB8, RGBA8, texture
from campaign_faces import CULL_BACK_FACES, cull_opposing_faces


class TextureTest(unittest.TestCase):
    def test_new_3ds_base_keeps_old_3ds_chain(self):
        image=Image.effect_noise((512,256),48).convert('RGBA')
        old=texture(image)
        high=texture(image,max_size=256)
        w,h,fmt,levels,clamp,reserved=struct.unpack_from('<HHBBBB',high)
        self.assertEqual((w,h),(256,128))
        self.assertEqual(struct.pack('<HHBBBB',w//2,h//2,fmt,levels-1,clamp,reserved),old[:8])
        self.assertEqual(high[8+w*h*2:],old[8:])
        # Very thin strips cannot have a valid 128x4 level on PICA.
        strip=texture(Image.new('RGBA',(512,8),'white'),max_size=256)
        self.assertLessEqual(max(struct.unpack_from('<HH',strip)),128)

    def test_512_base_preserves_both_hardware_fallbacks(self):
        image=Image.effect_noise((1024,512),48).convert('RGBA')
        high=texture(image,max_size=512)
        medium=texture(image,max_size=256)
        low=texture(image,max_size=128)
        w,h,fmt,levels,clamp,reserved=struct.unpack_from('<HHBBBB',high)
        self.assertEqual((w,h),(512,256))
        self.assertEqual(high[8+w*h*2:],medium[8:])
        self.assertEqual(high[8+w*h*2+(w//2)*(h//2)*2:],low[8:])
        strip=texture(Image.new('RGB',(1024,32),'white'),max_size=512)
        self.assertLessEqual(max(struct.unpack_from('<HH',strip)),128)

    def test_asymmetric_image_and_mip_chain(self):
        image = Image.new('RGB', (32, 16), 'red')
        image.paste('blue', (0, 8, 32, 16))
        image.putpixel((31, 15), (0, 255, 0))
        blob = texture(image)
        self.assertEqual(struct.unpack_from('<HHBBBB', blob), (32, 16, RGB565, 1, 0, 0))
        self.assertEqual(len(blob), 8 + 2 * (32 * 16 + 16 * 8))
        words = struct.unpack_from('<512H', blob, 8)
        self.assertEqual(words[0], 0x001f)            # bottom-left of the source
        self.assertEqual(words[3 * 64 + 21], 0x07e0)  # bottom-right, Morton x=7
        self.assertEqual(words[4 * 64], 0xf800)       # first texel in the next tile row
        self.assertEqual(struct.unpack_from('<H', blob, 8 + 1024)[0], 0x001f)

    def test_alpha_and_lightmap_boundaries(self):
        mask = Image.new('RGBA', (32, 32), (255, 119, 0, 85))
        alpha = texture(mask)
        self.assertEqual(struct.unpack_from('<HHBBBBH', alpha), (32, 32, RGBA4, 2, 0, 0, 0xf705))
        atlas = texture(mask.convert('RGB'), lightmap=True)
        self.assertEqual(struct.unpack_from('<HHBBBB', atlas), (32, 32, RGB8, 0, 1, 0))
        self.assertEqual(len(atlas), 8 + 32 * 32 * 3)
        self.assertEqual(atlas[8:11],bytes((0,119,255)))
        self.assertEqual(struct.unpack_from('<HHBBBB',texture(mask,lightmap=True)),(32,32,RGBA4,0,1,0))

    def test_lighting_gradient_retains_all_source_levels(self):
        image=Image.new('RGB',(128,8))
        for x in range(128):
            for y in range(8):image.putpixel((x,y),(x,x,x))
        atlas=texture(image,lightmap=True)
        self.assertEqual(set(atlas[8:]),set(range(128)))

    def test_sky_color_and_alpha_preserve_mip_fallback(self):
        image=Image.new('RGBA',(1024,128),(20,27,35,255))
        image.paste((22,30,55,103),(0,64,512,128))
        image.putpixel((0,127),(13,29,61,171))
        rgba=texture(image,max_size=1024,full_color=True)
        self.assertEqual(struct.unpack_from('<HHBBBB',rgba),(1024,128,RGBA8,4,0,0))
        self.assertEqual(rgba[8:12],bytes((171,61,29,13)))
        rgb=texture(image.convert('RGB'),max_size=1024,full_color=True)
        for blob,source,stride in ((rgba,image,4),(rgb,image.convert('RGB'),3)):
            fallback=texture(source,max_size=128,full_color=True)
            skipped=sum((1024>>level)*(128>>level)*stride for level in range(3))
            self.assertEqual(blob[8+skipped:],fallback[8:])

    def test_opaque_overlap_only_culls_the_opposing_faces(self):
        def triangle(points):
            return b''.join(struct.pack('<7f', *point, 0, 0, 0, 0) for point in points)
        a=(0,0,0)
        b=(2,0,0)
        c=(0,2,0)
        front=triangle((a,b,c))
        reverse=triangle((a,c,b))
        adjacent=triangle((b,c,(2,2,0)))
        separated=triangle(((0,0,.01),(0,2,.01),(2,0,.01)))
        opaque=texture(Image.new('RGB',(8,8),'white'))
        batches=[(0,0,-1,front+adjacent+separated),(0,1,-1,reverse),(0,0,0,reverse)]
        self.assertEqual(cull_opposing_faces(batches,[opaque]),[
            (0,0,-1,adjacent+separated),(0,CULL_BACK_FACES,-1,front),
            (0,1|CULL_BACK_FACES,-1,reverse),(0,0,0,reverse)])


if __name__ == '__main__':
    unittest.main()
