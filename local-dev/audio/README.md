# Modern original English audio

The complete Modern v1 audio package is checked in at config/mods/dk_modern_audio. It includes 734 playback WAVs: 125 in-game English voices, 569 original effect entries and 40 original campaign narrations. The default config/mods/load_order.cfg enables it in [after_base], alongside any configured texture and creature mods. Campaign and map overrides retain their higher priority.

Normal development builds sync the package into runtime/keeperfx; the x64 runtime shares that mods directory. Existing runtimes can install or verify the bundled copy without rebuilding the audio. Run this from the workspace directory after building the updated x64 engine:

    .\keeperfx\local-dev\audio\install-modern-audio.ps1

The runtime defaults to the sibling runtime/keeperfx-x64 directory. Use -RuntimeDirectory to select another matching x64 development runtime, or -PackageDirectory to select a different complete Modern v1 package. Add -WhatIf to validate all 734 checksums and preview the change without installing it. No model download or audio generation is needed.

The installer verifies every audio file before activation, preserves other mod entries, backs up both mod orders and enables dk_modern_audio last in [after_base]. It copies the provenance notice and manifest with the audio. Original sound banks remain intact. No separate audio launcher is required; launchers sharing this runtime use the same default. For an existing installation with custom mod choices, use the installer to merge the new default before a development config sync.

The treatment uses gentle cleanup and 65% reconstructed upper-band detail at 48 kHz, preserving the original performances, recording length and channel layout. Loudness follows the current game recordings with peak headroom. Four distinct noisy recordings retain cleanup only because their reconstruction failed alignment. Music and KeeperFX-only extra recordings are unchanged. See [asset provenance](../../config/mods/dk_modern_audio/NOTICE.md); the engine's GPL license does not relicense the original recordings.

To select the original audio again, remove or comment out dk_modern_audio in both the runtime and source config/mods/load_order.cfg files. Keep the audio directory to enable it again later.

Engine support includes numeric speech IDs 1-125, campaign-scoped WAV narration overrides and matching mentor-volume scaling. Effect redirects also apply to pan, volume, pitch and stop controls. Bank resets detach finished OpenAL sources before deleting their custom buffers.

Validate the checked-in WAV format, coverage, mappings and hashes using standard Python 3:

    python3 local-dev/audio/test_package.py

The control and buffer regression fixture uses the actual engine functions with synthetic sources and needs a C++ compiler. Run it in the dedicated keeperfx-dev distribution:

    python3 local-dev/audio/test_controls.py

Live validation covered the complete default package, real DX12 voice/effect playback, audio reset and campaign landview WAV routing. The default test preserved settings and the existing visual mods. A normal OpenGL run loaded the campaign and initialized audio but timed out during shutdown; full OpenGL shutdown qualification remains unresolved.
