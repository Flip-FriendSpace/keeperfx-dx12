# Possession-visible creature restoration tools

These source-only tools prepare and package reviewed first-person sprite actions. They do not include original game pixels, generated artwork, runtime archives or a selected art package. The FP bank is used for creature control/passenger views; the front strategy camera uses TD. These are not the controlled creature's weapon-hand sprites.

Install `requirements.txt` in a Python 3.10+ environment. Use an explicit workspace containing your own matching `runtime/keeperfx-x64/data/png_conv_pal.dat` and `palette.dat`. Keep output outside the source checkout. A local combined review manifest must identify each creature config/archive with its SHA256 and contain the source archive paths; a normal source clone does not supply that manifest or the commercial reference data.

From this repository, for example:

```powershell
python local-dev/art-prototype/possession/restore.py inventory --workspace C:/workspace --manifest C:/workspace/artifacts/combined-review.json --output C:/workspace/artifacts/possession
python local-dev/art-prototype/possession/restore.py prepare --workspace C:/workspace --output C:/workspace/artifacts/possession --slug imp
python local-dev/art-prototype/possession/restore.py process --workspace C:/workspace --output C:/workspace/artifacts/possession --slug imp --job idle-d0 --version v1
```

Use the job IDs emitted in `generation-plan.json`; the example ID is not a fixed inventory. `prepare` extracts exact FP reference frames and TD appearance context into a new creature directory. `process` expects an inspected transparent source-guided sheet at the generated path recorded by the plan. Retain every raw result and its prompt. The existing `package_imp_set.py` primitives apply one registration transform to a complete action/direction sequence. Original pose, silhouette, anchors and frame counts remain authoritative; per-frame fitting and changes to creature size are not substitutes for faithful art.

`colour.py` accepts the same `--workspace`, `--output` and `--slug`, plus `--jobs`, `--report-files` and a fresh `--tag`. It fits one source-derived mean-colour grade for complete actions. `tone.py` accepts the same common paths and a fresh `--tag`; it applies one pooled luminance/palette mapping per action to the jobs in `selection.json`. Alpha and geometry are preserved. Mean colour alone is insufficient: inspect shadows, q10/q90/q95/q99/extrema and actual RGB/palette distributions, then compare matched dark and lit game scenes. A glossy material must not be hidden by changing renderer lighting.

Review the emitted contact sheets and loops against original frames at native size. Diagnostic preview timing and pixel statistics do not establish engine-timed animation acceptance. Record each selected job's reviewed report path and SHA256 under `selection.json`'s `jobs` mapping. Then create a fresh candidate archive:

```powershell
python local-dev/art-prototype/possession/restore.py package --workspace C:/workspace --output C:/workspace/artifacts/possession --slug imp --target C:/workspace/artifacts/imp-candidate.zip
```

Only complete selected actions are admitted. Existing image members, TD data, held appearances, frame counts, origins and gameplay config remain unchanged. New FP entries retain original logical size through per-view resolution metadata. Source hashes, alpha, transparent borders, loader limits and deterministic ZIP bytes are checked. Ghost is refused because its additive-index encoding needs a separate workflow. Run without Python `-O`; preservation assertions must stay enabled.

Before selecting a candidate, run isolated loopback-only game checks for size, shadows, facing, action transitions, possession and return to overhead/hand views. Observe affected actions through ordinary simulation as well as directed poses. Preserve native fallbacks for unfinished or rejected actions. Normal play keeps the API disabled. Record exact hashes, reviewed scope, runtime evidence and remaining work; a generated sheet or passing renderer check does not establish appearance acceptance.

Synthetic checks, requiring no game data:

```powershell
python local-dev/art-prototype/possession/test_restore.py
python local-dev/art-prototype/possession/test_tone.py
```

The first covers 15 archive/source/coverage preservation guards; the second covers three palette-distribution cases. They do not replace visual or live review. Portrait, Call to Arms and campaign-map candidate pixels remain local and are not distributed by these tools.
