# SPDX-License-Identifier: GPL-2.0-or-later
"""Decode local original imp frames for visual reference; never distribute game data."""
from pathlib import Path
import json
import re
import struct
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[3]
DATA = ROOT / "runtime/keeperfx-x64/data"
OUT = ROOT / "artifacts/art-prototype/original-imp"
OUT.mkdir(parents=True, exist_ok=True)
source = (ROOT / "keeperfx/src/creature_graphics.c").read_text()
array = re.search(r"creature_list\[CREATURE_FRAMELIST_LENGTH\]\s*=\s*\{(.*?)\};", source, re.S).group(1)
indices = [int(n) for n in re.findall(r"\d+", re.sub(r"/\*.*?\*/|//[^\n]*", "", array, flags=re.S))]
tab = (DATA / "creature.tab").read_bytes()
blob = (DATA / "creature.jty").read_bytes()
pal = (DATA / "palette.dat").read_bytes()
assert len(pal) == 768 and max(pal) <= 63
palette = [tuple(c * 4 for c in pal[i:i+3]) + (255,) for i in range(0, 768, 3)]

def decode(index):
    record = struct.unpack_from("<I8Bhh", tab, index * 16)
    start, w, h, fw, fh, rotate, count, ox, oy, ax, ay = record
    end = struct.unpack_from("<I", tab, (index + 1) * 16)[0]
    encoded = memoryview(blob)[start:end]
    pos = 0
    pixels = [(0, 0, 0, 0)] * (w * h)
    for y in range(h):
        x = 0
        while True:
            assert pos < len(encoded), (index, y, pos)
            command = encoded[pos]
            pos += 1
            if command == 0:
                break
            count_run = command if command < 128 else 256 - command
            visible = min(count_run, w - x)
            if command < 128:
                assert pos + count_run <= len(encoded)
                for i in range(visible):
                    pixels[y*w+x+i] = palette[encoded[pos+i]]
                pos += count_run
            x += visible
    image = Image.new("RGBA", (w, h))
    image.putdata(pixels)
    frame = Image.new("RGBA", (fw, fh))
    frame.alpha_composite(image, (ox, oy))
    return frame, {"table_index": index, "size": [w,h], "frame_size": [fw,fh], "crop_offset": [ox,oy], "anchor": [ax,ay], "frames": count, "bytes_consumed": pos}

sheet = Image.new("RGB", (1500, 840), (37, 40, 45))
draw = ImageDraw.Draw(sheet)
manifest = {"source": "Local creature.tab/creature.jty with palette.dat; original pixels unchanged", "frames": []}
for row, (anim, label) in enumerate([(556, "Possession / level camera"), (557, "Overhead camera")]):
    first = indices[anim]
    for direction in range(5):
        frame, metadata = decode(first + direction)
        name = f"imp-idle-{anim}-direction-{direction}"
        frame.save(OUT / f"{name}.png")
        if anim == 556 and direction == 1:
            frame.resize((frame.width*16, frame.height*16), Image.Resampling.NEAREST).save(OUT / "imp-original-primary.png")
        enlarged = frame.resize((frame.width*7, frame.height*7), Image.Resampling.NEAREST)
        sheet.paste(enlarged, (direction*300+(300-enlarged.width)//2, row*420+65), enlarged)
        draw.text((direction*300+12, row*420+15), f"{label}\n{anim}, angle {direction}", fill=(235,235,235))
        metadata.update(animation=anim, direction=direction, path=f"{name}.png")
        manifest["frames"].append(metadata)
sheet.save(OUT / "imp-original-angles.png")
(OUT / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
print(json.dumps({"reference": str(OUT / "imp-original-angles.png"), "frames": len(manifest["frames"])}))
