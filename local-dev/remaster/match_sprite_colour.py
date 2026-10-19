# SPDX-License-Identifier: GPL-2.0-or-later
"""Deterministically match remastered sprite and campaign-frame colour to the originals.

Sprites: the game converts each PNG pixel with RGB//4 to the nearest png_conv_pal entry
(Manhattan, first index on ties) and shows palette.dat for that index. Each HD animation is
compared with the original creature.tab/jty frames it replaces (or, for KeeperFX-only objects,
KeeperFX's own native sprite for the same slot), pooled over directions and frames. Two
statistics are matched after that conversion: the coverage-weighted linear-light mean colour
(CIELAB L*, a*, b*) and the mean chroma C* on the native pixel grid (HD box-filtered back to
original size). The grade is a linear-light gain per channel (exposure and white balance),
then a CIELAB chroma scale about neutral. Creature archives receive one grade per creature,
with each animation moved part way towards its own fit (--creature-weight); object archives
are fitted per object. Original-pixel fallback frames (nearest-neighbour enlargements, or
scale 1) stay untouched. Graded pixels are written as the exact conversion-palette colour of
their index, so the game loads the intended index. Alpha, members, order, metadata and
sprites.json are unchanged.

Campaign frames (huge format): only pixels that differ from the original are graded and
re-quantized with the frontend quantizer, limited to palette entries the original or current
frame already uses. Native pixels and coverage stay exact.

Inputs are the outputs of tools/import_remaster_sprites.py and ui_publication.py built from
the bundles before the colour pass. apply_colour_match.py stores the result in the bundles.
"""
import argparse
import hashlib
import io
import json
import re
import sys
import zipfile
from pathlib import Path

import numpy as np
from PIL import Image

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / 'tools'))
sys.path.insert(0, str(REPO / 'local-dev/world-remaster/frontend-art'))
from remaster_sprite_codec import OriginalBank  # noqa: E402
from ui_publication import decode_huge, encode_huge, unpack_source  # noqa: E402
from frontend_reconstruct import quantize as frontend_quantize  # noqa: E402

sha = lambda b: hashlib.sha256(b).hexdigest()

# The 2026-10-19 pass: archives and frames that drifted from their originals.
CREATURES = ['imp', 'dark_mistress', 'archer', 'avatar', 'barbarian', 'bile_demon', 'bug', 'demonspawn', 'dragon',
             'dwarfa', 'fairy', 'fly', 'giant', 'horny', 'knight', 'monk', 'samurai', 'skeleton', 'spider',
             'tentacle', 'thief', 'tunneller', 'vampire', 'witch', 'wizard']
CREATURE_ARCHIVES = [name + '-sprites.zip' for name in CREATURES]
OBJECT_ARCHIVES = ['pickups.zip', 'trapdoors-sprites.zip', 'combat-curated.zip']
FRAMES = ['campgns/keeporig_lnd/viframe%02d.dat' % n for n in range(22)]

# sRGB <-> linear <-> CIELAB (D65)
M = np.array([[0.4124564, 0.3575761, 0.1804375], [0.2126729, 0.7151522, 0.0721750], [0.0193339, 0.1191920, 0.9503041]])
MI = np.linalg.inv(M)
WHITE = np.array([0.95047, 1.0, 1.08883])
IDENTITY = dict(gains=[1.0, 1.0, 1.0], chroma_scale=1.0)
GAIN_LIMITS, CAST_LIMITS, CHROMA_LIMITS = (0.3, 2.5), (0.7, 1.43), (0.7, 1.6)


def to_linear(c):
    c = np.asarray(c, np.float64) / 255
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def to_srgb(c):
    c = np.clip(c, 0, 1)
    return 255 * np.where(c <= 0.0031308, c * 12.92, 1.055 * c ** (1 / 2.4) - 0.055)


