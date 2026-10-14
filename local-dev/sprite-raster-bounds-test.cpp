// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#include "kfx/renderer/SpriteRasterBounds.h"
#include <cassert>
#include <climits>
#include <iostream>
using namespace kfx::render;
int main() {
    // Exact campaign failure: the 17431-pixel-high glow has no visible X pixels.
    assert(!SpriteRasterBounds(-16612,9448,3087).Visible());
    const auto y=SpriteRasterBounds(-2705,17431,2160);
    assert(y.first==2705&&y.end==4865&&SpriteRasterMappingSafe(y,65511,10675906));
    for(int q=y.first;q<y.end;++q) {
        const int64_t wanted=std::max<int64_t>((int64_t(q)+1)*65536-65511-1,0)/10675906;
        const int32_t actual=std::max(((q+1)<<16)-65511-1,0)/10675906;
        assert(wanted==actual);
        assert(106-actual==106-wanted); // Horizontal flip uses unchanged logical source bounds.
    }
    assert(SpriteRasterMappingSafe(SpriteRasterBounds(0,INT_MAX,16384),INT_MAX,INT_MAX));
    assert(!SpriteRasterMappingSafe(SpriteRasterBounds(-32767,65536,1),0,1)); // Shift overflow.
    assert(!SpriteRasterMappingSafe(SpriteRasterBounds(0,10,10),INT_MIN,1)); // Subtraction overflow.
    assert(!SpriteRasterMappingSafe(SpriteRasterBounds(0,1,1),0,0));
    assert(!SpriteRasterBounds(INT_MAX,INT_MAX,16384).Visible());
    assert(!SpriteRasterBounds(INT_MIN,INT_MAX,16384).Visible());
    assert(!SpriteRasterBounds(-4,4,64).Visible());assert(!SpriteRasterBounds(64,4,64).Visible());
    assert(SpriteRasterBounds(-4,5,64).end==5&&SpriteRasterBounds(63,4,64).end==1);
    std::cout<<"Sprite visible mapping bounds and campaign regression passed\n";
}
