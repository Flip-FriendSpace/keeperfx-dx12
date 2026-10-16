// SPDX-License-Identifier: GPL-2.0-or-later
#include "kfx/renderer/HdUiSpritePack.h"
#include <cassert>
#include <fstream>
#include <iterator>
#include <iostream>
#include <algorithm>
using namespace kfx::render;
static unsigned checks=0;
static void Check(bool yes){++checks;if(!yes){std::cerr<<"failed check "<<checks<<"\n";std::abort();}}
int main(int argc,char** argv){
    Check(HdUiFingerprint(reinterpret_cast<const uint8_t*>("hello"),5)==UINT64_C(0xa430d84680aabd0b));
    for(int n=1;n<argc;++n){
        std::ifstream f(argv[n],std::ios::binary);std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)),{});Check(!bytes.empty());
        std::vector<HdUiRecord> records;Check(ParseHdUiPack(bytes,records));Check(!records.empty());
        for(const auto& r:records){size_t used=0;Check(HdUiRleExtent(r.rle.data(),r.rle.size(),r.width,r.height,used));Check(used==r.rle.size());Check(r.width==r.logical_width*r.scale);Check(r.height==r.logical_height*r.scale);}
        const auto keep=records;
        // Every truncation is refused transactionally; accepted output survives.
        for(size_t k=0;k<bytes.size();k+=(bytes.size()/997+1)){
            auto short_bytes=std::vector<uint8_t>(bytes.begin(),bytes.begin()+k);Check(!ParseHdUiPack(short_bytes,records));Check(records.size()==keep.size());Check(records.front().rle==keep.front().rle);
        }
        for(size_t p:{size_t(0),size_t(4),size_t(6),size_t(8),size_t(16),size_t(20),size_t(24),size_t(25),size_t(26),size_t(36),size_t(40)}){
            auto bad=bytes;bad[p]^=0xff;Check(!ParseHdUiPack(bad,records));Check(records.front().rle==keep.front().rle);
        }
        auto trailing=bytes;trailing.push_back(0);Check(!ParseHdUiPack(trailing,records));
        // Fabricate a duplicate record index by copying the complete first record.
        const uint32_t size=uint32_t(bytes[36])|(uint32_t(bytes[37])<<8)|(uint32_t(bytes[38])<<16)|(uint32_t(bytes[39])<<24);
        std::vector<uint8_t> one(bytes.begin(),bytes.begin()+44+size);one[8]=2;one[9]=one[10]=one[11]=0;std::vector<uint8_t> first(bytes.begin()+12,bytes.begin()+44+size);one.insert(one.end(),first.begin(),first.end());Check(!ParseHdUiPack(one,records));
        std::cout<<argv[n]<<": "<<keep.size()<<" records accepted; malformed inputs refused\n";
    }
    size_t used=999;uint8_t literal0[]={1,0,0};Check(HdUiRleExtent(literal0,3,1,1,used)&&used==3);
    uint8_t overflow[]={2,1,1,0};Check(!HdUiRleExtent(overflow,4,1,1,used));
    uint8_t truncated[]={1};Check(!HdUiRleExtent(truncated,1,1,1,used));
    uint8_t skip[]={128,0};Check(HdUiRleExtent(skip,2,128,1,used));Check(!HdUiRleExtent(skip,2,127,1,used));
    Check(!HdUiRleExtent(skip,2,1025,1,used));
    std::cout<<"PASS "<<checks<<" checks\n";
}
