// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#ifndef KFX_FRONT_TERRAIN_PLAN_H
#define KFX_FRONT_TERRAIN_PLAN_H
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Raw column runs only. Screen occlusion, light and texture remapping stay live. */
struct FrontTerrainInputs {
    uint8_t present[8];
    uint16_t side_texture[8], top_texture[8];
    uint16_t solidmask;
    uint8_t ceiling_filled;
};
enum FrontTerrainOperationKind { FTP_Side, FTP_Top };
struct FrontTerrainOperation {
    uint16_t texture;
    uint8_t kind, height, lower_run;
};
struct FrontTerrainPlan {
    uint8_t count;
    struct FrontTerrainOperation operations[10];
};
struct FrontTerrainPlanStats {
    uint64_t requests, hits, builds, evictions, epoch_resets;
    size_t capacity_bytes;
    uint32_t capacity_entries;
};
/* Single game-thread use; consume the result before another resolve.
 * Coordinates are retained for caller compatibility; exact input content is the key. */
const struct FrontTerrainPlan* FrontTerrainPlanResolve(uint64_t epoch, uint16_t x, uint16_t y,
    const struct FrontTerrainInputs* inputs);
struct FrontTerrainPlanStats FrontTerrainPlanGetStats(void);
#ifdef __cplusplus
}
#endif
#endif
