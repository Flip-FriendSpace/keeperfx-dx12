# SPDX-License-Identifier: GPL-2.0-or-later
"""Deterministically match enhanced terrain colour to the original STANDARD tiles.

Each family (one generated master) receives one grade: a linear-light exposure gain,
then a hue rotation and chroma scale about neutral in CIELAB a*/b*. Neutral stone,
mortar and highlights stay neutral. The grade is fitted so the re-quantized tiles,
box-filtered back to 32 by 32, share the originals' mean L*, a* and b*. Texture
detail and contrast are left as accepted. Quantization uses
the accepted rule: RGB floor-divided by 4, Manhattan distance to the engine palette,
first-index tie break. The output folder can be exported with export_enhanced_terrain.py.
"""
import argparse
import hashlib
import json
import struct
from collections import defaultdict
from pathlib import Path

import numpy as np
from PIL import Image

parser = argparse.ArgumentParser()
parser.add_argument('--source', type=Path, required=True, help='Accepted local terrain artifacts (manifest.json, RGB masters, pack).')
parser.add_argument('--original-data', type=Path, required=True, help='Original data folder with tmapa000.dat, tmapb000.dat and palette.dat.')
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--iterations', type=int, default=12)
parser.add_argument('--tile-weight', type=float, default=0.5, help='0 uses the family grade; 1 fits every tile to its own original.')
args = parser.parse_args()


def sha(data):
    return hashlib.sha256(data).hexdigest()


source = json.loads((args.source / 'manifest.json').read_text(encoding='utf-8-sig'))
bank_a = (args.original_data / 'tmapa000.dat').read_bytes()
bank_b = (args.original_data / 'tmapb000.dat').read_bytes()
palette = (args.original_data / 'palette.dat').read_bytes()
assert sha(bank_a) == source['source_sha256'] and sha(bank_b) == source['source_b_sha256'], 'Original STANDARD source mismatch'
assert sha(palette) == source['palette_sha256'], 'Original palette mismatch'
pack = (args.source / 'hd-terrain-pilot.kfht').read_bytes()
assert sha(pack) == source['package']['sha256'], 'Accepted pack does not match its manifest'
if args.output.exists() and any(args.output.iterdir()):
    raise SystemExit(f'Output folder is not empty: {args.output}')
(args.output / 'tiles').mkdir(parents=True, exist_ok=True)

p6 = np.frombuffer(palette, np.uint8).reshape(256, 3).astype(np.int32)
p8 = (p6 * 4).astype(np.uint8)
lut = np.zeros(64 ** 3, np.uint8)
grid = np.stack(np.meshgrid(np.arange(64), np.arange(64), np.arange(64), indexing='ij'), -1).reshape(-1, 3)
for lo in range(0, len(grid), 16384):
    lut[lo:lo + 16384] = np.abs(grid[lo:lo + 16384, None, :] - p6[None]).sum(-1).argmin(1)


def quantize(rgb):
    q = rgb.astype(np.int32) // 4
    return lut[(q[..., 0] * 64 + q[..., 1]) * 64 + q[..., 2]]


M = np.array([[0.4124564, 0.3575761, 0.1804375], [0.2126729, 0.7151522, 0.0721750], [0.0193339, 0.1191920, 0.9503041]])
MI = np.linalg.inv(M)
WHITE = np.array([0.95047, 1.0, 1.08883])


def to_linear(rgb):
    c = np.asarray(rgb, np.float64) / 255
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def to_srgb(lin):
    c = np.clip(lin, 0, 1)
    return 255 * np.where(c <= 0.0031308, c * 12.92, 1.055 * c ** (1 / 2.4) - 0.055)


def lab(lin):
    f = (lin @ M.T) / WHITE
    f = np.where(f > (6 / 29) ** 3, np.cbrt(np.maximum(f, 0)), f / (3 * (6 / 29) ** 2) + 4 / 29)
    return np.stack([116 * f[..., 1] - 16, 500 * (f[..., 0] - f[..., 1]), 200 * (f[..., 1] - f[..., 2])], -1)


