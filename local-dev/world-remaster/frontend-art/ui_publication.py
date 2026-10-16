# SPDX-License-Identifier: GPL-2.0-or-later
"""Reconstruct source-bound, lossless UI layers into a separate staging folder.

No game settings or installed files are modified. Core decoding uses the standard
library; image operators require the exact pinned Pillow and NumPy versions.
Native masks, metrics, fallback pixels and palettes come from the hash-verified
game installation, never from the published layer objects.
"""
from pathlib import Path, PurePosixPath
import argparse
import hashlib
import importlib.util
import json
import re
import struct
import zlib

LIMIT = 64 * 1024 * 1024
MANIFEST_LIMIT = 4 * 1024 * 1024
HEADER = struct.Struct('<4sHHI')
RECORD = struct.Struct('<IHHHHBBHQII')


def require(condition, message):
    if not condition:
        raise ValueError(message)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def safe_path(root, relative):
    require(isinstance(relative, str) and '\\' not in relative and ':' not in relative,
            'Invalid relative path')
    parts = PurePosixPath(relative)
    require(not parts.is_absolute() and parts.parts and
            all(p not in ('.', '..') for p in parts.parts), 'Unsafe relative path')
    target = (Path(root) / relative).resolve()
    require(target.is_relative_to(Path(root).resolve()), 'Path leaves its root')
    return target


def read_limited(path, maximum=LIMIT):
    with Path(path).open('rb') as stream:
        data = stream.read(maximum + 1)
    require(len(data) <= maximum, 'File exceeds size limit: ' + str(path))
    return data


def fnv64(data):
    value = 14695981039346656037
    for byte in data:
        value = ((value ^ byte) * 1099511628211) & 0xffffffffffffffff
    return value


def rnc_crc(data):
    value = 0
    for byte in data:
        value ^= byte
        for _ in range(8):
            value = (value >> 1) ^ (0xa001 if value & 1 else 0)
    return value


def unpack_source(data):
    """Bounded RNC method 1, based on src/bflib_dernc.c (Jon Skeet/Tomasz Lis)."""
    if data[:3] != b'RNC':
        return data
    require(len(data) >= 18 and data[3] == 1, 'Unsupported RNC header')
    size, packed, output_crc, packed_crc = struct.unpack_from('>IIHH', data, 4)
    require(0 < size <= LIMIT and 0 < packed <= LIMIT and len(data) == packed + 18,
            'Invalid RNC lengths')
    payload = data[18:]
    require(rnc_crc(payload) == packed_crc, 'RNC packed CRC mismatch')
    position, bits, count = 0, int.from_bytes(payload[:2], 'little'), 16

    def word(at):
        require(at <= len(payload), 'RNC input overrun')
        return int.from_bytes(payload[at:at + 2].ljust(2, b'\0'), 'little')

    def read(n):
        nonlocal position, bits, count
        require(0 <= n <= 31, 'Invalid RNC bit length')
        value = bits & ((1 << n) - 1)
        bits >>= n
        count -= n
        if count < 16:
            position += 2
            bits |= word(position) << count
            count += 16
        return value

    def table():
        lengths = [read(4) for _ in range(read(5))]
        result, code = [], 0
        for length in range(1, max(lengths, default=0) + 1):
            for value, item in enumerate(lengths):
                if item == length:
                    reverse = int(format(code, '0%db' % length)[::-1], 2)
                    require(code < 1 << length, 'Oversubscribed RNC Huffman table')
                    result.append((length, reverse, value))
                    code += 1
            code <<= 1
        return result

    def huffman(entries):
        for length, code, value in entries:
            if bits & ((1 << length) - 1) == code:
                read(length)
                return value if value < 2 else (1 << (value - 1)) | read(value - 1)
        raise ValueError('Invalid RNC Huffman code')

    read(2)
    output = bytearray()
    while len(output) < size:
        require(position + 6 <= len(payload), 'Truncated RNC chunk')
        raw, distance, length = table(), table(), table()
        chunks = read(16)
        require(chunks > 0, 'Empty RNC chunk')
        for chunk in range(chunks):
            literal = huffman(raw)
            require(position + literal <= len(payload) and len(output) + literal <= size,
                    'RNC literal overrun')
            if literal:
                output.extend(payload[position:position + literal])
                position += literal
                count -= 16
                require(0 <= count <= 15, 'Invalid RNC bit state')
                bits = (bits & ((1 << count) - 1)) | (word(position) << count)
                count += 16
            if chunk + 1 < chunks:
                offset, copies = huffman(distance) + 1, huffman(length) + 2
                require(offset <= len(output) and len(output) + copies <= size,
                        'RNC back-reference overrun')
                for _ in range(copies):
                    output.append(output[-offset])
    require(len(output) == size and rnc_crc(output) == output_crc, 'RNC output mismatch')
    return bytes(output)


