// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#include "pre_inc.h"
#include "OwnedWorldLensProducer.h"
#include "ir/WorldCommands.h"
#include "bflib_video.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <stdexcept>
#include "post_inc.h"

namespace kfx::render {
OwnedWorldLensProducer::OwnedWorldLensProducer(OwnedRenderAssets& assets,uint64_t domain)
    :assets_(assets),domain_(domain),base_(PaletteIndexTableBytes),table_(PaletteIndexTableBytes)
{
    if(!domain||domain==UINT64_MAX)throw std::invalid_argument("Invalid owned lens source domain");
}
void OwnedWorldLensProducer::ResetEpoch(uint64_t epoch) noexcept
{
    if(epoch_==epoch)return;
    epoch_=epoch;generation_=1;current_={};revisions_={};have_shape_={};have_base_=have_display_=false;
}
OwnedAssetResult OwnedWorldLensProducer::Publish(size_t slot,OwnedAssetDesc desc,std::span<const uint8_t> bytes) noexcept
{
    auto& old=current_[slot];
    if(old.Valid()&&old.Desc()==desc&&std::equal(bytes.begin(),bytes.end(),old.Bytes().begin(),old.Bytes().end()))
        return {OwnedAssetStatus::Reused,OwnedAssetPressure::None,old};
    if(revisions_[slot]>=UINT64_MAX-1)return {};
    const OwnedAssetKey key{epoch_,domain_,slot+1,generation_,revisions_[slot]+1};
    const size_t pixel=desc.format==OwnedAssetFormat::RG16SIntLE?4:1;
    auto result=assets_.Publish(key,{desc,bytes,size_t(desc.width)*pixel});
    if(result){old=result.lease;revisions_[slot]=key.content_revision;shapes_[slot]=desc;have_shape_[slot]=true;}
    return result;
}
OwnedWorldLensResult OwnedWorldLensProducer::Update(const OwnedWorldLensInputs& input) noexcept
{
    OwnedWorldLensResult result;
    try {
        if(!input.evaluated||!input.epoch||input.epoch==UINT64_MAX||input.epoch<epoch_
            ||!input.native_width||!input.native_height||input.native_width>16384||input.native_height>16384
            ||uint64_t(input.native_width)*input.native_height>16u*1024u*1024u)return result;
        for(float value:input.clear_rgba)if(!std::isfinite(value)||value<0||value>1)return result;
        const auto& cmd=*input.evaluated;
        if(unsigned(cmd.type)>unsigned(LensPixelEffectType::Overlay))return result;
        ResetEpoch(input.epoch);
        if(!cmd.active){result.status=OwnedAssetStatus::Reused;result.ready=true;return result;}
        if(cmd.viewport_x<0||cmd.viewport_y<0||cmd.viewport_w<=0||cmd.viewport_h<=0
            ||uint64_t(cmd.viewport_x)+uint32_t(cmd.viewport_w)>input.native_width
            ||uint64_t(cmd.viewport_y)+uint32_t(cmd.viewport_h)>input.native_height)return result;
        auto& lens=result.lens;
        lens.kind=OwnedLensKind::Copy;lens.source_x=lens.destination_x=cmd.viewport_x;lens.destination_y=cmd.viewport_y;
        lens.viewport_width=uint32_t(cmd.viewport_w);lens.viewport_height=uint32_t(cmd.viewport_h);lens.clear_rgba=input.clear_rgba;
        const bool mist=cmd.type==LensPixelEffectType::Mist,overlay=cmd.type==LensPixelEffectType::Overlay;
        const bool remap=cmd.type==LensPixelEffectType::Displacement||cmd.type==LensPixelEffectType::Flyeye;
        std::array<bool,5> required{};
        std::array<OwnedAssetDesc,5> descriptions{};
        std::array<std::span<const uint8_t>,5> sources{};
        if(mist||overlay) {
            if(!input.display_palette.Valid()||input.display_palette.Key().epoch!=input.epoch
                ||input.display_palette.Desc()!=OwnedAssetDesc{OwnedAssetFormat::RGBA8,256,1}
                ||input.base_palette_rgb6.size()!=768)return result;
            const auto palette=input.display_palette.Bytes();
            for(size_t i=3;i<1024;i+=4)if(palette[i]!=255)return result;
            for(uint8_t value:input.base_palette_rgb6)if(value>63)return result;
            const bool base_changed=!have_base_||!std::equal(input.base_palette_rgb6.begin(),input.base_palette_rgb6.end(),base_palette_.begin());
            const bool display_changed=!have_display_||!std::equal(palette.begin(),palette.end(),display_palette_.begin());
            if(base_changed) {
                BuildPaletteIndexBase(base_,input.base_palette_rgb6,LbPaletteFindColour);
                std::copy(input.base_palette_rgb6.begin(),input.base_palette_rgb6.end(),base_palette_.begin());
                have_base_=true;table_=base_;
            } else if(display_changed&&have_display_) {
                for(auto key:palette_keys_)table_[key]=base_[key];
            }
            if(base_changed||display_changed) {
                ApplyPaletteIndexOverrides(table_,palette,palette_keys_);
                std::copy(palette.begin(),palette.end(),display_palette_.begin());have_display_=true;
            }
            lens.palette=input.display_palette;required[0]=true;descriptions[0]={OwnedAssetFormat::R8,512,512};sources[0]=table_;
            required[4]=true;descriptions[4]={OwnedAssetFormat::R8,512,512};sources[4]=base_;
        }
        if(mist) {
            if(cmd.mist_pixels.size()!=256*256||cmd.mist_lightness>63||!input.fade.Valid()
                ||input.fade.Key().epoch!=input.epoch||input.fade.Desc()!=OwnedAssetDesc{OwnedAssetFormat::R8,256,64})return result;
            lens.mist_offsets={cmd.mist_pos_x,cmd.mist_pos_y,cmd.mist_sec_x,cmd.mist_sec_y};
            for(float value:lens.mist_offsets)if(!std::isfinite(value)||value<0||value>=256)return result;
            lens.kind=OwnedLensKind::Mist;lens.fade=input.fade;lens.mist_lightness=cmd.mist_lightness;
            required[1]=true;descriptions[1]={OwnedAssetFormat::R8,256,256};sources[1]=cmd.mist_pixels;
        } else if(remap) {
            if(cmd.remap_w<=0||cmd.remap_h<=0||cmd.remap_w>16384||cmd.remap_h>16384
                ||uint64_t(cmd.remap_w)*cmd.remap_h>16u*1024u*1024u
                ||cmd.remap_pixels.size()!=uint64_t(cmd.remap_w)*cmd.remap_h*2)return result;
            lens.kind=OwnedLensKind::Remap;required[2]=true;
            descriptions[2]={OwnedAssetFormat::RG16SIntLE,uint32_t(cmd.remap_w),uint32_t(cmd.remap_h)};
            if constexpr(std::endian::native==std::endian::little) {
                sources[2]={reinterpret_cast<const uint8_t*>(cmd.remap_pixels.data()),cmd.remap_pixels.size()*2};
            } else {
                // Portable byte serialization preserves65535 and every other raw
                // unsigned coordinate; it never sign-extends an int16_t sample.
                remap_bytes_.resize(cmd.remap_pixels.size()*2);
                for(size_t i=0;i<cmd.remap_pixels.size();++i){const auto v=uint16_t(cmd.remap_pixels[i]);remap_bytes_[2*i]=uint8_t(v);remap_bytes_[2*i+1]=uint8_t(v>>8);}
                sources[2]=remap_bytes_;
            }
        } else if(overlay) {
            if(cmd.overlay_w<=0||cmd.overlay_h<=0||cmd.overlay_w>16384||cmd.overlay_h>16384
                ||uint64_t(cmd.overlay_w)*cmd.overlay_h>16u*1024u*1024u
                ||cmd.overlay_pixels.size()!=uint64_t(cmd.overlay_w)*cmd.overlay_h
                ||!std::isfinite(cmd.overlay_alpha)||cmd.overlay_alpha<0||cmd.overlay_alpha>1)return result;
            lens.kind=OwnedLensKind::Overlay;lens.overlay_alpha=uint16_t(cmd.overlay_alpha*256.0f+0.5f);
            required[3]=true;descriptions[3]={OwnedAssetFormat::R8,uint32_t(cmd.overlay_w),uint32_t(cmd.overlay_h)};sources[3]=cmd.overlay_pixels;
        }
        bool reshape=false;
        for(size_t i=0;i<5;++i)if(required[i]&&have_shape_[i]&&shapes_[i]!=descriptions[i])reshape=true;
        if(reshape) {
            if(generation_>=UINT64_MAX-1||!assets_.ResetBank(domain_,generation_+1))return result;
            ++generation_;current_={};revisions_={};have_shape_={};
        }
        result.status=OwnedAssetStatus::Reused;
        for(size_t i=0;i<5;++i)if(required[i]) {
            const auto asset=Publish(i,descriptions[i],sources[i]);
            if(!asset) {
                // Keep this command's successful publications, but let catalog
                // pressure reclaim auxiliary payloads needed only by older modes.
                if(asset.status==OwnedAssetStatus::WouldBlock) {
                    for(size_t slot=0;slot<5;++slot)if(!required[slot])current_[slot]={};
                    assets_.Collect(true);
                }
                result.status=asset.status;result.pressure=asset.pressure;result.lens={};return result;
            }
            if(asset.status!=OwnedAssetStatus::Reused)result.status=asset.status;
            switch(i){case 0:lens.rgb6=asset.lease;break;case 1:lens.density=asset.lease;break;case 2:lens.remap=asset.lease;break;case 3:lens.overlay=asset.lease;break;case 4:lens.rgb6_base=asset.lease;break;}
        }
        result.ready=true;return result;
    }catch(const std::bad_alloc&){result.status=OwnedAssetStatus::AllocationFailed;}
    catch(...){result.status=OwnedAssetStatus::InvalidSource;}
    result.lens={};return result;
}
}