def unlab(c):
    fy = (c[..., 0] + 16) / 116
    f = np.stack([fy + c[..., 1] / 500, fy, fy - c[..., 2] / 200], -1)
    return (np.where(f > 6 / 29, f ** 3, 3 * (6 / 29) ** 2 * (f - 4 / 29)) * WHITE) @ MI.T


def native(tile):
    data, index = (bank_a, tile) if tile < 544 else (bank_b, tile - 1000)
    rows = np.frombuffer(data, np.uint8).reshape(-1, 256)
    return rows[index // 8 * 32:index // 8 * 32 + 32, index % 8 * 32:index % 8 * 32 + 32]


def native_scale(indexed):
    return to_linear(p8[indexed]).reshape(32, 4, 32, 4, 3).mean((1, 3))


def stats(linear):
    c = lab(linear).reshape(-1, 3)
    return np.array([*c.mean(0), np.hypot(c[:, 1], c[:, 2]).mean()])


def grade(master, exposure, chroma, hue):
    c = lab(to_linear(master) * exposure)
    cos, sin = np.cos(np.radians(hue)) * chroma, np.sin(np.radians(hue)) * chroma
    c[..., 1], c[..., 2] = c[..., 1] * cos - c[..., 2] * sin, c[..., 1] * sin + c[..., 2] * cos
    return np.clip(np.rint(to_srgb(unlab(c))), 0, 255).astype(np.uint8)


def luminance(lightness):
    return ((lightness + 16) / 116) ** 3


def error(a, b):
    return float(np.abs(a[:3] - b[:3]).max())


def angle(c):
    return np.degrees(np.arctan2(c[2], c[1]))


# Pack order, native references and accepted payloads.
magic, version, count, reserved = struct.unpack_from('<4I', pack)
assert (magic, version, reserved) == (0x5448464b, 2, 0)
records, at = [], 16
for _ in range(count):
    tile, size, res = struct.unpack_from('<3I', pack, at)
    at += 12
    records.append((tile, size, pack[at:at + 1024], pack[at + 1024:at + 1024 + size * size]))
    at += 1024 + size * size
assert at == len(pack)

candidates = {c['tile']: c for c in source['candidates']}
masters, families = {}, defaultdict(list)
for tile, size, ref, pixels in records:
    c = candidates[tile]
    with Image.open(args.source / c['rgb_file']) as im:
        masters[tile] = np.asarray(im.convert('RGB'))
    assert quantize(masters[tile]).tobytes() == pixels, f'RGB master does not reproduce accepted tile {tile}'
    assert sha(ref) == c.get('source_native_sha256', sha(ref)) == sha(native(tile).tobytes()), f'Native reference mismatch {tile}'
    families[c.get('family') or c['label']].append(tile)

def measure(graded):
    return stats(np.concatenate([native_scale(quantize(g)).reshape(-1, 3) for g in graded.values()]))


def fit(tiles):
    """Iterate exposure, chroma and hue until the quantized tiles match the originals' mean L*, a*, b*."""
    target = stats(np.concatenate([to_linear(p8[native(t)]).reshape(-1, 3) for t in tiles]))
    exposure, chroma, hue = 1.0, 1.0, 0.0
    best = None
    for step in range(args.iterations):
        graded = {t: grade(masters[t], exposure, chroma, hue) for t in tiles}
        now = measure(graded)
        if best is None or error(now, target) < best[0]:
            best = (error(now, target), exposure, chroma, hue)
        if best[0] < 0.15:
            break
        exposure = float(np.clip(exposure * luminance(target[0]) / luminance(now[0]), 0.3, 2.0))
        chroma = float(np.clip(chroma * np.hypot(*target[1:3]) / max(np.hypot(*now[1:3]), 1e-6), 0.6, 1.6))
        hue = float(np.clip(hue + (angle(target) - angle(now) + 180) % 360 - 180, -30, 30))
    return target, best[1:]


report = {'method': __doc__.strip().splitlines()[0], 'tile_weight': args.tile_weight, 'families': {}, 'tiles': {}}
out = {}  # graded RGB masters
for family, tiles in sorted(families.items()):
    target, (exposure, chroma, hue) = fit(tiles)
    # Partial pooling: each tile moves part of the way from the family grade towards its own fit.
    for t in tiles:
        own = fit([t])[1] if args.tile_weight and len(tiles) > 1 else (exposure, chroma, hue)
        w = args.tile_weight
        params = (exposure * (own[0] / exposure) ** w, chroma * (own[1] / chroma) ** w, hue + w * (own[2] - hue))
        out[t] = grade(masters[t], *params)
        report['tiles'][str(t)] = {'family': family, 'exposure': params[0], 'chroma_scale': params[1], 'hue_rotation_degrees': params[2]}
    before = measure({t: masters[t] for t in tiles})
    after = measure({t: out[t] for t in tiles})
    names = ['L', 'a', 'b', 'C']
    report['families'][family] = {
        'tiles': len(tiles), 'exposure': exposure, 'chroma_scale': chroma, 'hue_rotation_degrees': hue,
        'original': dict(zip(names, np.round(target, 3).tolist())),
        'before': dict(zip(names, np.round(before, 3).tolist())),
        'after': dict(zip(names, np.round(after, 3).tolist())),
        'mean_delta_e76_before': round(float(np.linalg.norm(before[:3] - target[:3])), 3),
        'mean_delta_e76_after': round(float(np.linalg.norm(after[:3] - target[:3])), 3)}
    print(f'{family[:30]:30s} {len(tiles):3d}  dE {report["families"][family]["mean_delta_e76_before"]:5.2f} -> {report["families"][family]["mean_delta_e76_after"]:5.2f}  exposure {exposure:.3f} chroma {chroma:.3f} hue {hue:+.1f}  C {target[3]:.1f}/{before[3]:.1f}->{after[3]:.1f}')

# Write graded masters, indexed tiles and the replacement pack in the accepted order.
manifest = json.loads(json.dumps(source))
written = bytearray(struct.pack('<4I', magic, version, count, reserved))
for tile, size, ref, _ in records:
    c = candidates[tile]
    graded = out[tile]
    indexed_pixels = quantize(graded)
    raw = indexed_pixels.tobytes()
    prefix = f'tiles/tile-{tile:04d}'
    Image.fromarray(graded).save(args.output / f'{prefix}-rgb-colour.png')
    indexed = Image.frombytes('P', (size, size), raw)
    indexed.putpalette(p8.flatten().tolist())
    indexed.save(args.output / f'{prefix}-indexed-colour.png')
    entry = next(e for e in manifest['candidates'] if e['tile'] == tile)
    entry.update({'rgb_file': f'{prefix}-rgb-colour.png', 'indexed_file': f'{prefix}-indexed-colour.png', 'index8_file': None,
                  'index8_sha256': sha(raw), 'palette_colours_used': len(set(raw)),
                  'candidate_mean_rgb': p8[indexed_pixels].reshape(-1, 3).mean(0).tolist(),
                  'colour_match': {'from_rgb_file': c['rgb_file'], 'from_index8_sha256': c['index8_sha256']}})
    written += struct.pack('<3I', tile, size, 0) + ref + raw
(args.output / 'hd-terrain-pilot.kfht').write_bytes(written)
manifest['package'] = dict(source['package'], path=str(args.output / 'hd-terrain-pilot.kfht'), sha256=sha(written), bytes=len(written))
manifest['colour_match'] = {'source_package_sha256': source['package']['sha256'], 'report': 'colour-match-report.json'}
(args.output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8', newline='\n')
report.update({'source_package_sha256': source['package']['sha256'], 'package_sha256': sha(written), 'tiles': count})
(args.output / 'colour-match-report.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8', newline='\n')
print(json.dumps({'tiles': count, 'families': len(families), 'package_sha256': sha(written)}))
