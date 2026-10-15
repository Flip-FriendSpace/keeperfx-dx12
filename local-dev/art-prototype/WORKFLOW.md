Installation and prerequisites are described in [README.md](README.md). Selected enhanced PNGs and assembly metadata are published in `assets/creatures/` by explicit user approval. Original pixels, extracted references, generated source sheets, rejected variants, runtime ZIPs and local evidence reports remain outside Git. The installer assembles original fallback views locally from a compatible runtime; no image generation or historical artifacts are needed to install.

# Faithful creature sprite enhancement

This is the accepted local KeeperFX art workflow. The user approved the imp's original-sprite enhancement, five-direction idle/walk/dig sheets, corrected front digging pickaxe head and 4x in-game result on 2026-10-14. The revised remaining actions were then accepted: "This looks good. So Imp is done." The imp is complete within its recorded scope: 197 overhead frames and six hand-held frames; other possession views and shared effects retain original art. The current action register is `prototype.json`; do not infer completion from a source sheet alone. The same method now covers all 13 original heroes; the user accepted the roster comparison on 2026-10-15. Ordinary campaign play remains open.

## Visual contract

Enhance the original sprite, preserving its identity, pose, silhouette, proportions, camera elevation, facing and timing. Original extracted frames are the pose authority. Accepted enhanced artwork is the appearance authority. The rejected alternatives were an independently redesigned imp and a crude procedural Blender model. Do not restart those approaches unless requested.

The accepted imp has a broad bald brow, recessed dark eyes, thin long arms, bent legs, bare feet, warm ochre leathery skin, simple brown clothing, a large brown sack with an open gathered mouth, and a worn wood/pale iron pickaxe. Do not add ears, horns, a tail, armour or a red apron. Props must follow the original action: a dragging or hurt pose must not acquire a redundant pickaxe. The original front digging frame 2 correction rotates the **pickaxe head**, not the creature, by 90 degrees; retain its corrected source.

Use the built-in image generation tool with local source references, actual transparent alpha and explicit invariants. Read the imagegen skill for the tool contract. Do not substitute a procedural drawing or nearest-neighbour upscale for newly detailed art. Code is appropriate for reference extraction, slicing, geometric registration, palette conversion, previews and packaging. Preserve the generated alpha; do not chroma-key a coloured background into the sprites.

## Source and project map

Paths below are relative to the workspace containing `keeperfx/`:

- `runtime/keeperfx-x64/data/creature.tab`, `creature.jty`, `palette.dat`: installed original creature pixels. `png_conv_pal.dat` is the custom-sprite conversion palette.
- `keeperfx/src/creature_graphics.c`: animation-to-frame-table mapping; `keeperfx/config/creatrs/imp.cfg`: action mapping. Read actual configuration for each new creature.
- `keeperfx/local-dev/art-prototype/extract_imp_reference.py`: original RLE decoder; `prepare_imp_set.py`: original idle/walk/dig extraction; `prepare_imp_actions.py`: remaining imp action extraction.
- `artifacts/art-prototype/imp-faithful-v1.png`, `imp-overhead-v1.png`, `imp-walk-sheet-v1.png`: accepted appearance references.
- `artifacts/art-prototype/imp-set/`: approved directional source sheets, prompts, packed frames, interactive review, local mod and original integration evidence.
- `artifacts/art-prototype/imp-actions/`: remaining action references, source sheets, prompts and validation, as recorded by the current manifest.
- `keeperfx/local-dev/art-prototype/package_imp_set.py`: accepted component-aware slicing, sequence registration and exact palette conversion implementation.
- `keeperfx/local-dev/art-prototype/test_imp_set.ps1` / `.mjs`: controlled DX12 validation and Original/HD crowd comparison.
- `keeperfx/local-dev/art-prototype/play_imp.ps1`: opt-in user playtest. Run it directly with PowerShell; workspace shortcuts are not included in the repository.

Publish only the approved enhanced PNG frames and assembly metadata under `assets/creatures/`. Do not commit original pixels, extracted references, source sheets, rejected variants, runtime ZIPs, test captures, saves or build output. Keep original/source files and prompts locally for reproducibility. Reuse the existing dedicated `keeperfx-dev` WSL environment for engine builds.

