# SPDX-License-Identifier: GPL-2.0-or-later
# KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
"""Read the two declared marker pixels from a completed presentation PNG."""
import json
import sys

from PIL import Image

if len(sys.argv) != 6:
    raise SystemExit("Usage: indexed-fallback-pixels.py image x1 y1 x2 y2")
positions = [(int(sys.argv[2]), int(sys.argv[3])), (int(sys.argv[4]), int(sys.argv[5]))]
with Image.open(sys.argv[1]) as source:
    if source.format != "PNG":
        raise ValueError("Expected a PNG presentation")
    image = source.convert("RGBA")
    for x, y in positions:
        if not 0 <= x < image.width or not 0 <= y < image.height:
            raise ValueError("Marker lies outside the presentation")
    print(json.dumps({"dimensions": list(image.size), "rgba": [list(image.getpixel(point)) for point in positions]}))
