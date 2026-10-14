// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#ifndef KFX_TERRAIN_COLUMN_PLAN_H
#define KFX_TERRAIN_COLUMN_PLAN_H
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Column topology only. No projected coordinates or engine pointers. */
enum TerrainColumnMode { TCP_Isometric, TCP_Cluedo, TCP_Perspective, TCP_ModeCount };
enum TerrainColumnOperationKind { TCP_Back, TCP_Front, TCP_Left, TCP_Right, TCP_Top, TCP_Abyss, TCP_Bottom };
struct TerrainColumnOperation {
    uint16_t texture;
    uint8_t kind, height, unlit, top_remap;
};
struct TerrainColumnInputs {
    uint16_t masks[5]; /* current, back, front, left, right; already visibility/boundary resolved */
    uint16_t sides[8][4];
    uint16_t floor_texture, top_texture, lintel_texture, marked_land, marked_gold;
    uint8_t floor_height, lintel_height, floor_abyss;
    uint8_t mode, raw_floor_height, lintel_bottom_height, ceiling_height, draw_ceiling;
    uint16_t lintel_bottom_texture, ceiling_texture;
    uint8_t unexplored, tagged_valuable, valuable;
};
struct TerrainColumnPlan {
    uint8_t count;
    struct TerrainColumnOperation operations[37];
};
struct TerrainColumnPlanStats {
    uint64_t requests, hits, builds, evictions, epoch_resets;
    uint64_t mode_requests[TCP_ModeCount], mode_hits[TCP_ModeCount], mode_builds[TCP_ModeCount];
    size_t capacity_bytes;
    uint32_t capacity_entries;
};
/* Result is consumed immediately, before another resolve on the game thread. */
const struct TerrainColumnPlan* TerrainColumnPlanResolve(uint64_t epoch, uint16_t x, uint16_t y,
    const struct TerrainColumnInputs* inputs);
struct TerrainColumnPlanStats TerrainColumnPlanGetStats(void);
#ifdef __cplusplus
}
#endif
#endif
