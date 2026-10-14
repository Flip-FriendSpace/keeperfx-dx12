// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
// Narrow C ABI; engine types and headers stay in the isolated C target.
#pragma once
#include <stdint.h>
#include "kfx/renderer/SpriteScale.h"
#ifdef __cplusplus
extern "C" {
#endif
int LegacySpriteOracleXSteps(void);
int LegacySpriteOracleYSteps(void);
void LegacySpriteOracleTarget(uint8_t* pixels, int32_t width, int32_t height);
void LegacySpriteOracleNativeTarget(uint8_t* pixels, int32_t pitch, int32_t height,
    int32_t window_x, int32_t window_y, int32_t window_w, int32_t window_h, uint32_t guard);
// 1: uncaptured source read, 2: destination beyond owned allocation. Relative
// window offsets identify the first blocked copy; no blocked copy is executed.
int LegacySpriteOracleCopyFailure(int64_t* source, int64_t* destination, uint32_t* length);
void LegacySpriteOracleClear(void);
int LegacySpriteOracleDraw(const struct SpriteScale* scale,
    const uint8_t* rle, uint32_t source_width, uint32_t content_height,
    uint16_t flags, int alpha, int corrected, const uint8_t* remap,
    const uint8_t* ghost, const uint8_t* additive);
#ifdef __cplusplus
}
#endif
