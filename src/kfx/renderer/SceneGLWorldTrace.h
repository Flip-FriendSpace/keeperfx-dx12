// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
// Owned inputs of the executed OpenGL world pass, before world/UI overlays,
// swipe and lens composition. This is separate from indexed software replay.
#pragma once
#include "kfx/renderer/WorldScene.h"

namespace kfx::render {
inline constexpr uint32_t SceneGLMissingTile = UINT32_MAX;
enum class SceneGLWorldTargetKind : uint8_t { Screen = 0, LensSource = 1 };
struct SceneGLTerrainVertex {
    float u = 0, v = 0, shade = 0, stl_x = -1, stl_y = -1;
    bool operator==(const SceneGLTerrainVertex&) const = default;
};
struct SceneGLTerrainTriangle {
    uint32_t tile = SceneGLMissingTile;
    std::array<SceneGLTerrainVertex, 3> vertices{};
    bool operator==(const SceneGLTerrainTriangle&) const = default;
};
struct SceneGLShadowSource {
    uint16_t width = 0, height = 0;
    std::vector<uint8_t> coverage;
    bool operator==(const SceneGLShadowSource&) const = default;
};
struct SceneGLShadowVertex {
    float x = 0, y = 0, u = 0, v = 0;
    bool operator==(const SceneGLShadowVertex&) const = default;
};
struct SceneGLShadow {
    SceneObjectId caster;
    uint32_t entry = 0, source = 0;
    std::array<SceneGLShadowVertex, 4> vertices{};
    float z_ndc = 0, darken = 0;
    bool operator==(const SceneGLShadow&) const = default;
};
struct SceneGLWorldTrace {
    bool present = false, complete = false;
    SceneGLWorldTargetKind target_kind = SceneGLWorldTargetKind::Screen;
    uint32_t native_width = 0, native_height = 0;
    int32_t viewport_x = 0, viewport_y = 0, viewport_width = 0, viewport_height = 0;
    std::array<float, 4> clear_rgba{}; // Actual glClearColor input, not inferred palette colour.
    std::array<uint8_t, 1024> palette_rgba{};
    std::array<uint8_t, 16384> fade{};
    // Actual applied uniforms: fullbright, ambient, scale, gamma, lighting
    // mode, darkness mode, fog speed, fog density, tile filter.
    std::array<float, 9> shade_settings{};
    float time = 0;
    std::vector<uint16_t> lightmap; // Exactly 511*511, original R16UI uploads.
    // Byte-deduplicated in first-use order, including versions of animated
    // tiles sampled before/after uploads within the same command traversal.
    std::vector<std::array<uint8_t, 1024>> tiles;
    // Exactly parallel to outline.occluders, which own XY/Z/W and bucket identity.
    std::vector<SceneGLTerrainTriangle> terrain;
    // First-use deduplicated logical source masks from actual atlas uploads.
    // Outside logical bounds is defined zero, never uncaptured atlas padding.
    std::vector<SceneGLShadowSource> shadow_sources;
    std::vector<SceneGLShadow> shadows;
    bool operator==(const SceneGLWorldTrace&) const = default;
};
}
