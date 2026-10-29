# Modern original English audio

The complete Modern v2.1 audio package is checked in at config/mods/dk_modern_audio. It includes 734 playback WAVs: 125 in-game English voices, 569 original effect entries and 40 original campaign narrations. The default config/mods/load_order.cfg enables it in [after_base], alongside any configured texture and creature mods. Campaign and map overrides retain their higher priority.

Normal development builds sync the package into runtime/keeperfx; the x64 runtime shares that mods directory. Existing runtimes can install or verify the bundled copy without rebuilding the audio. Run this from the workspace directory after building the updated x64 engine:

    .\keeperfx\local-dev\audio\install-modern-audio.ps1

The runtime defaults to the sibling runtime/keeperfx-x64 directory. Use -RuntimeDirectory to select another matching x64 development runtime, or -PackageDirectory to select a different complete Modern v2.1 package. Add -WhatIf to validate all 734 checksums and preview the change without installing it. No model download or audio generation is needed.

The installer verifies every audio file before activation, preserves other mod entries, backs up both mod orders and enables dk_modern_audio last in [after_base]. It copies the provenance notice and manifest with the audio. Original sound banks remain intact. No separate audio launcher is required; launchers sharing this runtime use the same default. For an existing installation with custom mod choices, use the installer to merge the new default before a development config sync.

The treatment uses gentle cleanup and source-constrained upper-band detail at 48 kHz, preserving the original performances, recording length and channel layout. The reconstructed residual is capped against 15% of the cleaned original spectral magnitude, and fades out at the original source bandwidth. This replaces v1's unrestricted 65% high-band blend, which added metallic ringing to speech and effects. The lower original band remains intact. Cleanup is spectral noise reduction only. v2.1 removes v1's DC subtraction and 18/30 Hz high-pass: they removed nothing audible, but their short filter edge padding started 82 effects and 4 voices with a step of up to 15% full scale. Each play began with a click, most noticeable on frequent sounds such as digging and footsteps. Recording edges now match the originals. Loudness follows the current game recordings with peak headroom. Four distinct noisy recordings retain cleanup only because their reconstruction failed alignment. Music and KeeperFX-only extra recordings are unchanged. See [asset provenance](../../config/mods/dk_modern_audio/NOTICE.md); the engine's GPL license does not relicense the original recordings.

For a reversible original-audio comparison, add `-NativeAudio` to either installed
remaster play launcher, or run `local-dev/play.ps1 -WorkspacePath <workspace>
-NativeAudio`. The session removes only active `dk_modern_audio` entries and
restores the exact prior mod order on exit. Other mods and comments are preserved.
This option requires an installed remaster profile so the launcher can own the
restoration transaction. The normal Modern default remains available.

Mentor performances use `[speecheng]`; other languages retain their native bank
or their own configured overrides. Generic `[speech]` still applies to every
language. A matching `[speechfre]`, `[speechger]`, etc. wins within the same file.
Speech overrides are cleared on baseline loading and saved/restored with the
campaign snapshot. This config requires the corresponding updated engine.

Level sound configuration is loaded once, before creature configurations, in
base / after_base / campaign / after_campaign / map / after_map order. A late
map-file reload no longer changes the result. Save loading explicitly uses the
saved map number for sound configuration, even when a different map is selected
in the frontend. Custom sound buffers, names and redirects are rebuilt before
creature sounds register, so earlier map entries cannot remain in the registry.

Returning to menus loads only the base and enabled mods' global sound files.
Campaign/map files, including those inside mods, are excluded. Global mods in
all three order sections remain enabled; each legacy `mods/<name>/sounds.cfg`
is a fallback before that mod's `fxdata/sounds.cfg`. The rebuilt menu state also
replaces the saved sound snapshot, avoiding references to discarded buffers
when restarting the same campaign. `-NativeAudio` continues to control the
enabled mod list for its session.

A custom sound name is reused only when its resolved file path also matches.
A later layer can therefore replace that name with another file; already-loaded
bank entries retain their indices until the next reset.

`test_speech_language.py` compiles the actual parser under ASan/UBSan and checks
the 125 English mappings, six fallback codes, section matching and snapshot/reset
behaviour. Run it with a C++ compiler in `keeperfx-dev`.
`test_session_audio.ps1` checks the pure mod-order transformation; the remaster
launcher fixture also checks graphics/audio combinations and restoration.

`test_speech_loading.py` compiles the actual configuration loaders/parsers,
SoundManager registry/snapshot methods and custom-bank snapshot functions under
ASan/UBSan. Synthetic file I/O and WAV decoding cover two-map and explicit saved
map loading, root/fxdata mod priority, menu reset, language/native fallbacks,
optional map files, freeplay/fallback path roots and custom creature sample IDs.
It does not exercise playback devices or custom WAV resolution inside map ZIPs.

With Python 3.11 or newer, `audit_original_media.py --original <installation> --runtime <runtime>
--output <new-report-directory>` verifies six original music tracks and three
films against local original data. Add `--ffmpeg <executable> --ffprobe
<executable>` for complete decoding. Integrity, complete traversal and live
playback qualification are separate fields. `test_media_audit.py` verifies that
a changed file or interrupted traversal cannot leave a passing report. No media
or live playback is bundled by these tools.

Engine support includes numeric speech IDs 1-125, campaign-scoped WAV narration overrides and matching mentor-volume scaling. Effect redirects also apply to pan, volume, pitch and stop controls. Bank resets detach finished OpenAL sources before deleting their custom buffers.

Validate the checked-in WAV format, coverage, mappings and hashes using standard Python 3:

    python3 local-dev/audio/test_package.py

The control and buffer regression fixture uses the actual engine functions with synthetic sources and needs a C++ compiler. Run it in the dedicated keeperfx-dev distribution:

    python3 local-dev/audio/test_controls.py

Live validation covered the complete default package, real DX12 voice/effect playback, audio reset and campaign landview WAV routing. The default test preserved settings and the existing visual mods. Targeted OpenGL Eversmile startup, advancing simulation and clean shutdown now pass with sound enabled in English and French, using both fresh and retained settings. The smoke check verifies native-bank and SDL mixer initialization, rather than accepting a silent fallback. The [Windows render-thread regression](../render-thread-test.md) covers synchronous window messages during startup, presentation and shutdown. These checks do not qualify every audio device, audible quality or full campaign playback.

To reproduce the v2.1 package from the retained local masters, use `repair_high_band.py --master-directory <masters> --package-directory <previous-package> --output-directory <new-package>`. The masters directory needs the original `sources/`, the `clean/` and `modern/` float masters and `plan.json`. The script rebuilds each cleaned base from its source and keeps the v1 reconstructed detail. It requires NumPy, SciPy, SoundFile and pyloudnorm, validates frame/channel counts, preserves the protected original band, rejects any added edge step and checks four-times-oversampled peak headroom. Original banks and v1 masters are read-only inputs. Use `test_package.py --package-directory <new-package>` before installing.
