# Immutable opt-in review packages

`package-review.py` creates a new versioned local review bundle. It never deploys, modifies runtime assets, changes default report/selection files, deletes output, or declares appearance acceptance. Run only after the declared subset has passed its required review and the shared runtime lease is released.

```
python local-dev/fidelity/package-review.py <absolute-plan.json> --workspace C:/workspace --verify-plan
python local-dev/fidelity/package-review.py <absolute-plan.json> --workspace C:/workspace
```

The first command validates every input and computes the exact output identity without writing. The second repeats that validation, creates fresh owned asset/build directories, verifies each copied member, then writes only the versioned report/selection/proof. The launcher-visible report is written last. Existing output is refused. A failed build retains its partial directories and `package-review-state.jsonl` for inspection; do not point a launcher at partial output.

The base must be the preceding numbered report under `artifacts/remaster-integration/`, pinned by file SHA256 and manifest identity. All HD, Original, native and build members must match their declared closed sets; HD/Original owner markers must agree. Runtime baseline executables, native originals and palettes must also match. Read-only baseline checks permit only the established `campgns`, `ldata`, `data` and `mods` directory junctions from `runtime/keeperfx-x64` to the same-named `runtime/keeperfx` directory. Their exact target and suffix must match; nested/file links are refused. Copied inputs and outputs retain the strict no-link rule. Original and build/shader changes are forbidden. The build is frozen byte for byte under `artifacts/remaster-integration/builds/review-vN/`.

The workspace argument is mandatory so an isolated checkout cannot accidentally select a sibling runtime. The local manifests, reviewed assets and built executables are supplied by the developer; they are not part of this source-only tool. Python 3.10+ and the standard library are sufficient. A normal source clone alone does not contain the inputs needed to build a real review package.

Plan schema (all `source` paths are canonical forward-slash paths relative to the declared workspace; hashes below are placeholders):

```json
{
  "schema": 1,
  "version": "v21",
  "base_report": {
    "source": "artifacts/remaster-integration/combined-full-review-v20.json",
    "sha256": "<exact base report SHA256>",
    "manifest_sha256": "<exact base manifest SHA256>"
  },
  "changes": [
    {
      "variant": "HD",
      "file": "fxdata/imp-sprites.zip",
      "source": "artifacts/<reviewed candidate>/imp-sprites.zip",
      "sha256": "<candidate SHA256>",
      "before_sha256": "<existing HD member SHA256>"
    },
    {
      "variant": "native-frontend",
      "file": "campgns/keeporig_lnd/rgmap00.raw",
      "source": "artifacts/<reviewed candidate>/rgmap00.raw",
      "sha256": "<candidate SHA256>",
      "before_sha256": null,
      "native_control": {
        "original_sha256": "<runtime original SHA256>",
        "palette": {
          "path": "<absolute unchanged runtime .pal path>",
          "expected_sha256": "<palette SHA256>",
          "current_sha256": "<same palette SHA256>",
          "replace_palette": false
        }
      }
    }
  ],
  "evidence": [{"source": "artifacts/<review receipt>.json", "sha256": "<receipt SHA256>"}],
  "review": {
    "summary": "Concrete included scope and limits",
    "included": ["Exact technically passed subsets"],
    "pending": ["Exact combined bundle runtime review", "User appearance acceptance"],
    "user_appearance_accepted": false
  }
}
```

Every change needs the exact previous hash, or `null` only for a genuinely new package member. An existing native overlay retains its baseline contract unchanged; `native_control` may be omitted for it. A new native overlay requires the full explicit contract above and a pinned evidence record covering its original/palette binding. Native destinations are limited to frontend image families: front, loading, global map, campaign landview and victory frame files. A no-op, extra unlisted base member, link/reparse path, traversal, case collision, duplicate JSON key or changed source is rejected.

The output manifest uses canonical JSON (sorted keys, compact separators), excluding only derived root/name/hash/source fields. It binds the exact plan, builder, evidence, changed subset and complete preserved specs. Historical base evidence is retained. Prior coverage/preservation/pending/runtime prose moves into `historical_base_descriptors`; current counts, preserved-byte facts and explicit pending scope describe only this new bundle. Whole-bundle runtime status remains pending in the immutable report; later runtime receipts should be separate records bound to its manifest, not edits to the frozen report.

`python local-dev/fidelity/test_package_review.py` exercises these guards with synthetic bytes in a unique retained temporary directory. Set `KEEPERFX_TEST_OUTPUT` to an explicit artifact directory to choose its parent. It does not read or modify live game assets. Eighteen cases cover preservation, incomplete or changed inputs, unsafe paths, immutable output and the known read-only runtime junction layout; Windows junction cases require Windows.
