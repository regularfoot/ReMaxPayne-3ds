"""Bake grayscale UI glyphs at their final LCD sizes; no game data is used."""
import struct
from PIL import Image, ImageDraw, ImageFont

FIRST_SIZE, LAST_SIZE = 8, 27
ALPHABET = ''.join(chr(c) for c in range(32, 127))+''.join(chr(c) for c in range(1040, 1104))+'Ёё'


def font_data(path):
    records, pixels = bytearray(), bytearray()
    for size in range(FIRST_SIZE, LAST_SIZE+1):
        font = ImageFont.truetype(str(path), size)
        for char in ALPHABET:
            left, top, right, bottom = font.getbbox(char)
            mask = Image.new('L', (right-left, bottom-top))
            ImageDraw.Draw(mask).text((-left, -top), char, font=font, fill=255)
            records.extend(struct.pack('<IBBBbb3x', len(pixels), max(1, round(font.getlength(char))),
                                       mask.width, mask.height, left, top))
            pixels.extend(mask.tobytes())
    return struct.pack('<4I', FIRST_SIZE, LAST_SIZE, len(ALPHABET), len(pixels))+records+pixels
