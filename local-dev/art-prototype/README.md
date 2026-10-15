# Local creature sprite tools

This directory publishes source tools, review templates and workflow metadata only. Original Dungeon Keeper pixels, extracted references, generated artwork, sprite ZIPs, runtime executables, screenshots and evidence reports are not included. Supply original game data locally and keep all derived commercial-source assets and reports outside Git. Existing source licences and notices remain applicable.

## Layout and prerequisites

Use a checkout named `keeperfx` inside a workspace with sibling `runtime/keeperfx-x64/` and `artifacts/art-prototype/` directories. The scripts resolve those siblings from their own location; they do not download missing game assets or create a complete runtime. The extraction tools also expect source/config files under the checkout's `keeperfx` name. An arbitrarily named worktree needs its own matching layout before these tools can run.

Use Python 3 with Pillow and NumPy, Node.js and Windows PowerShell. Node clients use built-in modules. Game tests additionally need the locally built engine, original game data and palette, and the local diagnostic commands described in [WORKFLOW.md](WORKFLOW.md). Ordinary play requires no API listener; controlled tests verify their loopback listener and owned process.

From the `keeperfx/` checkout:

```powershell
powershell -NoProfile -File local-dev/art-prototype/play_imp.ps1 -QuickStart
powershell -NoProfile -File local-dev/art-prototype/play_heroes.ps1 -QuickStart
```

Workspace `.cmd` shortcuts are not part of this repository. Launchers require already prepared local packages; the heroes launcher verifies the report, marker and content hashes before installing its temporary mod override, preserves base/texture/audio mods, disables the API and restores prior load-order bytes after exit. Keep its window open until the game closes.

## Recorded scope

The imp has 197 enhanced overhead frames and six held frames. The thirteen original heroes have 1,491 overhead frames and 78 held frames. Other possession views and shared effects retain original art. The user accepted the roster visual review on 2026-10-15: "This looks great" (`roster_visual_review_accepted` in [prototype.json](prototype.json)). Ordinary campaign play remains pending. These recorded local checks do not supply the artwork or qualify every campaign, hardware configuration or OpenGL crowd workload.

Follow [WORKFLOW.md](WORKFLOW.md) for preparation, packaging, review and controlled validation. The current checkpoint is [HEROES-CHECKPOINT.md](HEROES-CHECKPOINT.md). Repacking or rerunning evidence aggregation must preserve the explicit visual decision recorded in the registry.
