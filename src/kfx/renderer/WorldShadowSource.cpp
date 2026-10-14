// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#include "pre_inc.h"
#include "kfx/renderer/WorldShadowSource.h"
#include "engine_buckets.h"
#include "engine_render.h"
#include <algorithm>
#include "post_inc.h"

namespace kfx::render {
WorldShadowGeometry GetWorldShadowGeometry(const BucketKindCreatureShadow& shadow) noexcept
{
    WorldShadowGeometry geometry;
    const PolyPoint* points[4] = { &shadow.vertex_first, &shadow.vertex_second,
        &shadow.vertex_third, &shadow.vertex_fourth };
    for (int i = 0; i < 4; ++i) {
        geometry.x[i] = (float)points[i]->X;
        geometry.y[i] = (float)points[i]->Y;
        // Preserve the original two divisions of TO_FIXED content-pixel UVs.
        geometry.u[i] = (float)points[i]->U / 65536.0f / (float)WorldShadowDecodeDimension;
        geometry.v[i] = (float)points[i]->V / 65536.0f / (float)WorldShadowDecodeDimension;
    }
    geometry.z_ndc = 2.0f * (float)shadow.bucket_idx / (float)(BUCKETS_COUNT - 1) - 1.0f;
    // create_shadows() clamps squared distance to [16,31]. Retain its existing
    // approximate linear falloff, including clamps for other producers.
    const float dist_sq = (float)shadow.vertex_first.S;
    const float t = std::min(std::max((dist_sq - 16.0f) / 15.0f, 0.0f), 1.0f);
    geometry.darken = 0.55f - 0.35f * t;
    return geometry;
}

WorldShadowSourceStatus ResolveWorldShadowSource(const BucketKindCreatureShadow& shadow,
    WorldShadowSourceView& source) noexcept
{
    source = {};
    WorldShadowSourceView resolved;
    const unsigned char* data = nullptr;
    if (!resolve_keepersprite_draw_data(shadow.anim_sprite, (short)shadow.angle,
            shadow.current_frame, &resolved.draw_idx, &data, &resolved.src_w,
            &resolved.src_h, nullptr))
        return WorldShadowSourceStatus::ResolutionFailed;

    size_t extent = 0;
    if (!get_keepersprite_source_extent(resolved.draw_idx, data, &extent))
        return WorldShadowSourceStatus::ExtentUnavailable;
    resolved.bytes = {data, extent};
    resolved.geometry = GetWorldShadowGeometry(shadow);

    int32_t identity_idx = -1;
    size_t identity_extent = 0;
    uint64_t stamp = 0;
    if (get_keepersprite_source_identity(resolved.draw_idx, data, &identity_idx,
            &identity_extent, &stamp)
        && identity_idx == resolved.draw_idx && identity_extent == extent)
        resolved.publication_stamp = stamp;
    source = resolved;
    return WorldShadowSourceStatus::Success;
}
}
