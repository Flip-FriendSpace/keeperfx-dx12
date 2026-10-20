# Reviewed sprite bundle

This bundle preserves the V13 sprite selection `dec01d5ddb2414ffccb98d38c1712996694b00aa639a049c8a776add7533bdbf`. It covers all 41 selected creature, hero, hand, world-object and effect archives, plus their 32 exact configuration files. The standalone mod basename is `keeper_remaster_default`. The enhanced pixels of 29 archives (25 creatures and heroes, the power hand, pickups, traps and doors, combat effects) are colour-matched to the original sprites; member names, alpha, `sprites.json`, original fallback frames and configuration are unchanged by those passes. The power hand, Troll, Bile Demon, Imp and Dark Mistress archives and `creatrs/troll.cfg` were then replaced by their later corrected art, as colour-matched for the local default package. The corrected Dark Mistress adds a new side-view slap frame (`slap/td/d2-01.png`) where V13 fell back to the original sprite.

Enhanced pixels and metadata are stored once as SHA256-addressed zlib objects. Exact RGB palette indices and the original alpha mask reconstruct the accepted RGBA pixels; the pinned PNG encoder must then reproduce every accepted PNG byte. This is lossless compression, not colour reduction. Enhanced PNGs that already exist unchanged in the repository (65 after the colour match) are reused through a hash-checked allowlist under assets/creatures/, verified against public/main at `2ef76fe54`; they are not stored again as new objects. No original fallback or reference pixels, runtime ZIPs, captures, saves or build output are included. Installation reconstructs those original members from the player's prepared local KeeperFX data and base `fxdata` archives.

## Reconstruct locally

From the repository root:

```text
python -B tools/import_remaster_sprites.py --assets assets/remaster-sprites --data PATH/TO/KEEPERFX/data --output PATH/TO/NEW/keeper_remaster_default
python -B tools/import_remaster_sprites.py --assets assets/remaster-sprites --output PATH/TO/NEW/keeper_remaster_default --verify-only
python -B tools/test_remaster_sprites.py
```

Use Python 3.10 or later and Pillow. The byte-exact fallback encoding was qualified with Pillow 12.3.0 and zlib 1.3.2. Another encoder version is accepted only if it produces the expected PNG bytes. A mismatch fails instead of silently changing the reviewed package.

The data root must contain the hash-compatible `creature.tab`, `creature.jty`, `palette.dat` and `png_conv_pal.dat`. Base custom originals are read from the sibling `fxdata/decorative_objects.zip` and `fxdata/creatures.zip`. Required hashes are recorded in the manifest. These files are read only.

The output must be a new directory or an existing installation with a matching receipt. Installation builds and verifies a sibling staging directory, then publishes it atomically without replacing unrelated files. It never changes game settings, mod order, enabled mods or game data. The coordinating installer may combine this output with UI, terrain and audio assets in its own final stage.

## Format and API

`manifest.json` has schema 2 and id `remaster-v13-sprites`. It records the reviewed selection, original-source hashes, 41 archive recipes, 32 configuration objects, counts and the output basename. An object descriptor records SHA256 and byte length before and after zlib compression. Its path is derived as `objects/XX/SHA256.z`; timestamps are absent.

`colour_match` records the colour pass: the previous manifest hash, the tool and report hashes, and for each changed archive its hashes before and after and its grade. Later passes, which may only change archives that no earlier pass changed, are appended to `colour_match.later_passes` in the same form; their archive entries also list the grade applied to each animation group (`unit_grades`). The power hand is such a pass. For colour-matched archives `reviewed_zip_sha256` identifies the graded archive. The pass is reproduced and stored with `local-dev/remaster/match_sprite_colour.py` and `apply_colour_match.py`; see the [remaster guide](../../local-dev/remaster/README.md#colour-match).

`archive_updates` records each replacement of whole archives or configuration files: the previous manifest hash, the source, each archive's hashes before and after with the changed members by source kind, each file's hashes, and the earlier `colour_match` records it supersedes. For updated archives `reviewed_zip_sha256` identifies the source archive. `local-dev/remaster/apply_archive_update.py` makes these updates; see the [remaster guide](../../local-dev/remaster/README.md#archive-updates).

Indexed stream `KFXPX1` stores exact RGB colours, an 8-bit colour-index plane, and the unchanged binary 1-bit or 8-bit alpha plane. Hidden RGB under transparent pixels is retained. `KFXRGBA1` supports exact full RGBA where more than 256 RGB colours exist. No quantization, filtering or resampling is used for enhanced artwork. The decoder checks dimensions, palette bounds, stream length, RGBA hash and final PNG hash.

Each compressed archive recipe lists every member in its original order. PNG members retain their exact byte hash, dimensions and RGBA hash. `sprites.json` is retained byte-for-byte, including aliases, view scales, offsets, raw-index/additive encodings and replacement bindings.

Member sources are:

- `enhanced`: restore exact PNG bytes from a lossless indexed/RGBA stream and checked encoder, an existing allowlisted published PNG, or a raw-PNG object when necessary.
- `original`: decode a hash-bound original table frame or local base-archive member, then apply only the recorded exact integer pixel replication and transparent padding. Header, pixels and PNG bytes must match.
- `transparent`: reconstruct a uniform fully transparent image from its dimensions and colour metadata.
- `metadata`: restore exact `sprites.json` bytes from an object.

The supported importer API is:

```python
build(assets: Path, data: Path | None, output: Path, verify_only=False) -> dict
```

It returns a receipt binding the manifest, original sources and all output hashes. Verification also checks every ZIP member against its recipe, so editing a receipt cannot conceal changed sprite bytes.

The local exporter accepts `--selection`, `--data`, `--destination` and `--report`. It verifies the exact V13 selection, identifies originals by an exhaustive local pixel comparison, and fails on an unreconstructable known fallback. Source-bank/reference pixels and local validation reports do not belong in the publication directory.

UI banks, native frontend overlays, terrain, audio and engine binaries are separate installer inputs. Existing older creature assets and installers remain compatible and unchanged. The allowlisted PNGs must be present at their existing repository paths; release packaging should retain them and their notice. The compact importer requires only Pillow; optional NumPy speeds the offline repacker but is not used to decode or install.


To reproduce compaction from the complete PNG-object publication:

```text
python -B tools/repack_remaster_sprites.py --source PATH/TO/PNG-BUNDLE --legacy-root PATH/TO/REPOSITORY --destination PATH/TO/NEW/assets/remaster-sprites --report PATH/TO/REPORT.json
```
