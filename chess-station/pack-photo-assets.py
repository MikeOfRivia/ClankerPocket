#!/usr/bin/env python3
"""Wrap embedded JPEG files as LVGL v9 image descriptors for the game chest."""
from pathlib import Path
import sys

assetdir=Path(sys.argv[1]); target=Path(sys.argv[2])
names=("splash","tray1","tray2","bangbot")
out=["#include <lvgl.h>","#include <stdint.h>",""]
for name in names:
 data=(assetdir/f"{name}.jpg").read_bytes()
 out.append(f"static const uint8_t {name}_jpeg[{len(data)}] = {{")
 for offset in range(0,len(data),24):
  out.append(" "+",".join(f"0x{c:02x}" for c in data[offset:offset+24])+",")
 out.append("};")
out.append("extern const lv_image_dsc_t treasure_images[4] = {")
for name in names:
 out.append(f" {{ .header = {{ .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RAW, .w = 800, .h = 480 }}, .data_size = sizeof({name}_jpeg), .data = {name}_jpeg }},")
out.append("};")
target.write_text("\n".join(out)+"\n")
print("Embedded photographs:",", ".join(names))
