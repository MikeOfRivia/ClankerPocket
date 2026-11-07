#!/usr/bin/env python3
"""Generate lightweight LVGL 9 ARGB8888 image assets for Chess Station."""
from PIL import Image, ImageDraw, ImageFont
import sys

font_file, target = sys.argv[1:3]
size = 44
font = ImageFont.truetype(font_file, 41)
symbols = "♔♕♖♗♘♙♚♛♜♝♞♟"
chunks = ['#include <lvgl.h>', '']
names = []
for i, symbol in enumerate(symbols):
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    draw = ImageDraw.Draw(img)
    # White pieces have dark outlines; black pieces have light outlines.
    is_black = i >= 6
    fill = (22, 24, 31, 255) if is_black else (253, 251, 242, 255)
    stroke = (244, 240, 226, 255) if is_black else (29, 33, 42, 255)
    bbox = draw.textbbox((0, 0), symbol, font=font, stroke_width=1)
    tw, th = bbox[2]-bbox[0], bbox[3]-bbox[1]
    x = (size-tw)/2-bbox[0]
    y = (size-th)/2-bbox[1]
    draw.text((x, y), symbol, font=font, fill=fill, stroke_width=1, stroke_fill=stroke)
    pixels = bytearray()
    for r,g,b,a in img.getdata():
        pixels.extend((b,g,r,a)) # LVGL 9 ARGB8888, little-endian byte order.
    name = f'chess_bitmap_{i}'
    names.append(name)
    chunks.append(f'static const uint8_t {name}[{len(pixels)}] = {{')
    for j in range(0, len(pixels), 32):
        chunks.append('  ' + ', '.join(f'0x{v:02x}' for v in pixels[j:j+32]) + ',')
    chunks.append('};')
    chunks.append('')
chunks.append('const lv_image_dsc_t chess_piece_images[12] = {')
for name in names:
    chunks.append(
        '  { .header = { .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_ARGB8888, '
        f'.w = {size}, .h = {size}, .stride = {size * 4} }}, '
        f'.data_size = sizeof({name}), .data = {name} }},'
    )
chunks.append('};')
with open(target, 'w', encoding='utf-8') as fh:
    fh.write('\n'.join(chunks) + '\n')
print(f'Generated {len(names)} chess sprites ({size}x{size}).')
