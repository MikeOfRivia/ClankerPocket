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
# Dedicated 380x440 RGB565 home emblem: 4 knights, 2 adults + 2 children.
# Artwork is drawn at build time, not loaded or decoded at runtime.
W,H=380,440
dark=(17,28,42)
gold=(224,179,99)
ivory=(248,237,210)
bronze=(138,100,57)
logo=Image.new("RGB",(W,H),dark)
d=ImageDraw.Draw(logo)

# A restrained gilded frame, checker accents and family crest.
d.rounded_rectangle((12,15,W-12,H-15),radius=22,outline=(116,88,52),width=3)
d.rounded_rectangle((19,22,W-19,H-22),radius=18,outline=(67,60,49),width=1)
for xx in range(36,W-36,20):
    d.rectangle((xx,37,xx+9,46),fill=(55,55,51) if (xx//20)%2 else (74,61,46))
d.line((45,302,W-45,302),fill=gold,width=2)

# Four original family knight emblems: large parents, small children.
def draw_knight(cx,cy,px,fill,stroke):
    f=ImageFont.truetype(font_file,px)
    d.text((cx,cy),"♞",font=f,anchor="mm",fill=fill,
           stroke_width=2 if px>105 else 1,stroke_fill=stroke)
    # Gold plinth under each piece, proportionate to its size
    radius=max(18,px//4)
    by=cy+px//3
    d.line((cx-radius,by,cx+radius,by),fill=gold,width=3)

draw_knight(138,166,143,ivory,bronze)
draw_knight(240,166,143,(233,200,140),(110,75,40))
draw_knight(64,228,89,(245,229,196),bronze)
draw_knight(317,228,89,(245,229,196),bronze)

def fit_font(text,max_width,start_size,min_size=12):
    for n in range(start_size,min_size-1,-1):
        f=ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSerif.ttf",n)
        if d.textbbox((0,0),text,font=f)[2]<=max_width:
            return f
    return f

d.text((W//2,328),"THE",font=fit_font("THE",W-55,19),anchor="mm",fill=gold)
d.text((W//2,359),"KNIGHT FAMILY",
       font=fit_font("KNIGHT FAMILY",W-45,30),anchor="mm",fill=ivory,
       stroke_width=1,stroke_fill=bronze)
d.text((W//2,398),"CHESS EXPERIENCE",
       font=fit_font("CHESS EXPERIENCE",W-62,20),anchor="mm",fill=gold)
d.line((33,397,60,397),fill=gold,width=2)
d.line((W-60,397,W-33,397),fill=gold,width=2)
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
