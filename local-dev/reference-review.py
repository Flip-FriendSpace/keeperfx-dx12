# SPDX-License-Identifier: GPL-2.0-or-later
# KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
"""Create review contact sheets and native-resolution crops from retained PNGs.

Requires Pillow. Outputs are local evidence containing installed game assets;
keep them outside the source repository. This tool never accepts a reference.
"""
import argparse
import hashlib
import json
from pathlib import Path
from PIL import Image, ImageDraw

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("directory", type=Path)
parser.add_argument("--crop", nargs=5, metavar=("CASE", "X", "Y", "WIDTH", "HEIGHT"))
parser.add_argument("--world-crops", action="store_true", help="Also retain unscaled 1200x1000 crops centred on each world viewport")
parser.add_argument("--door-crops", action="store_true", help="Retain native 640x640 crops around each projected door and a review panel")
args = parser.parse_args()

for report_file in args.directory.rglob("report.json"):
    report = json.loads(report_file.read_text(encoding="utf-8-sig"))
    if "renderer" not in report or not report.get("frames"):
        continue
    frames = report["frames"]
    overview = Image.new("RGB", (1280, ((len(frames) + 1) // 2) * 400), "#191919")
    labels = ImageDraw.Draw(overview)
    for index, frame in enumerate(frames):
        path = report_file.parent / frame["image"]
        assert hashlib.sha256(path.read_bytes()).hexdigest() == frame["imageSha256"]
        with Image.open(path) as source:
            source.load()
            # Preserve the indexed source through RGBA before contact-sheet RGB.
            native = source.convert("RGBA")
        preview = native.convert("RGB")
        preview.thumbnail((640, 360), Image.Resampling.LANCZOS)
        x, y = (index % 2) * 640, (index // 2) * 400
        overview.paste(preview, (x, y))
        labels.text((x + 10, y + 367), f'{frame["id"]} | {report["renderer"]}', fill="white")
        if args.door_crops:
            doors = [d for d in frame["metadata"].get("doorStates", []) if d.get("projected")]
            assert not doors or (native.width >= 640 and native.height >= 640), "Door crops require a 640x640 or larger source"
            panel = Image.new("RGB", (1920, ((len(doors) + 2) // 3) * 680), "#191919") if doors else None
            for door_index, door in enumerate(doors):
                sx, sy = door["screen"]
                left, top = max(0, min(native.width-640, sx-320)), max(0, min(native.height-640, sy-250))
                box = (left, top, left+640, top+640)
                crop = native.crop(box)
                name = f'{frame["id"]}-door-{door["slab"][0]}-{door["slab"][1]}.png'
                crop_path = report_file.parent / name
                crop.save(crop_path)
                crop_path.with_suffix(".json").write_text(json.dumps({"source": frame["image"], "sourceSha256": frame["imageSha256"],
                    "box": [left, top, 640, 640], "resize": False, "door": door,
                    "sha256": hashlib.sha256(crop_path.read_bytes()).hexdigest()}, indent=2)+"\n", encoding="utf-8")
                px, py = (door_index % 3)*640, (door_index // 3)*680
                panel.paste(crop.convert("RGB"), (px, py))
                ImageDraw.Draw(panel).text((px+10, py+647), f'model {door["model"]} | orientation {door["orientation"]} | state {door["state"]} | open {door["openAmount"]}', fill="white")
            if panel:
                panel_path = report_file.parent / f'{frame["id"]}-doors.png'
                panel.save(panel_path)
                print(panel_path)
        crop_box = None
        if args.world_crops:
            vx, vy, vw, vh = frame["metadata"]["viewport"]
            width, height = min(1200, vw), min(1000, vh)
            crop_box = (vx + (vw - width) // 2, vy + (vh - height) // 2, width, height)
        if args.crop and args.crop[0] == frame["id"]:
            crop_box = tuple(map(int, args.crop[1:]))
        if crop_box:
            left, top, width, height = crop_box
            assert 0 <= left < left + width <= native.width
            assert 0 <= top < top + height <= native.height
            name = f'{frame["id"]}-crop-{left}-{top}-{width}-{height}.png'
            crop_path = report_file.parent / name
            native.crop((left, top, left + width, top + height)).save(crop_path)
            evidence = {"source": frame["image"], "sourceSha256": frame["imageSha256"],
                        "box": [left, top, width, height], "resize": False,
                        "sha256": hashlib.sha256(crop_path.read_bytes()).hexdigest()}
            crop_path.with_suffix(".json").write_text(json.dumps(evidence, indent=2) + "\n", encoding="utf-8")
            print(crop_path)
    output = report_file.parent / "overview.png"
    overview.save(output)
    print(output)
