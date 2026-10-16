# Source-bound UI reconstruction

`ui_publication.py` rebuilds the accepted V13 UI selection from losslessly
compressed generated inputs and a verified local game installation. It writes
only a separate staging directory. The integration installer owns installation,
backups, mod order and default settings.

Install the exact dependencies in `requirements-publication.txt`. Run:

```powershell
python -B local-dev/world-remaster/frontend-art/ui_publication.py --assets assets/remaster-ui --game-root C:/Games/KeeperFX-original --output C:/Temp/KeeperFX-ui-stage
```

Add `--verify-only` for complete in-memory reconstruction without output writes.
The source installation must contain the original files identified in the
manifest, including their original compression. An already overlaid installation
needs the original-data backup as its source. A changed or missing source fails
before reconstruction or writes. No substitute palettes or guessed aliases are
accepted.

The Python API is
`build_ui_assets(asset_root, game_root, output_root, *, source_decoder=None, verify_only=False)`.
Import it with this script's directory on `sys.path`. `source_decoder`, if supplied,
accepts source bytes and returns decoded bytes; the default is the bounded RNC1
decoder based on the repository's GPL decompressor. Results are checked against
the decoded source hash in either case. The receipt contains `schema`, `selection`,
`manifest_sha256`, `verify_only` and `files`. Each file has `path`, `bytes`, `sha256`
and `pixel_sha256`. Native image pixel hashes cover zeroed transparent indices
followed by binary coverage. A UI bank's pixel hash covers its ordered record
pixel digests. Every full output file must also match the selected V13 hash.

Output paths are `ui/{font1-64,font2-64,gui1-64,gui2-64,frontbit,frontft1,frontft2,
frontft3,frontft4,netfont,pointer64,maphand}.kfui`, `ldata/front.raw`,
`data/loading32.raw`, `data/loading64.raw`, `data/gmap32.raw`, `data/gmap64.raw`
and `campgns/keeporig_lnd/viframe00.dat` through `viframe21.dat`.
The `maphand` stem is intentional. Omitted native slots stay omitted.

## Stored inputs and local reconstruction

The asset manifest references content-addressed `objects/<raw-sha256>.zlib` files.
Objects contain generated glyph alpha, generated registered RGBA, generated
material RGB float32 fields, generated indexed artwork/alpha, or bounded operator
metadata. Compression is lossless zlib level 9, with raw and compressed lengths
and hashes. Identical raw inputs share one object. PNG masters, raw duplicates
and distribution ZIPs are not included.

Native palettes, coverage, shadows, spatial colour fields, state differences,
decorative frame pixels, metrics and source fingerprints are reconstructed from
the installed data. Palette value 64 and each pipeline's historical colour
conversion remain explicit operations. The HUD and hand modules reproduce their
native compositing and aliases using the pinned image arithmetic. Frontend fonts
store generated alpha without the native RGB field. Frontend button/panel material
fields contain no native colour transfer. Huge frames retain native coverage and
joining edges; parchment maps restore the two native flame-backdrop corners.

Every source is verified before any operation. Decoders bound dimensions,
decompression, runs, file sizes and total staged output, and verify the complete
result before writing a fresh output directory. The staging directory must not
overlap either the asset directory or game directory. Corrupt or incompatible
inputs fail rather than falling back to a different appearance.

## Display and regression recipes

Sprite sidecars use physical 4x pixels at the original logical size. The large
backgrounds deliberately keep their supported native canvases: 640x480 and
320x200 raw images, and 960x720 huge frames. This preserves aspect ratio and
bounded native loader/map-buffer allocations. No navigation, hotspot, animation
phase, palette transition, map coordinate or text advance is changed by this
package.

Integration validation should exercise main/options/campaign/save/load navigation
with keyboard and pointer, hover/press/disabled controls, loading, victory/defeat
and statistics. Compare original and enhanced paths at 4:3 and 16:9, native and
integer-scaled UI, 1920x1080 and 4K. Check long translated labels, wrapping,
accents, source blank slots and DBC fallback with unchanged logical font metrics.
Campaign tests should include level markers, detail windows and parchment maps;
zoom/pan into and out of the landview, move between levels, return through menus,
and observe all fade/palette phases and frame edges. Check netfont's actual
campaign palette remap, rather than inferring it from a static sheet.

The exact-byte replay establishes packaging fidelity. It does not expand the
scope of the separately recorded user acceptance or claim new live display,
translation, GPU or campaign qualification. Movies remain with the cinematic
owner. Original reference pixels, comparison captures, runtime packages and the
detailed local generation ledger remain outside Git.

Run the synthetic bounds/failure checks with:

```text
python -B -m unittest discover -s local-dev/world-remaster/frontend-art -p test_ui_publication.py
```
