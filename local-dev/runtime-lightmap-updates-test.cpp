// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
// Independent CPU rectangle replay; no GPU, window or commercial assets.
// g++ -std=c++20 -O2 -Wall -Wextra -Werror -pedantic -Isrc local-dev/runtime-lightmap-updates-test.cpp -o /tmp/runtime-lightmap-updates-test
#include "kfx/renderer/d3d12/RuntimeLightmapUpdates.h"
#include <algorithm>
#include <iostream>
#include <random>
#include <stdexcept>
#include <vector>
using namespace kfx::render::dx12;
namespace {
constexpr size_t Side=511,Bytes=Side*Side*2;
uint64_t checks=0,replayed_bytes=0,random_cases=0;
void Check(bool value,const char* why){++checks;if(!value)throw std::runtime_error(why);}
void Set(std::vector<uint8_t>& p,size_t x,size_t y,uint16_t n){const auto at=(y*Side+x)*2;p[at]=uint8_t(n);p[at+1]=uint8_t(n>>8);}
RuntimeLightmapUpdatePlan Replay(const std::vector<uint8_t>& baseline,const std::vector<uint8_t>& target){auto plan=PlanRuntimeLightmapUpdates(baseline,target);Check(plan.count<=256,"Rectangle cap exceeded");std::vector<uint8_t> copy=baseline.empty()?std::vector<uint8_t>(Bytes,0xa5):baseline;std::vector<uint8_t> visited(Side*Side);uint64_t bytes=0;for(uint32_t i=0;i<plan.count;++i){const auto& r=plan.rectangles[i];Check(r.Valid()&&uint64_t(r.x)+r.width<=Side&&uint64_t(r.y)+r.height<=Side,"Invalid rectangle");bytes+=uint64_t(r.width)*r.height*2;for(uint32_t y=r.y;y<r.y+r.height;++y)for(uint32_t x=r.x;x<r.x+r.width;++x){const auto px=size_t(y)*Side+x;Check(!visited[px],"Rectangles overlap");visited[px]=1;copy[px*2]=target[px*2];copy[px*2+1]=target[px*2+1];}}Check(bytes==plan.upload_bytes,"Logical copied-byte accounting differs");Check(copy==target,"Replayed rectangles do not reproduce target");replayed_bytes+=bytes;return plan;}
void Invalid(const std::vector<uint8_t>& a,const std::vector<uint8_t>& b){bool threw=false;try{(void)PlanRuntimeLightmapUpdates(a,b);}catch(const std::invalid_argument&){threw=true;}Check(threw,"Invalid byte span accepted");}
}
int main(){try{
    std::vector<uint8_t> zero(Bytes),a(Bytes),b(Bytes);for(size_t y=0;y<Side;++y)for(size_t x=0;x<Side;++x)Set(a,x,y,uint16_t((x*71+y*137)^0xa591));
    auto cold=Replay({},a);Check(cold.count==1&&cold.upload_bytes==Bytes&&!cold.bounding_box_fallback,"Cold upload not full image");auto same=Replay(a,a);Check(same.count==0&&same.upload_bytes==0&&!same.bounding_box_fallback,"Identical bytes copied");
    b=a;b[0]^=1;b[1]^=128;b[Bytes-1]^=1;b[(510*Side)*2]^=1;b[510*2+1]^=1;auto edges=Replay(a,b);Check(edges.upload_bytes==8,"Both R16 bytes or last row/column missed");
    b=zero;for(size_t y=17;y<28;++y)for(size_t x=9;x<32;++x)Set(b,x,y,0x0100);auto merged=Replay(zero,b);Check(merged.count==1&&merged.upload_bytes==23*11*2&&!merged.bounding_box_fallback,"Equal consecutive runs did not coalesce");
    b=zero;for(size_t n=0;n<257;++n)Set(b,(n%129)*3,(n/129)*3,uint16_t(n+1));auto fallback=Replay(zero,b);Check(fallback.bounding_box_fallback&&fallback.count==1&&fallback.rectangles[0].x==0&&fallback.rectangles[0].y==0&&fallback.rectangles[0].width==385&&fallback.rectangles[0].height==4&&fallback.upload_bytes==385*4*2,"Scattered fallback did not report full dirty bounding box");
    Set(b,510,510,0xabcd);auto late=Replay(zero,b);Check(late.bounding_box_fallback&&late.count==1&&late.rectangles[0].width==511&&late.rectangles[0].height==511&&late.upload_bytes==Bytes,"Fallback stopped scanning before late final texel");
    std::vector<uint8_t> older=zero,latest=zero;Set(older,2,3,1);Set(latest,400,499,0xff00);auto skipped=Replay(older,latest);Check(skipped.upload_bytes==4,"Skipped revision failed to undo old image contents");
    std::mt19937 random(0x51ab19);for(unsigned test=0;test<160;++test){b=a;const unsigned changes=test%3?1+random()%200:300+random()%600;for(unsigned n=0;n<changes;++n)b[random()%Bytes]^=uint8_t(1+random()%255);Replay(a,b);if(test%4==0)a=b;++random_cases;}
    Invalid({},{});Invalid(a,std::vector<uint8_t>(Bytes-1));Invalid(a,std::vector<uint8_t>(Bytes+1));Invalid(std::vector<uint8_t>(Bytes-1),b);Invalid(std::vector<uint8_t>(Bytes+1),b);
    for(const auto& bad:std::vector<RuntimeLightmapRect>{{0,0,0,1},{0,0,1,0},{511,0,1,1},{0,511,1,1},{510,510,2,1},{0,0,UINT32_MAX,1},{UINT32_MAX,0,2,1}}){Check(!bad.Valid(),"Overflowing rectangle accepted");}
    Check(RuntimeLightmapRect{510,510,1,1}.Valid(),"Final texel rejected");
    std::cout<<"{\"passed\":true,\"checks\":"<<checks<<",\"randomCases\":"<<random_cases<<",\"replayedBytes\":"<<replayed_bytes<<",\"bothR16Bytes\":true,\"skippedRevisions\":true,\"bboxFallback\":true}\n";return 0;
}catch(const std::exception& e){std::cerr<<"CPU lightmap test failed: "<<e.what()<<'\n';return 1;}}
