// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#pragma once
#include "kfx/renderer/WorldVertex.h"
#include <array>

struct PolyPoint;
struct TerrainLightmapCoord;
struct BucketKindTexturedQuad;

namespace kfx::render {
// Logical variation/tile coordinates are retained in WorldVertex's legacy
// atlas_layer/tile fields. No physical atlas location or GPU name is produced.
struct WorldDrawProjection {
    int width, height;
    int bucket_count;
};
struct WorldQuadOrientation {
    std::array<long, 4> u, v;
};
using WorldDrawTriangle = std::array<WorldVertex, 3>;
float WorldBucketDepth(float bucket, int bucket_count) noexcept;
WorldDrawTriangle BuildWorldDrawTriangle(const WorldDrawProjection& projection,
    int bucket, int tile_id, const PolyPoint& a, const PolyPoint& b,
    const PolyPoint& c, const TerrainLightmapCoord* lightmap = nullptr) noexcept;
std::array<WorldDrawTriangle, 2> BuildWorldFrontQuad(
    const WorldDrawProjection& projection, int bucket, int pixel_scale,
    const BucketKindTexturedQuad& quad, const WorldQuadOrientation& orientation) noexcept;
}
