# SPDX-License-Identifier: GPL-2.0-or-later
"""Synthetic contracts for source-owned colour, geometry and semantic pixels."""
import struct
import unittest
import numpy as np
import ui_publication as ui
import restoration_reconstruct as rr


def fixture(name, index, pixels, colours, coverage=None):
    height, width = pixels.shape
    coverage = np.full_like(pixels, 255) if coverage is None else coverage
    folder = 'data' if name == 'gui1-64' else 'ldata'
    bank = {'name': name, 'data': folder + '/' + name + '.dat',
            'table': folder + '/' + name + '.tab', 'scale': 4,
            'palette': 'data/palette.dat' if folder == 'data' else 'ldata/front.pal'}
    table = bytes(index * 6) + struct.pack('<IBB', 0, width, height)
    sources = {bank['data']: ui.encode_sprite(pixels.tobytes(), coverage.tobytes(), width, height),
               bank['table']: table, bank['palette']: colours.tobytes()}
    return bank, sources


class RestorationTests(unittest.TestCase):
    def setUp(self):
        self.colours = np.zeros((256, 3), np.uint8)
        self.colours[4], self.colours[5] = [10, 7, 2], [12, 9, 3]
        self.colours[6], self.colours[7] = [55, 4, 0], [20, 20, 20]

    def test_portrait_uses_source_black_and_exact_native_coverage(self):
        pixels = np.full((8, 8), 4, np.uint8)
        pixels[:, 0] = 0
        coverage = np.full_like(pixels, 255)
        coverage[0] = 0
        bank, sources = fixture('gui1-64', 145, pixels, self.colours, coverage)
        record = {'index': 145, 'method': 'portrait-local-colour-v1', 'generated_rgb': 'field'}
        data = bytes([200, 100, 25]) * (32 * 32)
        indices, mask = rr.reconstruct_portrait(bank, record, sources, lambda key: data)
        actual = np.frombuffer(indices, np.uint8).reshape(32, 32)
        self.assertEqual(mask, rr.repeat4(coverage).tobytes())
        self.assertTrue(np.all(actual[:, :4] == 0))
        self.assertTrue(np.all(actual[:4] == 0))
        self.assertGreater(np.count_nonzero(actual[4:, 4:]), 0)
        with self.assertRaisesRegex(ValueError, 'RGB field size'):
            rr.reconstruct_portrait(bank, record, sources, lambda key: data[:-1])
        with self.assertRaisesRegex(ValueError, 'Unsupported portrait'):
            rr.reconstruct_portrait(bank, dict(record, index=144), sources, lambda key: data)
        with self.assertRaisesRegex(ValueError, 'scale 4'):
            rr.reconstruct_portrait(dict(bank, scale=2), record, sources, lambda key: data)

    def marker(self):
        pixels = np.full((12, 12), 252, np.uint8)
        pixels[:, 5] = 1
        pixels[4, 3:9] = 251
        pixels[-1] = 253
        bank, sources = fixture('lndflag_ens', 2, pixels, self.colours)
        for state in range(22):
            sources['campgns/keeporig_lnd/rgmap%02d.pal' % state] = self.colours.tobytes()
        record = {'index': 2, 'method': 'marker-wood-relief-v1', 'generated_relief': 'field'}
        return bank, record, sources, pixels

    def test_markers_preserve_glyph_colour_points_and_native_edge(self):
        bank, record, sources, pixels = self.marker()
        indices, mask = rr.reconstruct_marker(bank, record, sources, lambda key: b'\x01' * (48 * 48))
        actual, original = np.frombuffer(indices, np.uint8).reshape(48, 48), rr.repeat4(pixels)
        self.assertEqual(mask, b'\xff' * (48 * 48))
        protected = (original == 1) | (original == 251)
        self.assertTrue(np.array_equal(actual[protected], original[protected]))
        self.assertTrue(np.array_equal(actual[-16:], original[-16:]))
        self.assertGreater(np.count_nonzero(actual != original), 0)
        self.assertEqual(set(actual[actual != original]), {253})
        with self.assertRaisesRegex(ValueError, 'Invalid generated marker'):
            rr.reconstruct_marker(bank, record, sources, lambda key: b'\x03' * (48 * 48))
        with self.assertRaisesRegex(ValueError, 'Unsupported marker'):
            rr.reconstruct_marker(bank, dict(record, index=1), sources, lambda key: b'')

    def test_markers_reject_palette_semantics_that_change_in_one_state(self):
        bank, record, sources, _ = self.marker()
        changed = self.colours.copy()
        changed[252] = (20, 0, 0)
        sources['campgns/keeporig_lnd/rgmap21.pal'] = changed.tobytes()
        with self.assertRaisesRegex(ValueError, 'semantic palette indices differ'):
            rr.reconstruct_marker(bank, record, sources, lambda key: b'\x01' * (48 * 48))

    def test_controls_retain_icon_region_active_rims_and_original_indices(self):
        pixels = np.full((40, 16), 4, np.uint8)
        pixels[-2, 2] = 5
        pixels[:, 4], pixels[:, 8] = 6, 7
        coverage = np.full_like(pixels, 255)
        coverage[-1, -1] = 0
        bank, sources = fixture('frontbit', 89, pixels, self.colours, coverage)
        record = {'index': 89, 'method': 'control-relief-v1', 'generated_relief': 'field'}
        payload = np.full((160, 64), 255, dtype='<i2').tobytes()
        indices, mask = rr.reconstruct_controls(bank, record, sources, lambda key: payload)
        actual, original = np.frombuffer(indices, np.uint8).reshape(160, 64), rr.repeat4(pixels)
        self.assertEqual(mask, rr.repeat4(coverage).tobytes())
        self.assertTrue(np.array_equal(actual[:96], original[:96]))
        protected = (original == 6) | (original == 7)
        self.assertTrue(np.array_equal(actual[protected], original[protected]))
        self.assertTrue(set(actual.reshape(-1)) <= {0, 4, 5, 6, 7})
        self.assertTrue(np.any(actual[100:140] != original[100:140]))
        with self.assertRaisesRegex(ValueError, 'Invalid generated control'):
            rr.reconstruct_controls(bank, record, sources,
                                    lambda key: np.full((160, 64), 256, dtype='<i2').tobytes())
        with self.assertRaisesRegex(ValueError, 'relief size mismatch'):
            rr.reconstruct_controls(bank, record, sources, lambda key: payload[:-2])

    def landview(self):
        path = 'campgns/keeporig_lnd/rgmap00.raw'
        return {'source': path, 'output': path, 'palette': path[:-4] + '.pal',
                'dimensions': [1280, 960], 'format': 'raw', 'operator': 'landview',
                'method': 'landview-local-colour-v1', 'generated_rgb': 'field'}

    def test_only_exact_native_campaign_canvases_get_large_raw_exception(self):
        entry = self.landview()
        self.assertEqual(rr.landview_dimensions(entry), 1228800)
        for changes in ({'source': 'campgns/keeporig_lnd/rgmap22.raw'},
                        {'source': 'other.raw'}, {'output': 'other.raw'},
                        {'palette': 'other.pal'}, {'dimensions': [1280, 961]},
                        {'format': 'huge'}, {'operator': 'other'},
                        {'method': 'other'}, {'native_colour': 'legacy4'}):
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                rr.landview_dimensions(dict(entry, **changes))
        with self.assertRaisesRegex(ValueError, 'Invalid dimensions'):
            ui.decode_sprite(b'', 1280, 960)

    def test_landview_colour_is_rebuilt_from_local_state(self):
        entry = self.landview()
        colours = np.zeros((256, 3), np.uint8)
        colours[7], colours[8] = (32, 25, 10), (10, 32, 25)
        generated = bytes([128, 128, 128]) * 1228800
        results = []
        for source_index in (7, 8):
            original = bytes([source_index]) * 1228800
            result = rr.reconstruct_landview(entry, original, colours.tobytes(), lambda key: generated)
            # Error diffusion may use a few neighbouring palette entries, but
            # the same stored generated field must follow each native state.
            self.assertGreater(result.count(bytes([source_index])), len(result) * .98)
            results.append(result)
        self.assertNotEqual(*results)
        with self.assertRaisesRegex(ValueError, 'RGB field size mismatch'):
            rr.reconstruct_landview(entry, b'\x07' * 1228800, colours.tobytes(), lambda key: b'')


if __name__ == '__main__':
    unittest.main()
