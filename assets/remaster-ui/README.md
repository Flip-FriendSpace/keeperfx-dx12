# Generated UI reconstruction inputs

This bundle reproduces selection
`dec01d5ddb2414ffccb98d38c1712996694b00aa639a049c8a776add7533bdbf`:
12 sparse UI banks (2,514 records) and 27 frontend overlays. The generated
pixels of the 22 campaign-map frames (`viframe00` to `viframe21`) are
colour-matched to the original frames; `colour_match` in the manifest records
that pass. Native pixels and frame coverage are unchanged.

`manifest.json` binds every installed source, generated object and exact output.
The content-addressed zlib objects contain generated registered RGBA, glyph
alpha, material fields, indexed artwork, or reconstruction metadata. Native
palettes, masks, shadows, fallback pixels and spatial colour fields are supplied
by the user's verified local game installation. Final runtime composites,
commercial reference images, captures and runtime archives are not included.

Use the [reconstruction guide](../../local-dev/world-remaster/frontend-art/PUBLICATION.md)
and pinned dependencies. The tool produces a separate staging directory and
checks all output bytes and decoded pixels before writing. Installation and
runtime validation belong to the shared integration workflow.
