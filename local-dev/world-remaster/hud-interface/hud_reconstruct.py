# SPDX-License-Identifier: GPL-2.0-or-later
"""Reconstruct source-bound HUD sidecars from generated RGBA and installed banks.
No filesystem access, runtime changes or network access. load_object(sha256) must
return decompressed raw RGBA. Caller supplies verified installed file bytes; this
module checks their fingerprints again. Only exact pinned image arithmetic is
supported. See HUD-PUBLICATION.md for provenance and operation definitions.
"""
import hashlib
import struct


def _require(condition, message):
    if not condition:
        raise ValueError(message)


def _sha(data):
    return hashlib.sha256(data).hexdigest()


def _fnv(data):
    value = 14695981039346656037
    for byte in data:
        value = ((value ^ byte) * 1099511628211) & 0xffffffffffffffff
    return value


def _rle(indices, coverage, width, height):
    result = bytearray()
    for y in range(height):
        x = 0
        while x < width:
            opaque = coverage[y * width + x] >= 64
            end = x + 1
            while end < width and end - x < 127 and (coverage[y * width + end] >= 64) == opaque:
                end += 1
            result.append(end - x if opaque else 256 - (end - x))
            if opaque:
                result.extend(indices[y * width + x:y * width + end])
            x = end
        result.append(0)
    return bytes(result)


