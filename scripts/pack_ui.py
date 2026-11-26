"""Bake lower LCD artwork from the local release and a Cyrillic-capable system font."""
from io import BytesIO
from pathlib import Path
import argparse
import os
import re
import struct
import numpy as np
from PIL import Image, ImageDraw, ImageFont, ImageOps
from pack_native import member


def lower_ui(archives, output):
    def picture(path):
        return Image.open(BytesIO(member(archives/'x_english.ras', 'data/'+path+'.dds'))).convert('RGBA')

    def lcd(image):
        r, g, b = np.asarray(image.convert('RGB'), dtype=np.uint16).transpose(2, 0, 1)
        return ((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3)).astype('<u2')

    bg = picture('menu/bitmaps/menu_bg_generic')
    rows = np.flatnonzero((np.asarray(bg)[:, :, :3].max(axis=2) > 12).sum(axis=1) > 10)
    bg = bg.crop((0, int(rows[0]), bg.width, int(rows[-1])+1)).resize((320, 72), Image.Resampling.LANCZOS)
    health = picture('hud/bitmaps/Health_icon_background').getchannel('A')
    health = health.crop(health.getbbox()).resize((23, 51), Image.Resampling.LANCZOS)
    font_path = os.getenv('MP_UI_FONT') or next((f for f in (
        '/System/Library/Fonts/Supplemental/Arial Narrow Bold.ttf',
        '/usr/share/fonts/truetype/dejavu/DejaVuSansCondensed-Bold.ttf',
        '/usr/share/fonts/truetype/liberation2/LiberationSans-Bold.ttf') if Path(f).exists()), None)
    if not font_path:
        raise ValueError('Set MP_UI_FONT to a TrueType font with Latin and Cyrillic glyphs')
    alphabet = ''.join(chr(c) for c in range(32, 127))+''.join(chr(c) for c in range(1040, 1104))+'Ёё'
    with (output/'lower.ui').open('wb') as f:
        f.write(b'UI01')
        f.write(lcd(bg).tobytes())
        f.write(health.tobytes())
        for size in (15, 20, 24):
            font = ImageFont.truetype(font_path, size)
            for c in alphabet:
                mask = Image.new('L', (26, 32))
                ImageDraw.Draw(mask).text((0, 0), c, font=font, fill=255)
                width = max(1, round(font.getlength(c)))
                if width>mask.width or (mask.getbbox() and mask.getbbox()[3]>=mask.height):
                    raise ValueError(f'Glyph exceeds atlas cell: {c}')
                f.write(struct.pack('B',width))
                f.write(np.packbits(np.asarray(mask)>92, axis=1).tobytes())
    # Original Xbox backgrounds, toggles and slider artwork for the touch menu.
    with (output/'menu.ui').open('wb') as f:
        f.write(struct.pack('<4sIII',b'MNU1',4,320,240))
        for name in ('mainmenu','generic','loadsave','quit'):
            # Use the original submenu skyline consistently, without Max's
            # portrait or a baked title on any lower-screen menu page.
            image=picture('menu/bitmaps/menu_bg_generic')
            image=image.crop((0,60,image.width,452))
            image=ImageOps.fit(image,(320,240),Image.Resampling.LANCZOS,centering=(1,.5))
            f.write(lcd(image)[::-1].T.tobytes())
        for name in ('Off','On'):
            image=picture('menu/bitmaps/Menu_Toggle_'+name).resize((18,18),Image.Resampling.LANCZOS)
            f.write(lcd(image)[::-1].T.tobytes())
        image=picture('menu/bitmaps/slider_background').resize((112,14),Image.Resampling.LANCZOS)
        f.write(lcd(image)[::-1].T.tobytes())
        image=picture('menu/bitmaps/slider_marker').resize((12,18),Image.Resampling.LANCZOS)
        f.write(lcd(image)[::-1].T.tobytes())
    # The pause word is rendered with the world frame, before display transfer.
    from native_texture import texture
    pause=Image.new('RGBA',(128,32))
    font=ImageFont.truetype(font_path,24)
    draw=ImageDraw.Draw(pause);bounds=draw.textbbox((0,0),'ПАУЗА',font=font)
    draw.text(((128-bounds[2]+bounds[0])//2-bounds[0],(32-bounds[3]+bounds[1])//2-bounds[1]),
              'ПАУЗА',font=font,fill=(255,255,255,255))
    (output/'pause.tex').write_bytes(texture(pause))
    # UV rectangles come from the Xbox HUD definition, not localized name cards.
    hud=member(archives/'x_english.ras','data/hud/hud.txt').decode('latin1')
    constants={k:float(v) for k,v in re.findall(r'#define\s+([UV]_\wxOFFSET)\s+([0-9.]+)',hud)}
    atlas=picture('hud/bitmaps/WeaponHud').convert('L')
    names=('BERETTA','BERETTADUAL','DESERTEAGLE','PUMPSHOTGUN','SAWEDSHOTGUN','JACKHAMMER',
           'INGRAM','INGRAMDUAL','MP5','SNIPER','BASEBALLBAT','LEADPIPE','GRENADE','MOLOTOV','M79')
    sprites={}
    for block in hud.split('[SlotWeapon]')[1:]:
        name=re.search(r'ID\s*=\s*WEAPONID_(\w+)',block)
        lo=re.search(r'UVUpperLeft\s*=\s*\(\s*(U_\wxOFFSET)\s*,\s*(V_\wxOFFSET)',block)
        hi=re.search(r'UVLowerRight\s*=\s*\(\s*(U_\wxOFFSET)\s*,\s*(V_\wxOFFSET)',block)
        if name and lo and hi:
            box=(round(constants[lo[1]]*atlas.width),round(constants[lo[2]]*atlas.height),
                 round(constants[hi[1]]*atlas.width),round(constants[hi[2]]*atlas.height))
            sprites[name[1]]=atlas.crop(box).resize((64,24),Image.Resampling.LANCZOS).tobytes()
    with (output/'weapons.ui').open('wb') as f:
        f.write(struct.pack('<4sIII',b'WPN1',len(names),64,24))
        for name in names:f.write(sprites[name])
    art=picture('menu/bitmaps/menu_bg_credits')
    area=art.convert('RGB').getbbox()
    art=art.crop(area)
    # Each half of the original credits card is readable at physical LCD size.
    for i in range(2):
        page=art.crop((i*art.width//2,0,(i+1)*art.width//2,art.height))
        page=ImageOps.contain(page,(400,240),Image.Resampling.LANCZOS)
        target=Image.new('RGB',(400,240))
        target.paste(page,((400-page.width)//2,(240-page.height)//2))
        (output/f'credits{i}.565').write_bytes(lcd(target)[::-1].T.tobytes())


if __name__ == '__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('disc',type=Path)
    parser.add_argument('output',type=Path)
    args=parser.parse_args()
    lower_ui(args.disc/'english',args.output)
