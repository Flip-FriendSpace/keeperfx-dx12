// SPDX-License-Identifier: GPL-2.0-or-later
#include "kfx/renderer/HdTerrainPack.h"
#include <cassert>
#include <iostream>
using namespace kfx::render;
static void Word(std::vector<uint8_t>& b,uint32_t n){for(unsigned i=0;i<4;++i)b.push_back(uint8_t(n>>(8*i)));}
static void Set(std::vector<uint8_t>& b,size_t at,uint32_t n){for(unsigned i=0;i<4;++i)b[at+i]=uint8_t(n>>(8*i));}
int main(){
    std::vector<uint8_t> bytes;for(auto n:{0x5448464bu,1u,1u,0u,27u,128u,0u})Word(bytes,n);
    for(unsigned i=0;i<1024;++i)bytes.push_back(uint8_t(i));
    bytes.insert(bytes.end(),128*128,42);
    HdTerrainPack pack;assert(pack.Parse(bytes) && pack.Count()==1);
    std::vector<uint8_t> bank(32*256,199);
    for(size_t y=0;y<32;++y)for(size_t x=0;x<32;++x)bank[y*256+x]=uint8_t(y*32+x);
    TileSourceView view;view.bytes=bank;view.width=view.height=32;view.row_stride=256;view.mutation_serial=1;
    auto* record=pack.Match(27,view);assert(record && record->dimension==128 && record->pixels.size()==16384 && record->pixels[0]==42);
    assert(!pack.Match(28,view));
    // Row gaps are not native pixels; source changes need a fresh mutation serial.
    bank[32]=0;assert(pack.Match(27,view));bank[256+5]^=1;view.mutation_serial=2;assert(!pack.Match(27,view));
    bank[256+5]^=1;view.mutation_serial=3;assert(pack.Match(27,view));
    view.bank_offset=32;bank[0]^=1;assert(!pack.Match(27,view));bank[0]^=1;view.mutation_serial=4;assert(pack.Match(27,view));
    auto invalid=view;invalid.bytes=std::span<const uint8_t>(bank).first(31*256+31);assert(!pack.Match(27,invalid));
    invalid=view;invalid.variation=32;assert(!pack.Match(27,invalid));invalid=view;invalid.mutation_serial=0;assert(!pack.Match(27,invalid));
    for(size_t n:{size_t(0),size_t(3),size_t(15),size_t(16),size_t(27),bytes.size()-1})assert(!HdTerrainPack{}.Parse(std::span<const uint8_t>(bytes).first(n)));
    for(auto [offset,value]:{std::pair{size_t(0),0u}, {size_t(4),3u},{size_t(8),65u},{size_t(12),1u},{size_t(16),1544u},{size_t(20),127u},{size_t(20),32u},{size_t(24),1u}}){auto bad=bytes;Set(bad,offset,value);assert(!pack.Parse(bad));assert(pack.Count()==1);}
    auto trailing=bytes;trailing.push_back(0);assert(!pack.Parse(trailing));
    auto duplicate=bytes;Set(duplicate,8,2);duplicate.insert(duplicate.end(),bytes.begin()+16,bytes.end());assert(!pack.Parse(duplicate));
    assert(!HdTerrainPack{}.Parse(std::vector<uint8_t>(HdTerrainPack::MaxFileBytes+1)));
    for(uint32_t size:{32u,64u,128u,256u}){
        assert(TerrainTextureDimension(size));const auto ref=TerrainTextureReference(96,128,size);
        assert((ref&0xffffff)==515 && ((ref>>24)&3)==TerrainTextureScaleCode(size));
    }
    assert(!TerrainTextureDimension(31) && !TerrainTextureDimension(512));
    // Full-set v2 lookup follows physical animation frames, including B-bank
    // offsets and variation banks, without changing the native mutation serial.
    std::vector<uint8_t> full;
    for (auto n : {0x5448464bu,2u,632u,0u}) Word(full,n);
    for (uint32_t i=0;i<632;++i) {
        const uint32_t id=i<544?i:1000+i-544;
        Word(full,id);Word(full,128);Word(full,0);
        full.insert(full.end(),1024,uint8_t(id%251));
        full.insert(full.end(),128*128,uint8_t(id+17));
    }
    HdTerrainPack complete;assert(complete.Parse(full) && complete.Count()==632 && complete.Version()==2);
    std::vector<uint8_t> physical(HdTerrainPack::PhysicalBankBytes*2,199);
    const auto sourceFor=[&](uint32_t id,uint32_t variation=0) {
        const uint32_t index=id<544?id:id-1000+544;
        const size_t offset=variation*HdTerrainPack::PhysicalBankBytes+(index/8)*8192+(index%8)*32;
        for(size_t y=0;y<32;++y)std::fill_n(physical.data()+offset+y*256,32,uint8_t(id%251));
        TileSourceView source;source.bytes=std::span<const uint8_t>(physical).subspan(offset,31*256+32);
        source.width=source.height=32;source.row_stride=256;source.bank_offset=offset;
        source.mutation_serial=10;source.variation=variation;return source;
    };
    auto frameA=sourceFor(80),frameB=sourceFor(81),bankB=sourceFor(1083),otherVariation=sourceFor(81,1);
    const auto* held=complete.Match(548,frameA);assert(held && held->tile==80 && held->pixels[0]==97);
    const auto* advanced=complete.Match(548,frameB);assert(advanced && advanced->tile==81 && advanced->pixels[0]==98);
    assert(held->pixels[0]==97 && complete.Match(548,frameA)==held);
    assert(complete.Match(604,bankB)->tile==1083);
    assert(complete.Match(548,otherVariation)==advanced);
    physical[otherVariation.bank_offset]^=1;otherVariation.mutation_serial++;
    assert(!complete.Match(548,otherVariation));
    auto misaligned=frameA;misaligned.bank_offset++;assert(!complete.Match(548,misaligned));
    misaligned=frameA;misaligned.row_stride=32;assert(!complete.Match(548,misaligned));
    assert(!complete.Match(1544,frameA));
    auto badV2=full;Set(badV2,16,544);assert(!complete.Parse(badV2) && complete.Count()==632);
    badV2=full;Set(badV2,8,1089);assert(!complete.Parse(badV2));
    badV2=full;Set(badV2,4,1);assert(!complete.Parse(badV2));
    // Old packs still match the requested logical ID, independent of v2 offsets.
    assert(pack.Version()==1 && pack.Match(27,view) && !pack.Match(548,view));
    std::cout<<"HD terrain pack: authenticated strided sources, mutation cache, v1/v2 bounds, 632 records, A/B animation frames and resolution references passed\n";
}