def lab(lin):
    f = lin @ M.T / WHITE
    f = np.where(f > (6 / 29) ** 3, np.cbrt(f), f / (3 * (6 / 29) ** 2) + 4 / 29)
    return np.stack([116 * f[..., 1] - 16, 500 * (f[..., 0] - f[..., 1]), 200 * (f[..., 1] - f[..., 2])], -1)


def unlab(c):
    fy = (c[..., 0] + 16) / 116
    f = np.stack([fy + c[..., 1] / 500, fy, fy - c[..., 2] / 200], -1)
    return (np.where(f > 6 / 29, f ** 3, 3 * (6 / 29) ** 2 * (f - 4 / 29)) * WHITE) @ MI.T


def chroma(lin):
    c = lab(lin)
    return np.hypot(c[..., 1], c[..., 2])


def grade(rgb, g):
    """Linear-light gain per channel, then CIELAB chroma scale about neutral."""
    c = lab(to_linear(rgb) * np.asarray(g['gains']))
    c[..., 1:] *= g['chroma_scale']
    return np.clip(np.rint(to_srgb(unlab(c))), 0, 255).astype(np.uint8)


# Statistics are additive tuples: (linear colour sum, pixel count, chroma sum, chroma sample count).
def add(*stats):
    return tuple(sum(s[i] for s in stats) for i in range(4))


def summary(s):
    """L*, a*, b* of the mean linear colour, and mean C*."""
    return np.append(lab(s[0] / s[1]), s[2] / s[3])


def fit(target, measure, iterations):
    t, t_lin = summary(target), target[0] / target[1]
    gains, k, best = np.ones(3), 1.0, None
    for _ in range(iterations):
        now = measure(dict(gains=gains.tolist(), chroma_scale=k))
        n = summary(now)
        err = float(np.abs(n - t).max())
        if best is None or err < best[0]:
            best = (err, gains.copy(), k)
        if err < 0.15:
            break
        gains = gains * t_lin / np.maximum(now[0] / now[1], 1e-9)
        mean = np.exp(np.log(gains).mean())
        gains = np.clip(mean, *GAIN_LIMITS) * np.clip(gains / mean, *CAST_LIMITS)
        k = float(np.clip(k * t[3] / max(n[3], 1e-6), *CHROMA_LIMITS))
    return dict(error=best[0], gains=best[1].tolist(), chroma_scale=best[2])


def blend(family, own, w):
    return dict(gains=(np.asarray(family['gains']) ** (1 - w) * np.asarray(own['gains']) ** w).tolist(),
                chroma_scale=family['chroma_scale'] ** (1 - w) * own['chroma_scale'] ** w)


def applied(g):
    return dict(gains=g['gains'], chroma_scale=g['chroma_scale'])


def cfg_values(path):
    out, section = {}, None
    if not path.exists():
        return out
    for line in path.read_text(errors='replace').splitlines():
        line = line.split(';')[0].strip()
        m = re.match(r'\[(\w+)\]', line)
        if m:
            section = m.group(1)
            continue
        m = re.match(r'(\w+)\s*=\s*(\S+)', line)
        if m and section:
            out[(section, m.group(1))] = m.group(2)
    return out


