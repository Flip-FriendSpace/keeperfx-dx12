// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#ifndef KFX_STATE_NAVIGATION_H
#define KFX_STATE_NAVIGATION_H

#include "ariadne_points.h"
#include "ariadne_tringls.h"
#include "ariadne_regions.h"

// Runtime snapshot only. The wire representation visits fields explicitly.
struct NavigationSnapshot {
    struct Point points[POINTS_COUNT];
    int32_t point_count;
    int32_t point_index;
    int32_t point_free;
    struct Triangle triangles[TRIANLGLES_COUNT];
    int32_t triangle_count;
    int32_t triangle_index;
    int32_t triangle_free;
    struct RegionT regions[REGIONS_COUNT];
    int32_t cache[4][4];
};

#ifdef __cplusplus
extern "C" {
#endif
void capture_navigation_snapshot(struct NavigationSnapshot *snapshot);
void apply_navigation_snapshot(const struct NavigationSnapshot *snapshot);
TbBool validate_navigation_snapshot(const struct NavigationSnapshot *snapshot);
void install_replay_navigation_snapshot(const struct NavigationSnapshot *snapshot);
#ifdef __cplusplus
}
#endif
#endif