## Extract and generate

1. Read each unique configured animation, including aliases and shared effects. Obtain rotation mode, frame count, native frame size, cropped offsets and origin anchors from the original table. Do not assume every action has five views or six frames. Imp scream/death/grab are nonrotating; smoke has eight frames. Possession uses separate frames. Shared torture/gib effects are not unique imp poses.
2. Decode and save all native frames and metadata. Enlarge reference sheets using nearest-neighbour for legible source pixels. Use an equal-cell grid with original empty margins and temporal order. Prefer one direction/action sheet for longer cycles. Single-pose actions can share a direction strip if each row's facing is explicit.
3. Inspect all input images before passing their local paths to image generation. Label inputs: original pose/camera/grid authority; same-direction enhanced appearance reference; accepted multi-frame consistency reference. Describe exactly what changes and what stays fixed.
4. Keep the same character proportions, sack volume/opening, prop construction, materials and lighting across the entire cycle. No text, cell borders, scenery, baked ground shadows or coloured fringes. Require transparent margins and intact extremities/tool tips in every cell. Side and rear views must remain side and rear views.
5. Save each prompt and result under a descriptive action/direction name. Preserve the accepted previous version when revising. Inspect the sheet and an animated loop; repair a specific failing frame or invariant rather than regenerating the accepted set indiscriminately. Record paths and review status in the manifest so a later session can resume.

The first remaining-action pass drifted in relax, drag and smoke, as identified by the user. Check **each original frame against its replacement**, not just whether a generated sheet looks coherent. Relax and smoke use an inverted, planted pickaxe: hands at the top of the wooden shaft, metal head on the ground. Smoke principally moves the head with a cigarette at the lips; do not invent cigarette-waving arm gestures. Front dragging needs the centred two-handed grip and foreshortened tool rather than an idle carry. Explicit pose descriptions help, but rear-view prompts must describe only visible anatomy; instructions to show the face/hands can override the intended rear facing. Use the original pixels as the final authority, including frame order and motion amplitude.

Prompt skeleton (adapt the grid, action and actual source props):

> Faithful restoration of the original Dungeon Keeper [creature]. Image 1 is the authoritative pose, camera, frame order and grid reference. Image 2 is the approved same-direction appearance reference. Preserve image 1's silhouette, proportions, facing, original empty margins and common ground anchor; add image 2's restrained material detail. Produce exactly [count] frames in [cols] columns by [rows] rows, temporal order left-to-right then top-to-bottom. [Describe action and view.] Keep the same body, face, equipment construction, material colours and lighting in every frame; only pose changes. Preserve the full subject and all tool tips inside each cell. Genuine transparent background; no ground, cast shadow, labels, text or borders.

## Convert and package

The target is 4x **width and height** at unchanged world size, not a creature enlarged fourfold. Use the original timing and native anchors.

- Split connected sprites, not arbitrary rectangles through limbs or tools. `split()` groups alpha components by nominal cell while retaining components that cross boundaries. Inspect disconnected props/particles rather than blindly discarding them. Assert expected cell/frame count and real transparency.
- Apply **one common scale and translation to each direction/action sequence**, fitting its union bounds to the original sequence bounds. Independent frame fitting makes the body or pickaxe pulse in size. Use a shared canvas and retain original anchors. Keep at least a transparent border in packed images without cropping extremities.
- Convert RGB/4 to `png_conv_pal.dat` with Manhattan distance and the first-index tie break. Engine alpha is opaque at `>=64`, transparent below. Keep both full-colour source and palette-converted output for diagnosis.
- Custom animation entries use `td_resolution_scale: 4` and `fp_resolution_scale: 1` when possession retains originals. Use `fp_resolution_scale: 4` for the enhanced PowerGrab hand view. `resolution_scale` is the common default; per-view values override it. Allowed integer factors are 1, 2, 4, 8. Multiply PNG canvas dimensions, frame bounds, offsets and shadow offsets consistently. The validated local engine accepts custom PNG axes **up to 1024 pixels**, with world shadow sources up to 512. All original hero overhead frames fit at 4x (largest axis 508); knight held frames reach 596. `package_hero.py --max-dimension 1024` requires this engine. Do not clip oversized frames or change physical size to fit. See `artifacts/art-prototype/heroes/engine-large-sprites/engine-support-report.json` for build hashes and boundary/campaign/backend evidence. OpenGL keeps its 256 MiB atlas but reduces cached entries from 2048 to 512; broad OpenGL crowd performance is unqualified. Software keeps full creature detail and reduces only large shadow masks to logical resolution.
- `KeeperSpriteResolution.h` carries scale in reserved runtime frame-flag bits 8-9. Projection, mirrored anchors, selection geometry, shadow projection, liquid cutoff and captured-scene reconstruction use this metadata. Existing disk/save layouts are unchanged. Original frames use scale 1. Imp `sprite_size` stays 300; the former 75-size object fixture was superseded and is not the integration method.
- Override only sprite fields in the local mod's `creatrs/imp.cfg`. Map aliases deliberately: attack/ranged/cast reuse dig; pretty-dance/celebrate share one action; sleep/eat reuse idle, etc. Preserve gameplay rules. Hand-held rendering can select the first-person version even in overhead play: inspect and test it when replacing PowerGrab.
- Verify ZIP integrity, dimensions, frame/view counts, aliases, palette, alpha, borders and package hash. Do not promote an unreviewed or incomplete package over the accepted playtest.

