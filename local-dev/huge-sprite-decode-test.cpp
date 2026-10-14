// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#include "kfx/renderer/HugeSpriteDecode.h"
#include <cassert>
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>
using namespace kfx::render;
static void U32(std::vector<uint8_t>& v,uint32_t x){for(unsigned i=0;i<4;++i)v.push_back(uint8_t(x>>(8*i)));}
int main(int argc,char** argv){
    std::vector<uint8_t> offsets,data;U32(offsets,0);U32(data,2);data.push_back(0);data.push_back(9);U32(data,1);
    std::vector<uint8_t> pixels(3,7),coverage(3,7);
    assert(DecodeHugeSprite({data,offsets,3,1},pixels,coverage,3,1,0,0,16));
    assert((pixels==std::vector<uint8_t>{0,9,0}));assert((coverage==std::vector<uint8_t>{255,255,0}));
    {
        std::vector<uint8_t> scaled(12),mask(12);
        assert(DecodeHugeSprite({data,offsets,3,1},scaled,mask,6,2,0,0,32));
        for(size_t row=0;row<2;++row){
            assert((std::vector<uint8_t>(scaled.begin()+row*6,scaled.begin()+(row+1)*6)==std::vector<uint8_t>{0,0,0,9,9,0}));
            for(size_t col=0;col<6;++col)assert(mask[row*6+col]==(col<5?255:0));
        }
        scaled.resize(3);mask.resize(3);
        assert(DecodeHugeSprite({data,offsets,3,1},scaled,mask,3,1,1,0,16));
        assert((scaled==std::vector<uint8_t>{9,0,0}) && (mask==std::vector<uint8_t>{255,0,0}));
    }
    const auto original=pixels;
    for(size_t n=0;n<data.size();++n){assert(!DecodeHugeSprite({std::span(data).first(n),offsets,3,1},pixels,coverage,3,1,0,0,16));assert(pixels==original);}
    auto bad=data;bad[0]=4;assert(!DecodeHugeSprite({bad,offsets,3,1},pixels,coverage,3,1,0,0,16));
    std::vector<uint8_t> zero;U32(zero,0);U32(zero,0);assert(!DecodeHugeSprite({zero,offsets,3,1},pixels,coverage,3,1,0,0,16));
    auto bad_offsets=offsets;bad_offsets[0]=255;assert(!DecodeHugeSprite({data,bad_offsets,3,1},pixels,coverage,3,1,0,0,16));
    assert(!DecodeHugeSprite({data,offsets,3,1},pixels,coverage,3,1,0,0,0));
    if(argc==2){
        std::ifstream file(argv[1],std::ios::binary);assert(file);std::vector<uint8_t> bank{std::istreambuf_iterator<char>(file),{}};assert(bank.size()>2880);
        const std::span<const uint8_t> lines(bank.data(),2880),encoded(bank.data()+2880,bank.size()-2880);
        pixels.resize(960*720);coverage.resize(pixels.size());assert(DecodeHugeSprite({encoded,lines,960,720},pixels,coverage,960,720,0,0,16));
        size_t opaque=0,opaque_zero=0;
        // Independent source-row expansion is the exact 1:1 legacy result.
        for(size_t row=0;row<720;++row){size_t at=huge_detail::Read32(lines.data()+row*4),x=0;
            while(x<960){auto solid=huge_detail::Read32(encoded.data()+at);at+=4;
                for(size_t i=0;i<solid;++i,++x){assert(pixels[row*960+x]==encoded[at+i] && coverage[row*960+x]==255);++opaque;opaque_zero+=encoded[at+i]==0;}
                at+=solid;auto skip=huge_detail::Read32(encoded.data()+at);at+=4;
                for(size_t i=0;i<skip;++i,++x)assert(coverage[row*960+x]==0);
            }
        }
        std::cout<<"installed source: 960x720, opaque="<<opaque<<", opaque_index0="<<opaque_zero<<"\n";
    }
    std::cout<<"Huge sprite bounds, malformed rollback, opaque zero and source decode passed\n";
}
