# Completed review coverage

| Area | Included in this profile |
|---|---|
| Creature and world packages | 42 sprite archives and 33 configuration files; 30 creature models, reviewed room props, pickups, gates, traps, effects, hand sprites and eight CTA owner colours |
| Imp possession | All 191 configured FP frame uses; prior overhead and held artwork retained, with original anchors, timing and world size |
| UI and fonts | 14 source-bound banks containing 2,632 records: 569 HUD records, 1,362 font records, 87 frontend records, 505 pointers, 21 map hands and 88 campaign markers |
| Native frontend overlays | 49 files: the prior 27 loading/menu/victory-frame files plus 22 original campaign map states at native 1280x960 |
| Terrain | Existing 601 enhanced STANDARD tiles; 31 deliberate native cells remain unchanged |
| Audio | Existing 734 modern English WAVs; no new audio conversion in this change |
| Presentation | Accepted hand masking, Heart highlight limit, source-aware emission, smooth light cells, palette interpolation and filtered shadows |
| Text | 80% western text at height 720 and above, with measurement/centering/tooltip layout updated consistently; native sizing below 720 and for double-byte text |

These assets are selected by accepted V23 (`9a285bacf69a60ea22bd0974c915ed37f1d5ab1b93bf20b7a0bc93390b0065de`), extending the earlier V13 publication and its later corrected art. Existing sprite archive members and configurations outside Imp/CTA are unchanged. The 17 portraits extend the HUD bank; the 13 controls extend the frontend bank. Original map positions, markers, masks, palettes and text meaning remain authoritative. Original fallback animation frames and intentionally unchanged assets are reconstructed or loaded locally. Animation timing, frame counts, source mappings, logical dimensions and anchors are preserved.

The publication process verifies exact reconstruction separately from visual acceptance. The user accepted V23 on 2026-10-29. The Imp archive passed 464 matched stages: 344 directed frame/heading stages, 80 ticks across five creature instances and 40 ticks through native death phases. CTA passed eight owner-colour lifecycles in overhead and possession views, with matched red/blue controls. Both campaign marker styles entered original level 6. Options records 89-95 and the four sliders were exercised; all 13 new records were admitted. These are bounded checks, not complete campaign playthroughs or audible-quality qualification.

Remaining native artwork includes 3,012 possession frame uses across the other 29 creature models, 26 active multiplayer marker records, five campaign netfont symbols, nine torture-door banks (99 frames) and the torture cursor. The torture-background candidate remains held outside this publication. Original branding and legal notices are intentionally retained. Natural job acquisition, hauling/combat, all campaign transitions, multiplayer alliance states, full slider endpoints and broader resolution/GPU checks remain open.

The PR build passes original-campaign simulation on DX12, OpenGL and software. CPU sanitizers cover UI decoding (including malformed/truncated inputs), raw indexed sprite transparency, shadow boundaries and emissive/lighting contracts. Installation and launcher checks cover content hashes, source-data requirements, mod-name collisions, atomic file replacement and restoration. Local screenshots, runtime logs and failure evidence are retained outside the repository.

The earlier scoped RX 7900 GRE V13 GPU validation run recorded 20,619 submissions with no rejected frames or D3D12 validation warnings/errors. This is not a performance measurement or a closure of the broader resource-state stress investigation. The integrated-GPU resize warning remains open. The shared shutdown light-list diagnostic and an intermittent audio-device enumeration failure are also retained in the local record rather than suppressed.
