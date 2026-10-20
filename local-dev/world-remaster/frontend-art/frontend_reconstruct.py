# SPDX-License-Identifier: GPL-2.0-or-later
"""Rebuild generated frontend contours/materials over verified installed artwork."""
import struct
import numpy as np
import PIL
from PIL import Image, ImageFilter
from ui_publication import require, decode_sprite


def preflight():
    require(PIL.__version__ == '12.3.0' and np.__version__ == '2.3.5',
            'UI reconstruction requires Pillow==12.3.0 and numpy==2.3.5')


def native(sources, bank, index):
    table, data = sources['ldata/' + bank + '.tab'], sources['ldata/' + bank + '.dat']
    at, width, height = struct.unpack_from('<IBB', table, index * 6)
    indices, mask, _ = decode_sprite(memoryview(data)[at:], width, height)
    return (np.frombuffer(indices, np.uint8).reshape(height, width),
            np.frombuffer(mask, np.uint8).reshape(height, width))


def palette(raw):
    p = np.frombuffer(raw, np.uint8).reshape(256, 3)
    return ((p.astype(np.uint16) * 255) // 63).astype(np.uint8), p


def quantize(rgb, raw, allowed=None):
    allowed = np.arange(256) if allowed is None else allowed
    colours = raw[allowed].astype(np.int32)
    flat = np.asarray(rgb, np.int16).reshape(-1, 3) // 4
    result = np.empty(len(flat), np.uint8)
    for at in range(0, len(flat), 4096):
        diff = flat[at:at + 4096, None, :].astype(np.int32) - colours[None, :, :]
        squared, linear = (diff * diff).sum(2), np.abs(diff).sum(2)
        dark = colours[:, 2] ** 2 + 2 * (colours[:, 0] ** 2 + colours[:, 1] ** 2)
        score = (squared.astype(np.int64) * 193 + linear) * 20481 + dark
        exact = squared == 0
        chosen = np.where(exact.any(1), exact.argmax(1), score.argmin(1))
        result[at:at + 4096] = allowed[chosen]
    return result.reshape(np.asarray(rgb).shape[:2])


def nearest_field(indices, mask):
    height, width = mask.shape
    yy, xx = np.where(mask > 0)
    require(len(xx) > 0, 'Empty source ink')
    require(height * width * len(xx) <= 16 * 1024 * 1024, 'Native colour-field work limit exceeded')
    gy, gx = np.indices((height, width))
    distance = (gy.reshape(-1, 1) - yy[None, :]) ** 2 + (gx.reshape(-1, 1) - xx[None, :]) ** 2
    closest = distance.argmin(1)
    return indices[yy[closest], xx[closest]].reshape((height, width) + indices.shape[2:])


def repeat4(array):
    return np.repeat(np.repeat(array, 4, axis=0), 4, axis=1)


def reconstruct_font(bank, record, sources, load_object):
    preflight()
    name, index = bank['name'], record['index']
    original, mask = native(sources, name, index)
    height, width = mask.shape
    pal, raw = palette(sources[bank['palette']])
    _, shape_mask = native(sources, record['shape_bank'], record['shape_index'])
    ah, aw = shape_mask.shape[0] * 4, shape_mask.shape[1] * 4
    require(0 < aw <= 1024 and 0 < ah <= 1024, 'Invalid generated contour size')
    payload = load_object(record['generated_alpha'])
    require(len(payload) == aw * ah, 'Generated contour size mismatch')
    base_alpha = np.frombuffer(payload, np.uint8).reshape(ah, aw)
    alpha = np.zeros((height * 4, width * 4), np.uint8)
    oh, ow = min(height * 4, ah), min(width * 4, aw)
    alpha[:oh, :ow] = base_alpha[:oh, :ow]
    if name in ('frontft3', 'frontft4'):
        _, normal = native(sources, 'frontft2', index)
        # Match the common-origin state comparison, including native width changes.
        common = np.zeros((max(height, normal.shape[0]), max(width, normal.shape[1])), np.uint8)
        other = common.copy()
        common[:normal.shape[0], :normal.shape[1]] = normal
        other[:height, :width] = mask
        yy, xx = np.where(common != other)
        for x, y in zip(xx, yy):
            if x < width and y < height:
                alpha[y * 4:(y + 1) * 4, x * 4:(x + 1) * 4] = mask[y, x]
    allowed = np.unique(original[mask > 0])
    if name == 'netfont':
        if 153 <= index <= 162:
            regular, regular_mask = native(sources, name, index - 136)
            regular_allowed = np.unique(regular[regular_mask > 0])
            luma = pal[regular_allowed].astype(float) @ np.array([.2126, .7152, .0722])
            face = int(regular_allowed[luma.argmax()])
            core = (regular_mask > 0) & (regular == face)
            ink = repeat4(nearest_field(original, np.where(core, 255, 0).astype(np.uint8)))
        else:
            luma = pal[allowed].astype(float) @ np.array([.2126, .7152, .0722])
            face = int(allowed[luma.argmax()])
            core = (mask > 0) & (original == face)
            ink = np.full_like(alpha, face)
        yy, xx = np.where(core)
        cx, cy, ce, cf = int(xx.min()), int(yy.min()), int(xx.max() + 1), int(yy.max() + 1)
        sy, sx = np.where(shape_mask > 0)
        bx, by, ex, ey = int(sx.min()), int(sy.min()), int(sx.max() + 1), int(sy.max() + 1)
        require(0 <= bx < ex <= aw // 4 and 0 <= by < ey <= ah // 4,
                'Invalid generated ink registration')
        shape = Image.fromarray(base_alpha).crop((bx * 4, by * 4, ex * 4, ey * 4))
        body = np.zeros_like(alpha)
        body[cy * 4:cf * 4, cx * 4:ce * 4] = np.asarray(shape.resize(
            ((ce - cx) * 4, (cf - cy) * 4), Image.Resampling.LANCZOS))
        shadow = repeat4(np.where(core, 0, mask).astype(np.uint8))
        alpha = np.maximum(shadow, body)
        indices = repeat4(original)
        indices[body >= 64] = ink[body >= 64]
    else:
        field = nearest_field(pal[original], mask)
        rgb = np.asarray(Image.fromarray(field).resize((width * 4, height * 4), Image.Resampling.BICUBIC))
        indices = quantize(rgb, raw, allowed)
    return indices.tobytes(), np.where(alpha >= 64, 255, 0).astype(np.uint8).tobytes()


def reconstruct_material(bank, record, sources, load_object):
    preflight()
    index = record['index']
    original, mask = native(sources, 'frontbit', index)
    pal, raw = palette(sources[bank['palette']])
    indices = repeat4(original).copy()
    if record.get('material'):
        pw, ph, ox, oy, family = record['material_origin']
        require(0 < pw <= 256 and 0 < ph <= 256 and family in ('hugebutton', 'largebutton', 'panel'),
                'Invalid material geometry')
        payload = load_object(record['material'])
        require(len(payload) == pw * 4 * ph * 4 * 3 * 4, 'Material field size mismatch')
        field = np.frombuffer(payload, '<f4').reshape(ph * 4, pw * 4, 3)
        require(np.isfinite(field).all() and field.min() >= 0 and field.max() <= 255,
                'Invalid generated material')
        yy, xx = np.indices(indices.shape)
        material = field[(yy + oy * 4) % (ph * 4), (xx + ox * 4) % (pw * 4)].copy()
        refid = 3 if family == 'hugebutton' else 61 if family == 'largebutton' else 41
        ri, rm = native(sources, 'frontbit', refid)
        rgb = pal[ri].astype(float)
        brown = ((rm > 0) & (rgb[:, :, 0] < 100) & (rgb[:, :, 1] < 85) &
                 (rgb[:, :, 2] < 45) & (rgb[:, :, 0] > rgb[:, :, 1] * 1.05) &
                 (rgb[:, :, 1] > rgb[:, :, 2] * 1.15))
        desired, actual = np.median(rgb[brown], axis=0), np.median(field.reshape(-1, 3), axis=0)
        material = np.clip(material * np.divide(desired, actual, out=np.ones(3), where=actual > 0),
                           0, 255).astype(np.uint8)
        rgb = pal[original].astype(float)
        brown = ((mask > 0) & (rgb[:, :, 0] < 100) & (rgb[:, :, 1] < 85) &
                 (rgb[:, :, 2] < 45) & (rgb[:, :, 0] > rgb[:, :, 1] * 1.05) &
                 (rgb[:, :, 1] > rgb[:, :, 2] * 1.15))
        if family in ('hugebutton', 'largebutton'):
            brown[:4, :] = False
            brown[-4:, :] = False
            brown &= np.asarray(Image.fromarray(mask).filter(ImageFilter.MinFilter(7))) > 0
        else:
            if index in range(25, 31): brown[:4, :] = False
            if index in range(47, 53): brown[-4:, :] = False
            if index in (25, 33, 40, 47, 55, 71, 74, 80): brown[:, :7 if index == 74 else 4] = False
            if index in (30, 38, 45, 52, 60, 73, 76, 82): brown[:, -4:] = False
            if index in (*range(55, 61), *range(71, 74), *range(80, 83)):
                brown[:3, :] = False
                brown[-3:, :] = False
            if index in (74, 75, 76, 77): brown[:4, :] = False
            if index == 79: brown[-4:, :] = False
        changed = repeat4(brown)
        quantized = quantize(material, raw)
        indices[changed] = quantized[changed]
    return indices.tobytes(), repeat4(mask).tobytes()


def srgb_linear(c8):
    c = c8 / 255.0
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def cielab(c8):
    # Explicit products in a fixed order keep BLAS kernels out of the arithmetic.
    r = srgb_linear(np.asarray(c8, float))
    m = ((0.4124, 0.2126, 0.0193), (0.3576, 0.7152, 0.1192), (0.1805, 0.0722, 0.9505))
    xyz = np.stack([r[..., 0] * m[0][j] + r[..., 1] * m[1][j] + r[..., 2] * m[2][j] for j in range(3)], -1)
    xyz = xyz / np.array([0.95047, 1.0, 1.08883])
    f = np.where(xyz > 216 / 24389, np.cbrt(xyz), (24389 / 27 * xyz + 16) / 116)
    return np.stack([116 * f[..., 1] - 16, 500 * (f[..., 0] - f[..., 1]), 200 * (f[..., 1] - f[..., 2])], -1)


def catmull_rom4(x):
    """Separable Catmull-Rom 4x at physical pixel centres, edges clamped."""
    def weights(t, a=-0.5):
        t = np.abs(t)
        return np.where(t < 1, (a + 2) * t ** 3 - (a + 3) * t ** 2 + 1,
                        np.where(t < 2, a * t ** 3 - 5 * a * t ** 2 + 8 * a * t - 4 * a, 0))

    def axis(v, ax):
        n = v.shape[ax]
        src = (np.arange(n * 4) + 0.5) / 4 - 0.5
        base = np.floor(src).astype(int)
        acc = 0
        for k in range(-1, 3):
            shape = [1] * v.ndim
            shape[ax] = -1
            acc = acc + np.take(v, np.clip(base + k, 0, n - 1), axis=ax) * weights(src - (base + k)).reshape(shape)
        return acc
    return axis(axis(x, 0), 1)


def box4(x):
    h, w = x.shape[0] // 4, x.shape[1] // 4
    return x.reshape(h, 4, w, 4, *x.shape[2:]).mean((1, 3))


def gaussian(x, sigma):
    r = int(np.ceil(3 * sigma))
    k = np.exp(-0.5 * (np.arange(-r, r + 1) / sigma) ** 2)
    k /= k.sum()
    p = np.pad(x, r, mode='edge')
    p = sum(k[i] * p[i:i + x.shape[0], :] for i in range(2 * r + 1))
    return sum(k[i] * p[:, i:i + x.shape[1]] for i in range(2 * r + 1))


def steer4(v, a, luma, along=1.6, across=0.45, iso=0.6, radius=3):
    """Anisotropic kernel regression at 4x: each native sample spreads along its local edge
    (structure tensor of coverage and luma), not across it. v is premultiplied (h, w, c)."""
    h, w = a.shape
    ay, ax = np.gradient(a)
    ly, lx = np.gradient(luma)
    jxx = gaussian(ax * ax + 0.5 * lx * lx, 1.0)
    jyy = gaussian(ay * ay + 0.5 * ly * ly, 1.0)
    jxy = gaussian(ax * ay + 0.5 * lx * ly, 1.0)
    tr, det = jxx + jyy, jxx * jyy - jxy ** 2
    l1 = tr / 2 + np.sqrt(np.maximum(tr ** 2 / 4 - det, 0))
    l2 = tr - l1
    coh = np.where(tr > 1e-6, (l1 - l2) / np.maximum(l1 + l2, 1e-6), 0)
    theta = 0.5 * np.arctan2(2 * jxy, jxx - jyy)
    gx, gy = np.cos(theta), np.sin(theta)
    s_across, s_along = iso + (across - iso) * coh, iso + (along - iso) * coh
    oy, ox = np.meshgrid((np.arange(h * 4) + 0.5) / 4 - 0.5, (np.arange(w * 4) + 0.5) / 4 - 0.5, indexing='ij')
    num, den = np.zeros(oy.shape + (v.shape[2],)), np.zeros(oy.shape)
    by, bx = np.round(oy).astype(int), np.round(ox).astype(int)
    for dy in range(-radius, radius + 1):
        for dx in range(-radius, radius + 1):
            sy, sx = by + dy, bx + dx
            inside = (sy >= 0) & (sy < h) & (sx >= 0) & (sx < w)
            sy, sx = np.clip(sy, 0, h - 1), np.clip(sx, 0, w - 1)
            ry, rx = oy - sy, ox - sx
            u = rx * gx[sy, sx] + ry * gy[sy, sx]
            t = -rx * gy[sy, sx] + ry * gx[sy, sx]
            weight = np.exp(-0.5 * ((u / s_across[sy, sx]) ** 2 + (t / s_along[sy, sx]) ** 2)) * inside
            num += weight[..., None] * v[sy, sx]
            den += weight
    return num / np.maximum(den, 1e-9)[..., None]


def reconstruct_faithful(bank, record, sources, load_object):
    """4x upscale computed only from the native sprite and palette; nothing generated is stored.

    Edge-oriented kernel regression smooths the silhouette and colour, three smooth
    back-projection passes pull each 4x4 block's mean towards its native pixel, and each
    pixel takes the nearest palette entry in CIELAB (index 0 excluded).
    """
    preflight()
    require(record.get('method') == 'oriented-4x-v1' and bank['scale'] == 4, 'Unsupported faithful upscale')
    original, mask = native(sources, bank['name'], record['index'])
    pal = np.frombuffer(sources[bank['palette']], np.uint8).reshape(256, 3).astype(np.int16)
    a = (mask > 0).astype(float)
    target = srgb_linear(pal[original] * 4.0)
    col = target * a[..., None]
    luma = (col[..., 0] * 0.2126 + col[..., 1] * 0.7152 + col[..., 2] * 0.0722) ** (1 / 2.2)
    upscaled = steer4(np.dstack([col, a]), a, luma)
    coverage = upscaled[..., 3] >= 0.5
    colour = np.clip(upscaled[..., :3] / np.maximum(upscaled[..., 3], 1e-3)[..., None], 0, 1)
    covered = coverage[..., None].astype(float)
    for _ in range(3):
        blocks = box4(covered)
        mean = box4(colour * covered) / np.maximum(blocks, 1e-6)
        error = np.where((blocks > 0) & (a[..., None] > 0), target - mean, 0)
        colour = np.clip(colour + np.stack([catmull_rom4(error[..., c]) for c in range(3)], -1), 0, 1)
    encoded = np.where(colour <= 0.0031308, colour * 12.92, 1.055 * colour ** (1 / 2.4) - 0.055) * 255
    distance = ((cielab(encoded)[..., None, :] - cielab(pal[1:] * 4.0)[None, None]) ** 2).sum(-1)
    indices = np.where(coverage, distance.argmin(-1) + 1, 0).astype(np.uint8)
    return indices.tobytes(), np.where(coverage, 255, 0).astype(np.uint8).tobytes()


def reconstruct_native(entry, original, mask, load_object):
    """Apply only generated indexed colour; native frame masks/corners stay local."""
    preflight()
    width, height = entry['dimensions']
    payload = load_object(entry['generated_indices'])
    require(len(payload) == width * height, 'Generated image size mismatch')
    generated = np.frombuffer(payload, np.uint8).reshape(height, width)
    indices = np.frombuffer(original, np.uint8).reshape(height, width).copy()
    if entry['format'] == 'huge':
        alpha = load_object(entry['generated_alpha'])
        require(len(alpha) == width * height, 'Generated image alpha mismatch')
        alpha = np.frombuffer(alpha, np.uint8).reshape(height, width)
        coverage = np.frombuffer(mask, np.uint8).reshape(height, width)
        inner = np.asarray(Image.fromarray(coverage).filter(ImageFilter.MinFilter(5))) > 0
        use = inner & (alpha >= 64)
        indices[use] = generated[use]
    else:
        indices[:] = generated
        if entry.get('preserve_flame_corners'):
            bw, bh = (80, 100) if width == 640 else (40, 50)
            native = np.frombuffer(original, np.uint8).reshape(height, width)
            indices[:bh, :bw] = native[:bh, :bw]
            indices[:bh, width - bw:] = native[:bh, width - bw:]
    return indices.tobytes()
