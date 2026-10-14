// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#pragma once

#include <algorithm>
#include <cstdint>

// The simulation packs rows at map_subtiles_x + 1, not the renderer's maximum
// texture width. Replicate the map edge so filtering never reads stale padding.
inline void snapshot_lightmap(uint16_t* dst, int dst_width, int dst_height,
    const uint16_t* src, int src_width, int src_height)
{
    for (int y = 0; y < dst_height; ++y) {
        const uint16_t* row = src + std::min(y, src_height - 1) * src_width;
        uint16_t* out = dst + y * dst_width;
        const int count = std::min(src_width, dst_width);
        std::copy_n(row, count, out);
        std::fill(out + count, out + dst_width, row[count - 1]);
    }
}
