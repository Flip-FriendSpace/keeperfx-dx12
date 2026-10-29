# Source-bound UI reconstruction

`ui_publication.py` rebuilds the accepted V23 UI selection from losslessly
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
pixel digests. Every full output file must also match its manifest hash. V23 adds
17 portraits, 88 campaign-marker records, 13 controls and 22 native campaign-map
states. All 2,514 earlier sprite records and 27 native overlays retain their exact
output bytes. `restoration_update` retains the earlier selection and manifest hash.

Output paths are `ui/{font1-64,font2-64,gui1-64,gui2-64,frontbit,frontft1,frontft2,
frontft3,frontft4,netfont,pointer64,maphand,lndflag_ens,lndflag_pin}.kfui`, `ldata/front.raw`,
`data/loading32.raw`, `data/loading64.raw`, `data/gmap32.raw`, `data/gmap64.raw`
and `campgns/keeporig_lnd/viframe00.dat` through `viframe21.dat`, plus
`campgns/keeporig_lnd/rgmap00.raw` through `rgmap21.raw`.
The `maphand` stem is intentional. Omitted native slots stay omitted.

## Stored inputs and local reconstruction

The asset manifest references content-addressed `objects/<raw-sha256>.zlib` files.
Objects contain generated glyph alpha, generated registered RGBA, generated
material RGB float32 fields, generated indexed artwork/alpha, generated RGB8
fields, generated luminance relief, or bounded operator metadata. Compression
is lossless zlib level 9, with raw and compressed lengths and hashes. Identical
raw inputs share one object. PNG masters, raw duplicates
and distribution ZIPs are not included.

Native palettes, coverage, shadows, spatial colour fields, state differences,
decorative frame pixels, metrics and source fingerprints are reconstructed from
the installed data. Palette value 64 and each pipeline's historical colour
conversion remain explicit operations. The HUD and hand modules reproduce their
native compositing and aliases using the pinned image arithmetic. Frontend fonts
store generated alpha without the native RGB field. The `faithful` operator (frontbit
sprite 1, the main-menu cursor) stores nothing: it computes a 4x upscale from the native
sprite and palette with edge-oriented kernel regression, smooth back-projection and nearest
CIELAB palette entries, in explicit float64 arithmetic without BLAS. Frontend button/panel material
fields contain no native colour transfer. Huge frames retain native coverage and
joining edges; parchment maps restore the two native flame-backdrop corners.

The restoration operators use only these additional generated inputs:

- Portraits store full-canvas generated RGB resized once to the physical 4x
  dimensions with Lanczos. Local original colour fields and downsampled generated
  fields use a 0.35-native-pixel Gaussian blur and bicubic enlargement. The ratio
  `(source + 2) / (generated + 2)` is bounded to 0.08..4 before RGB rounding and
  the existing palette quantizer. Native black pixels and coverage remain exact.
- The 22 landviews share one generated RGB field resized once to 1280x960.
  Each original state's RGB and palette stay local. The colour fields use a
  1.25-native-pixel Gaussian blur and a ratio bounded to 0.02..16, followed by
  RGB rounding and fixed-palette Floyd-Steinberg diffusion. All output bytes
  match the accepted states. This is a native-resolution background recipe.
- Campaign markers store only generated luminance high-pass signs: Gaussian
  radius 4, thresholds above 4.5 or below -4.5, encoded as 0/1/2. These may switch
  only original wood indices 252/253 inside an eroded native wood mask. Original
  glyphs, red accents, coverage and the bottom four logical rows remain exact.
  The glyph/accent/wood indices must agree across all 22 bound campaign palettes.
  Records 0 and 1 retain native fallback; records 2..45 are present in each bank.
- Controls store generated signed 16-bit luminance residuals after a Gaussian
  radius 3 blur. Their contribution is multiplied by 0.32 and capped to 12 RGB
  levels before quantizing only to indices already present in the original
  control. The native mask, outer edge, pale icon strokes and active red rims
  are retained, with additional icon/track guards. Identical native records
  83..85 share the same generated field. The 13 records include alliance controls
  as well as the sliders and scrollbar; they are not all options-menu controls.

`restoration_reconstruct.py` contains the explicit operators and bounds. None
of their stored fields contains the original colour transfer, native palette,
native mask, glyph or protection map. The new source dependencies are the 22
original `rgmapNN.raw` files and both `lndflag_ens`/`lndflag_pin` DAT/TAB pairs;
their palettes were already bound. The complete source snapshot has 105 files.
The torture tower and unselected UI slots remain outside this update.

Every source is verified before any operation. Decoders bound dimensions,
decompression, runs, file sizes and total staged output, and verify the complete
result before writing a fresh output directory. The staging directory must not
overlap either the asset directory or game directory. Corrupt or incompatible
inputs fail rather than falling back to a different appearance.

## Display and regression recipes

Sprite sidecars use physical 4x pixels at the original logical size. The large
backgrounds deliberately keep their supported native canvases: 640x480 and
320x200 raw images, 1280x960 campaign landviews, and 960x720 huge frames. The
larger RAW exception is restricted to the exact 22 landview paths, dimensions,
palette bindings and operator; sprite dimension limits remain unchanged. This
preserves aspect ratio and bounded native loader/map-buffer allocations. No navigation, hotspot, animation
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
python -B -m unittest discover -s local-dev/world-remaster/frontend-art -p test_restoration_reconstruct.py
```
