# Gameplay preservation checks

These tools observe selected gameplay state independently of rendering and the
existing replay checksum. They do not change creature statistics, room costs,
simulation timing, save formats or replay protocols.

## Selected gameplay snapshots

`src/fidelity_state.c` writes deterministic integer JSON without changing `Game`,
calling simulation functions, invoking Lua or consuming random numbers. It
observes synchronized thing identity, position and health; creature experience,
hunger, anger, pay, jobs, spell durations and cooldowns; gold, research,
manufacturing and unlocks; rooms and slabs; original script conditions, triggers,
flags and timers; synchronized RNG; and player outcomes. Presentation timers,
cameras, sound and unsynchronized effects are excluded.

This is selected-domain equivalence. Arbitrary Lua globals/upvalues and VM RNG,
complete pathfinding state, every object union and every AI task are outside the
schema. Keep replay checks and domain-specific observations alongside it. A
matching snapshot is meaningful only when the run manifest also binds the
initial save, map, configuration, mod order, executable, source revision, seeds,
input stream and exact sampling turns.

For an owned disposable local test process, set `KFX_STATE_TEST=1` and
`KFX_FIDELITY_DIR` to an existing absolute output directory. Keep its API
loopback-only. Pause at the intended turn, then issue:

```text
statecheck fingerprint before-action
```

The command refuses normal sessions without the environment flags, multiplayer,
unpaused state, unsafe labels, overlong paths and existing output files. It does
not create the directory. The path must fit the engine's `DISKPATH_SIZE` limit;
choose a short output directory. Normal play keeps the API disabled.

The optional client only connects to an already-owned test process; it does not
launch, pause, advance or reconfigure the game:

```powershell
node local-dev/fidelity/capture.mjs C:/tests/fidelity before-action 100
node local-dev/fidelity/compare.mjs reference.json candidate.json comparison.json
```

Comparison requires the same level and exact turn, reports field differences
and per-domain hashes, and exits unsuccessfully on divergence. The first 100
differences are displayed while the full difference count is retained.

## Original campaign matrix

`original-campaign.json` inventories the original 20 maps, five secret maps and
moon bonus map by installed configuration/script hashes. The checked-in file is
an unqualified template: every map and mechanic domain starts `not_run`. Keep
your run-specific matrices and evidence outside the repository. No original
map data, save, image, runtime binary or captured gameplay is included here.

`mechanic-fixtures.json` proposes fourteen source-backed scenarios with expected
observations. Reach each result through ordinary simulation after recorded
setup; do not force the post-action state or alter game balance to obtain a pass.
Disputed historical behavior needs an original Dungeon Keeper Gold reference;
two graphical launchers of this fork are not independent historical references.

Check the template against your legally installed campaign files:

```powershell
node local-dev/fidelity/campaign-check.mjs C:/games/keeperfx
```

For a run-specific matrix, add `--matrix C:/tests/run-matrix.json --evidence-root
C:/tests`. Evidence paths in that matrix are relative to the supplied root.
An optional positional report path must be new.

The inventory check verifies content identity. It does not complete a map or
qualify gameplay. Map completion needs the matrix's hash-bound start, script/wave,
save/load, terminal-outcome and campaign-progress checks.

## Existing acceptance evidence

The repository does not ship the local renderer coverage inventory or its
private captures. Validate an external plan and its existing evidence with:

```powershell
node local-dev/fidelity/validate-coverage.mjs --plan C:/tests/coverage.json --workspace C:/tests
```

The plan contains `gates` and `pendingReferences` with `validationPlan` records.
Passed plans must reference a passed parent gate, supporting evidence and a
SHA256-bound acceptance document for the same gate. Gate 7 and Gate 8 preserve
their explicitly scoped single-player acceptance statuses. Paths must remain
inside the supplied evidence root. This command verifies existing acceptance
metadata only; it always reports `establishesNewAcceptance: false` and
`gate1Passed: false`, even when the supplied documents are valid. It does not
check source symbols, frame identity or actual coverage. `--output` writes a new
report and refuses an existing file.

## Reproducible isolated tests

Run the JavaScript checks with Node.js:

```powershell
node --test local-dev/fidelity/*.test.mjs
```

`test-speech-names.py` compiles the actual speech enum, name table, lookup, parser,
snapshot/reset and generic configuration loader under ASan/UBSan using a C++17
compiler. It tests 109 named and 125 numeric mappings, case folding, language
selection and precedence, and modeled map/campaign reload sequences. File I/O
and playback are in-memory fixtures; this is not a live audio test. By default
all generated files use a temporary directory. `--output NEW_DIRECTORY` retains
the source, log and source-hash receipt without overwriting earlier evidence.

In the dedicated `keeperfx-dev` WSL distribution, from the checkout:

```sh
python3 local-dev/fidelity/test-speech-names.py
bash local-dev/fidelity/build-snapshot-test.sh /tmp/keeperfx-fidelity-new
```

The snapshot builder expects the prepared MinGW x86/x64 compilers and SDL
headers under `/opt/keeperfx-build/RelWithDebInfo/deps`; override that dependency
root using `KFX_TEST_DEPS_ROOT`. Its output directory must be new. It does not
modify build caches or deploy executables. A test-only version header in that
output directory permits compilation before configuring a clean checkout; the
writer does not use version metadata. Run both resulting Windows test
executables with distinct new JSON paths, then compare their outputs:

```powershell
snapshot-test-i686.exe C:/tests/fidelity/x86.json
snapshot-test-x86_64.exe C:/tests/fidelity/x64.json
node local-dev/fidelity/compare.mjs C:/tests/fidelity/x86.json C:/tests/fidelity/x64.json
```

These tests cover 17 gameplay mutations, presentation exclusions, unchanged
source memory, architecture-independent output, malformed counts and exclusive
file creation. They do not substitute for full campaign, arbitrary Lua,
multiplayer or historical-executable qualification.
