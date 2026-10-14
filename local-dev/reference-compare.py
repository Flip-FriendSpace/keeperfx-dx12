# SPDX-License-Identifier: GPL-2.0-or-later
# KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
"""Measure a declared native-pixel region in two frozen reference images.

This reports differences, not visual acceptance. Keep output beside the local
references because both the crops and difference image contain game artwork.
"""
import argparse
import hashlib
import json
import subprocess
from pathlib import Path
from PIL import Image, ImageChops

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("directory", type=Path, help="Level directory with report.json")
parser.add_argument("first")
parser.add_argument("second")
parser.add_argument("--box", nargs=4, type=int, required=True, metavar=("X", "Y", "WIDTH", "HEIGHT"))
args = parser.parse_args()
report = json.loads((args.directory / "report.json").read_text(encoding="utf-8-sig"))
frames = [next(f for f in report["frames"] if f["id"] == name) for name in (args.first, args.second)]
for key in ("turn", "camera", "angles", "scaledZoom", "fov", "slabKinds", "objectLocations", "renderFlags"):
    assert frames[0]["metadata"][key] == frames[1]["metadata"][key], f"Changed frozen input: {key}"
fingerprints = []
fingerprint_helper = Path(__file__).resolve().with_name("reference-fingerprint.mjs")
for frame in frames:
    capture = args.directory / frame["file"]
    data = capture.read_bytes()
    assert hashlib.sha256(data).hexdigest() == frame["sha256"]
    scopes = ("frozen", "effectiveWorldResources", "appliedWorldState", "compositionResources", "compositionState")
    record = {name + suffix: frame[name + suffix] for name in scopes for suffix in ("Scheme", "Sha256")
              if name + suffix in frame}
    result = subprocess.run(["node", str(fingerprint_helper), str(capture), "--record"],
                            input=json.dumps(record), text=True, capture_output=True, check=True)
    fingerprints.append(json.loads(result.stdout))
assert all(fingerprints[0][key] == fingerprints[1][key] for key in ("frozenScheme", "frozenSha256")), \
    "Changed frozen scene/assets; cannot isolate the setting"
state_comparison = {}
for name in ("effectiveWorldResources", "appliedWorldState", "compositionResources", "compositionState"):
    if name + "Sha256" in fingerprints[0] or name + "Sha256" in fingerprints[1]:
        state_comparison[name + "Equal"] = all(fingerprints[0].get(name + suffix) == fingerprints[1].get(name + suffix)
                                               for suffix in ("Scheme", "Sha256"))
x, y, width, height = args.box
images = []
for frame in frames:
    path = args.directory / frame["image"]
    assert hashlib.sha256(path.read_bytes()).hexdigest() == frame["imageSha256"]
    with Image.open(path) as source:
        assert 0 <= x < x + width <= source.width and 0 <= y < y + height <= source.height
        images.append(source.convert("RGBA").convert("RGB").crop((x, y, x + width, y + height)))
diff = ImageChops.difference(*images)
mask = ImageChops.lighter(ImageChops.lighter(diff.getchannel("R"), diff.getchannel("G")), diff.getchannel("B"))
counts = mask.histogram()
prefix = args.directory / f"compare-{args.first}-{args.second}"
result = {"renderer": report["renderer"], "ids": [f["id"] for f in frames],
          "frozenScheme": fingerprints[0]["frozenScheme"], "frozenSha256": fingerprints[0]["frozenSha256"],
          "fingerprints": fingerprints, "stateComparison": state_comparison,
          "comparisonScope": "Equal scene/assets identity; effective resource and applied-state differences require separate review",
          "sourceSha256": [f["imageSha256"] for f in frames], "box": args.box,
          "changedPixels": width * height - counts[0], "totalPixels": width * height,
          "maximumChannelDifference": max(i for i, count in enumerate(counts) if count),
          "localDifferenceBounds": mask.getbbox(), "acceptance": "requires_visual_review"}
for label, image in zip(("first", "second", "difference"), [*images, diff]):
    image.save(Path(str(prefix) + f"-{label}.png"))
Path(str(prefix) + ".json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
print(json.dumps(result))
