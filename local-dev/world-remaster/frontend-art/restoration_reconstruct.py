# SPDX-License-Identifier: GPL-2.0-or-later
"""Source-bound portrait, campaign and control additions to the reviewed UI.

Stored fields contain only generated RGB or generated luminance relief. Original
colour fields, palette indices, glyphs, silhouettes and control states stay local.
"""
import re
import struct
import numpy as np
from PIL import Image, ImageFilter
from frontend_reconstruct import palette, preflight, quantize, repeat4
from ui_publication import decode_sprite, dimensions, require


def native(bank, record, sources):
    require(bank['scale'] == 4, 'Restoration sprites require scale 4')
    data, table = sources[bank['data']], sources[bank['table']]
    index = record['index']
    require(len(table) % 6 == 0 and 0 <= index < len(table) // 6, 'Invalid restoration sprite index')
    at, width, height = struct.unpack_from('<IBB', table, index * 6)
    dimensions(width * 4, height * 4)
    require(at < len(data), 'Invalid restoration sprite offset')
    indices, mask, _ = decode_sprite(memoryview(data)[at:], width, height)
    return (np.frombuffer(indices, np.uint8).reshape(height, width),
            np.frombuffer(mask, np.uint8).reshape(height, width))


def source_palette(raw):
    require(len(raw) == 768 and max(raw) <= 64, 'Invalid restoration palette')
    return palette(raw)


def rgb_field(payload, width, height):
    require(len(payload) == width * height * 3, 'Generated RGB field size mismatch')
    return Image.fromarray(np.frombuffer(payload, np.uint8).reshape(height, width, 3))


def erode(mask, radius):
    return np.asarray(Image.fromarray(mask.astype(np.uint8) * 255).filter(
        ImageFilter.MinFilter(radius * 2 + 1))) > 0


def reconstruct_portrait(bank, record, sources, load_object):
    preflight()
    require(bank['name'] == 'gui1-64' and bank['data'] == 'data/gui1-64.dat' and
            bank['table'] == 'data/gui1-64.tab' and bank['palette'] == 'data/palette.dat' and
            record['index'] in (*range(145, 161), 173) and
            record.get('method') == 'portrait-local-colour-v1', 'Unsupported portrait recipe')
    original, mask = native(bank, record, sources)
    height, width = original.shape
    pal, raw = source_palette(sources[bank['palette']])
    generated = rgb_field(load_object(record['generated_rgb']), width * 4, height * 4)
    # Keep the accepted float64 arithmetic and intermediate Pillow byte rounding.
    source = Image.fromarray(pal[original]).filter(ImageFilter.GaussianBlur(.35))
    denominator = generated.resize((width, height), Image.Resampling.BOX).filter(
        ImageFilter.GaussianBlur(.35))
    source = np.asarray(source.resize(generated.size, Image.Resampling.BICUBIC), float)
    denominator = np.asarray(denominator.resize(generated.size, Image.Resampling.BICUBIC), float)
    rgb = np.rint(np.clip(np.asarray(generated, float) * np.clip(
        (source + 2) / (denominator + 2), .08, 4), 0, 255)).astype(np.uint8)
    black = repeat4(original == 0)
    rgb[black] = 0
    indices, alpha = quantize(rgb, raw), repeat4(mask)
    indices[black | (alpha == 0)] = 0
    return indices.tobytes(), alpha.tobytes()


def reconstruct_marker(bank, record, sources, load_object):
    preflight()
    name = bank['name']
    require(name in ('lndflag_ens', 'lndflag_pin') and bank['data'] == 'ldata/' + name + '.dat' and
            bank['table'] == 'ldata/' + name + '.tab' and 2 <= record['index'] <= 45 and
            record.get('method') == 'marker-wood-relief-v1', 'Unsupported marker recipe')
    # These semantic indices must have the same meaning in every campaign state.
    palettes = [source_palette(sources['campgns/keeporig_lnd/rgmap%02d.pal' % state])[0]
                for state in range(22)]
    require(all(np.array_equal(pal[[1, 251, 252, 253]], palettes[0][[1, 251, 252, 253]])
                for pal in palettes), 'Marker semantic palette indices differ')
    original, mask = native(bank, record, sources)
    indices, alpha = repeat4(original).copy(), repeat4(mask)
    payload = load_object(record['generated_relief'])
    require(len(payload) == indices.size, 'Generated marker relief size mismatch')
    relief = np.frombuffer(payload, np.uint8).reshape(indices.shape)
    require(relief.max() <= 2, 'Invalid generated marker relief')
    mutable = erode(((indices == 252) | (indices == 253)) & (alpha > 0), 1)
    mutable[-16:] = False
    low, high = indices == 252, indices == 253
    indices[mutable & low & (relief == 1)] = 253
    indices[mutable & high & (relief == 2)] = 252
    indices[alpha == 0] = 0
    return indices.tobytes(), alpha.tobytes()


def reconstruct_controls(bank, record, sources, load_object):
    preflight()
    index = record['index']
    require(bank['name'] == 'frontbit' and bank['data'] == 'ldata/frontbit.dat' and
            bank['table'] == 'ldata/frontbit.tab' and bank['palette'] == 'ldata/front.pal' and
            83 <= index <= 95 and record.get('method') == 'control-relief-v1',
            'Unsupported control recipe')
    original, mask = native(bank, record, sources)
    pal, raw = source_palette(sources[bank['palette']])
    baseline, alpha = repeat4(original), repeat4(mask)
    payload = load_object(record['generated_relief'])
    require(len(payload) == baseline.size * 2, 'Generated control relief size mismatch')
    relief = np.frombuffer(payload, '<i2').reshape(baseline.shape)
    require(relief.min() >= -255 and relief.max() <= 255, 'Invalid generated control relief')
    rgb = pal[baseline]
    shade = np.clip(rgb.astype(float) + np.clip(relief.astype(float) * .32, -12, 12)[..., None],
                    0, 255).astype(np.uint8)
    allowed = np.unique(original[mask > 0])
    require(len(allowed) > 0, 'Empty control source')
    candidate = quantize(shade, raw, allowed)
    mutable = erode(alpha > 0, 4)
    mutable[:4] = mutable[-4:] = False
    mutable[:, :4] = mutable[:, -4:] = False
    mutable[rgb.max(2) <= 12] = False
    red = (rgb[..., 0] > 45) & (rgb[..., 0] > rgb[..., 1] * 1.7)
    pale = (rgb[..., 2] > 32) & (rgb[..., 2] > rgb[..., 0] * .65)
    mutable[red | pale] = False
    if index in (89, 90, 95):
        mutable[:96] = False
    if index in (92, 93, 94):
        mutable[:, :8] = mutable[:, -8:] = False
    candidate[~mutable] = baseline[~mutable]
    candidate[alpha == 0] = 0
    return candidate.tobytes(), alpha.tobytes()


def landview_dimensions(entry):
    """Only the 22 native landviews may exceed the sprite/legacy image limit."""
    source = entry['source']
    require(re.fullmatch(r'campgns/keeporig_lnd/rgmap(?:0[0-9]|1[0-9]|2[01])\.raw', source) is not None and
            entry['output'] == source and entry['palette'] == source[:-4] + '.pal' and
            entry['dimensions'] == [1280, 960] and entry['format'] == 'raw' and
            entry.get('operator') == 'landview' and entry.get('method') == 'landview-local-colour-v1' and
            not entry.get('native_colour'), 'Unsupported native landview recipe')
    return 1280 * 960


def reconstruct_landview(entry, original, palette_bytes, load_object):
    preflight()
    size = landview_dimensions(entry)
    require(len(original) == size, 'Native landview size mismatch')
    pal, _ = source_palette(palette_bytes)
    generated = rgb_field(load_object(entry['generated_rgb']), 1280, 960)
    original = np.frombuffer(original, np.uint8).reshape(960, 1280)
    source = np.asarray(Image.fromarray(pal[original]).filter(ImageFilter.GaussianBlur(1.25)), float)
    denominator = np.asarray(generated.filter(ImageFilter.GaussianBlur(1.25)), float)
    rgb = np.rint(np.clip(np.asarray(generated, float) * np.clip(
        (source + 2) / (denominator + 2), .02, 16), 0, 255)).astype(np.uint8)
    fixed_palette = Image.new('P', (1, 1))
    fixed_palette.putpalette(pal.reshape(-1).tolist())
    return Image.fromarray(rgb).quantize(palette=fixed_palette, dither=Image.Dither.FLOYDSTEINBERG).tobytes()