class Sprites:
    def __init__(self, data, source_c):
        self.bank = OriginalBank(data)
        self.pal = np.array(self.bank.palette, np.uint8)
        self.pal_lin = to_linear(self.pal)
        self.pal_chroma = chroma(self.pal_lin)
        self.conv = np.array(self.bank.conversion, np.uint8)
        conv6 = self.conv.astype(np.int32) // 4
        q = np.stack(np.meshgrid(np.arange(64), np.arange(64), np.arange(64), indexing='ij'), -1).reshape(-1, 1, 3)
        self.lut = np.empty(64 ** 3, np.uint8)
        for i in range(0, len(q), 4096):
            self.lut[i:i + 4096] = np.abs(q[i:i + 4096] - conv6[None]).sum(-1).argmin(1)
        arr = re.search(r'creature_list\[CREATURE_FRAMELIST_LENGTH\]\s*=\s*\{(.*?)\};', source_c, re.S).group(1)
        self.clist = [int(n) for n in re.findall(r'\d+', re.sub(r'/\*.*?\*/|//[^\n]*', '', arr, flags=re.S))]
        self.cache = {}
        # KeeperFX's own native-size sprites (new objects with no original frames).
        self.keeperfx = {}
        for path in sorted((Path(data).parent / 'fxdata').glob('*.zip')):
            with zipfile.ZipFile(path) as z:
                for entry in json.loads(z.read('sprites.json')):
                    self.keeperfx.setdefault(entry['name'], (path, entry))

    def index(self, rgb):
        q = rgb.astype(np.int32) // 4
        return self.lut[(q[..., 0] * 64 + q[..., 1]) * 64 + q[..., 2]]

    def anim(self, anim):
        if not 0 <= anim < len(self.clist):
            return None
        base = self.clist[anim]
        try:
            h = self.bank.header(base)
        except Exception:
            return None
        dirs, count = (5 if h[5] == 2 else 1), h[6]
        if not count:
            return None
        return dirs, count, (h[3], h[4]), [base + d * count + n for d in range(dirs) for n in range(count)]

    def index_stats(self, idx):
        return (self.pal_lin[idx].sum(0), len(idx), self.pal_chroma[idx].sum(), len(idx))

    def original_stats(self, anim):
        """All frames of an original animation (native size, so chroma is per pixel)."""
        if anim not in self.cache:
            parts = []
            for idx in self.anim(anim)[3]:
                try:
                    (fw, fh), px, cov = self.bank.indices(idx)
                except Exception:
                    continue  # empty original frame
                parts.append(np.frombuffer(px, np.uint8)[np.frombuffer(cov, np.uint8) >= 128])
            self.cache[anim] = self.index_stats(np.concatenate(parts))
        return self.cache[anim]

    def keeperfx_stats(self, name, view):
        """A KeeperFX native sprite view, as the game loads it."""
        key = (name, view)
        if key not in self.cache:
            path, entry = self.keeperfx[name]
            parts = []
            with zipfile.ZipFile(path) as z:
                for f in frames_of(entry, view):
                    rgba = np.asarray(Image.open(io.BytesIO(z.read(f['file']))).convert('RGBA'))
                    rgb = rgba[..., :3][rgba[..., 3] >= 128]
                    parts.append(rgb[:, 0] if entry.get('pixel_encoding') == 'raw-index' else self.index(rgb))
            self.cache[key] = self.index_stats(np.concatenate(parts))
        return self.cache[key]

    def reference_stats(self, ref):
        return self.original_stats(ref[1]) if ref[0] == 'original' else self.keeperfx_stats(ref[1], ref[2])


