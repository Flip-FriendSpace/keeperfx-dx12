# SPDX-License-Identifier: GPL-2.0-or-later
"""Prepare owner-specific CTA references and package inspected local masters.

Original references and generated masters remain local. This tool never installs
or selects a mod. Native geometry, phase lengths and source masks are authority.
"""
import argparse
import hashlib
import json
from pathlib import Path
import zipfile

import numpy as np
from PIL import Image, ImageDraw

COLOURS = ("red", "blue", "green", "yellow", "white", "purple", "black", "orange")
PHASES = ("birth", "alive", "leave")
VIEWS = ("td", "fp")
SCALE = 4
CELL = (256, 448)


def archive_write(archive, name, data):
    entry = zipfile.ZipInfo(name, (1980, 1, 1, 0, 0, 0))
    entry.compress_type = zipfile.ZIP_DEFLATED
    entry.external_attr = 0o100644 << 16
    archive.writestr(entry, data)


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def write_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("x", encoding="utf-8") as stream:
        json.dump(value, stream, indent=2)
        stream.write("\n")


def prepare(args):
    source_path = args.source.resolve()
    source = json.loads(source_path.read_text(encoding="utf-8-sig"))
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    register = {"schema": 1, "source_manifest": str(source_path),
                "source_manifest_sha256": sha(source_path), "owners": []}
    for colour in COLOURS[1:]:
        sheet = Image.new("RGBA", (CELL[0] * 9, CELL[1] * 6))
        sequences = []
        for phase_index, phase in enumerate(PHASES):
            asset = next(a for a in source["assets"] if a["color"] == colour and a["phase"] == phase)
            for view_index, view in enumerate(VIEWS):
                seq = next(s for s in asset["sequences"] if s["view"] == view)
                frames = []
                row = phase_index * 2 + view_index
                assert len(seq["frames"]) == (7, 8, 9)[phase_index]
                for i, frame in enumerate(seq["frames"]):
                    path = source_path.parent / asset["key"] / frame["file"]
                    assert sha(path) == frame["png_sha256"]
                    image = Image.open(path).convert("RGBA")
                    assert list(image.size) == frame["frame_size"]
                    w, h = image.width * SCALE, image.height * SCALE
                    x = i * CELL[0] + (CELL[0] - w) // 2
                    y = row * CELL[1] + CELL[1] - h - 12
                    assert w < CELL[0] and h + 12 < CELL[1]
                    sheet.alpha_composite(image.resize((w, h), Image.Resampling.NEAREST), (x, y))
                    frames.append({"source": str(path), "source_sha256": sha(path),
                                   "box": [x, y, x + w, y + h], "original": frame})
                sequences.append({"key": asset["key"], "phase": phase, "view": view, "frames": frames})
        reference = output / "references" / f"{colour}-lifecycle.png"
        reference.parent.mkdir(parents=True, exist_ok=True)
        if reference.exists():
            raise FileExistsError(reference)
        sheet.save(reference)
        register["owners"].append({"colour": colour, "reference": str(reference),
                                   "reference_sha256": sha(reference), "dimensions": list(sheet.size),
                                   "columns": 9, "rows": 6, "cell": list(CELL), "sequences": sequences})
    write_json(output / "source-register.json", register)


