# Enhanced creature sprites

The repository includes the selected enhanced PNG frames and their assembly metadata under `assets/creatures/`. Original Dungeon Keeper pixels, extracted references, source sheets, rejected versions, runtime sprite ZIPs, game captures and local evidence reports are not distributed. See [asset notice](../../assets/creatures/NOTICE.md) for provenance and scope. Existing source licences and notices remain applicable.

## Install into a prepared runtime

Use a compatible KeeperFX runtime with the large-sprite engine support in this branch and your locally supplied original game data. Its `data/` directory must contain `creature.tab`, `creature.jty` and `palette.dat`. Installation assembles the unchanged original fallback views locally alongside the published enhanced frames; no image generation or pre-existing art-prototype artifacts are needed.

Install Python 3.10 or newer with Pillow, then run from the repository:

```powershell
powershell -NoProfile -File local-dev/art-prototype/install_enhanced_sprites.ps1 -RuntimePath C:\Games\KeeperFX
```

The default runtime is `<workspace>/runtime/keeperfx-x64`, a sibling of the checkout. Use `-OriginalDataPath` if your original data lives elsewhere, and `-PythonExecutable` to select a Python executable. Prepared-runtime `data` and `mods` junctions are supported: explicitly selected roots are resolved to physical directories; links inside a mod are rejected. Close KeeperFX before installing. The installer stages and verifies the local mod, preserves an existing verified installation as a dated backup, and enables `enhanced_creatures` under `[after_map]` while preserving texture/audio/base mods. Known older local hero/imp preview entries are replaced to avoid duplicate overrides. The previous load-order bytes are saved as a dated file in the runtime mods directory. It does not launch the game or change API settings; normal preview play keeps the API disabled.

Runtime ZIPs contain original fallback frames and must remain local. For packaging without activating a mod, use the Python CLI directly with `--assets`, `--data` and required `--output` paths. The PowerShell wrapper is for installation and activation in an existing runtime. Workspace `.cmd` shortcuts are not included in this repository.

## Scope and development

The imp has 197 enhanced overhead frames and six held frames. The thirteen original heroes have 1,491 overhead frames and 78 held frames. Other possession views and shared effects retain original art. The user accepted the roster visual review on 2026-10-15: "This looks great" (`roster_visual_review_accepted` in [prototype.json](prototype.json)). Ordinary campaign play remains pending; local fixtures do not qualify every campaign or broad OpenGL crowd performance.

The extraction and generation workflow in [WORKFLOW.md](WORKFLOW.md) is for development and corrections, not installation. Those tools expect a checkout named `<workspace>/keeperfx` with sibling runtime/artifact directories, Python with Pillow/NumPy, Node and Windows PowerShell. Node clients use built-in modules. Review templates require locally prepared comparison data and original references. The installer accepts an explicit runtime path and does not require this historical development layout.

The current checkpoint is [HEROES-CHECKPOINT.md](HEROES-CHECKPOINT.md). Preserve the explicit visual decision when aggregating evidence again. Keep source sheets, original pixels, local reports, runtime packages, saves and build output outside Git.