class Sample:
    """Opaque HD pixels of some frames, with native-grid blocks for chroma (as the audit measures it)."""

    def __init__(self, sp, frames):
        keys, blocks, threshold, offset = [], [], [], 0
        for rgba, s in frames:
            h, w = rgba.shape[:2]
            yy, xx = np.nonzero(rgba[..., 3] >= 128)
            rgb = rgba[yy, xx, :3].astype(np.int32)
            keys.append((rgb[:, 0] << 16) | (rgb[:, 1] << 8) | rgb[:, 2])
            bh, bw = h // s, w // s
            inside = (yy < bh * s) & (xx < bw * s)
            blocks.append(np.where(inside, offset + (yy // s) * bw + xx // s, -1))
            threshold.append(np.full(bh * bw, 0.5 * s * s))
            offset += bh * bw
        self.sp = sp
        uniq, inv = np.unique(np.concatenate(keys), return_inverse=True)
        self.rgb = np.stack([uniq >> 16, (uniq >> 8) & 255, uniq & 255], -1).astype(np.uint8)
        self.counts = np.bincount(inv, minlength=len(uniq)).astype(np.float64)
        block = np.concatenate(blocks)
        valid = block >= 0
        self.block, self.block_inv, self.blocks = block[valid], inv[valid], offset
        filled = np.bincount(self.block, minlength=offset)
        self.keep = filled >= np.concatenate(threshold)
        self.filled = filled[self.keep].astype(np.float64)

    def measure(self, g):
        shown = self.sp.pal_lin[self.sp.index(grade(self.rgb, g))]
        sums = np.stack([np.bincount(self.block, weights=shown[self.block_inv, c], minlength=self.blocks) for c in range(3)], -1)
        c = chroma(sums[self.keep] / self.filled[:, None])
        return (self.counts @ shown, self.counts.sum(), c.sum(), len(c))


def frames_of(entry, view):
    return [f for d in entry.get(view) or [] for f in (d if isinstance(d, list) else [d]) if isinstance(f, dict) and 'file' in f]


def is_native(rgba, scale):
    """Original fallback: scale 1, or opaque pixels made of uniform scale-by-scale blocks."""
    if scale <= 1:
        return True
    h, w = rgba.shape[:2]
    for oy in range(scale):
        for ox in range(scale):
            hh, ww = (h - oy) // scale * scale, (w - ox) // scale * scale
            block = rgba[oy:oy + hh, ox:ox + ww].reshape(hh // scale, scale, ww // scale, scale, 4)
            if (block == block[:, :1, :, :1]).all() and not rgba[:oy].any() and not rgba[:, :ox].any() \
                    and not rgba[oy + hh:].any() and not rgba[:, ox + ww:].any():
                return True
    return False


def content_key(rgba):
    """Visible pixels cropped to their bounds, so the same picture on another canvas matches."""
    ys, xs = np.nonzero(rgba[..., 3])
    if not len(ys):
        return None
    return sha(rgba[ys.min():ys.max() + 1, xs.min():xs.max() + 1].tobytes())


def match_anim(sp, entry, view, frames, ids, scale):
    dirs = entry.get(view) or []
    fw0, fh0 = frames[0]['frame_w'], frames[0]['frame_h']
    best = None
    for base in sorted(ids):
        for anim in (base - 1, base, base + 1):
            info = sp.anim(anim)
            if not info:
                continue
            nd, cnt, (ow, oh), _ = info
            score = (abs(nd - len(dirs)) * 10 + abs(cnt - len(frames) // max(len(dirs), 1)) * 2
                     + (abs(ow * scale - fw0) + abs(oh * scale - fh0)) / max(scale, 1) / 4 + (0 if anim == base else 3))
            if best is None or score < best[0]:
                best = (score, anim)
    return best if best and best[0] <= 5 else None


def grade_archive(sp, path, hd_cfg, orig_cfg, per_unit_weight, iterations, share_identical=False):
    z = zipfile.ZipFile(path)
    sprites = json.loads(z.read('sprites.json'))
    pixels = {}
    for name in z.namelist():
        if name.endswith('.png'):
            pixels[name] = np.asarray(Image.open(io.BytesIO(z.read(name))).convert('RGBA'))
    # Each sprite entry: its HD member files and the native frames it replaces.
    units, scales = [], {}
    for entry in sprites:
        ids = set()
        if isinstance(entry.get('replace_original'), int):
            ids.add(entry['replace_original'])
        ids.update(int(n) for n in re.findall(r'_(\d+)(?:_|$)', entry['name']))
        names = {entry['name']} & sp.keeperfx.keys()
        for key, value in hd_cfg.items():
            if value == entry['name'] and orig_cfg.get(key, '').isdigit():
                ids.add(int(orig_cfg[key]))
            elif value == entry['name'] and orig_cfg.get(key) in sp.keeperfx:
                names.add(orig_cfg[key])
        members, refs, natives = set(), set(), set()
        for view in ('td', 'fp'):
            frames = frames_of(entry, view)
            if not frames:
                continue
            scale = entry.get(view + '_resolution_scale', entry.get('resolution_scale', 1))
            for f in frames:
                (natives if is_native(pixels[f['file']], scale) else members).add(f['file'])
                scales[f['file']] = scale
            if any(f['file'] in members for f in frames):
                found = match_anim(sp, entry, view, frames, ids, scale)
                if found:
                    refs.add(('original', found[1]))
                else:
                    # No original frames: compare with KeeperFX's own native sprite for the same slot.
                    for name in sorted(names):
                        native = sp.keeperfx[name][1]
                        refs.update([('keeperfx', name, v) for v in (view, 'fp' if view == 'td' else 'td') if frames_of(native, v)][:1])
        if members:
            units.append(dict(names=[entry['name']], members=members, refs=refs))
    # Entries sharing member files must share one grade; with share_identical, so must entries
    # showing an identical frame on different canvases (such as a pose's shared endpoint).
    content = {n: content_key(pixels[n]) if share_identical else n for u in units for n in u['members']}
    shared = lambda a, b: ({content[n] for n in a['members']} & {content[n] for n in b['members']}) - {None}
    merged = []
    for u in units:
        for m in [m for m in merged if shared(m, u)]:
            merged.remove(m)
            u = dict(names=m['names'] + u['names'], members=m['members'] | u['members'], refs=m['refs'] | u['refs'])
        merged.append(u)
    natives_all = {n for n in pixels if n not in set().union(*(u['members'] for u in merged))}
    for u in merged:
        u['sample'] = Sample(sp, [(pixels[n], scales[n]) for n in sorted(u['members'])])

    def target_of(unit_list):
        return add(*(sp.reference_stats(r) for r in sorted({r for u in unit_list for r in u['refs']})))

    def measure_of(unit_list):
        return lambda g: add(*(u['sample'].measure(g) for u in unit_list))

    matched = [u for u in merged if u['refs']]
    report = dict(units=[], native_members=len(natives_all))
    if not matched:
        return z, pixels, {}, dict(report, skipped='no matched original or KeeperFX reference')
    family = fit(target_of(matched), measure_of(matched), iterations)
    report.update(family=family, family_target=summary(target_of(matched)).tolist(),
                  family_before=summary(measure_of(matched)(IDENTITY)).tolist(),
                  family_after=summary(measure_of(matched)(family)).tolist())
    grades = {}
    for u in merged:
        if u['refs'] and len(matched) > 1:
            own = fit(target_of([u]), measure_of([u]), iterations)
            g = blend(family, own, per_unit_weight)
        else:
            own, g = None, applied(family)
        for n in u['members']:
            grades[n] = g
        stats = (lambda g_: summary(measure_of([u])(g_)).tolist())
        report['units'].append(dict(sprites=u['names'], members=len(u['members']), references=[list(r) for r in sorted(u['refs'])],
                                    own_fit=own, applied=g, before=stats(IDENTITY), after=stats(g),
                                    target=(summary(target_of([u])).tolist() if u['refs'] else None)))
    return z, pixels, grades, report


def write_archive(sp, z, pixels, grades, out_path):
    """Same members, order, timestamps and compression; only graded PNG pixels change."""
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, 'w') as out:
        for info in z.infolist():
            data = z.read(info)
            if info.filename in grades:
                rgba = pixels[info.filename].copy()
                rgba[..., :3] = sp.conv[sp.index(grade(rgba[..., :3], grades[info.filename]))]
                png = io.BytesIO()
                Image.fromarray(rgba, 'RGBA').save(png, format='PNG', optimize=False)
                data = png.getvalue()
            new = zipfile.ZipInfo(info.filename, date_time=info.date_time)
            new.compress_type, new.external_attr, new.create_system = info.compress_type, info.external_attr, info.create_system
            out.writestr(new, data)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_bytes(buf.getvalue())


def grade_frames(game, ui, entries, weight, iterations):
    """Campaign frames: grade only generated (non-native) pixels, re-quantize to used entries."""
    load = lambda p: unpack_source(Path(p).read_bytes())
    items = []
    for e in entries:
        w, h = e['dimensions']
        pal6 = np.frombuffer(load(game / e['palette'])[:768], np.uint8).reshape(256, 3)
        pal = pal6.astype(np.uint8) * 4
        pal_lin = to_linear(pal)
        pal_chroma = chroma(pal_lin)
        oi, om = decode_huge(load(game / e['source']), w, h)
        hi, hm = decode_huge(load(ui / e['output']), w, h)
        oi, om = np.frombuffer(bytes(oi), np.uint8).reshape(h, w), np.frombuffer(bytes(om), np.uint8).reshape(h, w)
        hi, hm = np.frombuffer(bytes(hi), np.uint8).reshape(h, w), np.frombuffer(bytes(hm), np.uint8).reshape(h, w)
        assert (om > 0).sum() and np.array_equal(om > 0, hm > 0), 'Frame coverage differs: ' + e['output']
        mask = hm > 0
        generated = mask & (hi != oi)
        allowed = np.union1d(np.unique(oi[mask]), np.unique(hi[mask]))
        stats = lambda idx, pal_lin=pal_lin, pal_chroma=pal_chroma: (pal_lin[idx].sum(0), len(idx), pal_chroma[idx].sum(), len(idx))
        uniq, inverse = np.unique(hi[generated], return_inverse=True)
        counts = np.bincount(inverse.ravel(), minlength=len(uniq)).astype(np.float64)
        fixed = stats(hi[mask & ~generated])

        def remap(g, uniq=uniq, pal=pal, pal6=pal6, allowed=allowed):
            return frontend_quantize(grade(pal[uniq][None], g), pal6, allowed)[0]

        def measure(g, remap=remap, counts=counts, fixed=fixed, pal_lin=pal_lin, pal_chroma=pal_chroma):
            idx = remap(g)
            return add(fixed, (counts @ pal_lin[idx], counts.sum(), counts @ pal_chroma[idx], counts.sum()))
        items.append(dict(entry=e, w=w, h=h, hi=hi, hm=hm, generated=generated, inverse=inverse,
                          remap=remap, measure=measure, target=stats(oi[mask]), generated_share=float(generated.sum() / mask.sum())))
    family = fit(add(*(it['target'] for it in items)), lambda g: add(*(it['measure'](g) for it in items)), iterations)
    out, report = {}, dict(family=family, frames=[])
    for it in items:
        own = fit(it['target'], it['measure'], iterations)
        g = blend(family, own, weight)
        new = it['hi'].copy()
        new[it['generated']] = it['remap'](g)[it['inverse'].ravel()]
        out[it['entry']['output']] = encode_huge(new.tobytes(), it['hm'].tobytes(), it['w'], it['h'])
        report['frames'].append(dict(file=it['entry']['output'], generated_share=it['generated_share'], own_fit=own, applied=g,
                                     target=summary(it['target']).tolist(), before=summary(it['measure'](IDENTITY)).tolist(),
                                     after=summary(it['measure'](g)).tolist()))
    return out, report


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--sprites', type=Path, required=True, help='import_remaster_sprites.py output (fxdata/, creatrs/)')
    ap.add_argument('--ui', type=Path, help='ui_publication.py output holding the campaign frames')
    ap.add_argument('--game', type=Path, required=True, help='prepared runtime root with original data/ and fxdata/')
    ap.add_argument('--keeperfx', type=Path, default=REPO, help='KeeperFX checkout (original configs and creature_list)')
    ap.add_argument('--creature-archive', action='append', help='default (nothing selected): the 2026-10-19 creature list')
    ap.add_argument('--object-archive', action='append', help='default (nothing selected): pickups, trapdoors and combat effects')
    ap.add_argument('--frame', action='append', help='native frontend output path; default (nothing selected): viframe00-21 with --ui')
    ap.add_argument('--ui-manifest', type=Path, default=REPO / 'assets/remaster-ui/manifest.json')
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--creature-weight', type=float, default=0.5)
    ap.add_argument('--frame-weight', type=float, default=0.5)
    ap.add_argument('--iterations', type=int, default=16)
    ap.add_argument('--share-identical', action='store_true', help='entries showing an identical frame share one grade')
    args = ap.parse_args()
    # The 2026-10-19 lists apply only when nothing is selected explicitly.
    explicit = any(v is not None for v in (args.creature_archive, args.object_archive, args.frame))
    creatures = args.creature_archive or ([] if explicit else CREATURE_ARCHIVES)
    objects = args.object_archive or ([] if explicit else OBJECT_ARCHIVES)
    frames = args.frame or ([] if explicit or not args.ui else FRAMES)
    if frames and not args.ui:
        raise SystemExit('--frame needs --ui.')
    if args.output.exists() and any(args.output.iterdir()):
        raise SystemExit('Output must be a new or empty directory.')
    sp = Sprites(args.game / 'data', (args.keeperfx / 'src/creature_graphics.c').read_text())
    orig_cfg, hd_cfg = {}, {}
    for p in (args.keeperfx / 'config/creatrs').glob('*.cfg'):
        orig_cfg.update({(p.stem + ':' + s, k): v for (s, k), v in cfg_values(p).items()})
    for p in (args.sprites / 'creatrs').glob('*.cfg'):
        hd_cfg.update({(p.stem + ':' + s, k): v for (s, k), v in cfg_values(p).items()})
    for name in ('objects.cfg', 'trapdoor.cfg'):
        orig_cfg.update({(name + ':' + s, k): v for (s, k), v in cfg_values(args.keeperfx / 'config/fxdata' / name).items()})
        hd_cfg.update({(name + ':' + s, k): v for (s, k), v in cfg_values(args.sprites / 'fxdata' / name).items()})
    report = dict(method=__doc__, tool_sha256=sha(Path(__file__).read_bytes()),
                  limits=dict(gain=GAIN_LIMITS, cast=CAST_LIMITS, chroma=CHROMA_LIMITS),
                  sources={n: sp.bank.hashes[n] for n in sp.bank.hashes}, archives={}, frames=None)
    for name, weight in [(n, args.creature_weight) for n in creatures] + [(n, 1.0) for n in objects]:
        src = args.sprites / 'fxdata' / name
        z, pixels, grades, r = grade_archive(sp, src, hd_cfg, orig_cfg, weight, args.iterations, args.share_identical)
        r.update(source_sha256=sha(src.read_bytes()), unit_weight=weight, share_identical=args.share_identical,
                 graded_members=len(grades))
        if grades:
            dest = args.output / 'fxdata' / name
            write_archive(sp, z, pixels, grades, dest)
            r['output_sha256'] = sha(dest.read_bytes())
        report['archives'][name] = r
        f = r.get('family', IDENTITY)
        print(f"{name}: {len(grades)} graded, gains {np.round(f['gains'], 3).tolist()} chroma {f['chroma_scale']:.3f} "
              f"error {f.get('error', 0):.2f}", flush=True)
    if frames:
        manifest = json.loads(args.ui_manifest.read_text(encoding='utf-8-sig'))
        entries = [e for e in manifest['native'] if e['output'] in frames]
        assert len(entries) == len(frames) and all(e['format'] == 'huge' for e in entries)
        for e in entries:
            for key in ('source', 'palette'):
                expected = manifest['sources'][e[key]]['sha256']
                if sha((args.game / e[key]).read_bytes()) != expected:
                    raise SystemExit('Original frontend source differs (overlay installed?): ' + e[key])
        for e in entries:
            if sha((args.ui / e['output']).read_bytes()) != e['sha256']:
                raise SystemExit('Frame differs from the UI manifest: ' + e['output'])
        data, r = grade_frames(args.game, args.ui, entries, args.frame_weight, args.iterations)
        for name, raw in data.items():
            dest = args.output / name
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_bytes(raw)
        report['frames'] = r
        print('frames:', len(data), 'family', r['family'], flush=True)
    (args.output / 'colour-match-report.json').write_text(json.dumps(report, indent=1), encoding='utf-8')


if __name__ == '__main__':
    main()
