// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
// Owned raw upload preparation shared by frozen rendering and diagnostics.
#pragma once
#include "Dx12Device.h"
#include "kfx/renderer/SceneAssets.h"

namespace kfx::render::dx12 {
struct SpriteReplayUpload {
    Image atlas;
    std::vector<PaletteQuad> quads;
    std::vector<int32_t> command_quads; // -1 for clipped/empty submissions.
    std::vector<bool> quirk_by_command;
    uint64_t quirk_commands = 0, invisible_commands = 0;
};
SpriteReplayUpload BuildSpriteReplayUpload(const SceneCapture&, std::span<const SceneSpriteDraw>);
PaletteTerrainVertex ConvertIndexedTerrainVertex(const SceneRasterVertex&, bool quantize);
struct IndexedSceneUpload {
    Image atlas;
    std::vector<PalettePrimitive> primitives;
    std::vector<PaletteTriangle> triangles;
    std::vector<PaletteShadowTriangle> shadows;
    size_t clipped_sprites = 0;
    std::array<uint32_t, 12> bucket_counts{};
    uint32_t excluded_sprite_calls = 0, excluded_shadow_entries = 0;
};
// Pure owned-data conversion: no game globals, raster oracle, readback or
// background inference. Caller must separately supply a proven target clear.
// UI buckets 4/5/7/8/10 remain counted, not rendered by this world-only path.
IndexedSceneUpload BuildIndexedSceneUpload(const SceneCapture&, bool quantize_vertices = true);
void QuantizeIndexedSceneUpload(IndexedSceneUpload&, const SceneCapture&);
}
