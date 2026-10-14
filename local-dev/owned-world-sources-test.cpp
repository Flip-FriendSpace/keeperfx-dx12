// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#include "kfx/renderer/OwnedWorldSources.h"
#include <cassert>
#include <iostream>
#include <memory>

using namespace kfx::render;
int main()
{
    OwnedRenderAssets assets(1);
    auto sources=std::make_unique<OwnedWorldSources>(assets,4);
    std::array<uint8_t,1024> display{};
    std::array<uint8_t,768> sprite{};
    std::array<uint8_t,16384> fade{};
    for(size_t i=0;i<256;++i) {
        display[i*4]=uint8_t(i); display[i*4+1]=uint8_t(255-i); display[i*4+3]=255;
        sprite[i*3]=uint8_t(i%64); sprite[i*3+1]=uint8_t((i/4)%64);
    }
    for(size_t i=0;i<fade.size();++i) fade[i]=uint8_t(i);
    std::array<uint16_t,6> lights{0x102,0x304,0x506,0x708,0x90a,0xb0c};
    OwnedWorldSourceInputs input;
    input.epoch=1; input.display_palette_rgba=display; input.sprite_palette_rgb6=sprite;
    input.base_palette_rgb6=sprite; input.fade=fade; input.lightmap=lights;
    input.lightmap_width=2; input.lightmap_height=3;
    input.settings.shade_scale=input.settings.shade_gamma=input.settings.fog_speed=1;
    auto first=sources->Update(input); assert(first);
    const auto word=[&](const OwnedAssetLease& image,size_t x,size_t y) {
        const size_t at=(y*511+x)*2; const auto bytes=image.Bytes();
        return unsigned(bytes[at]) | (unsigned(bytes[at+1])<<8);
    };
    assert(word(first.state.lightmap,0,0)==0x102);
    assert(word(first.state.lightmap,510,0)==0x304);
    assert(word(first.state.lightmap,0,510)==0x90a);
    assert(word(first.state.lightmap,510,510)==0xb0c);
    assert(first.state.palette.Bytes()[4]==1);
    assert(first.state.sprite_palette.Bytes()[4]==4); // Display and RGB6 remain distinct.
    const auto baseline=assets.Stats();
    auto same=sources->Update(input); assert(same && same.status==OwnedAssetStatus::Reused);
    assert(assets.Stats().publications==baseline.publications);
    assert(assets.Stats().payload_bytes==baseline.payload_bytes);

    lights[0]=0xffff;
    auto changed=sources->Update(input); assert(changed);
    assert(changed.state.palette.Key()==first.state.palette.Key());
    assert(changed.state.lightmap.Key().content_revision==first.state.lightmap.Key().content_revision+1);
    assert(word(changed.state.lightmap,0,0)==0xffff && word(first.state.lightmap,0,0)==0x102);
    assert(assets.Stats().publications==baseline.publications+1);
    auto invalid=input; invalid.lightmap_height=4;
    assert(!sources->Update(invalid));
    assert(sources->Update(input).state.lightmap.Key()==changed.state.lightmap.Key());

    // Level reset cannot mutate an older frame's owned palette or lightmap.
    assert(assets.AdvanceEpoch(2)); input.epoch=2; display[4]=9;
    auto reset=sources->Update(input); assert(reset && reset.state.palette.Key().epoch==2);
    assert(reset.state.palette.Bytes()[4]==9 && first.state.palette.Bytes()[4]==1);
    assert(word(first.state.lightmap,510,510)==0xb0c);

    // Pressure after publishing the small globals retains progress for retry.
    OwnedAssetLimits limits; limits.bytes=1024+16384;
    OwnedRenderAssets small(1,limits);
    auto constrained=std::make_unique<OwnedWorldSources>(small,4);
    input.epoch=1;
    assert(constrained->Update(input).status==OwnedAssetStatus::WouldBlock);
    const auto publications=small.Stats().publications;
    assert(publications==2);
    assert(constrained->Update(input).status==OwnedAssetStatus::WouldBlock);
    assert(small.Stats().publications==publications);
    std::cout<<"Owned world sources: reuse, changed lights, old frame ownership, epoch reset and pressure passed\n";
}
