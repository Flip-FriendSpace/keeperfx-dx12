// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
// Owned OpenGL outline diagnostics. Separate from indexed palette replay.
#pragma once
#include <array>
#include <cstdint>
#include <vector>

namespace kfx::render {
inline constexpr uint32_t SceneOutlineNoDraw = UINT32_MAX;
struct SceneOutlineVertex {
    // Actual GL vertex inputs, including bucket-derived NDC Z and camera Z.
    // Production shader emits (x*w,y*w,z*w,w), preserving its float arithmetic.
    float x = 0, y = 0, z = 0;
    float w = 1;
};
struct SceneOutlineOccluder {
    uint32_t entry = 0;
    std::array<SceneOutlineVertex, 3> vertices{};
};
enum class SceneOutlineExecutionPath : uint8_t { Instanced = 0, AtlasFallback = 1, TextureFallback = 2 };
struct SceneOutlineSprite {
    uint32_t draw = 0; // Existing owned SceneSpriteDraw; never an engine pointer.
    int32_t dst_x = 0, dst_y = 0, dst_w = 0, dst_h = 0;
    int32_t phase_x = 0, phase_y = 0, step_x = 0, step_y = 0;
    float z_ndc = 0, outline_z_ndc = 0, source_alpha = 1;
    uint64_t sort_key = 0;
    uint32_t flags = 0;
    int8_t owner = -1;
    bool wants_outline = false, additive = false, darkens = false;
    // Actual GL allocation padding is not owned. This flags covered right or
    // bottom source borders where defined zero-padding may change edge output.
    bool source_border_covered = false;
    SceneOutlineExecutionPath path = SceneOutlineExecutionPath::Instanced;
    std::array<float, 4> owner_rgba{};
    // Source dimensions, content clipping, remap and full logical coverage
    // come from draw. Coverage outside that full sprite is always zero;
    // neighbours beyond content_height still read the owned full sprite.
};
enum class SceneOutlineCommandKind : uint8_t { Outline = 0, Sprite = 1, GlowDarken = 2 };
enum class SceneOutlineColourResolution : uint8_t {
    Glow = 0, ClutIdentity = 1, ClutRemap = 2, ClutCapacityIdentity = 3,
    TextureIdentity = 4, TextureRemap = 5
};
struct SceneOutlineColourBinding {
    uint32_t lookup_row = UINT32_MAX;
    SceneOutlineColourResolution resolution = SceneOutlineColourResolution::Glow;
};
struct SceneOutlineCommand {
    SceneOutlineCommandKind kind = SceneOutlineCommandKind::Sprite;
    uint32_t sprite = 0, group = 0;
};
struct SceneOutlineTrace {
    // V7/v9 extension, GL backend only. This explicitly describes RGB
    // composition, not a replacement for original indexed palette operators.
    bool present = false, complete = false;
    uint8_t mode = 0; // 0 none, 1 silhouette, 2 source-texel cardinal edge.
    float alpha = 0;
    // Actual GL palette transform used by glow. Row-major 3x3.
    std::array<float, 9> palette_transform{};
    // Canonical GL D24 depth, depth range [0,1], clear value 1. NDC Z maps
    // to window Z by (Z+1)/2. Actual opaque order; depth writes use ALWAYS. Sprite depth is
    // read-only, outline GREATER and normal LEQUAL. All occluders precede
    // these commands; creature shadows are outside this diagnostic extension.
    std::vector<SceneOutlineOccluder> occluders;
    std::vector<SceneOutlineSprite> sprites;
    // Actual instance order. Each flush group emits outlines before normal
    // sprites/glow-darken runs. Groups can skip excluded non-owned draws.
    std::vector<SceneOutlineCommand> commands;
    // V9 owns the effective submitted lookup bytes, including cache-capacity
    // identity fallback and initialized/stale row zero. Never infer these from
    // requested remaps or the frame palette. Rows are deduplicated in first-use
    // order; bindings parallel sprites. TextureFallback ignores lookup alpha.
    std::vector<std::array<uint8_t, 1024>> colour_rows;
    std::vector<SceneOutlineColourBinding> colour_bindings;
};
}
