# Generated UI reconstruction inputs

This bundle reproduces selection
`9a285bacf69a60ea22bd0974c915ed37f1d5ab1b93bf20b7a0bc93390b0065de`:
14 sparse UI banks (2,632 records) and 49 frontend overlays. The generated
pixels of the 22 campaign-map frames (`viframe00` to `viframe21`) are
colour-matched to the original frames; `colour_match` in the manifest records
that pass. Native pixels and frame coverage are unchanged. The main-menu cursor (frontbit sprite 1) is a faithful 4x upscale recomputed from the original sprite by the `faithful` operator; `record_updates` records that change, and no pixels are stored for it.

`manifest.json` binds every installed source, generated object and exact output.
The content-addressed zlib objects contain generated registered RGBA, glyph
alpha, material fields, indexed artwork, RGB fields, luminance relief, or reconstruction metadata. Native
palettes, masks, shadows, fallback pixels and spatial colour fields are supplied
by the user's verified local game installation. Final runtime composites,
commercial reference images, captures and runtime archives are not included.

The accepted restoration adds 17 portraits, 22 native campaign-map states,
88 campaign-marker records and 13 controls. `restoration_update` preserves the
previous selection identity. All earlier sprite records and native overlays
remain byte-identical. New recipes reconstruct the original colour fields,
glyphs, native masks and state semantics locally; only generated RGB/material
detail is stored. The torture tower and remaining native UI slots are excluded.

Use the [reconstruction guide](../../local-dev/world-remaster/frontend-art/PUBLICATION.md)
and pinned dependencies. The tool produces a separate staging directory and
checks all output bytes and decoded pixels before writing. Installation and
runtime validation belong to the shared integration workflow.
