// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
// Owned final terrain submissions for diagnostic painter-order replay.
// These projected primitives accompany, rather than replace, WorldScene's
// world-space columns and the modern terrain mesh.
#pragma once
#include "kfx/renderer/WorldScene.h"
#include <array>
#include <cstdint>
#include <vector>

namespace kfx::render {
struct SceneRasterVertex {
    // Graphics-window-local integer pixels, exactly as submitted to C.
    int32_t x = 0, y = 0;
    // Original 16.16 values. Keep their low bits even when a particular
    // rasterizer truncates them; Gpoly and Trig do not use identical rules.
    // Exception: Trig mode 7 ignores vertex S and producer subdivisions leave
    // it uninitialized. Its captured s MUST be zero, without reading source S;
    // the actual constant shade is SceneTerrainTriangle::colour.
    int32_t u = 0, v = 0, s = 0;
};

enum class SceneRasterizer : uint8_t { Gpoly = 0, Trig = 1 };

struct SceneTerrainTriangle {
    uint32_t entry = 0; // Index in SceneDrawTrace::entries.
    SceneRasterizer rasterizer = SceneRasterizer::Gpoly;
    uint8_t mode = 0; // Original vec_mode, not a modern material classification.
    uint8_t colour = 0; // Mode-7 vec_colour shade; zero for mode 5, which ignores it.
    // Byte offset of vec_map into SceneAssets::texture_pixels. This preserves
    // animated texture selection and the original bank's row layout. It is
    // neither a logical tile ID nor a process address. Oracle consumers must
    // validate every address range their specific rasterizer mode can access.
    // Gpoly mode 5 uses the original 256-byte row pitch and mask 0x1f1f:
    // the maximum relative address is 7967, not a densely packed 32x32 tile.
    // Trig mode 7 normally uses 8-bit U and 5-bit V (8192-byte span).
    // Colour 32 instead selects mode 2: bypass fade and use full 8-bit U/V,
    // requiring a 65536-byte span. Vertex S remains ignored in both cases.
    uint32_t texture_offset = 0;
    // Original argument order, before rasterizer sorting and edge setup.
    std::array<SceneRasterVertex, 3> vertices{};
};

struct SceneShadowMask {
    // Only the rectangle actually initialized by the original mask decoder.
    // Never persist unrelated bytes from the shared scratch buffer.
    uint16_t width = 0, height = 0;
    std::vector<uint8_t> coverage; // Tight rows; canonical 0 or 255.
};
struct SceneShadowTriangle {
    SceneObjectId caster;
    uint32_t entry = 0, mask = 0;
    uint8_t shade = 0; // Actual mode-10 destination fade row, currently 16..31.
    // Original XY/UV argument order. S is ignored and must be canonical zero.
    std::array<SceneRasterVertex, 3> vertices{};
};

enum class SceneDrawCommandKind : uint8_t { Sprite = 0, Triangle = 1, Shadow = 2 };

struct SceneDrawCommand {
    SceneDrawCommandKind kind = SceneDrawCommandKind::Sprite;
    // Sprite indexes SceneDrawTrace::draws; Triangle indexes
    // SceneRasterTrace::triangles. Each payload appears exactly once.
    uint32_t index = 0;
};

struct SceneRasterTrace {
    std::vector<SceneTerrainTriangle> triangles;
    // Actual submission order across both payload types. Never reconstruct it
    // by sorting buckets or concatenating per-kind arrays: ties and expanded
    // front-view/possession primitives must preserve producer order.
    std::vector<SceneDrawCommand> commands;
    // v5 software diagnostic shadow payloads. A bucket owns one mask and two
    // ordered triangles. Excluded casters never acquire identities or masks.
    uint32_t excluded_shadow_entries = 0;
    std::vector<SceneShadowMask> shadow_masks;
    std::vector<SceneShadowTriangle> shadows;
};
}