def dimensions(width, height):
    require(0 < width <= 1024 and 0 < height <= 1024, 'Invalid dimensions')
    return width * height


def decode_sprite(data, width, height):
    indices, mask = bytearray(dimensions(width, height)), bytearray(width * height)
    at = 0
    for y in range(height):
        x = 0
        while True:
            require(at < len(data), 'Truncated sprite')
            command = data[at]
            at += 1
            if not command:
                break
            count = command if command < 128 else 256 - command
            require(x + count <= width, 'Sprite row overrun')
            if command < 128:
                require(at + count <= len(data), 'Truncated sprite literals')
                start = y * width + x
                indices[start:start + count] = data[at:at + count]
                mask[start:start + count] = b'\xff' * count
                at += count
            x += count
    return indices, mask, at


def encode_sprite(indices, mask, width, height):
    require(len(indices) == len(mask) == dimensions(width, height), 'Sprite buffer mismatch')
    output = bytearray()
    for y in range(height):
        x, start = 0, y * width
        while x < width:
            opaque, end = mask[start + x] >= 64, x + 1
            while end < width and end - x < 127 and (mask[start + end] >= 64) == opaque:
                end += 1
            output.append(end - x if opaque else 256 - (end - x))
            if opaque:
                output.extend(indices[start + x:start + end])
            x = end
        output.append(0)
    return bytes(output)


def decode_huge(data, width, height):
    indices, mask = bytearray(dimensions(width, height)), bytearray(width * height)
    require(len(data) >= height * 4, 'Truncated huge-image table')
    for y in range(height):
        at, x = height * 4 + struct.unpack_from('<I', data, y * 4)[0], 0
        while x < width:
            require(at + 4 <= len(data), 'Truncated huge-image run')
            count = struct.unpack_from('<I', data, at)[0]
            at += 4
            require(count <= width - x and at + count + 4 <= len(data), 'Huge-image overrun')
            start = y * width + x
            indices[start:start + count] = data[at:at + count]
            mask[start:start + count] = b'\xff' * count
            at += count
            skip = struct.unpack_from('<I', data, at)[0]
            at += 4
            require(skip <= width - x - count and count + skip > 0, 'Invalid huge-image skip')
            x += count + skip
    return indices, mask


def encode_huge(indices, mask, width, height):
    offsets, output = [], bytearray()
    for y in range(height):
        offsets.append(len(output))
        x, row = 0, y * width
        while x < width:
            start = x
            while x < width and mask[row + x]:
                x += 1
            output.extend(struct.pack('<I', x - start))
            output.extend(indices[row + start:row + x])
            start = x
            while x < width and not mask[row + x]:
                x += 1
            output.extend(struct.pack('<I', x - start))
    return struct.pack('<' + 'I' * height, *offsets) + output


def enlarge(data, width, height, scale):
    require(scale in (1, 2, 4, 8), 'Invalid scale')
    dimensions(width * scale, height * scale)
    result = bytearray()
    for y in range(height):
        row = b''.join(bytes([v]) * scale for v in data[y * width:(y + 1) * width])
        result.extend(row * scale)
    return result


