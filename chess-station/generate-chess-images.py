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
# TRON-inspired green Knight Family emblem. Pixel-sharp accents
# pre-rendered to RGB565; runtime contains no image transforms or filters.
W,H=380,440
dark=(5,14,11)
neon=(86,255,115)
lime=(168,255,185)
shade=(31,105,58)
muted=(12,38,24)
logo=Image.new("RGB",(W,H),dark)
d=ImageDraw.Draw(logo)

# Tech-frame with beveled corners, bright keylines, faint circuit traces.
frame=[(25,16),(355,16),(370,31),(370,409),(355,424),
       (25,424),(10,409),(10,31),(25,16)]
d.line(frame,fill=shade,width=4,joint="curve")
d.line([(29,22),(351,22),(364,35),(364,405),(351,418),(29,418),
        (16,405),(16,35),(29,22)],fill=neon,width=2,joint="curve")
for y in (58,85,275,288):
    d.line((25,y,65,y),fill=shade,width=1)
    d.line((315,y,354,y),fill=shade,width=1)
for x in (33,347):
    d.line((x,62,x,102),fill=shade,width=1)
    d.line((x,257,x,284),fill=shade,width=1)
for x in range(35,350,32):
    d.line((x,39,x+13,39),fill=(28,80,44),width=2)
d.line((42,303,338,303),fill=neon,width=2)

# Distinctive quartet: two large adult knights and two smaller child knights.
# Two-tone duotone silhouettes read cleanly on the little 7-inch panel.
def draw_knight(cx,cy,px,fill,stroke):
    f=ImageFont.truetype(font_file,px)
    d.text((cx,cy),"♞",font=f,anchor="mm",fill=fill,
           stroke_width=2 if px>105 else 1,stroke_fill=stroke)
    pedestal=max(17,px//4)
    foot=cy+px//3
    d.line((cx-pedestal,foot,cx+pedestal,foot),fill=neon,width=3)
    d.line((cx-pedestal+8,foot+5,cx+pedestal-8,foot+5),fill=shade,width=2)
draw_knight(134,160,141,lime,shade)
draw_knight(244,160,141,(69,219,103),(17,91,47))
draw_knight(72,236,89,lime,shade)
draw_knight(311,236,89,(95,224,130),shade)

def fit_font(text,max_width,start_size,min_size=12):
    for n in range(start_size,min_size-1,-1):
        f=ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",n)
        if d.textbbox((0,0),text,font=f)[2]<=max_width:
            return f
    return f

d.text((W//2,331),"THE",font=fit_font("THE",W-50,18),anchor="mm",fill=shade)
d.text((W//2,361),"KNIGHT FAMILY",
       font=fit_font("KNIGHT FAMILY",W-44,28),anchor="mm",fill=lime)
d.text((W//2,396),"CHESS EXPERIENCE",
       font=fit_font("CHESS EXPERIENCE",W-45,20),anchor="mm",fill=neon)
d.line((30,396,55,396),fill=neon,width=2)
d.line((325,396,350,396),fill=neon,width=2)
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
    ld.text((85,27),caption,font=button_font,anchor="mm",fill=(135,255,153,255))
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
