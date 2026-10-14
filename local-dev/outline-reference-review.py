# SPDX-License-Identifier: GPL-2.0-or-later
# KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
"""Check exact outline coverage in the frozen nine-owner OpenGL grid.

Usage: outline-reference-review.py reference-level-dir silhouette-run edge-run output.json
Requires Pillow, numpy and Node.js. This checks coverage, not terrain/owner RGB
composition or general world parity. Native crops contain private game artwork.
"""
import argparse
import hashlib
import json
import struct
import subprocess
from pathlib import Path

import numpy as np
from PIL import Image


def require(condition, message):
    if not condition:
        raise ValueError(message)


def binding(path, expected=None):
    path = path.resolve()
    data = path.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    if expected is not None:
        require(digest == expected.lower(), f"Changed source hash: {path}")
    return {"path": str(path), "bytes": len(data), "sha256": digest}


def read_json(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def local_file(directory, name):
    path = (directory / name).resolve()
    require(path.is_relative_to(directory.resolve()), f"File outside evidence directory: {name}")
    return path


def capture_info(path, frame):
    # Reuse the bounded native-wire parser and versioned frozen identity rules.
    helper = Path(__file__).with_name("reference-fingerprint.mjs").resolve()
    program = """
      import {readFile} from 'node:fs/promises';
      import {pathToFileURL} from 'node:url';
      const helper=await import(pathToFileURL(process.argv[2]));
      const bytes=await readFile(process.argv[1]);
      const parsed=helper.parseSceneCapture(bytes);
      console.log(JSON.stringify({...parsed,...helper.sceneFingerprint(bytes)}));
    """
    completed = subprocess.run(["node", "--input-type=module", "--eval", program, str(path), str(helper)],
                               check=True, capture_output=True, text=True)
    info = json.loads(completed.stdout)
    require(info["version"] == 7, "Reference outline capture must be v7")
    for key in ("frozenScheme", "frozenSha256"):
        require(info[key] == frame[key], f"Recorded {key} does not match captured input")
    data = path.read_bytes()
    offset = info["offsets"]["outline"]
    require(data[offset] == 1 and data[offset + 1] == 1, "Missing complete owned GL outline extension")
    info["mode"] = data[offset + 2]
    start = info["offsets"]["outlineSprites"]
    states = []
    for index in range(struct.unpack_from("<I", data, start)[0]):
        p = start + 4 + index * 82
        draw, x, y, width, height = struct.unpack_from("<Iiiii", data, p)
        owner = struct.unpack_from("<b", data, p + 60)[0]
        if info["mode"] and data[p + 61] and not data[p + 62]:
            states.append({"draw": draw, "owner": owner, "box": [x, y, width, height]})
    info["outlines"] = states
    return info


def rgba_image(path, size):
    with Image.open(path) as image:
        require(image.size == tuple(size), f"Image dimensions disagree: {path}")
        return np.array(image.convert("RGBA"))


def gpu_run(directory, frame, capture_path, viewport):
    paths = {name: directory / (name + ".json") for name in ("manifest", "report", "outline-replay")}
    documents = {name: read_json(path) for name, path in paths.items()}
    manifest, main, replay = (documents[name] for name in ("manifest", "report", "outline-replay"))
    require(main["status"] == "passed" and main["debugLayer"] and main["gpuValidation"]
            and main["validationErrors"] == main["validationWarnings"] == 0, "GPU run did not pass validation")
    require(replay["passed"] and not replay["error"], "Outline replay did not pass")
    require(replay["rasterComparisonAccepted"] is False and replay["gate2Accepted"] is False,
            "Review must preserve pending raster/Gate 2 acceptance")
    require(replay["mode"] == frame["metadata"]["settings"]["outline"] and replay["viewport"] == viewport[2:],
            "GPU replay mode/viewport does not match selected frame")
    arguments = manifest["arguments"]
    require("--outline-replay" in arguments and "--gpu-validation" in arguments, "Wrong GPU run arguments")
    capture_arg = arguments[arguments.index("--scene-capture") + 1].strip('"')
    require(Path(capture_arg).resolve() == capture_path.resolve() == Path(replay["capture"]).resolve(),
            "GPU run is bound to another capture")
    require(manifest["sceneCaptureSha256"].lower() == frame["sha256"].lower(), "GPU captured input hash mismatch")
    executable_hash = manifest["executableSha256"].lower()
    require(len(executable_hash) == 64 and all(c in "0123456789abcdef" for c in executable_hash), "Missing recorded executable hash")
    bindings = {name: binding(path) for name, path in paths.items()}
    width, height = viewport[2:]
    pixels = {}
    for backend in ("dx12", "opengl"):
        raw = directory / f"outline-depth-coverage-{backend}.rgba"
        png = directory / f"outline-depth-coverage-{backend}.png"
        require(raw.stat().st_size == width * height * 4, f"Wrong raw RGBA length: {raw}")
        data = np.frombuffer(raw.read_bytes(), dtype=np.uint8).reshape(height, width, 4)
        require(np.array_equal(data, rgba_image(png, [width, height])), f"Raw pixels and PNG disagree: {png}")
        require(np.all(data[:, :, 3] == 255) and np.all((data[:, :, 0] == 0) | (data[:, :, 0] == 255))
                and np.array_equal(data[:, :, 0], data[:, :, 1]) and np.array_equal(data[:, :, 0], data[:, :, 2]),
                "Depth coverage artifact must be opaque black/white")
        pixels[backend] = data
        bindings[backend] = {"raw": binding(raw), "png": binding(png)}
    require(np.array_equal(pixels["dx12"], pixels["opengl"]), "GPU/production-GL depth coverage artifacts disagree")
    depth_case = next(c for c in replay["cases"] if c["case"] == "outline-depth-coverage")
    require(depth_case["compared_pixels"] == width * height and depth_case["different_pixels"] == 0
            and depth_case["different_channels"] == 0 and depth_case["changed_coverage_disagreement"] == 0,
            "Reported coverage comparison disagrees with raw output")
    return pixels["dx12"], {"directory": str(directory.resolve()), "bindings": bindings,
                            "executable": {"path": manifest.get("executablePath"), "recordedSha256": executable_hash,
                                           "verification": "historical_hash_only"},
                            "adapter": main["adapter"], "mode": replay["mode"],
                            "rasterComparisonAccepted": replay["rasterComparisonAccepted"],
                            "gate2Accepted": replay["gate2Accepted"]}


def save_image(array, path):
    Image.fromarray(array).save(path)
    return binding(path)


def review(args):
    reference = args.reference.resolve()
    report_path = reference / "report.json"
    report = read_json(report_path)
    require(report["passed"] and report["renderer"] == "OPENGL", "Reference suite failed or is not OpenGL")
    frames = []
    for mode in range(3):
        matching = [f for f in report["frames"] if f["metadata"]["settings"]["outline"] == mode
                    and f["id"].startswith("outline-")]
        require(len(matching) == 1, f"Need exactly one reported outline frame for mode {mode}")
        require(not matching[0]["expectationFailures"], "Reference expectation failures")
        frames.append(matching[0])
    viewport = frames[0]["metadata"]["viewport"]
    require(viewport == [753, 0, 3087, 2160], "This review is bounded to the declared frozen grid viewport")
    require(frames[0]["imageSize"] == [3840, 2160], "Wrong native screenshot dimensions")
    base_metadata = dict(frames[0]["metadata"])
    base_metadata.pop("sequence")
    base_metadata.pop("settings")
    infos, bindings, images = [], [], []
    for frame in frames:
        metadata = dict(frame["metadata"])
        metadata.pop("sequence")
        settings = metadata.pop("settings")
        require(metadata == base_metadata, "Reference frame camera/world/objects changed")
        require({k: v for k, v in settings.items() if k != "outline"}
                == {k: v for k, v in frames[0]["metadata"]["settings"].items() if k != "outline"}, "Other reference settings changed")
        require(frame["imageSize"] == frames[0]["imageSize"], "Reference screenshot dimensions changed")
        capture = local_file(reference, frame["file"])
        image = local_file(reference, frame["image"])
        bindings.append({"id": frame["id"], "capture": binding(capture, frame["sha256"]),
                         "image": binding(image, frame["imageSha256"]), "visualReview": frame["visualReview"]})
        info = capture_info(capture, frame)
        require(info["mode"] == settings["outline"], "Captured mode differs from reference metadata")
        infos.append(info)
        images.append(rgba_image(image, frame["imageSize"]))
    require(all(info["frozenSha256"] == infos[0]["frozenSha256"] for info in infos), "Frozen scene/assets changed")
    creatures = [o for o in base_metadata["objectLocations"] if o["class"] == 5 and o["drawClass"] == 2]
    require(len(creatures) == 9 and sorted(o["owner"] for o in creatures) == list(range(9))
            and all(o["projected"] for o in creatures), "Reference is not the frozen nine-owner creature grid")
    require(infos[1]["outlines"] == infos[2]["outlines"] and len(infos[1]["outlines"]) == 9
            and sorted(s["owner"] for s in infos[1]["outlines"]) == list(range(9)), "Captured owner outline grid changed")
    x, y, width, height = viewport
    cropped = [image[y:y + height, x:x + width] for image in images]
    gpu, gpu_bindings = [], []
    for run, frame in zip((args.silhouette, args.edge), frames[1:]):
        pixels, evidence = gpu_run(run.resolve(), frame, local_file(reference, frame["file"]), viewport)
        gpu.append(pixels)
        gpu_bindings.append(evidence)
    target = args.output.resolve()
    crops = target.parent / (target.stem + "-crops")
    crops.mkdir(parents=True, exist_ok=True)
    comparisons = []
    masks = []
    for frame_index, (label, actual, coverage) in enumerate(zip(("silhouette", "edge"), cropped[1:], gpu), start=1):
        delta = np.any(actual != cropped[0], axis=2)
        white = coverage[:, :, 0] == 255
        masks.append(delta)
        full_delta = np.any(images[frame_index] != images[0], axis=2)
        comparisons.append({"mode": label, "nativePixelsCompared": width * height,
                            "actualChangedPixels": int(delta.sum()), "gpuCoveredPixels": int(white.sum()),
                            "actualChangesOutsideGpuCoverage": int((delta & ~white).sum()),
                            "gpuCoverageWithoutActualChange": int((white & ~delta).sum()),
                            "equalCoverageMasks": bool(np.array_equal(delta, white)),
                            "excludedScreenChangedPixels": int(full_delta.sum() - delta.sum()),
                            "deltaMask": save_image(np.uint8(delta) * 255, crops / f"{label}-native-delta-mask.png")})
    assigned = np.zeros((height, width), dtype=np.uint8)
    owners = []
    for state in sorted(infos[1]["outlines"], key=lambda s: s["owner"]):
        sx, sy, sw, sh = state["box"]
        require(0 <= sx < sx + sw <= width and 0 <= sy < sy + sh <= height, "Grid source rectangle outside viewport")
        assigned[sy:sy + sh, sx:sx + sw] += 1
        box = [max(0, sx - 12), max(0, sy - 12), min(width, sx + sw + 12), min(height, sy + sh + 12)]
        left, top, right, bottom = box
        record = {"owner": state["owner"], "draw": state["draw"], "sourceRectangle": state["box"],
                  "cropViewportBox": [left, top, right - left, bottom - top],
                  "cropScreenBox": [left + x, top + y, right - left, bottom - top], "crops": {}}
        for name, array in zip(("actual-none", "actual-silhouette", "actual-edge", "dx12-silhouette-coverage", "dx12-edge-coverage"), [*cropped, *gpu]):
            record["crops"][name] = save_image(array[top:bottom, left:right], crops / f"owner-{state['owner']}-{name}.png")
        record["actualChangedPixels"] = {name: int(mask[sy:sy + sh, sx:sx + sw].sum())
                                         for name, mask in zip(("silhouette", "edge"), masks)}
        owners.append(record)
    require(not np.any(assigned > 1), "Grid outlined source rectangles overlap; owner attribution is ambiguous")
    require(all(not np.any((pixels[:, :, 0] == 255) & (assigned == 0)) for pixels in gpu), "GPU coverage outside nine owner rectangles")
    return {"schema": 1, "passed": all(c["equalCoverageMasks"] for c in comparisons),
            "scope": "Exact changed-pixel coverage for this frozen nine-owner grid with no foreground sprites over the outlines; native viewport only",
            "comparison": "Any unequal RGBA byte against the actual none screenshot; no epsilon; opaque white DX12 depth coverage",
            "limitations": ["Does not compare terrain colours or full-world RGB composition", "Does not prove coverage with foreground sprites over an outline",
                            "No general renderer, gameplay, performance or Gate 2 acceptance", "Source-border allocation padding remains an explicit GL diagnostic limitation"],
            "visualReview": "pending", "rasterComparisonAccepted": False, "gate2Accepted": False,
            "viewport": viewport, "nativeScreenshotSize": frames[0]["imageSize"],
            "frozenScheme": infos[0]["frozenScheme"], "frozenSha256": infos[0]["frozenSha256"],
            "referenceReport": binding(report_path), "frames": bindings, "gpuRuns": gpu_bindings,
            "comparisons": comparisons, "owners": owners}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for argument in ("reference", "silhouette", "edge", "output"):
        parser.add_argument(argument, type=Path)
    args = parser.parse_args()
    try:
        result = review(args)
    except (ValueError, KeyError, OSError, subprocess.CalledProcessError) as error:
        result = {"schema": 1, "passed": False, "scope": "Frozen nine-owner outline grid coverage review",
                  "error": str(error), "visualReview": "pending", "rasterComparisonAccepted": False, "gate2Accepted": False}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"output": str(args.output.resolve()), "passed": result["passed"],
                      "comparisons": [{k: c[k] for k in ("mode", "actualChangedPixels", "gpuCoveredPixels", "actualChangesOutsideGpuCoverage", "gpuCoverageWithoutActualChange")}
                                      for c in result.get("comparisons", [])], "error": result.get("error", "")}))
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
