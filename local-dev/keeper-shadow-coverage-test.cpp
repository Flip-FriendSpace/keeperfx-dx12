// SPDX-License-Identifier: GPL-2.0-or-later
#include "kfx/renderer/KeeperShadowCoverage.h"
#include "kfx/renderer/KeeperShadowMask.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

int main()
{
    assert(keeper_shadow_coverage_uv(1023.0f/256, 2) == 511.0f/256);
    assert(keeper_shadow_coverage_uv(511.0f/256, 1) == 511.0f/256);
    assert(keeper_shadow_coverage_uv(659.0f/256, 2) == 329.5f/256);

    // Exact 1024 boundary, awkward Bile axes, odd padding, source stride, mirrors.
    for (const auto& [width,height] : {std::pair{660,396}, {1024,1024}, {659,397}, {100,70}}) {
        constexpr int stride=1024*2;
        std::vector<unsigned char> source(size_t(stride)*1024,0);
        std::vector<unsigned char> guarded(512*512*2+32,0xa5);
        auto* target=guarded.data()+16;
        for(int y=0;y<height;++y)for(int x=0;x<width;++x) {
            source[size_t(y)*stride+x*2]=uint8_t((x+y)%256);
            source[size_t(y)*stride+x*2+1]=((x/8+y/8)%2)?255:0;
        }
        for(bool mirror:{false,true}) {
            if(mirror)for(int y=0;y<height;++y)for(int x=0;x<width/2;++x)
                for(int c=0;c<2;++c)std::swap(source[size_t(y)*stride+x*2+c],source[size_t(y)*stride+(width-x-1)*2+c]);
            assert(keeper_shadow_reduce_rg(target,512*512*2,source.data(),source.size(),width,height,stride));
            for(int y=0;y<512;++y)for(int x=0;x<512;++x) {
                assert(target[(y*512+x)*2]==0); // Coverage-only; no colour alias.
                assert(target[(y*512+x)*2+1]==(x<(width+1)/2&&y<(height+1)/2
                    ? source[size_t(y*2)*stride+x*4+1]:0));
            }
            assert(std::all_of(guarded.begin(),guarded.begin()+16,[](auto b){return b==0xa5;}));
            assert(std::all_of(guarded.end()-16,guarded.end(),[](auto b){return b==0xa5;}));
        }
        const auto before=guarded;
        assert(!keeper_shadow_reduce_rg(target,512*512*2,source.data(),source.size(),1025,height,stride));
        assert(!keeper_shadow_reduce_rg(target,512*512*2-1,source.data(),source.size(),width,height,stride));
        assert(!keeper_shadow_reduce_rg(target,512*512*2,source.data(),1,width,height,stride));
        assert(!keeper_shadow_reduce_rg(target,512*512*2,source.data(),source.size(),width,height,width*2-1));
        assert(!keeper_shadow_reduce_rg(nullptr,512*512*2,source.data(),source.size(),width,height,stride));
        assert(guarded==before);
        assert(!keeper_shadow_reduce_rg(source.data(),source.size(),source.data(),source.size(),width,height,stride));
    }
    float u[4]={0,0,511.0f/256,511.0f/256},v[4]={511.0f/256,0,0,511.0f/256};
    assert(keeper_shadow_coverage_factor(512,512,u,v)==1); // Legacy UVs untouched.
    u[2]=u[3]=659.0f/256;v[0]=v[3]=395.0f/256;
    assert(keeper_shadow_coverage_factor(660,396,u,v)==2);
    assert(keeper_shadow_coverage_factor(100,70,u,v)==2); // Larger frame padding.
    assert(std::floor(double(u[2]/2)*256)==329);
    u[2]=u[3]=1023.0f/256;
    assert(keeper_shadow_coverage_factor(1024,1024,u,v)==2);
    assert(std::floor(double(u[2]/2)*256)==511);
    assert(keeper_shadow_coverage_factor(1025,1024,u,v)==0);
    u[2]=4;assert(keeper_shadow_coverage_factor(1024,1024,u,v)==0);
    u[2]=std::numeric_limits<float>::quiet_NaN();assert(keeper_shadow_coverage_factor(1024,1024,u,v)==0);
    for(int32_t id:{0,1,32767,std::numeric_limits<int32_t>::max()}) {
        assert(keeper_shadow_atlas_cache_key(id,0)==id);
        assert(keeper_shadow_atlas_cache_key(id,1)<0);
        assert(keeper_shadow_atlas_cache_key(id,0)!=keeper_shadow_atlas_cache_key(id,1));
    }
    std::vector<unsigned char> software_source(1024*1024,255),software_guard(256*256+32,0xa5);
    assert(keeper_shadow_logical_mask(software_guard.data()+16,software_source.data(),660,396,1024,4));
    assert(software_guard[16+98*256+164]==255&&software_guard[16+99*256+164]==0);
    assert(keeper_shadow_logical_mask(software_guard.data()+16,software_source.data(),1024,1024,1024,4));
    assert(std::all_of(software_guard.begin(),software_guard.begin()+16,[](auto b){return b==0xa5;}));
    assert(std::all_of(software_guard.end()-16,software_guard.end(),[](auto b){return b==0xa5;}));
    const auto before=software_guard;
    assert(!keeper_shadow_logical_mask(software_guard.data()+16,software_source.data(),1025,1024,1024,4));
    assert(!keeper_shadow_logical_mask(software_guard.data()+16,software_source.data(),1024,1024,1024,2));
    assert(software_guard==before);
    std::puts("1024 shadow boundaries,660x396/padding,mirrors,UVs,cache variants and guards passed");
}
