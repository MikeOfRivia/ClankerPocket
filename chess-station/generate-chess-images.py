#!/usr/bin/env python3
"""Generate lightweight LVGL 9 ARGB8888 image assets for Chess Station."""
from PIL import Image, ImageDraw, ImageFont
import sys

font_file, target = sys.argv[1:3]
size = 44
font = ImageFont.truetype(font_file, 41)
# Use filled glyph silhouettes for BOTH sides; color distinguishes the teams.
symbols = "♚♛♜♝♞♟♚♛♜♝♞♟"
chunks = ['#include <lvgl.h>', '']
names = []
for i, symbol in enumerate(symbols + symbols):
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    draw = ImageDraw.Draw(img)
    # White pieces have dark outlines; black pieces have light outlines.
    is_black = (i % 12) >= 6
    fill = (22, 24, 31, 255) if is_black else (253, 251, 242, 255)
    stroke = (244, 240, 226, 255) if is_black else (29, 33, 42, 255)
    bbox = draw.textbbox((0, 0), symbol, font=font, stroke_width=1)
    tw, th = bbox[2]-bbox[0], bbox[3]-bbox[1]
    x = (size-tw)/2-bbox[0]
    y = (size-th)/2-bbox[1]
    draw.text((x, y), symbol, font=font, fill=fill, stroke_width=1, stroke_fill=stroke)
    if i >= 12:
        # Device is physically turned clockwise; turn sprites CCW in flash.
        img = img.rotate(270 if i < 18 else 90, expand=False)
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
chunks.append('extern const lv_image_dsc_t chess_piece_images[24] = {')
for name in names:
    chunks.append(
        '  { .header = { .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_ARGB8888, '
        f'.w = {size}, .h = {size}, .stride = {size * 4} }}, '
        f'.data_size = sizeof({name}), .data = {name} }},'
    )
chunks.append('};')
# Draw an original branded chess crest (not a text-only menu heading).
# Opaque RGB565 keeps flash use low and avoids expensive alpha blending.
from PIL import ImageFilter
W,H=440,238
logo=Image.new("RGB",(W,H),(17,28,42))
d=ImageDraw.Draw(logo)
gold=(224,179,99); pale=(247,222,166); muted=(117,91,58)
# Gold double-lined shield, with a subtle checker field.
outline=[(W//2,10),(W//2+65,31),(W//2+65,106),(W//2+47,139),(W//2,167),(W//2-47,139),(W//2-65,106),(W//2-65,31)]
d.line(outline+[outline[0]],fill=gold,width=4,joint="curve")
inner=[(W//2,17),(W//2+58,37),(W//2+58,103),(W//2+42,134),(W//2,158),(W//2-42,134),(W//2-58,103),(W//2-58,37)]
d.line(inner+[inner[0]],fill=muted,width=2,joint="curve")
for yy in range(40,116,19):
    for xx in range(W//2-48,W//2+49,19):
        if ((xx-(W//2-48))//19+(yy-40)//19)%2==0:
            d.rectangle((xx,yy,xx+18,yy+18),fill=(36,49,58))
knight=ImageFont.truetype(font_file,112)
d.text((W//2,81),"♞",font=knight,anchor="mm",fill=pale,stroke_width=2,stroke_fill=(99,67,37))
serif=ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSerif.ttf",38)
small=ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSerif.ttf",21)
d.text((W//2,187),"CHESS",font=serif,anchor="mm",fill=pale,stroke_width=1,stroke_fill=muted)
d.text((W//2,220),"S T A T I O N",font=small,anchor="mm",fill=gold)
d.line((60,195,133,195),fill=gold,width=2)
d.line((307,195,380,195),fill=gold,width=2)
buf=bytearray()
for red,green,blue in logo.getdata():
    c=((red>>3)<<11)|((green>>2)<<5)|(blue>>3)
    buf.extend((c&255,c>>8))
chunks.append(f'static const uint8_t logo_pixels[{len(buf)}] = {{')
for j in range(0,len(buf),32):
    chunks.append('  '+', '.join(f'0x{v:02x}' for v in buf[j:j+32])+',')
chunks.append('};')
chunks.append('extern const lv_image_dsc_t chess_logo = {')
chunks.append('  .header = { .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565,')
chunks.append(f'    .w = {W}, .h = {H}, .stride = {W*2} }},')
chunks.append('  .data_size = sizeof(logo_pixels), .data = logo_pixels,')
chunks.append('};')
# Pre-render 90-degree-clockwise 2P labels rather than rotating LVGL text
# dynamically. Avoids software-renderer transform overhead on this device.
button_font = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 23)
button_names = ["NEW GAME", "MAIN MENU"]
for i,caption in enumerate(button_names):
    label = Image.new("RGBA",(170,54),(0,0,0,0))
    ld=ImageDraw.Draw(label)
    ld.text((85,27),caption,font=button_font,anchor="mm",fill=(250,245,232,255))
    rotated = label.rotate(270,expand=True)  # 90 degrees clockwise
    raw = bytearray()
    for red,green,blue,alpha in rotated.getdata():
        raw.extend((blue,green,red,alpha))
    chunks.append(f'static const uint8_t rotated_button_{i}[{len(raw)}] = {{')
    for k in range(0,len(raw),32):
        chunks.append('  '+', '.join(f'0x{v:02x}' for v in raw[k:k+32])+',')
    chunks.append('};')
chunks.append('extern const lv_image_dsc_t chess_rotated_buttons[2] = {')
for i in range(2):
    chunks.append('  { .header = { .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_ARGB8888, .w = 54, .h = 170, .stride = 216 }, '
                  f'.data_size = sizeof(rotated_button_{i}), .data = rotated_button_{i} }},')
chunks.append('};')
with open(target, 'w', encoding='utf-8') as fh:
    fh.write('\n'.join(chunks) + '\n')
print(f'Generated {len(names)} chess sprites ({size}x{size}).')