`package_imp_actions.py` extends the accepted ZIP without changing its existing entries or PNG bytes. It records source-sheet hashes and packs the versioned corrections for relax/drag/smoke. `imp_actions_review.html` is the reproducible comparison page. `test_imp_set.ps1 -Mode Actions -AssetSet imp-actions` runs the expanded action fixture, recording forced-animation coverage separately from real creature instances and interactions.

## Review and test

Compare original / enhanced native / enhanced 4x at the same displayed body size in the local HTML review. Check all facings, mirrored headings, pauses, timing, grounded feet, intact props, sack continuity and silhouette readability at normal zoom. A sprite-sheet preview is not in-game validation.

Use temporary local runtime overrides and restore settings/mod order afterwards. Tests own their game process and verify the API listener is exactly loopback and belongs to that PID. Normal play keeps the API disabled. Do not stop an unrelated running user game.

The existing validation compares original/native/4x fixtures at sprite_size 300, cycles headings/frames, verifies real creature animation IDs and unchanged size, captures lighting/shadows, and checks original-action and possession transitions. Keep the camera fixed after simulation steps; gameplay may reset a diagnostic camera between steps. For new actions, add actual state transitions where practical and distinguish forced animation coverage from naturally triggered gameplay. PowerGrab, slap and death need their own checks. Inspect captured game pixels, not only exit codes. Relevant engine changes require build and campaign smoke checks using `local-dev/README.md`.

For performance, compare Original/HD 144-imp runs at identical settings: 1920x1080, VSync off, 5-second warmup, 20-second sample, validation off for measurement. Keep a separate debug/GPU-validation run. Record median/p95 producer, submission, GPU upload/render and application admission timings, rejected frames and memory endpoints. Independent live runs differ slightly; do not claim precise speedups or scanout FPS. The earlier accepted idle/walk/dig experiment measured about 0.38 ms median GPU work for either set using 256x256 source slots. The current large-sprite engine uses adaptive 256/512/1024 RGBA slots. A source that crosses a slot boundary can increase allocation and upload costs; measure the actual roster rather than assuming DX12 makes additional texels free.

Finish by recording checked scope, remaining original/shared assets, package/executable hashes, evidence paths and user acceptance in `prototype.json` and the artifact report. Leave the next session an explicit resume point. Use the opt-in launcher for hands-on review; it restores the previous mod order on exit. `play_imp.ps1` now defaults to the accepted full `imp-actions` package; `-AssetSet imp-set` retains the earlier idle/walk/dig comparison.

## Original hero roster

The completed local roster contains wizard, barbarian, archer, monk, dwarfa, knight, avatar, tunneller, witch, giant, fairy, thief and samurai. The inventory and original frame metadata are under `artifacts/art-prototype/heroes/`. This covers 1,491 overhead frames and 78 held frames. KeeperFX additions are inventoried separately and are outside this batch. The user accepted the roster visual review on 2026-10-15 ("This looks great"); ordinary campaign play remains pending. Acceptance is recorded separately from packaging and automated checks in `prototype.json`.

