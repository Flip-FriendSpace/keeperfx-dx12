# Enhanced original terrain

The DX12 preview uses the accepted 4x original terrain by default when `fxdata/standard-hd.kfht` is installed. The checked-in [asset bundle](../../assets/terrain/standard-hd/README.md) includes all 601 enhanced PNGs and the exact indexed runtime pixels. A prepared runtime with matching original Dungeon Keeper STANDARD data is required.

## Install and play

Run from the workspace described in the [developer guide](../README.md), with KeeperFX closed:

```powershell
./keeperfx/local-dev/build.ps1 -Architecture x64 -DX12
./keeperfx/local-dev/play.ps1 -DX12Preview
```

The DX12 build installs the bundle after syncing configuration. To install or verify independently:

```powershell
./keeperfx/local-dev/terrain/install_enhanced_terrain.ps1
./keeperfx/local-dev/terrain/install_enhanced_terrain.ps1 -VerifyOnly
```

Use `-RuntimePath` for another prepared runtime, `-OriginalDataPath` for its original data directory, or `-AssetsPath` for another checked-out bundle. Installation needs Windows PowerShell and .NET; Python and image generation are not needed. The schema 2 bundle stores the indexed payload in `terrain.index8.gz`. The installer authenticates the compressed file, decompresses it within the declared size bound, and verifies the decoded hash before rebuilding the exact KFHT v2 pack. Original A/B banks, palette, per-tile fingerprints and PNG hashes are also verified. Compression happens only in the published bundle; the installed pack and game loading remain unchanged. It writes an ownership receipt, preserves other mods/settings, and disables only the active `keeper_enhanced_default` entry to select the original artwork. Changed files receive backups. Unknown or modified output files are refused.

For the native terrain during a DX12 session:

```powershell
./keeperfx/local-dev/play.ps1 -DX12Preview -NativeTerrain
```

Direct executable launches can set `KFX_HD_TERRAIN_PACK=off` (or `0`) to opt out, or provide an explicit pack path as an override. The launcher also accepts `-HdTerrainPack`. Restart the game after changing the selection. Missing or invalid packs fall back to native textures; incompatible source tiles also fall back individually. OpenGL and software retain their existing rendering.

## Coverage and rendering

The bundle replaces 601 of the 632 populated physical STANDARD A/B cells at 128 by 128. The 24 woman/paired-figure relief cells, six solid cells and debug cell retain their originals. The first accepted 21 replacements are preserved byte for byte. Physical B identities start at 1000; the 61 animated logical aliases follow the native eight-phase table across all 32 variations, rather than freezing a logical tile to one enhanced frame.

The producer authenticates each replacement against the actual native 32 by 32 pixels and source generation. World geometry and UV coverage remain unchanged. Runtime atlas references carry resolution independently of tile position; nearest sampling wraps within the tile and bilinear sampling converts palette indices to colours before interpolation. Supported dimensions are 32, 64, 128 and 256. KFHT v1 keeps its existing 64-record/8 MiB contract; v2 uses physical identities with bounds of 1,088 records/80 MiB.

## Verify

```powershell
./keeperfx/local-dev/hd-terrain-pack-test.ps1
python ./keeperfx/local-dev/terrain/test_install_enhanced_terrain.py
./keeperfx/local-dev/terrain/test_terrain.ps1 -Variant Original -FullTerrain
./keeperfx/local-dev/terrain/test_terrain.ps1 -Variant Default -FullTerrain
python ./keeperfx/local-dev/terrain/verify_complete_terrain.py --before '<Original run>' --after '<Default run>' --manifest ./keeperfx/assets/terrain/standard-hd/manifest.json --animations ./runtime/keeperfx/fxdata/textureanim.toml --output ./artifacts/terrain/verification.json
```

Native tests use ASan/UBSan in `keeperfx-dev`. Twelve installer tests use synthetic fixtures, including invalid gzip, decoded size bounds and decoded hash mismatches. The live fixture owns a disposable loopback API session, restores settings, and writes local captures/receipts. `compare_captures.py` builds local visual comparison sheets from those runs (Pillow required). Do not publish captures or locally rebuilt packs containing original data.

[validation.json](validation.json) records the reviewed scope: both x64 builds, 32 matching camera/terrain stages, all 601 replacements, 17,568 animation bindings, normal automatic loading and native opt-out with the API disabled. Tracked resource peaks remain within configured budgets. This does not establish whole-campaign performance, distant shimmer, every door gameplay state or full renderer qualification.

Broader GPU stress qualification remains open: GPU validation error 1358 reports an immutable page SRV in COPY_DEST layout. Same-host controls reproduce it with legacy and current terrain samplers, including unrelated outline/lens draws. The focused terrain runs have zero rejected submissions and zero GPU validation messages; the broader suite is not claimed as passing.

## Maintain the bundle

Keep the original pixels, generation masters, prompts and comparison captures in local artifacts. Export accepted replacements with:

```powershell
python ./keeperfx/local-dev/terrain/export_enhanced_terrain.py --source '<accepted local terrain artifacts>' --output ./keeperfx/assets/terrain/standard-hd
```

The exporter requires Pillow, checks the accepted 601-record pack, and verifies every PNG against its runtime INDEX8 pixels. It publishes enhanced pixels and native hashes only, with a gzip payload containing no filename or timestamp. Review tiling joins, multi-cell motifs, palette/light response and temporal consistency before changing a family. Preserve the agreed original relief exceptions unless separately reviewed.
