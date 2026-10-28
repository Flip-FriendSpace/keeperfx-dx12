# SPDX-License-Identifier: GPL-2.0-or-later
"""Pure colour fitter extracted unchanged from the established 2026-10-19 pass.
Original match_sprite_colour.py SHA256:
26f5328ed4f910a1ff1e598970967b4c73ea2acd4b633cb55f526c3f50d90141.
"""
import numpy as np
from PIL import Image
M = np.array([[.4124564,.3575761,.1804375],[.2126729,.7151522,.0721750],[.0193339,.1191920,.9503041]])
MI = np.linalg.inv(M)
WHITE = np.array([.95047,1,1.08883])
GAIN_LIMITS, CAST_LIMITS, CHROMA_LIMITS = (.3,2.5),(.7,1.43),(.7,1.6)

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