def reconstruct_hud_bank(bank_manifest, verified_sources, load_object):
    """Return exact KFUI v1 bytes. All native pixels stay in installed inputs.

    bank_manifest: HUD bank JSON from generated-input-staging/hud/banks.
    verified_sources: dict of basename -> bytes for palette.dat, BANK.dat, BANK.tab.
    load_object: callable SHA256 -> decompressed generated raw RGBA bytes.
    Raises ValueError before returning a pack if any dependency/result differs.
    """
    import PIL
    import numpy as np
    from PIL import Image, ImageFilter
    _require(PIL.__version__ == '12.3.0' and np.__version__ == '2.3.5',
             'HUD reconstruction requires Pillow==12.3.0 and numpy==2.3.5')
    m = bank_manifest
    _require(m.get('schema') == 'keeperfx-hud-generated-ops-v1', 'Unsupported HUD manifest')
    bank = m['bank']
    _require(bank in ('gui1-64', 'gui2-64', 'font1-64', 'font2-64'), 'Unsupported HUD bank')
    for filename, metadata in m['source_files'].items():
        data = verified_sources[filename]
        _require(isinstance(data, bytes) and _sha(data) == metadata['sha256'], 'Native source mismatch: ' + filename)
        _require(len(data) == metadata['bytes'], 'Native source length mismatch: ' + filename)
    dat = verified_sources[bank + '.dat']
    tab = verified_sources[bank + '.tab']
    palette = verified_sources['palette.dat']
    _require(not dat.startswith(b'RNC') and len(tab) % 6 == 0 and len(palette) == 768,
             'Expected canonical uncompressed DAT/TAB and RGB6 palette')
    pal = np.frombuffer(palette, np.uint8).reshape(256, 3).astype(np.int16)
    rgbpal = (pal * 4).clip(0, 255).astype(np.uint8)
    records = {r['index']: r for r in m['records']}
    _require(len(records) == len(m['records']), 'Duplicate HUD record')
    native_cache, image_cache, active = {}, {}, set()

    def native(index):
        if index in native_cache:
            return native_cache[index]
        r = records[index]
        _require(0 <= index < len(tab) // 6, 'Native index outside TAB')
        offset, width, height = struct.unpack_from('<IBB', tab, index * 6)
        _require([width, height] == r['logical_dimensions'], 'Native metrics mismatch')
        pixels = np.zeros((height, width, 4), np.uint8)
        pos = offset
        for y in range(height):
            x = 0
            while True:
                _require(pos < len(dat), 'Truncated native RLE')
                run = struct.unpack_from('b', dat, pos)[0]
                pos += 1
                if run == 0:
                    break
                if run < 0:
                    x -= run
                else:
                    _require(x + run <= width and pos + run <= len(dat), 'Native RLE bounds')
                    values = np.frombuffer(dat[pos:pos + run], np.uint8)
                    pixels[y, x:x + run, :3] = rgbpal[values]
                    pixels[y, x:x + run, 3] = 255
                    pos += run
                    x += run
                _require(x <= width, 'Native RLE row overflow')
        raw = dat[offset:pos]
        _require(_sha(raw) == r['source_rle_sha256'] and f'{_fnv(raw):016x}' == r['source_fnv64'],
                 'Native sprite fingerprint mismatch')
        image = Image.fromarray(pixels)
        native_cache[index] = image
        return image

    def lum(array):
        return 0.2126 * array[:, :, 0] + 0.7152 * array[:, :, 1] + 0.0722 * array[:, :, 2]

    def render(index):
        if index in image_cache:
            return image_cache[index]
        _require(index not in active, 'Cyclic HUD operation graph')
        active.add(index)
        r = records[index]
        source = native(index)
        size = (source.width * 4, source.height * 4)
        operation = r['operation']
        if operation in ('exact_native_identity_alias', 'native_spatial_colour_transfer'):
            parent = r['parent_index']
            parent_image = render(parent).resize(size, Image.Resampling.LANCZOS)
            if operation == 'exact_native_identity_alias':
                result = parent_image.copy()
            else:
                dtype = np.float32 if r['arithmetic'] == 'float32' else float
                base = np.array(native(parent).resize(size, Image.Resampling.NEAREST)).astype(dtype)
                target = np.array(source.resize(size, Image.Resampling.NEAREST)).astype(dtype)
                array = np.array(parent_image).astype(dtype)
                gain = np.clip(1 + (lum(array) - lum(base)) / (lum(base) + 32), .6, 1.45)
                array[:, :, :3] = np.clip(target[:, :, :3] * gain[:, :, None], 0, 255)
                array[:, :, 3] = target[:, :, 3]
                result = Image.fromarray(array.astype(np.uint8))
        else:
            _require(operation in ('generated_overlay_native_composite',
                'generated_overlay_native_composite_font_contour',
                'generated_overlay_native_composite_native_tonefield'), 'Unknown HUD operation')
            obj = r['generated_object']
            raw = load_object(obj['sha256'])
            _require(isinstance(raw, bytes) and len(raw) == size[0] * size[1] * 4
                     and _sha(raw) == obj['sha256'], 'Generated RGBA object mismatch')
            layer = Image.frombytes('RGBA', size, raw)
            base_image = source.resize(size, Image.Resampling.NEAREST)
            original = np.array(base_image)
            array = np.array(Image.alpha_composite(base_image, layer))
            array[:, :, 3] = original[:, :, 3]
            if operation.endswith('_font_contour'):
                mask = base_image.getchannel('A')
                inner = np.array(mask.filter(ImageFilter.MinFilter(3))) > 0
                outer = np.array(mask.filter(ImageFilter.MaxFilter(3))) > 0
                generated = np.array(layer)[:, :, 3] >= 128
                coverage = inner | (generated & outer)
                bounds = np.zeros_like(coverage)
                bbox = source.getbbox()
                _require(bbox is not None, 'Empty native glyph')
                bounds[bbox[1] * 4:bbox[3] * 4, bbox[0] * 4:bbox[2] * 4] = True
                coverage &= bounds
                array[:, :, 3] = coverage.astype(np.uint8) * 255
            elif operation.endswith('_native_tonefield'):
                base = original.astype(float)
                gain = np.clip(1 + (lum(array.astype(float)) - lum(base)) / (lum(base) + 32), .65, 1.4)
                array[:, :, :3] = np.clip(base[:, :, :3] * gain[:, :, None], 0, 255).astype(np.uint8)
            seam = r.get('seam_texture_pixels', 0)
            if seam:
                array[:seam] = original[:seam]
                array[-seam:] = original[-seam:]
                array[:, :seam] = original[:, :seam]
                array[:, -seam:] = original[:, -seam:]
            result = Image.fromarray(array)
        image_cache[index] = result
        active.remove(index)
        return result

    blobs = []
    for index, r in sorted(records.items()):
        im = render(index)
        array = np.array(im)
        unique, inverse = np.unique((array[:, :, :3] // 4).astype(np.int16).reshape(-1, 3), axis=0, return_inverse=True)
        lookup = np.empty(len(unique), np.uint8)
        for start in range(0, len(unique), 2048):
            lookup[start:start + 2048] = np.abs(unique[start:start + 2048, None, :] - pal[None, :, :]).sum(2).argmin(1)
        lookup[lookup == 0] = 1
        indices = lookup[inverse].reshape(array.shape[:2])
        coverage = array[:, :, 3]
        if bank == 'font1-64':
            indices[coverage > 0] = 1
        encoded = _rle(indices.tobytes(), coverage.tobytes(), *im.size)
        _require(_sha(encoded) == r['expected_rle_sha256'] and len(encoded) == r['expected_rle_bytes'],
                 'Reconstructed HUD RLE mismatch: ' + str(index))
        width, height = r['logical_dimensions']
        blobs.append(struct.pack('<IHHHHBBHQII', index, width, height, im.width, im.height, 4, 0, 0,
                                 int(r['source_fnv64'], 16), len(encoded), 0) + encoded)
    result = struct.pack('<4sHHI', b'KFUI', 1, 32, len(blobs)) + b''.join(blobs)
    _require(len(result) == m['expected_pack']['bytes'] and _sha(result) == m['expected_pack']['sha256'],
             'Reconstructed HUD bank mismatch')
    return result
