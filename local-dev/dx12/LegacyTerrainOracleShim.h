// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
// Isolated C ABI for the original Gpoly rasterizer.
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
struct LegacyTerrainPoint { int32_t x, y, u, v, s; };
void LegacyTerrainOracleDraw(const struct LegacyTerrainPoint points[3],
    uint8_t* pixels, uint32_t width, uint32_t height,
    const uint8_t* texture, const uint8_t* fade);
void LegacyTerrainOracleClear(void);
int LegacyTerrainOracleDrawTrig(const struct LegacyTerrainPoint points[3],
    uint8_t* pixels, uint32_t width, uint32_t height,
    const uint8_t* texture, const uint8_t* fade, uint8_t colour);
void LegacyTerrainOracleClearTrig(void);
// Original mode 2 probes share mode 10's edge/UV preparation. The caller
// validates initialized mask bounds using probes before invoking mode 10.
int LegacyShadowOracleDrawTrig(const struct LegacyTerrainPoint points[3],
    uint8_t* pixels, uint32_t width, uint32_t height,
    const uint8_t* texture, const uint8_t* fade, uint8_t shade, int probe);
#ifdef __cplusplus
}
#endif
