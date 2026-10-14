// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#include "PaletteIndexTable.h"
#include <array>
#include <stdexcept>

namespace kfx::render {
void BuildPaletteIndexBase(std::span<uint8_t> table,std::span<const uint8_t> palette,PaletteNearestColour nearest)
{
    if(table.size()!=PaletteIndexTableBytes||palette.size()!=768||!nearest)
        throw std::invalid_argument("Invalid palette index base inputs");
    std::array<uint8_t,16*16*16> grid;
    for(unsigned r=0;r<16;++r)for(unsigned g=0;g<16;++g)for(unsigned b=0;b<16;++b)
        grid[(r<<8)|(g<<4)|b]=nearest(palette.data(),uint8_t(4*r+3),uint8_t(4*g+3),uint8_t(4*b+3));
    for(unsigned r=0;r<64;++r)for(unsigned g=0;g<64;++g)for(unsigned b=0;b<64;++b)
        table[(r<<12)|(g<<6)|b]=grid[((r>>2)<<8)|((g>>2)<<4)|(b>>2)];
}
void ApplyPaletteIndexOverrides(std::span<uint8_t> table,std::span<const uint8_t> palette,std::span<uint32_t> keys)
{
    if(table.size()!=PaletteIndexTableBytes||palette.size()!=1024||keys.size()!=256)
        throw std::invalid_argument("Invalid palette index override inputs");
    const auto channel=[](uint8_t value){return (unsigned(value)*63+127)/255;};
    for(int i=255;i>=0;--i) {
        const auto key=(channel(palette[i*4])<<12)|(channel(palette[i*4+1])<<6)|channel(palette[i*4+2]);
        keys[size_t(i)]=key;table[key]=uint8_t(i);
    }
}
}
