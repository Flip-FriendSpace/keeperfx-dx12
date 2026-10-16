// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX remaster changes: 2026-10-16.
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <span>

namespace kfx::render {
// Presentation only. Never changes simulation lights or reads concealed things.
// Modes deliberately fit the unused colour of Normal/GlowAdd draw records.
enum class EmissiveSource : uint8_t { None, EncodedGlow, Spell, Portal, Heart };
inline bool EmissivePresentationEnabled() noexcept {
    // Immutable for this process; exact token avoids accidentally enabling "0".
    static const bool enabled=[] {
        const char* value=std::getenv("KFX_DX12_EMISSIVE");
        return value && std::strcmp(value,"source-v1")==0;
    }();
    return enabled;
}
inline EmissiveSource ClassifyEmissiveSource(uint32_t /*animation*/,bool additive,
    uint32_t thing_class=0,uint32_t model=0) noexcept {
    // Native Heart flames already encode their glow. Match consumers before
    // generic additive classification; shared spell/torch artwork is unaffected.
    if(thing_class==1) switch(model) {
    case 111: case 120: case 121: case 122:
    case 162: case 165: case 167: case 169:return EmissiveSource::None;
    }
    // Additive is explicit engine metadata, not a palette-brightness guess.
    if(additive)return EmissiveSource::EncodedGlow;
    // Configured HD aliases replace Thing.anim_sprite itself. Canonical
    // consumer identity survives that change; numeric animation matching does not.
    // These stable class/model IDs are authenticated against engine enums in
    // the producer and inventoried config consumer records in the handoff.
    if(thing_class==1 && model==5)return EmissiveSource::Heart;
    if(thing_class==3 && model==37)return EmissiveSource::Portal;
    if(thing_class==2) switch(model) {
    case 1: case 3: case 5: case 10: case 24: case 25: case 28:
        return EmissiveSource::Spell;
    }
    if(thing_class==3) switch(model) {
    case 1: case 9: case 15: case 18: case 39: case 42: case 52:
    case 53: case 60: case 115:return EmissiveSource::Spell;
    }
    return EmissiveSource::None;
}
inline std::array<float,4> EmissiveParameters(EmissiveSource source) noexcept {
    // Gain/bloom/mask and opt-in sentinel. Heart lighting is bounded before lookup.
    switch(source) {
    case EmissiveSource::EncodedGlow:return {0.18f,0.10f,0,0.9375f};
    case EmissiveSource::Spell:return {0.12f,0.06f,0.25f,0.9375f};
    case EmissiveSource::Portal:return {0.08f,0.04f,0.25f,0.9375f};
    case EmissiveSource::Heart:return {};
    default:return {};
    }
}
// Preserve source detail before the native overbright palette rows quantize it
// away. Only authenticated complete fade rows may change; tint/flash maps,
// cursors and additive draws remain authoritative. No global table is mutated.
inline const uint8_t* HeartPresentationRemap(bool enabled,bool cursor,bool additive,
    uint32_t thing_class,uint32_t model,const uint8_t* remap,
    std::span<const uint8_t> fade_tables) noexcept {
    if(!enabled || cursor || additive || thing_class!=1 || model!=5 || !remap
        || fade_tables.size()!=64*256)return remap;
    const uintptr_t address=reinterpret_cast<uintptr_t>(remap);
    const uintptr_t base=reinterpret_cast<uintptr_t>(fade_tables.data());
    if(address<base)return remap;
    const uintptr_t offset=address-base;
    if(offset>fade_tables.size()-256 || offset%256!=0)return remap;
    const unsigned row=unsigned(offset/256);
    if(row<=32)return remap;
    // Native rows33..63 amplify by up to3.90625 before palette requantization.
    // Retain a restrained, monotonic light pulse: neutral through1.1875x.
    const unsigned effective=32+(row-32)*2/31;
    return fade_tables.data()+effective*256;
}
inline bool EmissiveLavaMaterial(uint32_t logical_tile,uint32_t variation) noexcept {
    // STANDARD only. Animated aliases follow the actual native source offset.
    return variation==0 && ((logical_tile>=56 && logical_tile<=71)
        || logical_tile==546 || logical_tile==547);
}
inline constexpr std::array<std::array<float,3>,8> EmissiveSteps{{
    {16.f/255,16.f/255,16.f/255},{24.f/255,16.f/255,0},
    {24.f/255,4.f/255,4.f/255},{8.f/255,8.f/255,24.f/255},
    {8.f/255,24.f/255,8.f/255},{12.f/255,0,12.f/255},
    {8.f/255,8.f/255,8.f/255},{24.f/255,12.f/255,4.f/255}
}};
inline std::array<float,3> PositiveEncodedEmission(uint8_t index) noexcept {
    if(index<1 || index>64)return {};
    const unsigned code=index-1,family=code/8,strength=code%8;
    if(family==6 || strength==0)return {};
    auto result=EmissiveSteps[family];for(auto& v:result)v*=float(strength);return result;
}
// Optional lighting-owner input: use the exact immutable, registered frame
// selected by the producer. No wall clock, phase oscillator, temporal history,
// hidden light search or automatic simulation-light modulation is permitted.
struct EmissiveFrameEnergy {
    uint64_t epoch=0,slot=0,incarnation=0,source_revision=0;
    uint32_t frame=0;
    bool valid=false;
    float gain=0; // Mean positive encoded energy, normalized to the same canvas.
    std::array<float,3> rgb{}; // Existing palette-space code values, NOT linear RGB.
};
inline EmissiveFrameEnergy SampleEncodedFrame(std::span<const uint8_t> rgba,
    uint32_t width,uint32_t height,uint32_t stride,EmissiveFrameEnergy identity={}) noexcept {
    identity.valid=false;identity.gain=0;identity.rgb={};
    if(!width || !height || width>1024 || height>1024 || stride<uint64_t(width)*4
        || uint64_t(height-1)*stride+uint64_t(width)*4>rgba.size())return identity;
    for(uint32_t y=0;y<height;++y)for(uint32_t x=0;x<width;++x) {
        const size_t at=size_t(y)*stride+size_t(x)*4;
        // Glow uses raw index even where coverage is unset, matching Outline.
        const auto c=PositiveEncodedEmission(rgba[at]);
        for(size_t k=0;k<3;++k)identity.rgb[k]+=c[k];
    }
    for(auto& c:identity.rgb)c/=float(uint64_t(width)*height);
    identity.gain=std::max({identity.rgb[0],identity.rgb[1],identity.rgb[2]});
    identity.valid=true;
    return identity;
}
}
