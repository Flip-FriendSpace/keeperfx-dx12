# Developer guide

This is an unofficial Windows x64 and DirectX 12 single-player preview based on [KeeperFX](https://github.com/dkfans/keeperfx). OpenGL remains available. There are no packaged binaries for this fork yet. The upstream starting revision is `fb0ab7f4d152286d25e73189530ed5536a360478`; see also the upstream [building guide](https://github.com/dkfans/keeperfx/wiki/Building-KeeperFX).

## Reviewed remaster defaults

The [combined remaster installer](remaster/README.md) adds the completed V13 creature, world, hand, HUD, frontend and font review using lossless generated-asset bundles. Installation makes the reviewed profile the default for `play.ps1` and the root `Play KeeperFX.cmd`: western text at 80% above the low-resolution boundary, enhanced art, smooth lighting and the accepted emissive treatment. `-Original` selects the art comparison for one session. Normal play keeps the API and GPU validation disabled.

The installer reconstructs native fallback pixels from your own matching game data, verifies all resulting hashes and retains recoverable installation backups. Frontend overlays and mod order are restored after each game. See the remaster guide for dependencies, arbitrary runtime paths, recovery, coverage and open validation limits.

## Enhanced original terrain

The DX12 build installs the bundled 601-tile original terrain enhancement and loads it automatically. The 24 relief exceptions and seven solid/debug cells remain native. Use `play.ps1 -DX12Preview -NativeTerrain` to opt out for a session. See the [terrain guide](terrain/README.md) for the installer, actual asset bundle, tests and the open broader GPU stress limitation.

## Workspace and prerequisites

The PowerShell wrappers expect a source checkout and writable sibling directories:

```text
workspace/
  keeperfx/                 source checkout
  runtime/keeperfx/         matching upstream runtime and original game data
  runtime/keeperfx-x64/     local x64 executables, dependencies and saves
  downloads/               hash-verified dependency archives
  artifacts/               local build logs and test output
```

Run examples from `workspace/`. The default sibling directory names follow this layout. Build, dependency, play and smoke wrappers also accept `-WorkspacePath` for an isolated source worktree; `play.ps1` additionally accepts `-RuntimePath` for a separately installed runtime. Keep the workspace path reasonably short: the engine's log path buffer is 144 bytes and the preview launcher checks its full log path before starting.

Use Windows with WSL2, PowerShell 7, Git, `curl.exe` and 7-Zip for the runtime archives. Node.js is needed for live test clients; Python 3 is needed for the source/schema tests. Windows 11 and an RX 7900 GRE are the tested DX12 environment. Other hardware and Windows configurations need qualification.

Create a dedicated Ubuntu 24.04 WSL2 distribution named `keeperfx-dev`. The wrappers address it explicitly and do not create it. Obtain an official Ubuntu root filesystem, verify its publisher-provided checksum and use `wsl --import` with a new storage directory. No root filesystem or preinstalled WSL environment is included. Do not reuse an existing personal distribution.

Open the dedicated distribution with `wsl -d keeperfx-dev -u root`. Inside it, install the prerequisites and select the Win32-thread-model cross-compilers:

```bash
apt-get update
apt-get install build-essential g++-mingw-w64-i686 g++-mingw-w64-x86-64 cmake ninja-build libpng16-16t64 git ca-certificates curl unzip zip pkg-config clang-18 python3-clang-18
update-alternatives --set i686-w64-mingw32-gcc /usr/bin/i686-w64-mingw32-gcc-win32
update-alternatives --set i686-w64-mingw32-g++ /usr/bin/i686-w64-mingw32-g++-win32
update-alternatives --set x86_64-w64-mingw32-gcc /usr/bin/x86_64-w64-mingw32-gcc-win32
update-alternatives --set x86_64-w64-mingw32-g++ /usr/bin/x86_64-w64-mingw32-g++-win32
```

Build caches live under `/opt/keeperfx-build/` on the Linux filesystem. Run the dedicated distribution as a user who can write there. Debug and DX12 builds use separate caches.

## Runtime assets and dependencies

Supply these matching upstream archives yourself. Verify their SHA256 hashes, extract the complete release into `runtime/keeperfx/`, then overlay the alpha patch:

| Archive | SHA256 |
|---|---|
| [KeeperFX 1.4.0 complete](https://github.com/dkfans/keeperfx/releases/download/v1.4.0/keeperfx_1_4_0_complete.7z) | `82D9D5634E8EA6CABB8F62FBD4830B758919893863CD313517AF256A3040A279` |
| [1.4.0.5491 alpha patch](https://keeperfx.net/download/alpha/keeperfx-1_4_0_5491-Alpha-patch.7z) | `0F04767A0A4F82CACC9C9FDC561D84ABB39886FE94AD25AA312DED47E5208A00` |

The scripts pin build number 5491; updating it requires matching the source and asset versions. The runtime must contain `keeperfx.exe` and the usual game directories. No existing local cache is assumed.

A legally obtained copy of Dungeon Keeper is required. Import its required original data and available music/movies using your own installation path:

```powershell
./keeperfx/local-dev/import-original.ps1 -OriginalPath '<your Dungeon Keeper installation>'
```

Initialize a missing runtime configuration from `keeperfx/config/keeperfx.cfg`. Back up existing saves, settings and campaign progress first. Keep experimental builds separate from your ordinary installation.

Dependency preparation downloads the versions in the checked-in JSON manifests and verifies their SHA256 hashes:

```powershell
./keeperfx/local-dev/prepare-dependencies.ps1 -Architecture x64
./keeperfx/local-dev/prepare-dx12-dependencies.ps1
```

The build wrapper also performs these steps. The x64 dependency cache includes the Linux `png2ico` icon tool; no x86 build/cache is required for an x64 build. DX12 preparation supplies the pinned Agility SDK, DirectX headers, shader compiler and D3D12 Memory Allocator. `prepare-x64-runtime.ps1` shares large asset directories with the base runtime through junctions while keeping x64 executables, saves, replays and logs separate. The build deploys required DLLs, compiled shaders and dependency notices.

The remastered original English recordings are included with a separate provenance notice. Other original game data, independently installed texture packs, runtime packages, captures, screenshots, saves and private settings are not part of this release. Keep them outside the checkout and do not publish them with test output.

## Build and play

Close KeeperFX before building or switching executables.

```powershell
./keeperfx/local-dev/build.ps1 -Architecture x64
./keeperfx/local-dev/build.ps1 -Architecture x64 -DX12
./keeperfx/local-dev/play.ps1 -DX12Preview
./keeperfx/local-dev/play.ps1 -DX12Preview -QuickStart
./keeperfx/local-dev/play.ps1 -DX12Preview -OpenGLFallback
```

The default build produces `keeperfx-local.exe`; DX12 produces `keeperfx-dx12.exe` in the x64 runtime. DX12 is x64-only and becomes the default launcher backend after installing the combined remaster profile; without that profile it remains an explicit `-DX12Preview` choice. `-Jobs` controls build parallelism and `-Configuration Debug` selects a separate cache. The retained x86 path uses `-Architecture x86` without `-DX12`.

The preview launcher creates `keeperfx-dx12-preview.cfg` from the normal configuration on first use. It preserves other preview preferences while selecting `DX12` or `OPENGL`, `API_ENABLED=FALSE` and `REPLAY_MAX_SIZE=131072` (128 MiB). It leaves normal `keeperfx.cfg` untouched. Saves, game settings and renderer preferences remain shared user state, not a disposable profile.

Preview launches disable inherited diagnostic controls and DX12 validation. Each writes a unique backend-named log under `runtime/keeperfx-x64/preview-logs/`; an existing default log is archived before native logging removes it. The fallback restarts the same executable with OpenGL. Close the current process first; fallback cannot recover unsaved progress.

## Checks and measurements

Live campaign checks require your prepared runtime and original assets. They temporarily enable the loopback API for their own session:

```powershell
./keeperfx/local-dev/smoke.ps1 -Architecture x64 -Renderer DX12 -Sound
./keeperfx/local-dev/smoke.ps1 -Architecture x64 -Renderer OPENGL -Sound
./keeperfx/local-dev/state-test.ps1 -Architecture x64 -Renderer DX12
./keeperfx/local-dev/dx12-foundation.ps1 -RuntimeWorldTest -GpuValidation -NoVSync
```

Read each runner before use. Live wrappers refuse an existing game/API listener and reserved save-slot conflicts, preserve their declared configuration/settings/save scope and write to sibling `artifacts/`. Keep the API loopback-only and disabled for normal play. Local codec/resync tests do not qualify multiplayer sessions.

GPU validation needs Windows Graphics Tools. `dx12-install-tools.ps1` explicitly installs that development prerequisite with administrator privileges; ordinary preview play does not enable validation. Procedural GPU/CPU fixtures and source tests are in `local-dev/`. Reference workflows must generate their captured inputs locally; no historical capture corpus or test report is supplied.

`render-benchmark.ps1` supports live DX12 workloads, separate warm-up/measurement windows and bounded cycles/reloads. It disables debug/GPU validation and restores its declared test state. VSync-on runs require eligible display-power endpoint checks. Application Present-return intervals are not displayed FPS; independently measure display events before making a pacing claim.

## Compatibility and known limits

- Back up old saves and start a fresh campaign. Legacy raw-memory saves and old replay versions are rejected without migration. Current replays use version 9. Mixed-architecture replay/network sessions and incompatible Lua bytecode frames are deliberately rejected.
- The preview enforces a 128 MiB replay cap. Existing normal configurations can retain an older cap; a mid-game snapshot can exceed a smaller limit.
- Replay integrity covers additive thing-position/heading/owner, slab and dig-task data, not full simulation-state equality. Only Lua `Game` table data is serialized; arbitrary VM random state, other globals and upvalues are not preserved.
- Native 3840 x 2160 training and combat measurements on the RX 7900 GRE met the scoped 60 FPS target in 120-second and 10-second display windows. The 120 FPS stretch target, wider hardware and all-map performance are unqualified. Outliers still occur.
- Strict validation on the tested AMD integrated GPU retains DXGI warning 1424 during resize. Validation-disabled fixture success does not qualify full iGPU gameplay. Physical TDR, automatic device recovery and broad multi-monitor/DPI behavior are unqualified.
- A shared shutdown light-list diagnostic can appear despite normal exit. Specialised localisation, tooltip/hand variants and full movie/audio coverage are not exhaustive.
- This preview is single-player only. Multiplayer, network resync and interruption qualification are deferred; existing network code has not been removed.

## Licensing

KeeperFX source is GPL version 2 or, at your option, any later version; see [LICENSE](../LICENSE) and source headers. Preserve upstream contributor credits and dependency notices. The source licence does not grant redistribution rights to original Dungeon Keeper data or independently installed assets.

## Modern original English audio

[Modern original English audio](audio/README.md) is included as 734 remastered WAVs and enabled by the default mod order. Development builds deploy the bundled package; the installer merges it into existing mod choices without replacing original sound banks. It combines with texture and creature mods, while later campaign/map overrides retain priority. See the separate [recording provenance notice](../config/mods/dk_modern_audio/NOTICE.md).

## Enhanced creature sprite pipeline

[Creature sprite tools](art-prototype/README.md) include the approved enhanced PNG frames and assembly metadata under `assets/creatures/`. The installer combines them with original fallback frames from your prepared compatible runtime; original pixels, extracted references, source sheets, runtime ZIPs and local reports stay outside Git. No image generation or pre-existing art artifacts are needed to install.

Run `powershell -NoProfile -File local-dev/art-prototype/install_enhanced_sprites.ps1 -RuntimePath C:\Games\KeeperFX` with Python 3.10 or newer and Pillow installed. It verifies and stages the mod, keeps dated backups, preserves texture/audio/base mod choices and enables `enhanced_creatures` under `[after_map]`. It does not launch the game or modify API settings. See the [installation guide](art-prototype/README.md) and [asset notice](../assets/creatures/NOTICE.md).

The user accepted the thirteen-hero roster visual review on 2026-10-15 ("This looks great"); ordinary campaign play remains pending. Development extraction/review tools additionally use the documented workspace layout, NumPy, Node and PowerShell. Scope and limits remain in the [workflow](art-prototype/WORKFLOW.md) and [acceptance registry](art-prototype/prototype.json).
