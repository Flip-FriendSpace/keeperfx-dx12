# SPDX-License-Identifier: GPL-2.0-or-later
"""Synthetic tests for apply_archive_update.py: classification, reuse, rebuild and refusal."""
from pathlib import Path
import io
import json
import sys
import tempfile
import unittest
import zipfile

from PIL import Image

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parents[1] / 'tools'))
sys.path.insert(0, str(HERE))
import import_remaster_sprites as installer  # noqa: E402
from remaster_sprite_codec import (AssetError, OriginalBank, HEADER, SCHEMA, IDENTITY, AUTHORITY,  # noqa: E402
                                   digest, encode_json, png_bytes, store_object)
import apply_archive_update as updater  # noqa: E402

ARCHIVE = 'fxdata/test-sprites.zip'


def sprites_json(td, fp):
    return json.dumps([{'name': 'TEST', 'td': [[{'file': f} for f in td]], 'fp': [[{'file': f} for f in fp]]}]).encode()


def write_zip(path, members):
    path.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED) as archive:
        for name, raw in members:
            archive.writestr(name, raw)


class ArchiveUpdateTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='.archive-update-test-', dir=HERE)
        self.root = Path(self.tmp.name)
        self.assets, self.data, self.source = self.root / 'assets/remaster-sprites', self.root / 'game/data', self.root / 'source'
        self.data.mkdir(parents=True)
        self.assets.mkdir(parents=True)
        palette = bytearray(768)
        palette[3:6] = bytes((20, 30, 40))
        files = {'creature.tab': HEADER.pack(0, 1, 1, 3, 3, 0, 1, 1, 1, -1, -2), 'creature.jty': bytes((1, 1, 0)),
                 'palette.dat': bytes(palette), 'png_conv_pal.dat': bytes(768)}
        for name, raw in files.items():
            (self.data / name).write_bytes(raw)
        self.bank = OriginalBank(self.data)
        self.fallback = self.bank.image(0, 'source-rgb')
        enhanced = Image.new('RGBA', (4, 4), (8, 8, 8, 255))
        self.members = [('enhanced.png', png_bytes(enhanced)), ('fallback.png', png_bytes(self.fallback)),
                        ('sprites.json', sprites_json(['enhanced.png'], ['fallback.png']))]
        recipe_frame = {'table_index': 0, 'header': self.bank.header(0), 'mode': 'source-rgb', 'crop': [0, 0, 3, 3],
                        'scale': 1, 'canvas': [3, 3], 'paste': [0, 0], 'background': [0, 0, 0, 0]}
        sources = [{'kind': 'enhanced', 'object': store_object(self.assets, self.members[0][1])},
                   {'kind': 'original', 'recipe': recipe_frame, 'encoder': {'compress_level': 6, 'optimize': False}},
                   {'kind': 'metadata', 'object': store_object(self.assets, self.members[2][1])}]
        frames = []
        for (name, raw), source in zip(self.members, sources):
            frame = {'name': name, 'sha256': digest(raw), 'bytes': len(raw), 'referenced': True, 'source': source}
            if name.endswith('.png'):
                image = Image.open(io.BytesIO(raw)).convert('RGBA')
                frame.update(size=list(image.size), rgba_sha256=digest(image.tobytes()))
            frames.append(frame)
        recipe = {'schema': SCHEMA, 'archive': ARCHIVE, 'reviewed_zip_sha256': 'a' * 64, 'members': frames}
        cfg = b'[sprites]\nStand=TEST\n'
        manifest = {'schema': SCHEMA, 'id': IDENTITY, 'authority_manifest_sha256': AUTHORITY,
                    'original_files_sha256': self.bank.hashes, 'original_archives_sha256': {},
                    'archives': [{'file': ARCHIVE, 'recipe': store_object(self.assets, encode_json(recipe)),
                                  'reviewed_zip_sha256': 'a' * 64, 'members': 3,
                                  'counts': {'enhanced': 1, 'original': 1, 'metadata': 1}}],
                    'counts': {'enhanced': 1, 'original': 1, 'metadata': 1},
                    'files': [{'file': 'creatrs/test.cfg', 'sha256': digest(cfg), 'bytes': len(cfg),
                               'object': store_object(self.assets, cfg)}],
                    'colour_match': {'archives': {ARCHIVE: {}}}}
        (self.assets / 'manifest.json').write_bytes(encode_json(manifest))
        self.before = self.snapshot()

    def tearDown(self):
        self.tmp.cleanup()

    def snapshot(self):
        return {p.relative_to(self.assets).as_posix(): p.read_bytes() for p in self.assets.rglob('*') if p.is_file()}

    def rebuild(self):
        output = self.root / 'out'
        installer.build(self.assets, self.data, output)
        with zipfile.ZipFile(output / ARCHIVE) as archive:
            return output, [(n, archive.read(n)) for n in archive.namelist()]

    def test_changed_members_are_classified_and_rebuild_exactly(self):
        graded = png_bytes(Image.new('RGBA', (4, 4), (9, 7, 6, 255)))
        doubled = png_bytes(self.fallback.resize((6, 6), Image.Resampling.NEAREST))
        members = [('enhanced.png', graded), ('fallback.png', self.members[1][1]), ('extra.png', doubled),
                   ('sprites.json', sprites_json(['enhanced.png', 'extra.png'], ['fallback.png']))]
        write_zip(self.source / ARCHIVE, members)
        cfg = b'[sprites]\nStand=TEST2\n'
        (self.source / 'creatrs').mkdir()
        (self.source / 'creatrs/test.cfg').write_bytes(cfg)
        updater.update(self.assets, self.source, self.data, [ARCHIVE], ['creatrs/test.cfg'], 'synthetic')
        manifest = json.loads((self.assets / 'manifest.json').read_bytes())
        entry = manifest['archive_updates'][0]
        self.assertEqual(entry['archives'][ARCHIVE]['changed_by_source'], {'enhanced': 1, 'metadata': 1, 'original': 1})
        self.assertEqual(entry['supersedes_colour_match'], [ARCHIVE])
        self.assertEqual(manifest['counts'], {'enhanced': 1, 'metadata': 1, 'original': 2})
        self.assertEqual(manifest['archives'][0]['reviewed_zip_sha256'], digest((self.source / ARCHIVE).read_bytes()))
        output, rebuilt = self.rebuild()
        self.assertEqual(rebuilt, members)
        self.assertEqual((output / 'creatrs/test.cfg').read_bytes(), cfg)
        # The replaced enhanced PNG and metadata objects are gone; the old cfg object too.
        stored = {p.stem for p in (self.assets / 'objects').rglob('*.z')}
        self.assertNotIn(digest(self.members[0][1]), stored)
        self.assertNotIn(digest(self.members[2][1]), stored)

    def test_repository_png_is_reused_not_stored(self):
        graded = png_bytes(Image.new('RGBA', (4, 4), (1, 2, 3, 255)))
        legacy = self.root / 'assets/creatures/test/graded.png'
        legacy.parent.mkdir(parents=True)
        legacy.write_bytes(graded)
        write_zip(self.source / ARCHIVE, [('enhanced.png', graded), *self.members[1:]])
        updater.update(self.assets, self.source, self.data, [ARCHIVE], [], 'synthetic')
        manifest = json.loads((self.assets / 'manifest.json').read_bytes())
        self.assertEqual(manifest['published_pngs'], {'assets/creatures/test/graded.png':
                                                      {'sha256': digest(graded), 'bytes': len(graded)}})
        self.assertEqual(self.rebuild()[1], [('enhanced.png', graded), *self.members[1:]])

    def test_unreconstructable_low_resolution_fallback_is_refused(self):
        # The changed enhanced member is stored first; the refusal must remove it again.
        graded = png_bytes(Image.new('RGBA', (4, 4), (9, 7, 6, 255)))
        changed = png_bytes(Image.new('RGBA', (3, 3), (200, 0, 0, 255)))
        write_zip(self.source / ARCHIVE, [('enhanced.png', graded), ('fallback.png', changed), self.members[2]])
        with self.assertRaises(AssetError):
            updater.update(self.assets, self.source, self.data, [ARCHIVE], [], 'synthetic')
        self.assertEqual(self.snapshot(), self.before)

    def test_original_fallback_becomes_new_art_only_when_named(self):
        # fallback.png is used at td scale 4 here, so only the --new-art confirmation guards it.
        new = png_bytes(Image.new('RGBA', (3, 3), (200, 0, 0, 255)))
        metadata = json.dumps([{'name': 'TEST', 'td_resolution_scale': 4, 'fp': [],
                                'td': [[{'file': 'enhanced.png'}, {'file': 'fallback.png'}]]}]).encode()
        members = [self.members[0], ('fallback.png', new), ('sprites.json', metadata)]
        write_zip(self.source / ARCHIVE, members)
        with self.assertRaises(AssetError):
            updater.update(self.assets, self.source, self.data, [ARCHIVE], [], 'synthetic')
        self.assertEqual(self.snapshot(), self.before)
        with self.assertRaises(AssetError):
            updater.update(self.assets, self.source, self.data, [ARCHIVE], [], 'synthetic',
                           [(ARCHIVE, 'fallback.png'), (ARCHIVE, 'enhanced.png')])
        self.assertEqual(self.snapshot(), self.before)
        updater.update(self.assets, self.source, self.data, [ARCHIVE], [], 'synthetic', [(ARCHIVE, 'fallback.png')])
        manifest = json.loads((self.assets / 'manifest.json').read_bytes())
        self.assertEqual(manifest['archive_updates'][0]['new_art_replacing_original_fallbacks'], [ARCHIVE + ':fallback.png'])
        self.assertEqual(self.rebuild()[1], members)

    def test_unknown_archive_is_refused(self):
        with self.assertRaises(AssetError):
            updater.update(self.assets, self.source, self.data, ['fxdata/other.zip'], [], 'synthetic')
        self.assertEqual(self.snapshot(), self.before)

    def test_explicit_additions_rebuild_exactly_and_preserve_existing_outputs(self):
        added = 'fxdata/new-sprites.zip'
        metadata = json.dumps([{'name': 'NEW', 'td_resolution_scale': 4,
                               'td': [[{'file': 'new.png'}]], 'fp': [[{'file': 'native.png'}]]}]).encode()
        members = [('new.png', png_bytes(Image.new('RGBA', (8, 8), (17, 22, 31, 255)))),
                   ('native.png', self.members[1][1]), ('sprites.json', metadata)]
        write_zip(self.source / added, members)
        config = b'[sprite0]\nBlue=NEW\n'
        (self.source / 'fxdata/spritecolors.toml').write_bytes(config)
        before = json.loads(self.before['manifest.json'])
        updater.update(self.assets, self.source, self.data, [], [], 'synthetic',
                       add_archives=[added], add_files=['fxdata/spritecolors.toml'],
                       publication_identity='remaster-v23-sprites')
        manifest = json.loads((self.assets / 'manifest.json').read_bytes())
        self.assertEqual(manifest['archives'][0], before['archives'][0])
        self.assertEqual(manifest['files'][0], before['files'][0])
        self.assertEqual(manifest['id'], 'remaster-v23-sprites')
        change = manifest['archive_updates'][-1]
        self.assertIsNone(change['archives'][added]['before_zip_sha256'])
        self.assertIsNone(change['files']['fxdata/spritecolors.toml']['before_sha256'])
        self.assertEqual(change['previous_publication_identity'],
                         {'id': IDENTITY, 'authority_manifest_sha256': AUTHORITY})
        self.assertEqual(manifest['archives'][1]['counts'], {'enhanced': 1, 'metadata': 1, 'original': 1})
        output, original = self.rebuild()
        self.assertEqual(original, self.members)
        with zipfile.ZipFile(output / added) as archive:
            self.assertEqual([(n, archive.read(n)) for n in archive.namelist()], members)
        self.assertEqual((output / 'fxdata/spritecolors.toml').read_bytes(), config)
        receipt = installer.build(self.assets, None, output, verify_only=True)
        self.assertEqual(receipt['id'], manifest['id'])
        self.assertEqual(receipt['authority_manifest_sha256'], manifest['authority_manifest_sha256'])
        receipt['authority_manifest_sha256'] = AUTHORITY
        (output / installer.RECEIPT).write_bytes(encode_json(receipt))
        with self.assertRaises(AssetError):
            installer.build(self.assets, None, output, verify_only=True)

    def test_addition_collisions_paths_and_wrong_identity_are_refused(self):
        cases = [dict(add_archives=[ARCHIVE]), dict(add_archives=['fxdata/TEST-sprites.zip']),
                 dict(add_archives=['fxdata/new.zip', 'fxdata/NEW.zip']),
                 dict(add_archives=['fxdata/../new.zip']), dict(add_archives=['creatrs/new.zip']),
                 dict(add_files=['fxdata/source.png']), dict(add_files=['fxdata/nested/new.toml']),
                 dict(add_files=['fxdata/new.toml'], publication_identity='unreviewed')]
        for options in cases:
            with self.subTest(options=options), self.assertRaises(AssetError):
                updater.update(self.assets, self.source, self.data, [], [], 'synthetic', **options)
            self.assertEqual(self.snapshot(), self.before)

    def test_new_archive_unclassified_native_or_unreferenced_pixels_are_refused(self):
        added = 'fxdata/new-sprites.zip'
        art = png_bytes(Image.new('RGBA', (3, 3), (200, 0, 0, 255)))
        for view, scale, extra in [('td', 1, False), ('fp', 1, False), ('td', 4, True)]:
            with self.subTest(view=view, scale=scale, extra=extra):
                entry = {'name': 'NEW', 'td': [], 'fp': [], 'resolution_scale': scale}
                entry[view] = [[{'file': 'new.png'}]]
                members = [('new.png', art), ('sprites.json', json.dumps([entry]).encode())]
                if extra:
                    members.insert(0, ('unreferenced.png', art))
                write_zip(self.source / added, members)
                with self.assertRaises(AssetError):
                    updater.update(self.assets, self.source, self.data, [], [], 'synthetic', add_archives=[added])
                self.assertEqual(self.snapshot(), self.before)

    def test_new_archive_late_failure_rolls_back_stored_objects(self):
        added = 'fxdata/new-sprites.zip'
        metadata = json.dumps([{'name': 'NEW', 'td': [[{'file': 'new.png'}]], 'fp': [],
                               'resolution_scale': 4}]).encode()
        write_zip(self.source / added, [('new.png', png_bytes(Image.new('RGBA', (8, 8), (17, 22, 31, 255)))),
                                       ('sprites.json', metadata)])
        with self.assertRaises(AssetError):
            updater.update(self.assets, self.source, self.data, [], [], 'synthetic',
                           add_archives=[added], add_files=['fxdata/missing.toml'])
        self.assertEqual(self.snapshot(), self.before)


if __name__ == '__main__':
    unittest.main()
