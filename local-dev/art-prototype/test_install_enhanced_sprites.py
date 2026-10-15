# SPDX-License-Identifier: GPL-2.0-or-later
"""Focused synthetic-data tests; no original game assets are needed."""
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest
import zipfile
from PIL import Image

spec = importlib.util.spec_from_file_location("installer", Path(__file__).with_name("install_enhanced_sprites.py"))
installer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(installer)


class InstallerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.data = self.root / "data"
        self.assets = self.root / "assets"
        self.output = self.root / "mod"
        self.data.mkdir()
        self.assets.mkdir()
        self.header = [0, 2, 1, 4, 3, 0, 1, 1, 1, -2, -3]
        self.write_data(bytes([2, 1, 2, 0]))

    def write_data(self, blob):
        sentinel = [len(blob), 1, 1, 1, 1, 0, 1, 0, 0, 0, 0]
        (self.data / "creature.tab").write_bytes(installer.HEADER.pack(*self.header) + installer.HEADER.pack(*sentinel))
        (self.data / "creature.jty").write_bytes(blob)
        palette = bytearray(768)
        palette[3:9] = bytes([1, 2, 3, 4, 5, 6])
        (self.data / "palette.dat").write_bytes(palette)

    def make_assets(self):
        originals = installer.OriginalFrames(self.data)
        header, rgba_hash, _ = originals.decode(0)
        buffer = io.BytesIO()
        Image.new("RGBA", (8, 8), (8, 12, 16, 255)).save(buffer, format="PNG")
        (self.assets / "frames").mkdir()
        png = buffer.getvalue()
        (self.assets / "frames/enhanced.png").write_bytes(png)
        sprites = [{"name": "TEST_HD_IDLE", "rotatable": False, "td_resolution_scale": 4,
                    "fp_resolution_scale": 1, "td": [[{"file": "idle/td.png"}]],
                    "fp": [[{"file": "idle/fp.png"}]]}]
        recipe = {"schema": 1, "slug": "test", "config": "[sprites]\nStand = TEST_HD_IDLE\n",
                  "sprites": sprites, "frames": {
                      "idle/td.png": {"enhanced": "frames/enhanced.png", "sha256": installer.digest(png)},
                      "idle/fp.png": {"original": {"table_index": 0, "header": header, "rgba_sha256": rgba_hash}}}}
        raw = json.dumps(recipe).encode()
        (self.assets / "recipe.json").write_bytes(raw)
        manifest = {"schema": 1, "id": installer.IDENTITY, "creatures": [
            {"slug": "test", "recipe": "recipe.json", "sha256": installer.digest(raw)}]}
        (self.assets / "manifest.json").write_text(json.dumps(manifest))
        return recipe

    def test_original_frame_palette_transparency_and_anchor(self):
        _, _, png = installer.OriginalFrames(self.data).decode(0)
        image = Image.open(io.BytesIO(png))
        self.assertEqual(image.size, (4, 3))
        self.assertEqual(image.getpixel((1, 1)), (4, 8, 12, 255))
        self.assertEqual(image.getpixel((2, 1)), (16, 20, 24, 255))
        self.assertEqual(image.getpixel((0, 0)), (0, 0, 0, 0))

    def test_truncated_run_row_and_index_rejected(self):
        for blob in (bytes([3, 1, 2]), bytes([2, 1, 2]), bytes([255])):
            with self.subTest(blob=blob):
                self.write_data(blob)
                with self.assertRaises(installer.InstallError):
                    installer.OriginalFrames(self.data).decode(0)
        with self.assertRaises(installer.InstallError):
            installer.OriginalFrames(self.data).decode(-1)

    def test_invalid_table_and_frame_bounds(self):
        self.header[0] = 1000
        self.write_data(bytes([0]))
        with self.assertRaises(installer.InstallError):
            installer.OriginalFrames(self.data).decode(0)
        self.header[0], self.header[7] = 0, 4
        self.write_data(bytes([0]))
        with self.assertRaises(installer.InstallError):
            installer.OriginalFrames(self.data).decode(0)

    def test_legacy_overwide_runs_are_consumed_and_clipped(self):
        self.write_data(bytes([4, 1, 2, 3, 4, 0]))
        _, _, png = installer.OriginalFrames(self.data).decode(0)
        self.assertEqual(Image.open(io.BytesIO(png)).getpixel((2, 1)), (16, 20, 24, 255))

    def test_path_traversal_and_windows_aliases_rejected(self):
        for name in ("../escape", "/absolute", "a\\b", "C:/x", "a/../b", "a//b", "a/CON.png", "a/x.", "a/x "):
            with self.subTest(name=name), self.assertRaises(installer.InstallError):
                installer.safe_relative(name)

    def test_deterministic_build_idempotence_and_verification(self):
        recipe = self.make_assets()
        first = installer.build(self.assets, self.data, self.output)
        second = self.root / "other"
        installer.build(self.assets, self.data, second)
        self.assertEqual((self.output / "fxdata/test-sprites.zip").read_bytes(),
                         (second / "fxdata/test-sprites.zip").read_bytes())
        self.assertEqual(installer.build(self.assets, self.data, self.output), first)
        self.assertEqual(installer.build(self.assets, None, self.output, True), first)
        with zipfile.ZipFile(self.output / "fxdata/test-sprites.zip") as archive:
            self.assertEqual(archive.namelist(), ["idle/td.png", "idle/fp.png", "sprites.json"])
            self.assertTrue(all(info.date_time == (1980, 1, 1, 0, 0, 0) for info in archive.infolist()))
            self.assertEqual(json.loads(archive.read("sprites.json")), recipe["sprites"])

    def test_source_mutation_rejected_without_partial_install(self):
        self.make_assets()
        blob = bytearray((self.data / "creature.jty").read_bytes())
        blob[1] = 2
        (self.data / "creature.jty").write_bytes(blob)
        with self.assertRaises(installer.InstallError):
            installer.build(self.assets, self.data, self.output)
        self.assertFalse(self.output.exists())
        self.assertEqual(list(self.root.glob(".mod-*")), [])

    def test_missing_local_inputs_leave_no_output(self):
        self.make_assets()
        (self.data / "palette.dat").unlink()
        with self.assertRaises(installer.InstallError):
            installer.build(self.assets, self.data, self.output)
        self.assertFalse(self.output.exists())

    def test_recipe_hash_and_archive_path_rejected(self):
        recipe = self.make_assets()
        recipe["sprites"][0]["td"][0][0]["file"] = "../escape.png"
        raw = json.dumps(recipe).encode()
        (self.assets / "recipe.json").write_bytes(raw)
        with self.assertRaises(installer.InstallError):
            installer.build(self.assets, self.data, self.output)
        manifest = json.loads((self.assets / "manifest.json").read_text())
        manifest["creatures"][0]["sha256"] = installer.digest(raw)
        (self.assets / "manifest.json").write_text(json.dumps(manifest))
        with self.assertRaises(installer.InstallError):
            installer.build(self.assets, self.data, self.output)
        self.assertFalse(self.output.exists())

    def test_enhanced_corruption_and_unknown_output_rejected(self):
        self.make_assets()
        (self.assets / "frames/enhanced.png").write_bytes(b"corrupt")
        with self.assertRaises(installer.InstallError):
            installer.build(self.assets, self.data, self.output)
        self.output.mkdir()
        (self.output / "user.txt").write_text("preserve")
        with self.assertRaises(installer.InstallError):
            installer.build(self.assets, self.data, self.output)
        self.assertEqual((self.output / "user.txt").read_text(), "preserve")

    def test_installed_corruption_and_extra_files_rejected(self):
        self.make_assets()
        installer.build(self.assets, self.data, self.output)
        cfg = self.output / "creatrs/test.cfg"
        original = cfg.read_bytes()
        cfg.write_bytes(b"changed")
        with self.assertRaises(installer.InstallError):
            installer.build(self.assets, None, self.output, True)
        cfg.write_bytes(original)
        (self.output / "unexpected").write_bytes(b"x")
        with self.assertRaises(installer.InstallError):
            installer.build(self.assets, None, self.output, True)

    def test_gameplay_sections_are_not_installed(self):
        recipe = self.make_assets()
        recipe['config'] += '[attributes]\nHealth = 99999\n'
        raw = json.dumps(recipe).encode()
        (self.assets / 'recipe.json').write_bytes(raw)
        manifest_path = self.assets / 'manifest.json'
        manifest = json.loads(manifest_path.read_text())
        manifest['creatures'][0]['sha256'] = installer.digest(raw)
        manifest_path.write_text(json.dumps(manifest))
        with self.assertRaises(installer.InstallError):
            installer.build(self.assets, self.data, self.output)
        self.assertFalse(self.output.exists())

    def test_explicit_data_root_alias_is_supported(self):
        alias = self.root / "data-alias"
        try:
            alias.symlink_to(self.data, target_is_directory=True)
        except OSError:
            self.skipTest("Symlink creation unavailable")
        self.assertEqual(installer.OriginalFrames(alias).decode(0),
                         installer.OriginalFrames(self.data).decode(0))

    def test_atomic_publish_refuses_existing_directory(self):
        stage = self.root / "stage"
        target = self.root / "target"
        stage.mkdir()
        target.mkdir()
        (stage / "payload").write_text("new")
        (target / "user").write_text("preserve")
        with self.assertRaises(OSError):
            installer.rename_new_directory(stage, target)
        self.assertEqual((target / "user").read_text(), "preserve")
        self.assertEqual((stage / "payload").read_text(), "new")

    def test_symlink_source_rejected(self):
        self.make_assets()
        link = self.assets / "linked"
        try:
            link.symlink_to(self.data, target_is_directory=True)
        except OSError:
            self.skipTest("Symlink creation unavailable")
        with self.assertRaises(installer.InstallError):
            installer.read_regular(link / "palette.dat")


if __name__ == "__main__":
    unittest.main()
