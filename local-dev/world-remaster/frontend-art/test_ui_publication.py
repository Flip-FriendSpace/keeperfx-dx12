# SPDX-License-Identifier: GPL-2.0-or-later
"""Synthetic failure/bounds checks; no original game data is included."""
from pathlib import Path
import json
import struct
import tempfile
import unittest
import zlib
import ui_publication as ui


class PublicationTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.assets, self.game, self.output = (self.root / name for name in ('assets', 'game', 'output'))
        self.assets.mkdir()
        self.game.mkdir()

    def manifest(self, source=b'original'):
        metadata = {'bytes': len(source), 'sha256': ui.digest(source),
                    'decoded_bytes': len(source), 'decoded_sha256': ui.digest(source)}
        value = {'schema': 1, 'selection': 'synthetic', 'sources': {'native.dat': metadata},
                 'banks': [], 'native': [], 'objects': {}}
        (self.assets / 'manifest.json').write_text(json.dumps(value))
        return value

    def test_source_missing_does_not_create_output(self):
        self.manifest()
        with self.assertRaises(FileNotFoundError):
            ui.build_ui_assets(self.assets, self.game, self.output)
        self.assertFalse(self.output.exists())

    def test_source_changed_does_not_create_output(self):
        self.manifest()
        (self.game / 'native.dat').write_bytes(b'modified')
        with self.assertRaisesRegex(ValueError, 'Native source mismatch'):
            ui.build_ui_assets(self.assets, self.game, self.output)
        self.assertFalse(self.output.exists())

    def test_output_cannot_overlap_game(self):
        with self.assertRaisesRegex(ValueError, 'Output must be separate'):
            ui.build_ui_assets(self.assets, self.game, self.game / 'stage')

    def test_unsafe_paths(self):
        for relative in ('../other', '/absolute', 'C:/data', 'a\\b', 'a/../../b', 'a:stream'):
            with self.subTest(relative=relative), self.assertRaises(ValueError):
                ui.safe_path(self.assets, relative)

    def object(self, raw, expected_bytes=None, trailing=b''):
        key = ui.digest(raw)
        compressed = zlib.compress(raw, 9) + trailing
        folder = self.assets / 'objects'
        folder.mkdir(exist_ok=True)
        (folder / (key + '.zlib')).write_bytes(compressed)
        return key, {key: {'bytes': len(raw) if expected_bytes is None else expected_bytes,
                          'compressed_bytes': len(compressed), 'compressed_sha256': ui.digest(compressed)}}

    def test_object_corruption(self):
        key, objects = self.object(b'generated pixels')
        (self.assets / 'objects' / (key + '.zlib')).write_bytes(b'corrupt')
        with self.assertRaisesRegex(ValueError, 'Compressed object mismatch'):
            ui.load_object(self.assets, objects, key)

    def test_object_bounded_expansion(self):
        key, objects = self.object(b'x' * 100000, expected_bytes=8)
        with self.assertRaisesRegex(ValueError, 'Invalid compressed layer'):
            ui.load_object(self.assets, objects, key)

    def test_object_trailing_stream_rejected(self):
        key, objects = self.object(b'generated', trailing=zlib.compress(b'extra'))
        with self.assertRaisesRegex(ValueError, 'Invalid compressed layer'):
            ui.load_object(self.assets, objects, key)

    def valid_image_manifest(self):
        value = self.manifest(b'\x01\x02')
        (self.game / 'native.dat').write_bytes(b'\x01\x02')
        target = b'\x03\x04'
        key, objects = self.object(target)
        value['objects'] = objects
        value['native'] = [{'source': 'native.dat', 'output': 'data/image.raw',
                            'dimensions': [2, 1], 'format': 'raw', 'generated_indices': key,
                            'bytes': 2, 'sha256': ui.digest(target),
                            'pixel_sha256': ui.pixel_digest(target, b'\xff\xff')}]
        (self.assets / 'manifest.json').write_text(json.dumps(value))
        return value

    def test_wrong_final_hash_never_writes_partial_output(self):
        value = self.valid_image_manifest()
        value['native'][0]['sha256'] = '0' * 64
        (self.assets / 'manifest.json').write_text(json.dumps(value))
        with self.assertRaisesRegex(ValueError, 'Output hash mismatch'):
            ui.build_ui_assets(self.assets, self.game, self.output)
        self.assertFalse(self.output.exists())

    def test_verify_only_is_repeatable_and_writes_nothing(self):
        self.valid_image_manifest()
        first = ui.build_ui_assets(self.assets, self.game, self.output, verify_only=True)
        second = ui.build_ui_assets(self.assets, self.game, self.output, verify_only=True)
        self.assertEqual(first, second)
        self.assertFalse(self.output.exists())

    def test_occupied_output_is_preserved(self):
        self.valid_image_manifest()
        self.output.mkdir()
        sentinel = self.output / 'unrelated.txt'
        sentinel.write_bytes(b'keep')
        with self.assertRaisesRegex(ValueError, 'Staging output must be empty'):
            ui.build_ui_assets(self.assets, self.game, self.output)
        self.assertEqual(sentinel.read_bytes(), b'keep')

    def test_sprite_opaque_zero_and_transparency(self):
        indices, mask = bytes([0, 22, 18, 0, 19, 21]), bytes([255, 0, 255, 255, 255, 0])
        encoded = ui.encode_sprite(indices, mask, 3, 2)
        decoded, coverage, extent = ui.decode_sprite(encoded, 3, 2)
        self.assertEqual(bytes(coverage), mask)
        self.assertEqual(ui.pixel_digest(decoded, coverage), ui.pixel_digest(indices, mask))
        self.assertEqual(extent, len(encoded))

    def test_faithful_upscale_is_derived_from_native_pixels_only(self):
        from frontend_reconstruct import reconstruct_faithful
        indices = bytes([0, 5, 5, 0, 5, 9, 9, 5, 5, 9, 9, 5, 0, 5, 5, 0])
        mask = bytes(255 if value else 0 for value in indices)
        palette = bytearray(768)
        palette[15:18], palette[27:30] = bytes((40, 10, 5)), bytes((60, 50, 20))
        sources = {'ldata/frontbit.tab': struct.pack('<IBB', 0, 4, 4),
                   'ldata/frontbit.dat': ui.encode_sprite(indices, mask, 4, 4), 'ldata/front.pal': bytes(palette)}
        bank, record = {'name': 'frontbit', 'palette': 'ldata/front.pal', 'scale': 4}, {'index': 0, 'method': 'oriented-4x-v1'}
        upscaled, coverage = reconstruct_faithful(bank, record, sources, None)
        self.assertEqual((upscaled, coverage), reconstruct_faithful(bank, record, sources, None))
        self.assertEqual((len(upscaled), len(coverage)), (256, 256))
        self.assertTrue(all(value > 0 for value, alpha in zip(upscaled, coverage) if alpha))
        self.assertTrue(all(value == 0 for value, alpha in zip(upscaled, coverage) if not alpha))
        self.assertEqual(set(upscaled[16 * 7 + 6:16 * 7 + 10]), {9})
        self.assertEqual(coverage[0], 0)
        with self.assertRaises(ValueError):
            reconstruct_faithful(bank, dict(record, method='other'), sources, None)

    def test_sprite_overrun_and_truncation(self):
        for encoded in (b'\x02\x07', b'\x03\x01\x02\x03\x00', b'\xfe\x00'):
            with self.subTest(encoded=encoded), self.assertRaises(ValueError):
                ui.decode_sprite(encoded, 1, 1)

    def test_huge_mask_roundtrip_and_bad_skip(self):
        indices, mask = bytes([1, 0, 2, 3, 0, 0]), bytes([255, 0, 255, 255, 0, 0])
        encoded = ui.encode_huge(indices, mask, 3, 2)
        decoded, coverage = ui.decode_huge(encoded, 3, 2)
        self.assertEqual(ui.pixel_digest(decoded, coverage), ui.pixel_digest(indices, mask))
        with self.assertRaises(ValueError):
            ui.decode_huge(struct.pack('<III', 0, 0, 0), 1, 1)

    def test_rnc_invalid_headers_and_crc(self):
        for raw in (b'RNC', b'RNC\x02' + bytes(20), b'RNC\x01' + struct.pack('>IIHHH', 8, 1, 0, 1, 0) + b'x'):
            with self.subTest(raw=raw), self.assertRaises(ValueError):
                ui.unpack_source(raw)


if __name__ == '__main__':
    unittest.main()
