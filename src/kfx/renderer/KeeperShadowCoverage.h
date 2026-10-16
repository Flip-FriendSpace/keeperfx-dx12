// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX remaster changes: 2026-10-16.
#ifndef KFX_KEEPER_SHADOW_COVERAGE_H
#define KFX_KEEPER_SHADOW_COVERAGE_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* A separate namespace prevents reduced shadow coverage from becoming a colour
 * sprite cache hit. Callers first reject unknown (negative) sprite identities. */
static inline int32_t keeper_shadow_atlas_cache_key(int32_t sprite_id, int reduced)
{
    return reduced ? -1 - sprite_id : sprite_id;
}

/* UVs remain in source-pixel / 256 units. A frame may include transparent
 * padding beyond the cropped source; use both extents to choose the variant. */
static inline int keeper_shadow_coverage_factor(int width, int height,
    const float *u, const float *v)
{
    if (!u || !v || width <= 0 || height <= 0 || width > 1024 || height > 1024) return 0;
    int factor = width > 512 || height > 512 ? 2 : 1;
    for (int i = 0; i < 4; ++i) {
        if (!(u[i] >= 0 && u[i] <= 1023.0f / 256.0f)
            || !(v[i] >= 0 && v[i] <= 1023.0f / 256.0f)) return 0;
        if (u[i] * 256.0f > 511 || v[i] * 256.0f > 511) factor = 2;
    }
    return factor;
}

/* Atlas edge coordinates describe initialized texels, including the last
 * texel of a reduced 1024 source. Keep capture and live geometry identical. */
static inline float keeper_shadow_coverage_uv(float uv, int factor)
{
    const float reduced = uv / factor;
    return reduced > 511.0f / 256.0f ? 511.0f / 256.0f : reduced;
}

/* Keep OpenGL's 512x512 layers and total atlas budget unchanged. Only oversized
 * shadow coverage is reduced; colour sprites retain their full-resolution path.
 * Distinct non-overlapping initialized storage is required. Failure writes no bytes. */
static inline int keeper_shadow_reduce_rg(unsigned char *target, size_t target_bytes,
    const unsigned char *source, size_t source_bytes, int width, int height, int source_stride)
{
    const size_t target_size = 512u * 512u * 2u;
    if (!target || !source || width <= 0 || height <= 0 || width > 1024 || height > 1024
        || source_stride <= 0
        || (size_t)source_stride < (size_t)width * 2u || target_bytes < target_size
        || source_bytes / (size_t)source_stride < (size_t)height)
        return 0;
    const uintptr_t src = (uintptr_t)source, dst = (uintptr_t)target;
    if (src > UINTPTR_MAX - source_bytes || dst > UINTPTR_MAX - target_size
        || (dst < src + source_bytes && src < dst + target_size))
        return 0;
    memset(target, 0, target_size);
    for (int y = 0; y < (height + 1) / 2; ++y)
        for (int x = 0; x < (width + 1) / 2; ++x)
            target[((size_t)y * 512u + (size_t)x) * 2u + 1u]
                = source[(size_t)(y * 2) * (size_t)source_stride + (size_t)(x * 2) * 2u + 1u];
    return 1;
}
#endif
