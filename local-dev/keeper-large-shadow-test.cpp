// SPDX-License-Identifier: GPL-2.0-or-later
#include "kfx/renderer/KeeperShadowMask.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <vector>

int main()
{
    std::vector<unsigned char> source(512*512);
    std::vector<unsigned char> guard(256*256+32,0xa5);
    auto *target=guard.data()+16;
    for (int y=0;y<508;++y)for(int x=0;x<508;++x)source[y*512+x]=((x/4+y/4)%2)?255:0;
    assert(keeper_shadow_logical_mask(target,source.data(),508,508,512,4));
    for(int y=0;y<127;++y)for(int x=0;x<127;++x)assert(target[y*256+x]==((x+y)%2?255:0));
    for(int y=0;y<256;++y)for(int x=0;x<256;++x)if(x>=127||y>=127)assert(target[y*256+x]==0);
    assert(std::all_of(guard.begin(),guard.begin()+16,[](auto x){return x==0xa5;}));
    assert(std::all_of(guard.end()-16,guard.end(),[](auto x){return x==0xa5;}));
    // Independent mirrored coverage authority retains logical ground geometry.
    std::fill(source.begin(),source.end(),0);
    for(int y=0;y<268;++y)for(int x=0;x<164;++x)source[y*512+x]=(x<40&&y>200)?255:0;
    assert(keeper_shadow_logical_mask(target,source.data(),164,268,512,4));
    assert(target[51*256+9]==255&&target[51*256+10]==0);
    for(int y=0;y<268;++y)std::reverse(source.begin()+y*512,source.begin()+y*512+164);
    assert(keeper_shadow_logical_mask(target,source.data(),164,268,512,4));
    assert(target[51*256+31]==255&&target[51*256+30]==0);
    const auto before=guard;
    assert(!keeper_shadow_logical_mask(target,source.data(),513,268,512,4));assert(guard==before);
    assert(!keeper_shadow_logical_mask(target,source.data(),512,512,512,1));assert(guard==before);
    assert(keeper_shadow_logical_mask(target,source.data(),512,512,512,2));
    std::puts("{\"largeSoftwareShadowMask\":\"passed\",\"mirroredCoverage\":\"passed\",\"guardBytes\":\"passed\"}");
}
