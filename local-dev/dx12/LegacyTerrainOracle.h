// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
// Diagnostic-only original software terrain rasterizer. Never GPU input.
#pragma once
#include "kfx/renderer/SceneAssets.h"
#include "kfx/renderer/SceneRasterTrace.h"

namespace kfx::render::dx12 {
struct LegacyTerrainInspection {
    // All arrays are viewport-sized. Indices use a zero-filled background;
    // coverage is independently measured (0/255), including opaque index zero.
    std::vector<uint8_t> indices, coverage;
    // Original sampled texture coordinates and 0..63 shade. Gpoly wraps U/V
    // to 0..31. Trig7 wraps U to 0..255 and V to 0..31, except colour 32
    // wraps both to 0..255 and bypasses fade. Its shade is captured constant.
    // Values
    // outside coverage are zero. Synthetic tables expose the C rasterizer's
    // actual samples without reimplementing interpolation or scan conversion.
    std::vector<uint8_t> sample_u, sample_v, shade;
};

// Gpoly mode 5 and Trig mode 7. Throws on unsupported modes, unsafe arithmetic/table
// inputs, or any framebuffer guard mutation. No engine state is accessed.
std::vector<uint8_t> ReplayLegacyTerrainTriangle(const SceneTerrainTriangle& triangle,
    const SceneAssets& assets, uint32_t width, uint32_t height,
    const std::vector<uint8_t>& background);
// Efficient ordered terrain-only run: reuses guarded surfaces and tables,
// checking every command. Use separate calls for runs separated by sprites;
// never reorder a mixed painter stream by collecting all its triangles here.
std::vector<uint8_t> ReplayLegacyTerrainTriangles(std::span<const SceneTerrainTriangle> triangles,
    const SceneAssets& assets, uint32_t width, uint32_t height,
    const std::vector<uint8_t>& background);
LegacyTerrainInspection InspectLegacyTerrainTriangle(const SceneTerrainTriangle& triangle,
    const SceneAssets& assets, uint32_t width, uint32_t height);
}
