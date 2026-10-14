// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
// Integer sampling maps for replaying the captured legacy sprite submissions.
#pragma once
#include "kfx/renderer/SceneAssets.h"

namespace kfx::render {
struct SceneSpriteMapping {
    // Graphics-window coordinates. Vector lengths are the visible rectangle.
    int32_t x = 0, y = 0;
    // Source coordinates in the owned sprite, before remapping. -1 is a hole.
    // Coverage still comes from SceneSprite::pixels, independently of index 0.
    std::vector<int32_t> source_x, source_y;
    // Legacy mirrored solid/remap upscaling also copies a background pixel
    // before each positive RLE run into repeated rows. This is not sprite
    // coverage and cannot be reconstructed from v3's decoded asset. Callers
    // must report it separately when comparing the intended sampling to C.
    bool legacy_row_copy_quirk = false;
};

// Reconstructs the actual software scaling tables and source traversal, rather
// than the approximate inverse SpriteScaleAxis mapping. Empty vectors mean no
// visible sprite pixels. Throws for uninitialized table references and legacy
// arithmetic overflow. See legacy_row_copy_quirk for the destination-copy bug.
SceneSpriteMapping MapSceneSprite(const SceneSpriteDraw& draw, const SceneSprite& sprite);
}
