// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#pragma once
#include <algorithm>
#include <cstdint>
#include <limits>

namespace kfx::render {
// Visible coordinates in the ORIGINAL sprite-local pixel grid. Clipping changes
// vertex endpoints, never sampling phase, step, source bounds or horizontal flip.
struct SpriteRasterAxis {
    int32_t first=0,end=0;
    bool Visible() const noexcept {return first<end;}
};
inline SpriteRasterAxis SpriteRasterBounds(int32_t origin,int32_t extent,uint32_t viewport) noexcept
{
    if(extent<=0 || !viewport)return {};
    const int64_t first=std::max<int64_t>(0,-int64_t(origin));
    const int64_t end=std::min<int64_t>(extent,int64_t(viewport)-origin);
    if(first>=end)return {};
    return {int32_t(first),int32_t(end)};
}
inline bool SpriteRasterMappingSafe(SpriteRasterAxis visible,int32_t phase,int32_t step) noexcept
{
    if(step<=0)return false;
    if(!visible.Visible())return true;
    // Outline.hlsl computes ((q+1)<<16)-phase-1 in signed32 before division.
    // Check each intermediate over the full visible interval, not the projected
    // rectangle (which can be enormous during the ordinary heart fly-in).
    const int64_t first=(int64_t(visible.first)+1)*65536;
    const int64_t last=int64_t(visible.end)*65536;
    constexpr int64_t lo=std::numeric_limits<int32_t>::min(),hi=std::numeric_limits<int32_t>::max();
    return first>=0 && last<=hi && first-phase>=lo+1 && last-phase<=hi;
}
}