def palette_indices(pixels, palette):
    rgb6 = (pixels[:, :, :3] // 4).astype(np.int16)
    unique, inverse = np.unique(rgb6.reshape(-1, 3), axis=0, return_inverse=True)
    indices = []
    for i in range(0, len(unique), 2048):
        indices.extend(np.abs(unique[i:i+2048, None, :] - palette[None, :, :]).sum(2).argmin(1))
    return np.array(indices, np.uint8)[inverse].reshape(pixels.shape[:2])


def palette_lab(palette):
    rgb = palette.astype(float) * 4 / 255
    linear = np.where(rgb <= .04045, rgb / 12.92, ((rgb + .055) / 1.055) ** 2.4)
    xyz = linear @ np.array([[.4124564, .3575761, .1804375], [.2126729, .7151522, .0721750], [.0193339, .1191920, .9503041]]).T
    f = xyz / np.array([.95047, 1, 1.08883])
    f = np.where(f > (6/29)**3, np.cbrt(f), f / (3*(6/29)**2) + 4/29)
    return np.stack([116*f[:, 1]-16, 500*(f[:, 0]-f[:, 1]), 200*(f[:, 1]-f[:, 2])], -1)


def source_tone_indices(source, candidate, visible, display_palette):
    """One pooled source-derived tone curve, with no frame or position fitting."""
    lab = palette_lab(display_palette)
    source_visible, candidate_visible = source[visible], candidate[visible]
    sx, sc = np.unique(lab[source_visible, 0], return_counts=True)
    dx, dc = np.unique(lab[candidate_visible, 0], return_counts=True)
    sp, dp = (np.cumsum(sc)-sc/2)/sc.sum(), (np.cumsum(dc)-dc/2)/dc.sum()
    targets = np.interp(dp, sp, sx)
    desired = lab.copy()
    desired[:, 0] = np.interp(lab[:, 0], dx, targets)
    allowed = np.unique(source_visible)
    delta = desired[:, None, :] - lab[allowed][None, :, :]
    delta[:, :, 0] *= 8
    lut = allowed[np.square(delta).sum(-1).argmin(1)]
    mapped = lut[candidate]
    q = [0, .1, .5, .9, .95, .99, 1]
    return mapped, {"method": "One monotone L* curve pooled across the whole sequence, quantized only to original source-used display-palette colours; no per-frame fit or geometry edits",
                    "source_L_quantiles": np.quantile(lab[source_visible, 0], q).tolist(),
                    "before_L_quantiles": np.quantile(lab[candidate_visible, 0], q).tolist(),
                    "after_L_quantiles": np.quantile(lab[mapped[visible], 0], q).tolist(),
                    "allowed_indices": allowed.tolist(), "palette_lut": lut.tolist()}


def package(args):
    register_path = args.register.resolve()
    register = json.loads(register_path.read_text())
    assert sha(register["source_manifest"]) == register["source_manifest_sha256"]
    source = json.loads(Path(register["source_manifest"]).read_text())
    assert [o['colour'] for o in register['owners']] == list(COLOURS[1:])
    for owner in register['owners']:
        assert owner['dimensions'] == [CELL[0]*9, CELL[1]*6]
        assert [(s['phase'], s['view']) for s in owner['sequences']] == [(p, v) for p in PHASES for v in VIEWS]
        for sequence in owner['sequences']:
            asset = next(a for a in source['assets'] if a['color'] == owner['colour'] and a['phase'] == sequence['phase'])
            assert sequence['key'] == asset['key']
            original_sequence = next(s for s in asset['sequences'] if s['view'] == sequence['view'])
            assert len(sequence['frames']) == len(original_sequence['frames'])
            row = PHASES.index(sequence['phase'])*2 + VIEWS.index(sequence['view'])
            for i, (frame, original) in enumerate(zip(sequence['frames'], original_sequence['frames'])):
                assert frame['original'] == original
                assert Path(frame['source']).resolve() == (Path(register['source_manifest']).parent/asset['key']/original['file']).resolve()
                assert frame['source_sha256'] == original['png_sha256']
                w, h = (v*SCALE for v in original['frame_size'])
                x, y = i*CELL[0]+(CELL[0]-w)//2, row*CELL[1]+CELL[1]-h-12
                assert frame['box'] == [x, y, x+w, y+h]
    red = json.loads(args.red_manifest.read_text())
    red_zip = Path(red["mod_root"]) / "fxdata/cta-red-sprites.zip"
    assert sha(red_zip) == red["zip_sha256"]
    target = args.output.resolve()
    if target.exists():
        raise FileExistsError(target)
    target.mkdir(parents=True)
    mod = target / "mod/fidelity_cta_owners_review/fxdata"
    mod.mkdir(parents=True)
    palette = np.frombuffer(args.palette.read_bytes(), np.uint8).reshape(256, 3).astype(np.int16)
    display_palette = np.frombuffer(args.display_palette.read_bytes(), np.uint8).reshape(256, 3).astype(np.int16)
    assert all(a['source'].get('palette_sha256', sha(args.display_palette)) == sha(args.display_palette) for a in source['assets'])
    archive = mod / "cta-owners-sprites.zip"
    receipts = []
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as output:
        with zipfile.ZipFile(red_zip) as original_red:
            entries = json.loads(original_red.read("sprites.json"))
            for name in original_red.namelist():
                if name != "sprites.json":
                    archive_write(output, name, original_red.read(name))
        for owner in register["owners"]:
            assert sha(owner["reference"]) == owner["reference_sha256"]
            master = args.masters / f'{owner["colour"]}-lifecycle.png'
            sheet = Image.open(master).convert("RGBA")
            rw, rh = owner["dimensions"]
            factor = min(rw / sheet.width, rh / sheet.height)
            size = (round(sheet.width * factor), round(sheet.height * factor))
            registered = Image.new("RGBA", (rw, rh))
            registered.alpha_composite(sheet.resize(size, Image.Resampling.LANCZOS), ((rw-size[0])//2, (rh-size[1])//2))
            owner_entries = {}
            for sequence in owner["sequences"]:
                key, view = sequence["key"], sequence["view"]
                source_asset = next(a for a in source['assets'] if a['key'] == key)
                entry = owner_entries.setdefault(key, {"name": "FIDELITY_CTA_" + key.upper().replace("-", "_"),
                                                      "rotatable": False, "resolution_scale": SCALE, "td": [], "fp": []})
                originals, natives, paints, frames = [], [], [], []
                for frame in sequence["frames"]:
                    assert sha(frame["source"]) == frame["source_sha256"]
                    image = Image.open(frame["source"]).convert("RGBA")
                    if source_asset['source']['kind'] == 'installed-custom-png':
                        a = np.array(image)
                        a[:, :, :3] = (display_palette[palette_indices(a, palette)] * 4).astype(np.uint8)
                        image = Image.fromarray(a)
                    originals.append(image)
                    natives.append(np.array(image.resize((image.width*SCALE, image.height*SCALE), Image.Resampling.NEAREST)))
                    paints.append(np.array(registered.crop(frame["box"])))
                native, raw = np.stack(natives), np.stack(paints)
                visible = native[:, :, :, 3] >= 64
                covered = visible & (raw[:, :, :, 3] >= 64)
                if not np.any(covered):
                    raise ValueError(f"No generated coverage for {key} {view}")
                gain = np.clip(native[:, :, :, :3][covered].mean(0) / np.maximum(raw[:, :, :, :3][covered].mean(0), 1), .25, 2)
                weight = raw[:, :, :, 3:4].astype(float)/255
                rgb = np.clip(np.rint(raw[:, :, :, :3]*gain*weight + native[:, :, :, :3]*(1-weight)), 0, 255).astype(np.uint8)
                rgb[~visible] = 0
                pixels = np.concatenate([rgb, np.where(visible[:, :, :, None], 255, 0).astype(np.uint8)], 3)
                stable = np.all(native == native[0], axis=(0, 3))
                reference_frame = next((i for i, n in enumerate(native) if np.count_nonzero(n[:, :, 3]) > 8), 0)
                pixels[:, stable] = pixels[reference_frame, stable]
                source_indices = np.stack([palette_indices(p, display_palette) for p in native])
                candidate_indices = np.stack([palette_indices(p, display_palette) for p in pixels])
                candidate_indices, tone = source_tone_indices(source_indices, candidate_indices, visible, display_palette)
                images = [Image.fromarray(np.dstack([(palette[index]*4).astype(np.uint8), p[:, :, 3]])) for p, index in zip(pixels, candidate_indices)]
                shown_images = [Image.fromarray(np.dstack([(display_palette[index]*4).astype(np.uint8), p[:, :, 3]])) for p, index in zip(pixels, candidate_indices)]
                final_pixels = np.stack([np.array(image) for image in images])
                assert np.array_equal(np.stack([palette_indices(p, palette) for p in final_pixels]), candidate_indices)
                assert np.all(final_pixels[:, stable] == final_pixels[0, stable])
                fallback = []
                for i, (frame, image) in enumerate(zip(sequence["frames"], images)):
                    info = frame["original"]
                    name = f"{key}/{view}-d0-f{i:02}.png"
                    dest = target / "frames" / name
                    dest.parent.mkdir(parents=True, exist_ok=True)
                    image.save(dest)
                    archive_write(output, name, dest.read_bytes())
                    assert np.array_equal(np.asarray(image)[:, :, 3] > 0, visible[i])
                    fallback.append(float(np.count_nonzero(visible[i] & ~covered[i]))/max(1, np.count_nonzero(visible[i])))
                    frames.append({"file": name, "frame_w": image.width, "frame_h": image.height,
                                   "offset_x": -info["anchor"][0]*SCALE, "offset_y": -info["anchor"][1]*SCALE,
                                   "offset_w": 0, "offset_h": 0, "shadow_offset": info.get("shadow_offset", 0)*SCALE,
                                   "frame_flags": info.get("frame_flags", 0)})
                entry[view].append(frames)
                w, h = images[0].size
                board = Image.new("RGB", (w*3+24, (h*2+40)*3), (32, 35, 40))
                draw = ImageDraw.Draw(board)
                loops = []
                for i, (image, original) in enumerate(zip(shown_images, originals)):
                    x, y = (i%3)*(w+8), (i//3)*(h*2+40)
                    draw.text((x+2, y+2), f"{key} {view} {i} source / candidate", fill="white")
                    ref = original.resize(image.size, Image.Resampling.NEAREST)
                    board.paste(ref, (x, y+20), ref)
                    board.paste(image, (x, y+h+24), image)
                    loop = Image.new("RGB", (original.width*2+24, original.height+20), (32, 35, 40))
                    loop.paste(original, (4, 16), original)
                    small = image.resize(original.size, Image.Resampling.BOX)
                    loop.paste(small, (original.width+16, 16), small)
                    loops.append(loop)
                comparison = target / f"{key}-{view}-comparison.png"
                board.save(comparison)
                loops[0].save(target/f"{key}-{view}-loop.gif", save_all=True, append_images=loops[1:], duration=50, loop=0, disposal=2)
                receipts.append({"key": key, "view": view, "frames": len(frames), "master": str(master),
                                 "master_sha256": sha(master), "reference_sha256": owner["reference_sha256"],
                                 "registration_scale": factor, "rgb_gain": gain.tolist(),
                                 "source_tone": tone,
                                 "fallback_mean": float(np.mean(fallback)), "fallback_max": max(fallback),
                                 "source_masks_exact": True, "anchors_and_phase_order_exact": True,
                                 "stationary_source_pixels_with_new_changes": 0,
                                 "comparison": str(comparison), "runtime_validated": False})
            entries.extend(owner_entries.values())
        archive_write(output, "sprites.json", json.dumps(entries, separators=(",", ":")).encode("utf-8"))
    rows = []
    for i, phase in enumerate(PHASES):
        names = ['"FIDELITY_CTA_'+c.upper()+'_'+phase.upper()+'"' for c in COLOURS]
        names.insert(5, "0")
        rows.append("    ["+str(867+i)+", "+", ".join(names)+"],")
    (mod/"spritecolors.toml").write_text("animationIds = [\n"+"\n".join(rows)+"\n]\n")
    with zipfile.ZipFile(archive) as result, zipfile.ZipFile(red_zip) as original_red:
        assert result.testzip() is None
        assert sum(n.endswith(".png") for n in result.namelist()) == 384
        assert json.loads(result.read("sprites.json"))[:3] == json.loads(original_red.read("sprites.json"))
        for name in original_red.namelist():
            if name != "sprites.json":
                assert result.read(name) == original_red.read(name)
    write_json(target/"manifest.json", {"schema": 1, "kind": "local_review_candidate_only",
              "source_manifest": register["source_manifest"], "source_manifest_sha256": register["source_manifest_sha256"],
              "source_register_sha256": sha(register_path), "red_zip_sha256": sha(red_zip), "red_members_unchanged": True,
              "palette_sha256": sha(args.palette), "display_palette_sha256": sha(args.display_palette), "mod_root": str(mod.parent), "zip_sha256": sha(archive),
              "config_sha256": sha(mod/"spritecolors.toml"), "owners": list(COLOURS), "frame_uses": 384,
              "sequences": receipts, "native_neutral_unchanged": True, "phase_frame_counts": [7, 8, 9],
              "source_lifecycle": source["lifecycle"], "objects_and_magic_config_modified": False,
              "runtime_modified": False, "active_manifest_modified": False, "runtime_validated": False,
              "user_appearance_accepted": False, "promotion_allowed": False})


def main():
    if not __debug__:
        raise RuntimeError("Run without -O; source-binding assertions must remain enabled")
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("prepare")
    p.add_argument("--source", type=Path, required=True)
    p.add_argument("--output", type=Path, required=True)
    p = sub.add_parser("package")
    for flag in ("register", "masters", "red-manifest", "palette", "display-palette", "output"):
        p.add_argument("--"+flag, type=Path, required=True)
    args = parser.parse_args()
    (prepare if args.command == "prepare" else package)(args)


if __name__ == "__main__":
    main()
