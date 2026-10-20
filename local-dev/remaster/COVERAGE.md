# Completed review coverage

| Area | Included in this profile |
|---|---|
| Creature and world packages | 41 sprite archives and 32 configuration files; 30 creature models, reviewed room props, pickups, gates, traps, effects and hand sprites |
| UI and fonts | 12 source-bound banks containing 2,514 records: 552 HUD records, 1,362 font records, 74 frontend records, 505 pointers and 21 map hands |
| Native frontend overlays | 27 loading/menu/map-frame files reconstructed from generated layers and local original data |
| Terrain | Existing 601 enhanced STANDARD tiles; 31 deliberate native cells remain unchanged |
| Audio | Existing 734 modern English WAVs; no new audio conversion in this change |
| Presentation | Accepted hand masking, Heart highlight limit, source-aware emission, smooth light cells, palette interpolation and filtered shadows |
| Text | 80% western text at height 720 and above, with measurement/centering/tooltip layout updated consistently; native sizing below 720 and for double-byte text |

These are completed, reviewed assets selected by V13 (`dec01d5ddb2414ffccb98d38c1712996694b00aa639a049c8a776add7533bdbf`). On 2026-10-19 the enhanced pixels of 28 sprite archives and the 22 campaign-map frames were colour-matched to the original art, followed by the power hand. The power hand, Troll, Bile Demon, Imp and Dark Mistress archives and the Troll configuration were then replaced by their corrected, colour-matched art. Orc, sorceror, ghost and hell hound sprites, the remaining effect and room archives, and the other frontend overlays keep their V13 colours. The package does not claim that every original asset has a replacement. Original fallback animation frames and other intentionally unchanged assets are reconstructed or loaded locally. Animation timing, frame counts, source mappings, logical dimensions and anchors are preserved.

The publication process verifies exact reconstruction separately from visual acceptance. The user accepted the combined V13 preview on 2026-10-16. Wider creature/action interactions, ownership variants, possession, localisation and GPU stress coverage remain bounded by the existing validation record. Additional portraits, campaign geography, utility screens, supplemental creatures, music and cinematics remain separate unfinished workstreams.

The PR build passes original-campaign simulation on DX12, OpenGL and software. CPU sanitizers cover UI decoding (including malformed/truncated inputs), raw indexed sprite transparency, shadow boundaries and emissive/lighting contracts. Installation and launcher checks cover content hashes, source-data requirements, mod-name collisions, atomic file replacement and restoration. Local screenshots, runtime logs and failure evidence are retained outside the repository.

The earlier scoped RX 7900 GRE V13 GPU validation run recorded 20,619 submissions with no rejected frames or D3D12 validation warnings/errors. This is not a performance measurement or a closure of the broader resource-state stress investigation. The integrated-GPU resize warning remains open. The shared shutdown light-list diagnostic and an intermittent audio-device enumeration failure are also retained in the local record rather than suppressed.
