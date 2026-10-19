# Reviewed remaster defaults

This package installs the completed V13 review as the default Windows launcher profile. It combines the reviewed creature and world sprites, hand and pointers, HUD, frontend art and fonts with the existing enhanced terrain and modern English audio packages. Sprite and campaign-frame colours are matched to the original art (see [Colour match](#colour-match)).

The profile uses 80% western text at 720p and above, smooth light cells, interpolated light palettes, filtered shadows and the reviewed source-aware emissive treatment. Original animation timing, logical sprite dimensions and anchors remain unchanged. The Dungeon Heart highlight clamp and hand masking corrections are included. Low-resolution and double-byte text retain native sizing.

## Install

Use a matching KeeperFX runtime and your legally obtained original Dungeon Keeper data as described in [the developer guide](../README.md). Python 3.11 or newer with the exact reconstruction dependencies is required:

```powershell
python -m pip install -r local-dev/remaster/requirements.txt
powershell -NoProfile -File local-dev/remaster/install.ps1 -RuntimePath C:\Games\KeeperFX
powershell -NoProfile -File local-dev/play.ps1 -RuntimePath C:\Games\KeeperFX
```

The installer checks original data hashes, reconstructs all selected assets in a separate staging directory, verifies output hashes and installs the profile with recoverable backups. Missing or incompatible original data, encoder output or generated payloads stop installation. No artwork is regenerated during installation. Original frontend files remain in place until a play session activates the replacements.

For the documented sibling workspace layout, use the root `Play KeeperFX Enhanced.cmd` and `Play KeeperFX Original.cmd`, or `local-dev/play.ps1`. A source worktree can explicitly select its runtime workspace:

```powershell
./local-dev/play.ps1 -WorkspacePath C:\Projects\DK1
./local-dev/play.ps1 -WorkspacePath C:\Projects\DK1 -Original
./local-dev/play.ps1 -WorkspacePath C:\Projects\DK1 -OpenGLFallback
```

The original comparison disables the remaster sprite/UI/terrain presentation and restores native text size. Modern audio and unrelated user mods remain enabled. Both launches keep the loopback API disabled. DX12 debug/GPU validation is off for normal play and is a separate explicit diagnostic mode.

The launcher verifies the installed files, temporarily applies the frontend overlays and mod order, waits for its game process, and restores those files byte for byte. It never stops an existing game. An interrupted session retains its recovery journal; after closing the game, recover with:

```powershell
./local-dev/remaster/play-remaster.ps1 -WorkspacePath C:\Projects\DK1 -Recover
```

`play.ps1 -NativeProfile` selects the direct development launcher without the installed profile. This is intended for diagnostics and existing-backend regression checks.

## Lossless packaging and provenance

Generated replacements use compressed, content-addressed objects and explicit reconstruction recipes. Repeated content is stored once. Original fallback frames, native masks and colour fields are decoded from the user's hash-verified installation. They are not included in the publication payload. The installer checks exact sprite member bytes, source mappings, metadata and final UI package hashes; compression does not downscale, quantize or alter the reviewed pixels.

Runtime ZIPs, combined packages, native game pixels, palettes, extracted reference sheets, screenshots, saves and build output remain outside Git. The original game assets retain their owners' rights. See the asset notices under `assets/remaster-sprites/` and `assets/remaster-ui/`, and the existing terrain/audio notices.

## Colour match

The enhanced pixels of 28 sprite archives (25 creatures and heroes, pickups, traps and doors, combat effects) and the 22 campaign-map frames have been colour-matched to the original art. To repeat that pass, build both bundles as they were before it, grade them and store the result:

```powershell
python -B tools/import_remaster_sprites.py --assets assets/remaster-sprites --data C:\Games\KeeperFX\data --output C:\Temp\remaster-sprites
python -B local-dev/world-remaster/frontend-art/ui_publication.py --assets assets/remaster-ui --game-root C:\Games\KeeperFX --output C:\Temp\remaster-ui
python -B local-dev/remaster/match_sprite_colour.py --sprites C:\Temp\remaster-sprites --ui C:\Temp\remaster-ui --game C:\Games\KeeperFX --output C:\Temp\remaster-graded
python -B local-dev/remaster/apply_colour_match.py --graded C:\Temp\remaster-graded --before-ui C:\Temp\remaster-ui
```

Each HD animation is compared with the original frames it replaces, after the game's own conversion to the palette. KeeperFX-only objects (sentry, ballista, disarmed traps, barrel) are compared with KeeperFX's native sprites. The grade is a linear-light gain per channel (exposure and white balance) followed by a chroma scale about neutral, fitted so the mean L\*a\*b\* and the chroma measured on the original pixel grid match. Each creature receives one grade, and each animation then moves halfway towards its own fit (`--creature-weight`); objects are fitted one by one. Original fallback frames, alpha, `sprites.json` and archive structure are unchanged. In the campaign frames, only generated pixels are graded, and they are re-quantized to palette entries the frame already uses. The patch tool never grades an archive twice: a later pass may only change archives that no earlier pass changed, and it is appended to the bundle's record. It checks that only enhanced members changed and stores the graded pixels losslessly. A UI bundle that already records a colour match is refused. Rebuild both bundles afterwards and compare the outputs with the graded files.

A second pass matched the power hand (`hand-cursors.zip`). Each hand animation is fitted on its own, and Hold shares Pickup's grade because its frame is Pickup's last frame on another canvas (`--share-identical`). Selecting an archive or frame turns off the default lists:

```powershell
python -B local-dev/remaster/match_sprite_colour.py --sprites C:\Temp\remaster-sprites --game C:\Games\KeeperFX --object-archive hand-cursors.zip --share-identical --output C:\Temp\remaster-hand
python -B local-dev/remaster/apply_colour_match.py --graded C:\Temp\remaster-hand
```

## Validation and limits

The reviewed source is built for both the default and DX12 x64 targets in the dedicated `keeperfx-dev` distribution. Original-campaign simulation checks cover DX12, OpenGL and software. Decoder, raw-index, shadow, lighting and emissive contract tests use sanitizers and the accepted UI sidecars. Run the portable CPU suite inside that distribution:

```bash
python3 local-dev/remaster/test_contracts.py --output /tmp/keeperfx-remaster-tests
```

Add `--ui-packs <locally-installed-ui-directory>` for full positive and malformed-package checks. GPU validation and performance measurements are separate from these CPU tests.

The user accepted the combined V13 visual preview on 2026-10-16 and the colour-matched sprites and campaign frames on 2026-10-19. The power-hand pass, made the same day at the user's request, has not yet been checked in game. This acceptance does not qualify every creature action, localisation, map or GPU. The broader resource-state stress investigation and the integrated-GPU resize warning remain open. Captures and failed evidence are retained locally. No new all-map performance claim is made by this publication.

Installer failure injection uses synthetic temporary files only:

```powershell
python -B local-dev/remaster/test_install_transactions.py
powershell -NoProfile -File local-dev/remaster/test_launcher.ps1 -OutputPath C:\Temp\keeperfx-launcher-tests
```
