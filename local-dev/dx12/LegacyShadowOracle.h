// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
// Diagnostic-only original software shadow rasterizer. Never GPU input.
#pragma once
#include "kfx/renderer/SceneAssets.h"

namespace kfx::render {
struct SceneShadowTriangle;
struct SceneShadowMask;
namespace dx12 {
struct LegacyShadowInspection {
    // Zero-background original mode-10 output; independent binary masks.
    std::vector<uint8_t> indices, geometric_coverage, coverage;
    // Actual original mode-2 samples, 0..255, including mask holes. Mode 2
    // shares mode 10's original edge/UV preparation and full-byte wrapping.
    std::vector<uint8_t> sample_u, sample_v;
};
std::vector<uint8_t> ReplayLegacyShadowTriangle(const SceneShadowTriangle& triangle,
    const SceneShadowMask& mask, const SceneAssets& assets,
    uint32_t width, uint32_t height, const std::vector<uint8_t>& background);
// Ordered shadow-only run. Reuses targets, probes and tables; each command's
// sampled coordinates are checked before the original reads its owned mask.
// Never collect shadows across intervening terrain or sprite commands.
std::vector<uint8_t> ReplayLegacyShadowTriangles(std::span<const SceneShadowTriangle> triangles,
    std::span<const SceneShadowMask> masks, const SceneAssets& assets,
    uint32_t width, uint32_t height, const std::vector<uint8_t>& background);
LegacyShadowInspection InspectLegacyShadowTriangle(const SceneShadowTriangle& triangle,
    const SceneShadowMask& mask, const SceneAssets& assets, uint32_t width, uint32_t height);
}
}
