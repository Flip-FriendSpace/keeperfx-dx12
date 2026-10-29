# Call to Arms owner artwork

These tools prepare local review candidates from legally installed originals and
separately generated, inspected masters. They never install or select a mod.
Python 3.11+, Pillow and NumPy are required. Do not run Python with `-O`.

`restore.py prepare --source <original-source-manifest.json> --output <new-folder>`
creates a source register and seven owner-specific lifecycle references. Each
reference has nine columns and six rows: TD/FP birth, TD/FP alive, TD/FP leave.
Native phase lengths are 7/8/9. Empty cells and incomplete emergence/collapse
remain empty or incomplete. White/purple/black/orange are the installed custom
colour families; red/blue/green/yellow originate in the original sprite bank.

Create the seven transparent raster masters from their corresponding references,
retaining the complete sheet layout. Do not recolour red or share one owner's
mask with another. Keep prompts and generated source files in local artifacts.
Place the inspected masters in `<masters>/<colour>-lifecycle.png`.
The manifest binds each selected master by SHA256. Keep the generation service,
prompt, original reference and generated-output path in a separate local ledger;
the portable tool does not require access to a model or a remote service.

```
python -B restore.py package --register <source-register.json> --masters <masters> --red-manifest <reviewed-red/manifest.json> --palette <data/png_conv_pal.dat> --display-palette <data/palette.dat> --output <new-candidate-folder>
python -B verify.py --manifest <new-candidate-folder/manifest.json> --register <source-register.json> --red-manifest <reviewed-red/manifest.json> --report <new-candidate-folder/verification.json> --review
```

Packaging retains red PNG bytes and sprite metadata exactly. Each other owner's
sheet gets one uniform registration transform; every output uses that owner's
original alpha, native canvas, anchor and phase order. Uncovered generated pixels
use original pixels before the shared sequence colour correction; their final
colours are not claimed byte-identical. Whole-sequence stationary source pixels remain
stationary. A fixed RGB gain and a pooled monotone lightness curve use the original
display-palette colours for each complete sequence. PNG encoding uses
`png_conv_pal.dat`; comparisons show `palette.dat`. Both palettes are hash-bound.
No per-frame normalization, per-frame fitting or invented intermediate poses occur.
The archive is deterministic for identical inputs. `verify.py` independently
checks all 384 frame uses, including red, and the complete owner mapping.
Both commands refuse existing outputs. A failed package may leave a partial
directory for diagnosis; only a completed manifest followed by successful
independent verification qualifies a package. Use a fresh versioned directory
for a retry and retain the failed output until it has been examined.

The output is an opt-in mod with only a sprite ZIP and `spritecolors.toml`.
Neutral remains zero. Object definitions, powers, upkeep, attraction, speed and
gameplay configuration are untouched. Preview loops use 50 ms per frame, while
the engine continues to own simulation timing. Exact moving RGB differences and
human appearance acceptance are separate from mask/geometry checks.

Keep originals, generated sheets, packages, captures and review reports outside
Git. A coordinator must run the disposable live lifecycle fixture on the exact
candidate/build pins and verify restoration before considering promotion.
