// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
// Owned pre-overlay enhanced world inputs. No engine pointers or rendered pixels.
#pragma once
#include "OutlinePass.h"
#include <cmath>
#include <stdexcept>
#include <vector>

namespace kfx::render::dx12 {
inline constexpr uint32_t MissingEnhancedTile = UINT32_MAX;
enum class EnhancedWorldRasterOrigin : uint8_t { ViewportLocal = 0, Native = 1 };
enum class EnhancedFadeAddressing : uint8_t { Sampler = 0, TexelLoad = 1 };
using EnhancedTile = std::array<uint8_t, 32 * 32>;
struct EnhancedTerrainVertex {
    OutlineDepthVertex position;
    float u = 0, v = 0, shade = 1, stl_x = -1, stl_y = -1;
};
struct EnhancedTerrainTriangle {
    std::array<EnhancedTerrainVertex, 3> vertices{};
    uint32_t tile = MissingEnhancedTile;
};
struct EnhancedShadowVertex { float x = 0, y = 0, u = 0, v = 0; };
struct EnhancedShadowQuad {
    std::array<EnhancedShadowVertex, 4> vertices{};
    float z_ndc = 0, darken = 0;
    uint32_t mask_x = 0, mask_y = 0, source_width = 0, source_height = 0;
};
struct EnhancedWorldPass {
    // Diagnostic A/B only. Capture bytes and production GL oracle stay unchanged.
    EnhancedFadeAddressing fade_addressing = EnhancedFadeAddressing::Sampler;
    // The returned resident frame is viewport_width x viewport_height. Native
    // frame coordinates remain necessary for production fog/checkerboard math.
    uint32_t native_width = 0, native_height = 0;
    int32_t viewport_x = 0, viewport_y = 0;
    uint32_t viewport_width = 0, viewport_height = 0;
    std::array<float, 4> clear_rgba{0,0,0,1};
    SpriteColourRow palette_rgba{};
    std::array<uint8_t, 256 * 64> fade{};
    // Actual applied order: fullbright, ambient, scale, gamma, lighting mode,
    // darkness mode, fog speed, fog density, tile filter. No inferred settings.
    std::array<float, 9> shade_settings{0,0,1,1,0,1,1,0.4f,0};
    float time = 0;
    std::vector<uint16_t> lightmap; // Exactly 511x511, including initialized padding.
    std::vector<EnhancedTile> tiles;
    std::vector<EnhancedTerrainTriangle> terrain;
    // Executed 012/023 quads, after all terrain and before every sprite group.
    // Normalized source UVs sample a 256x256 nearest/clamp coverage layer.
    std::vector<EnhancedShadowQuad> shadows;
};
inline void ValidateEnhancedWorldPass(const EnhancedWorldPass& pass, uint32_t atlas_width,
    uint32_t atlas_height, uint64_t atlas_bytes, size_t colour_rows)
{
    const auto require=[](bool value,const char* reason){if(!value)throw std::invalid_argument(reason);};
    require(pass.fade_addressing==EnhancedFadeAddressing::Sampler||pass.fade_addressing==EnhancedFadeAddressing::TexelLoad,
        "Invalid enhanced fade addressing mode");
    require(pass.native_width && pass.native_height && pass.native_width<=16384 && pass.native_height<=16384
        && uint64_t(pass.native_width)*pass.native_height*4<=64*1024*1024
        && pass.viewport_width && pass.viewport_height && pass.viewport_x>=0 && pass.viewport_y>=0
        && uint64_t(pass.viewport_x)+pass.viewport_width<=pass.native_width
        && uint64_t(pass.viewport_y)+pass.viewport_height<=pass.native_height,
        "Invalid enhanced world target or viewport");
    for(float value:pass.clear_rgba)require(std::isfinite(value)&&value>=0&&value<=1,"Invalid enhanced clear colour");
    require(pass.lightmap.size()==511*511 && pass.tiles.size()<=65536 && pass.terrain.size()<=262144
        && pass.shadows.size()<=65536,"Enhanced world count or lightmap bound exceeded");
    require(atlas_bytes+uint64_t(colour_rows)*1024+uint64_t(pass.tiles.size())*1024
        +pass.lightmap.size()*2+pass.palette_rgba.size()+pass.fade.size()<=64*1024*1024,
        "Enhanced world owned textures exceed 64 MiB");
    const auto& s=pass.shade_settings;
    for(float value:s)require(std::isfinite(value),"Nonfinite enhanced shade state");
    require((s[0]==0||s[0]==1)&&s[1]>=0&&s[1]<=1&&s[2]>=0.1f&&s[2]<=3
        &&s[3]>=0.1f&&s[3]<=3&&(s[4]==0||s[4]==1)&&(s[5]==0||s[5]==1||s[5]==2)
        &&s[6]>=0.1f&&s[6]<=5&&s[7]>=0&&s[7]<=1&&(s[8]==0||s[8]==1)
        &&std::isfinite(pass.time)&&std::abs(pass.time)<=16777216,"Invalid enhanced shade state");
    for(const auto& triangle:pass.terrain){
        require(triangle.tile==MissingEnhancedTile||triangle.tile<pass.tiles.size(),"Invalid enhanced tile reference");
        for(const auto& v:triangle.vertices){
            const auto& p=v.position;
            require(std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z)&&std::isfinite(p.w)
                &&std::abs(p.x)<=65536&&std::abs(p.y)<=65536&&std::abs(p.z)<=16
                &&p.w>=1&&p.w<=2147483648.0f&&std::floor(p.w)==p.w,"Invalid enhanced terrain position");
            require(std::isfinite(v.u)&&std::isfinite(v.v)&&std::abs(v.u)<=32768&&std::abs(v.v)<=32768
                &&std::isfinite(v.shade)&&std::abs(v.shade)<=1024
                &&std::isfinite(v.stl_x)&&std::isfinite(v.stl_y)&&std::abs(v.stl_x)<=65536&&std::abs(v.stl_y)<=65536,
                "Invalid enhanced terrain attributes");
        }
    }
    for(const auto& shadow:pass.shadows){
        require(shadow.source_width&&shadow.source_height&&shadow.source_width<=256&&shadow.source_height<=256
            &&uint64_t(shadow.mask_x)+shadow.source_width<=atlas_width
            &&uint64_t(shadow.mask_y)+shadow.source_height<=atlas_height
            &&std::isfinite(shadow.z_ndc)&&std::abs(shadow.z_ndc)<=16
            &&std::isfinite(shadow.darken)&&shadow.darken>=0&&shadow.darken<=1,"Invalid enhanced shadow source");
        for(const auto& p:shadow.vertices)require(std::isfinite(p.x)&&std::isfinite(p.y)
            &&std::abs(p.x)<=16777216&&std::abs(p.y)<=16777216&&std::isfinite(p.u)&&std::isfinite(p.v)
            &&std::abs(p.u)<=32768&&std::abs(p.v)<=32768,"Invalid enhanced shadow vertex");
    }
}
}