def palette_remap(palette, mode):
    """Reconstruct retained source RGB's exact quantizer result, not a stored palette."""
    require(len(palette) == 768 and max(palette) <= 64, 'Invalid source palette')
    colours = [tuple(palette[i:i + 3]) for i in range(0, 768, 3)]
    require(mode in ('renderer8', 'legacy4'), 'Unknown native colour reconstruction')
    result = []
    for colour in colours:
        rgb = tuple(((v * 255 // 63) % 256 if mode == 'renderer8' else min(v * 4, 255)) // 4
                    for v in colour)
        scores = []
        for index, candidate in enumerate(colours):
            differences = [a - b for a, b in zip(rgb, candidate)]
            squared = sum(v * v for v in differences)
            linear = sum(abs(v) for v in differences)
            dark = candidate[2] ** 2 + 2 * (candidate[0] ** 2 + candidate[1] ** 2)
            scores.append((0, index) if squared == 0 else
                          (1, (squared * 193 + linear) * 20481 + dark, index))
        result.append(min(range(256), key=scores.__getitem__))
    return bytes(result)


def load_object(root, objects, key):
    require(re.fullmatch('[0-9a-f]{64}', key or '') is not None and key in objects,
            'Unknown layer object')
    entry = objects[key]
    require(0 < entry['bytes'] <= LIMIT and 0 < entry['compressed_bytes'] <= LIMIT,
            'Invalid object sizes')
    packed = read_limited(safe_path(root, 'objects/' + key + '.zlib'), entry['compressed_bytes'])
    require(len(packed) == entry['compressed_bytes'] and digest(packed) == entry['compressed_sha256'],
            'Compressed object mismatch')
    decoder = zlib.decompressobj()
    data = decoder.decompress(packed, entry['bytes'] + 1)
    require(len(data) == entry['bytes'] and decoder.eof and not decoder.unused_data and
            not decoder.unconsumed_tail and digest(data) == key, 'Invalid compressed layer')
    return data


def pixel_digest(indices, mask):
    # Canonical indexed pixels followed by binary coverage; transparent indices zero.
    return digest(bytes(v if mask[i] else 0 for i, v in enumerate(indices)) + bytes(mask))


def unpack_ui_records(data):
    require(len(data) >= 12 and len(data) <= LIMIT, 'Invalid reconstructed UI pack size')
    magic, version, size, count = HEADER.unpack_from(data)
    require((magic, version, size) == (b'KFUI', 1, 32) and count <= 4096, 'Invalid UI pack header')
    at, records = 12, {}
    for _ in range(count):
        require(at + 32 <= len(data), 'Truncated UI record')
        record = RECORD.unpack_from(data, at)
        index, width, height, pw, ph, scale, flags, reserved, fingerprint, length, reserved2 = record
        require(index not in records and scale in (1, 2, 4, 8) and
                (pw, ph) == (width * scale, height * scale) and flags == reserved == reserved2 == 0,
                'Invalid UI record fields')
        at += 32
        require(0 < length <= len(data) - at, 'Invalid UI RLE length')
        indices, mask, extent = decode_sprite(memoryview(data)[at:at + length], pw, ph)
        require(extent == length, 'UI RLE trailing bytes')
        records[index] = (width, height, scale, fingerprint, indices, mask)
        at += length
    require(at == len(data), 'UI pack trailing bytes')
    return records


def owner_module(filename, directory):
    path = Path(__file__).resolve().parent.parent / directory / (filename + '.py')
    spec = importlib.util.spec_from_file_location(filename, path)
    require(spec is not None and spec.loader is not None, 'Missing UI reconstruction module')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def build_ui_assets(asset_root, game_root, output_root, *, source_decoder=None, verify_only=False):
    """Validate all inputs, reconstruct all outputs, then write a fresh staging tree.

    source_decoder, when supplied, takes compressed source bytes and returns
    decoded bytes. The built-in bounded RNC1 decoder is otherwise used.
    Receipt: schema/selection/manifest_sha256/files[{path,bytes,sha256,pixel_sha256}].
    verify_only performs the same complete reconstruction without writing output.
    """
    asset_root, game_root, output_root = map(lambda p: Path(p).resolve(),
                                            (asset_root, game_root, output_root))
    for root in (asset_root, game_root):
        require(not output_root.is_relative_to(root) and not root.is_relative_to(output_root),
                'Output must be separate from assets and game data')
    raw_manifest = read_limited(asset_root / 'manifest.json', MANIFEST_LIMIT)
    manifest = json.loads(raw_manifest)
    require(manifest['schema'] == 1 and len(manifest['sources']) <= 100 and
            len(manifest['banks']) <= 32 and len(manifest['native']) <= 64 and
            len(manifest['objects']) <= 10000, 'Unsupported manifest or limits')
    # Mandatory first pass: do not attempt reconstruction from missing/modified originals.
    sources = {}
    for relative, entry in manifest['sources'].items():
        data = read_limited(safe_path(game_root, relative))
        require(len(data) == entry['bytes'] and digest(data) == entry['sha256'],
                'Native source mismatch: ' + relative)
        sources[relative] = data
    decode = source_decoder or unpack_source
    decoded = {}

    def source(relative):
        require(relative in sources, 'Unbound native dependency')
        if relative not in decoded:
            value = decode(sources[relative])
            entry = manifest['sources'][relative]
            require(len(value) <= LIMIT and len(value) == entry['decoded_bytes'] and
                    digest(value) == entry['decoded_sha256'], 'Decoded native source mismatch: ' + relative)
            decoded[relative] = value
        return decoded[relative]

    pending, files = [], []
    def finish(entry, data, pixels):
        relative = entry['output']
        safe_path(output_root, relative)
        require(relative not in {f['path'] for f in files}, 'Duplicate output path')
        require(len(data) <= LIMIT and len(data) == entry['bytes'] and digest(data) == entry['sha256'],
                'Output hash mismatch: ' + relative)
        require(sum(len(value) for _, value in pending) + len(data) <= 128 * 1024 * 1024,
                'Total output limit exceeded')
        pending.append((relative, data))
        files.append({'path': relative, 'bytes': len(data), 'sha256': digest(data), 'pixel_sha256': pixels})

    objects = manifest['objects']
    from frontend_reconstruct import preflight, reconstruct_font, reconstruct_material, reconstruct_native
    preflight()
    object_loader = lambda key: load_object(asset_root, objects, key)
    for bank in manifest['banks']:
        native, table = source(bank['data']), source(bank['table'])
        require(len(table) % 6 == 0 and len(bank['records']) <= 4096, 'Invalid native sprite table')
        # Owner modules receive only verified decoded game bytes and bounded generated objects.
        owner_records = {}
        if bank.get('operator') == 'hud':
            reconstruct_hud_bank = owner_module('hud_reconstruct', 'hud-interface').reconstruct_hud_bank
            inputs = {Path(relative).name: source(relative) for relative in
                      (bank['data'], bank['table'], bank['palette'])}
            recipe = json.loads(object_loader(bank['recipe']))
            owner_records = unpack_ui_records(reconstruct_hud_bank(recipe, inputs, object_loader))
        elif bank.get('operator') in ('hand', 'mixed'):
            reconstruct_hand_bank = owner_module('hand_ui_publication', 'hand-cursors').reconstruct_hand_bank
            inputs = {relative: source(relative) for relative in manifest['sources']}
            recipe = json.loads(object_loader(bank['hand_recipe']))
            owner_records = unpack_ui_records(reconstruct_hand_bank(recipe, inputs, object_loader))
        output = bytearray(HEADER.pack(b'KFUI', 1, 32, len(bank['records'])))
        pixel_hashes, previous = [], -1
        for record in bank['records']:
            index = record['index']
            require(previous < index < len(table) // 6, 'Invalid or unsorted sprite index')
            previous = index
            offset, width, height = struct.unpack_from('<IBB', table, index * 6)
            require(offset < len(native), 'Native sprite offset overrun')
            indices, mask, extent = decode_sprite(memoryview(native)[offset:], width, height)
            scale = bank['scale']
            fingerprint = fnv64(memoryview(native)[offset:offset + extent])
            if index in owner_records:
                ow, oh, os, of, indices, mask = owner_records.pop(index)
                require((ow, oh, os, of) == (width, height, scale, fingerprint),
                        'Owner reconstruction changed native metrics/fingerprint')
            elif record.get('operator') in ('font', 'material'):
                inputs = {relative: source(relative) for relative in manifest['sources']}
                operation = reconstruct_font if record['operator'] == 'font' else reconstruct_material
                indices, mask = operation(bank, record, inputs, object_loader)
            else:
                raise ValueError('Unimplemented UI record operation')
            pixels = pixel_digest(indices, mask)
            require(pixels == record['pixel_sha256'], 'Reconstructed sprite pixel mismatch')
            pixel_hashes.append(bytes.fromhex(pixels))
            data = encode_sprite(indices, mask, width * scale, height * scale)
            output.extend(RECORD.pack(index, width, height, width * scale, height * scale,
                                      scale, 0, 0, fingerprint, len(data), 0))
            output.extend(data)
        require(not owner_records, 'Unexpected owner UI records')
        finish(bank, bytes(output), digest(b''.join(pixel_hashes)))
    for entry in manifest['native']:
        width, height = entry['dimensions']
        native = source(entry['source'])
        if entry['format'] == 'huge':
            indices, mask = decode_huge(native, width, height)
        else:
            require(entry['format'] == 'raw' and len(native) == dimensions(width, height),
                    'Invalid raw native source')
            indices, mask = bytearray(native), bytearray(b'\xff' * len(native))
        if entry.get('native_colour'):
            remap = palette_remap(sources[entry['palette']], entry['native_colour'])
            indices = bytearray(indices.translate(remap))
        indices = reconstruct_native(entry, indices, mask, object_loader)
        pixels = pixel_digest(indices, mask)
        require(pixels == entry['pixel_sha256'], 'Reconstructed native-image pixel mismatch')
        data = encode_huge(indices, mask, width, height) if entry['format'] == 'huge' else bytes(indices)
        finish(entry, data, pixels)
    if not verify_only:
        require(not output_root.exists() or not any(output_root.iterdir()), 'Staging output must be empty')
        output_root.mkdir(parents=True, exist_ok=True)
        for relative, data in pending:
            path = safe_path(output_root, relative)
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
    return {'schema': 1, 'selection': manifest['selection'], 'manifest_sha256': digest(raw_manifest),
            'verify_only': verify_only, 'files': files}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--assets', type=Path, required=True)
    parser.add_argument('--game-root', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--verify-only', action='store_true')
    args = parser.parse_args()
    receipt = build_ui_assets(args.assets, args.game_root, args.output, verify_only=args.verify_only)
    print(json.dumps(receipt, indent=2) + '\n')


if __name__ == '__main__':
    main()
