// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#include "kfx/renderer/PaletteIndexTable.h"
#include <array>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <vector>
using namespace kfx::render;
namespace {
unsigned calls=0;
uint8_t Nearest(const uint8_t*,uint8_t r,uint8_t g,uint8_t b) {
    assert((r&3)==3&&(g&3)==3&&(b&3)==3);++calls;
    return uint8_t((r/4)*17+(g/4)*5+b/4);
}
}
int main() {
    std::array<uint8_t,768> base{};
    std::vector<uint8_t> table(PaletteIndexTableBytes);
    BuildPaletteIndexBase(table,base,Nearest);assert(calls==4096);
    for(unsigned r=0;r<64;++r)for(unsigned g=0;g<64;++g)for(unsigned b=0;b<64;++b)
        assert(table[(r<<12)|(g<<6)|b]==uint8_t((r/4)*17+(g/4)*5+b/4));
    const auto fallback=table;
    std::array<uint8_t,1024> rgba{};std::array<uint32_t,256> keys{};
    for(unsigned i=0;i<256;++i){rgba[i*4]=uint8_t(i);rgba[i*4+1]=uint8_t(255-i);rgba[i*4+2]=127;rgba[i*4+3]=255;}
    ApplyPaletteIndexOverrides(table,rgba,keys);
    for(unsigned i=0;i<256;++i) {
        unsigned winner=i;
        for(unsigned j=0;j<i;++j)if(keys[j]==keys[i]){winner=j;break;}
        assert(table[keys[i]]==winner);
    }
    assert(((keys[0]>>6)&63)==63&&(keys[0]&63)==31&&(keys[255]>>12)==63);
    for(auto key:keys){table[key]=fallback[key];}
    assert(table==fallback);
    rgba.fill(0);ApplyPaletteIndexOverrides(table,rgba,keys);assert(table[0]==0);
    std::cout<<"PaletteIndexTable: 262144 grid entries, 4096 exact sample calls, quantization, lowest duplicates and restoration passed\n";
}
