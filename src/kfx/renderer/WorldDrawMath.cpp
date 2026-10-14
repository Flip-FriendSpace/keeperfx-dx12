// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#include "pre_inc.h"
#include "kfx/renderer/WorldDrawMath.h"
#include "engine_buckets.h"
#include "engine_textures.h"
#include "post_inc.h"

namespace kfx::render {
float WorldBucketDepth(float bucket, int bucket_count) noexcept
{
    return 2.0f * bucket / (float)(bucket_count - 1) - 1.0f;
}
WorldDrawTriangle BuildWorldDrawTriangle(const WorldDrawProjection& projection,
    int bucket, int tile_id, const PolyPoint& a, const PolyPoint& b,
    const PolyPoint& c, const TerrainLightmapCoord* lightmap) noexcept
{
    WorldDrawTriangle vertices;
    const PolyPoint* points[3] = {&a, &b, &c};
    const float depth = WorldBucketDepth((float)bucket, projection.bucket_count);
    const float variation = (float)(tile_id / TEXTURE_BLOCKS_COUNT);
    const int tile = tile_id % TEXTURE_BLOCKS_COUNT;
    for (int i = 0; i < 3; ++i) {
        auto& vertex = vertices[i];
        vertex.x = (float)points[i]->X / (float)projection.width * 2.0f - 1.0f;
        vertex.y = 1.0f - (float)points[i]->Y / (float)projection.height * 2.0f;
        vertex.z = depth;
        vertex.u = (float)points[i]->U / 65536.0f;
        vertex.v = (float)points[i]->V / 65536.0f;
        vertex.shade = (float)(points[i]->S >> 16) / 32.0f;
        vertex.stl_x = lightmap ? (float)lightmap[i].x : -1.0f;
        vertex.stl_y = lightmap ? (float)lightmap[i].y : -1.0f;
        vertex.camera_z = 1.0f;
        vertex.atlas_layer = variation;
        vertex.wx = vertex.wy = vertex.wz = 0.0f;
        vertex.tile = (float)tile;
    }
    return vertices;
}
std::array<WorldDrawTriangle, 2> BuildWorldFrontQuad(
    const WorldDrawProjection& projection, int bucket, int pixel_scale,
    const BucketKindTexturedQuad& quad, const WorldQuadOrientation& orientation) noexcept
{
    // Preserve draw_texturedquad_block arithmetic, including pixel-size division.
    PolyPoint a{}, d{}, b{}, c{};
    a.X = (quad.texture_x >> 8) / pixel_scale;
    a.Y = (quad.texture_y >> 8) / pixel_scale;
    d.X = ((quad.zoom_x + quad.texture_x) >> 8) / pixel_scale;
    d.Y = (quad.texture_y >> 8) / pixel_scale;
    b.X = ((quad.zoom_x + quad.texture_x) >> 8) / pixel_scale;
    b.Y = ((quad.zoom_y + quad.texture_y) >> 8) / pixel_scale;
    c.X = (quad.texture_x >> 8) / pixel_scale;
    c.Y = ((quad.zoom_y + quad.texture_y) >> 8) / pixel_scale;
    PolyPoint* points[4] = {&a, &d, &b, &c};
    const long shades[4] = {quad.shade_intensity0, quad.shade_intensity1,
        quad.shade_intensity2, quad.shade_intensity3};
    for (int i = 0; i < 4; ++i) {
        points[i]->U = orientation.u[i] + quad.texture_scroll.x.val;
        points[i]->V = orientation.v[i] + quad.texture_scroll.y.val;
        points[i]->S = shades[i];
    }
    int tile;
    switch (quad.marked_mode) {
    case 0: tile = TEXTURE_LAND_MARKED_LAND; break;
    case 1: tile = TEXTURE_LAND_MARKED_GOLD; break;
    default: tile = (int)quad.texture_idx; break;
    }
    return {BuildWorldDrawTriangle(projection, bucket, tile, a, d, b),
        BuildWorldDrawTriangle(projection, bucket, tile, a, b, c)};
}
}
