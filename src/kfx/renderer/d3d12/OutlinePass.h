// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
// Owned inputs for the outline visibility diagnostic, not a world RGB renderer.
#pragma once
#include <array>
#include <cstdint>

namespace kfx::render::dx12 {
struct OutlineDepthVertex {
    float x = 0, y = 0, z = 0, w = 1; // Original NDC and actual camera_z/W.
};
struct OutlineDepthTriangle {
    std::array<OutlineDepthVertex, 3> vertices{};
};
enum class OutlinePassKind : uint8_t { Silhouette, Edge, VisibleCoverage, NormalSprite, GlowAdd, GlowDarken };
using SpriteColourRow = std::array<uint8_t, 256 * 4>;
inline constexpr uint32_t NoSpriteColourRow = UINT32_MAX;
struct OutlinePassDraw {
    OutlinePassKind kind = OutlinePassKind::Silhouette;
    int32_t x = 0, y = 0, width = 0, height = 0;
    int32_t phase_x = 0, phase_y = 0, step_x = 65536, step_y = 65536;
    uint32_t mask_x = 0, mask_y = 0, source_width = 0, source_height = 0, content_height = 0;
    bool flip = false;
    bool premultiplied = true; // Instanced GL path; fallback uses straight-alpha blending.
    float z_ndc = 0; // Already includes the captured outline bias when applicable.
    std::array<float, 4> colour{}; // Straight input RGBA; blending follows the captured path.
    uint32_t lookup_row = NoSpriteColourRow; // NormalSprite only; actual resolved GL row.
    float source_alpha = 1;
    // Instanced = premultiplied, atlas fallback = neither flag, texture
    // fallback = this flag only. Texture fallback ignores lookup-row alpha.
    bool texture_fallback = false;
};
}
