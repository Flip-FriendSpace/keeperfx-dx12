// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#include "kfx/renderer/KeeperSpriteDecode.h"
#include <array>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <iterator>
#include <random>
#include <stdexcept>
#include <vector>
using namespace kfx::render;
static void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
static std::vector<uint8_t> Read(const char* path) {
    std::ifstream file(path, std::ios::binary);
    Require(bool(file), "Cannot open corpus input");
    return {std::istreambuf_iterator<char>(file), {}};
}
// Independent old normal-renderer algorithm, with input bounds solely to make
// this test oracle safe. Do not call it with missing row sentinels/literals.
static std::vector<uint8_t> Legacy(std::span<const uint8_t> source, unsigned w, unsigned h,
    unsigned padded_w, unsigned padded_h, size_t stride, size_t& consumed) {
    std::vector<uint8_t> result(stride * padded_h, 0);
    size_t p = 0;
    for (unsigned y = 0; y < h; ++y) {
        size_t x = 0;
        for (;;) {
            Require(p < source.size(), "Legacy oracle missing command");
            const int command = source[p] < 128 ? int(source[p]) : int(source[p]) - 256;
            ++p;
            if (command == 0) break;
            if (command < 0) { x += size_t(-command); continue; }
            for (int i = 0; i < command; ++i) {
                Require(p < source.size(), "Legacy oracle truncated literal");
                if (x < w) { result[y * stride + x * 2] = source[p]; result[y * stride + x * 2 + 1] = 255; }
                ++p; ++x;
            }
        }
    }
    Require(padded_w >= w, "Bad oracle target");
    consumed = p;
    return result;
}
static void Compare(std::span<const uint8_t> source, unsigned w, unsigned h,
    unsigned pw = 256, unsigned ph = 256, size_t stride = 512) {
    size_t consumed = 0;
    const auto expected = Legacy(source, w, h, pw, ph, stride, consumed);
    std::vector<uint8_t> guard(stride * ph + 32, 0xa5);
    auto target = std::span<uint8_t>(guard).subspan(16, stride * ph);
    const auto decoded = DecodeKeeperSpriteRG({target,pw,ph,stride},source,w,h);
    Require(bool(decoded), "Valid decode rejected");
    Require(decoded.bytes_consumed == consumed, "Wrong consumed extent");
    Require(std::equal(target.begin(),target.end(),expected.begin()), "Legacy pixels/padding changed");
    Require(std::all_of(guard.begin(),guard.begin()+16,[](uint8_t x){return x==0xa5;})
        && std::all_of(guard.end()-16,guard.end(),[](uint8_t x){return x==0xa5;}), "Guard overwritten");
}
static void Focused() {
    const std::vector<uint8_t> mixed = {3,0,50,255,253,2,51,56,0,0,128,127};
    Compare(mixed,8,2,12,5,27); // trailing bytes ignored, short/empty rows and stride padding
    Compare(std::vector<uint8_t>{128,1,0,0},256,1); // signed -128 then opaque index zero
    Compare(std::vector<uint8_t>{128,128,128,3,0,1,2,0},1,1); // clips skips/literals without overflow
    Compare(std::vector<uint8_t>{4,7,8,9,10,0},2,1); // over-wide literal clipping
    Compare(std::vector<uint8_t>{0,0},1,2); // zero is a row terminator, not a skip/run
    // Hero TD canvases reach 508 pixels and the knight held view reaches 596.
    // Exercise complete runs across signed-byte boundaries, initialized atlas
    // padding, target strides and all guard bytes through the production decoder.
    for (unsigned width : {254u,255u,256u,508u,512u,596u,1024u}) {
        std::vector<uint8_t> large;
        for (unsigned y=0;y<3;++y) {
            unsigned left=width;
            while(left) { const unsigned run=std::min(left,127u);large.push_back(uint8_t(run));
                for(unsigned x=0;x<run;++x)large.push_back(uint8_t((left+x+y)%256));
                left-=run; }
            large.push_back(0);
        }
        const unsigned dimension=width>512?1024:(width>256?512:256);
        Compare(large,width,3,dimension,dimension,dimension*2);
    }
    const std::vector<uint8_t> complete={2,0,255,0,255,1,12,0};
    for(size_t n=0;n<complete.size();++n) {
        std::array<uint8_t,32> out; out.fill(0xa7);
        const auto r=DecodeKeeperSpriteRG({out,4,4,8},std::span(complete).first(n),2,2);
        Require(!r,"Truncated source accepted");
        Require(std::all_of(out.begin(),out.end(),[](uint8_t x){return x==0xa7;}),"Failure mutated output");
    }
    std::array<uint8_t,32> out; out.fill(0x88);
    auto bad=[&](KeeperSpriteRGTarget t,unsigned w,unsigned h){
        Require(!DecodeKeeperSpriteRG(t,complete,w,h),"Invalid layout accepted");
        Require(std::all_of(out.begin(),out.end(),[](uint8_t x){return x==0x88;}),"Invalid layout mutated output");
    };
    bad({out,4,4,8},0,1); bad({out,4,4,8},5,1); bad({out,4,4,8},1,5);
    bad({out,4,4,7},1,1); bad({out,4,5,8},1,1); bad({out,4,4,SIZE_MAX},1,1);
    Require(!DecodeKeeperSpriteRG({out,4,4,8},std::span<const uint8_t>(out),1,1),"Aliased source accepted");
    // Decoded owned output remains independent when borrowed source is changed/freed.
    auto source=complete;
    Require(bool(DecodeKeeperSpriteRG({out,4,4,8},source,2,2)),"Owned output decode failed");
    const auto owned=out;
    source.assign(source.size(),0); source.clear(); source.shrink_to_fit();
    Require(out==owned && out[1]==255 && out[0]==0,"Decoded ownership/index zero lost");
    std::mt19937 random(0x4210u);
    for(unsigned trial=0;trial<4000;++trial) {
        const unsigned w=1+random()%256,h=1+random()%12;
        std::vector<uint8_t> encoded;
        for(unsigned y=0;y<h;++y) {
            const unsigned commands=random()%20;
            for(unsigned c=0;c<commands;++c) {
                const unsigned count=1+random()%127;
                if(random()&1) encoded.push_back(uint8_t(256-count));
                else { encoded.push_back(uint8_t(count)); for(unsigned i=0;i<count;++i) encoded.push_back(uint8_t(random())); }
            }
            encoded.push_back(0);
        }
        encoded.push_back(0xff); // allocation slack must not become an extra row
        Compare(encoded,w,h,256,16,516);
    }
    std::cout << "{\"focused\":\"passed\",\"randomizedLegacyComparisons\":4000}\n";
}
static uint32_t U32(const uint8_t* p) {return uint32_t(p[0])|uint32_t(p[1])<<8|uint32_t(p[2])<<16|uint32_t(p[3])<<24;}
static void Corpus(const char* tab_path,const char* data_path) {
    const auto table=Read(tab_path),data=Read(data_path);
    Require(table.size()%16==0,"Invalid installed table length");
    const size_t records=table.size()/16,limit=std::min<size_t>(9149,records?records-1:0);
    size_t decoded=0,empty=0,trailing=0;
    for(size_t i=0;i<limit;++i) {
        const uint8_t* meta=table.data()+i*16;
        const uint32_t begin=U32(meta),end=U32(meta+16);
        const unsigned w=meta[4],h=meta[5];
        Require(end>=begin && end<=data.size(),"Invalid installed frame extent");
        if(!w || !h) { Require(!w&&!h,"Half-empty installed frame"); ++empty; continue; }
        const auto encoded=std::span(data).subspan(begin,end-begin);
        size_t consumed=0;
        const auto expected=Legacy(encoded,w,h,256,256,512,consumed);
        std::array<uint8_t,256*256*2> output;
        const auto result=DecodeKeeperSpriteRG({output,256,256,512},encoded,w,h);
        if(!result || result.bytes_consumed!=consumed || !std::equal(output.begin(),output.end(),expected.begin()))
            throw std::runtime_error("Installed frame differs at ID "+std::to_string(i));
        ++decoded; trailing+=encoded.size()-consumed;
        // Pipe only to a hash consumer; no commercial output is saved by this runner.
        Require(std::fwrite(output.data(),1,output.size(),stdout)==output.size(),"Hash stream write failed");
    }
    std::cerr << "{\"corpus\":\"passed\",\"records\":"<<records<<",\"decoded\":"<<decoded
        <<",\"emptyPlaceholders\":"<<empty<<",\"trailingBytes\":"<<trailing<<"}\n";
}
int main(int argc,char** argv) {
    try {if(argc==3) Corpus(argv[1],argv[2]); else {Require(argc==1,"Expected no args or table/data corpus paths"); Focused();} return 0;}
    catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
