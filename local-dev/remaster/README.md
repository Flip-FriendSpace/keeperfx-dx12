# Reviewed remaster defaults

This package installs the accepted V23 review as the default Windows launcher profile. It retains the earlier creature and world sprites, hand and pointers, HUD, frontend art, fonts, enhanced terrain and modern English audio, and adds all 191 Imp possession frame uses, 384 Call to Arms frame uses for eight owners, 17 portraits, 22 native-size original campaign maps, 88 campaign marker records and 13 frontend controls. See [coverage and limits](COVERAGE.md). Sprite and victory/results-frame colours retain their original-guided colour match (see [Colour match](#colour-match)). The power hand, Troll, Bile Demon, Imp and Dark Mistress retain their earlier corrections, and the main-menu cursor remains a faithful 4x upscale of the original.

The profile uses 80% western text at 720p and above, smooth light cells, interpolated light palettes, filtered shadows and the reviewed source-aware emissive treatment. Original animation timing, logical sprite dimensions and anchors remain unchanged. The Dungeon Heart highlight clamp and hand masking corrections are included. Low-resolution and double-byte text retain native sizing.

## Install

Use a matching KeeperFX runtime and your legally obtained original Dungeon Keeper data as described in [the developer guide](../README.md). Python 3.11 or newer with the exact reconstruction dependencies is required:

```powershell
python -m pip install -r local-dev/remaster/requirements.txt
powershell -NoProfile -File local-dev/remaster/install.ps1 -RuntimePath C:\Games\KeeperFX
powershell -NoProfile -File local-dev/play.ps1 -RuntimePath C:\Games\KeeperFX
```

The installer checks original data hashes, reconstructs all selected assets in a separate staging directory, verifies output hashes and installs the profile with recoverable backups. Missing or incompatible original data, encoder output or generated payloads stop installation. No artwork is regenerated during installation. Original frontend files remain in place until a play session activates the replacements. V23 keeps the existing installation-family ID and mod name so an owned V13 installation upgrades through the same backup transaction. The launcher accepts either exact selection, with 12 UI banks for V13 and 14 for V23; mixed or unrecognized selections are refused.

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

The hand, Bile Demon, Imp and Dark Mistress archives graded by these passes have since been replaced by corrected art that was graded by the same method before it was stored; see below. Their earlier `colour_match` records describe the previous members.

## Archive updates

`apply_archive_update.py` replaces whole sprite archives or configuration files in the sprite bundle with reviewed files from another package. New outputs require explicit addition flags:

```powershell
python -B local-dev/remaster/apply_archive_update.py --source C:\Temp\corrected --data C:\Games\KeeperFX\data --archive fxdata/troll-sprites.zip --file creatrs/troll.cfg --note "Where the files come from"
python -B local-dev/remaster/apply_archive_update.py --source C:\Temp\reviewed-additions --data C:\Games\KeeperFX\data --add-archive fxdata/new-reviewed-sprites.zip --add-file creatrs/new-reviewed.cfg --note "Reviewed artwork and configuration additions"
python -B local-dev/remaster/test_archive_update.py
```

These are generic operation examples. The addition example requires both output paths to be absent from the bundle; it is not a command for replaying the complete V23 publication from V13.

`--source` holds the files at their bundle paths. Members whose bytes are unchanged keep their stored source. Changed members are classified as the exporter does: pixels that the player's original data reproduces become recipes, fully transparent frames become transparent recipes, and the rest are stored losslessly as enhanced art. A changed original fallback that cannot be reconstructed is refused, so original pixels are not published by mistake. A member that was rebuilt from original data may become enhanced art only when it is named with `--new-art ARCHIVE:MEMBER`. The update is appended to `archive_updates` in the manifest, and objects nothing references are removed. A failure before the manifest is written removes the objects it stored. Rebuild the bundle afterwards and compare it with the source files.

`--add-archive` accepts a new `fxdata/*.zip`; `--add-file` accepts a new `.cfg` or `.toml` directly inside `fxdata/` or `creatrs/`. Additions reject existing outputs, case collisions and unsafe paths. In a new archive, pixels not reproduced by original-data or transparent recipes must be referenced at enhanced resolution; unreferenced or native-resolution unknown pixels fail. The source must still be reviewed art: this classification is a safeguard, not proof of authorship. `--publication-identity` selects an explicitly supported identity/authority pair and records the previous pair in history.

The accepted V23 update adds 191 Imp first-person frames and 384 Call to Arms owner frames. The Imp source archive is `d241a0afa68386e8c407e3fd58a1f3e94d9ee0830ca94b0cac83d385071d82eb`; Call to Arms is `d92ed83011e3446d3d0750efd198d9d5401e8d9356eee065a839fdff20bb06b7`; the owner mapping is `ca041a2ce94ee02b63e2803827fd7946e0c68e01d2619714d21ca6b0c0ef865d`. Their complete member/configuration bytes reproduce the accepted V23 package. All 40 other archives and existing 32 configurations are unchanged. Source ZIPs, original pixels, model masters and local review captures are not publication inputs to commit.

On 2026-10-19 this replaced the power hand, Troll, Bile Demon, Imp and Dark Mistress archives and `creatrs/troll.cfg` with the corrected art from the local default package. The hand, Bile Demon, Imp and Dark Mistress had been colour-matched there by the method above; the corrected Troll already measured 1.2 ΔE from the original. The Dark Mistress slap frame `slap/td/d2-01.png` is new art where V13 used the original sprite. The rebuilt archives match the local package member for member.

## Main-menu cursor

The generated main-menu cursor (frontbit sprite 1, the Horned Reaper head) had a different face from the original. It is replaced by a 4x upscale computed only from the native sprite and palette: edge-oriented kernel regression, three smooth back-projection passes towards the original pixel colours, then the nearest palette entry in CIELAB. No generated pixels are stored. The UI manifest's `faithful` operator recomputes the cursor from the player's data, and the record's pixel hash must match. Its coverage and colour decisions clear their thresholds by at least 1.5e-5, far above floating-point rounding, and NumPy's AVX-512, AVX2 and baseline SSE code paths and two BLAS kernels all produced the same cursor.

## Validation and limits

The reviewed source is built for both the default and DX12 x64 targets in the dedicated `keeperfx-dev` distribution. Original-campaign simulation checks cover DX12, OpenGL and software. Decoder, raw-index, shadow, lighting and emissive contract tests use sanitizers and the accepted UI sidecars. Run the portable CPU suite inside that distribution:

```bash
python3 local-dev/remaster/test_contracts.py --output /tmp/keeperfx-remaster-tests
```

Add `--ui-packs <locally-installed-ui-directory>` for full positive and malformed-package checks. GPU validation and performance measurements are separate from these CPU tests.

The user accepted the combined V13 visual preview on 2026-10-16. On 2026-10-19 the user accepted the corrected Troll, Hand, Bile Demon and Dark Mistress art, the colour-matched sprites and campaign frames, and the faithful main-menu cursor (from a comparison sheet). The corrected Imp frames, which fix the Imp held by the hand, come from the same accepted package. The final hand colour pass, made the same day at the user's request, and the menu cursor have not yet been checked in game. This acceptance does not qualify every creature action, localisation, map or GPU. The broader resource-state stress investigation and the integrated-GPU resize warning remain open. Captures and failed evidence are retained locally. No new all-map performance claim is made by this publication.

The user accepted V23 on 2026-10-29. Its exact Imp archive passed 464 matched engine stages, with directed frame/heading coverage separated from five creature instances and a native death transition. CTA checks cover all eight owner-colour lifecycles; red and blue also have matched controls. Frontend checks cover both campaign marker styles entering level 6, visible Options controls and bounded movement of the four sliders. Combined Enhanced and Original launch checks passed with the API disabled and exact restoration. This does not establish natural hauling/combat, every campaign or door interaction, all slider endpoints, multiplayer alliance states or broad display/GPU coverage. The torture candidate remains excluded. One cold OpenGL/French startup exceeded the fixture timeout; its cause remains unresolved despite passing retries.

Installer failure injection uses synthetic temporary files only:

```powershell
python -B local-dev/remaster/test_install_transactions.py
powershell -NoProfile -File local-dev/remaster/test_launcher.ps1 -OutputPath C:\Temp\keeperfx-launcher-tests
```