For each creature, retain `source-manifest.json`, `generation-plan.json`, selected versioned source sheets, exact prompts, registered frames, `review-data.json`, `package-report.json` and game evidence. Do not rerun preparation over an artist's existing plan. The original aliases are intentional: for example, avatar Eat/Celebrate and Scream/Smoke share source sequences; fairy idle/walk source frames are identical. Keep their animation IDs and mappings while reusing only genuinely identical artwork.

Use the original frame to resolve equipment and poses: tunneller has two axes; dwarf held frames need one connected shaft and the original alternating arm positions; consumed food disappears when it does in the source. Check every frame and facing against the original before treating coherent-looking art as finished. Shorter reference rows can repair a frame whose pose or camera drifts. Reject visible sheet borders and extra limbs or weapons, retain rejected versions locally, and record the selected correction explicitly.

From `keeperfx/`, run these commands with the configured Python and Node runtimes:

```text
python local-dev/art-prototype/package_hero.py <slug> --max-dimension 1024
powershell -File local-dev/art-prototype/test_hero.ps1 -Hero <slug>
python local-dev/art-prototype/update_hero_review.py
python local-dev/art-prototype/package_heroes.py
powershell -File local-dev/art-prototype/test_heroes_diagnostic.ps1
powershell -File local-dev/art-prototype/test_heroes_benchmark.ps1 -Variant Original
powershell -File local-dev/art-prototype/test_heroes_benchmark.ps1 -Variant HD
node local-dev/art-prototype/compare_heroes_benchmark.mjs <original-run> <hd-run> <comparison.json>
```

Only one game test may own the runtime at a time, including tests in other chats. An occupied API listener or unrelated game is a reason to wait; never stop it. Individual fixtures bind successful runs to the exact ZIP hash. They cover forced frame/heading sequences separately from real movement, action instances, slap, collapse/death, possession return and pickup. Fairy airborne movement legitimately selects Stand; its Ambulate frames are checked separately. Keep failed attempts and their explanation instead of overwriting them.

The combined mod contains all 13 heroes and the pinned accepted imp: 1,688 overhead and 84 held frames across 158 animation entries. `package_heroes.py` checks the source reports, frame counts, archive integrity, unique animation names and sprite-only config files, then records content hashes in `combined-package-report.json` and the ownership marker. Rebuild only after deliberate source changes. `play_heroes.ps1` checks that manifest and all contents, enables the combined preview alongside existing base, texture and audio mods, keeps the API disabled, and restores the prior load-order bytes on exit. Run `powershell -File local-dev/art-prototype/play_heroes.ps1 -QuickStart` from `keeperfx/`; workspace shortcuts are not included in the repository.

The combined diagnostic covers original/HD object controls, real roster creatures, mirrored headings, multiple actions and water cutoff with GPU validation enabled. Keep the water test under the controlled sprite row, away from the live creature row: simulation can remove creatures placed on unsuitable water terrain. Record executable and package hashes with the reports. A later engine rebuild does not retrospectively validate an older run against the new binary.

The roster benchmark compares 168 visible creatures, 12 each of 14 models, at identical initial positions, original world size, camera and settings. Use 1920x1080, VSync off, five seconds warmup and twenty seconds sampling. Both variants load the HD archives; Original switches the sprite mappings back to original frames. This measures steady-state rendering/simulation of the mappings, not cold loading or total memory savings from removing HD archives. Keep debug/GPU validation off for timing and run the diagnostic separately. Broad OpenGL crowd performance, every campaign and natural combat/hauling remain outside these fixture claims.

Serve `artifacts/art-prototype/` on loopback and open `heroes/review.html` for the roster comparison. The refresh tool updates individual pages and test links without changing packaged artwork. Each page plays original, enhanced at native resolution and enhanced at 4x at the same displayed size; use its action/facing selectors and pause/step controls. Record static frame audits, browser spot checks, captured game inspection and user acceptance separately in `heroes/integration-report.json`. `prototype.json` and `HEROES-CHECKPOINT.md` identify the current state for future sessions. `finalize_heroes.py` checks the package and report hashes before recording the completed validation batch; user acceptance remains a separate recorded decision.
