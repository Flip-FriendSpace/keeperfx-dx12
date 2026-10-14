// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
// Owned executed UI quads inside the world/lens bracket. These records retain
// resolved graphics inputs, never live sprite handles, remap pointers or UI state.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

namespace kfx::render {
enum class SceneGLOverlayPhase : uint8_t { DepthWorld = 0, FlatWorld = 1, Swipe = 2 };
enum class SceneGLOverlayMode : uint8_t { Sprite = 0, Colored = 1, Solid = 2, Fade = 3, Clut = 4 };
inline constexpr uint32_t NoSceneGLOverlayResource = UINT32_MAX;
using SceneGLOverlayColourRow = std::array<uint8_t, 1024>;
struct SceneGLOverlaySource {
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> indices; // Original texture dimensions/layout; normalized UVs are unchanged.
    std::vector<uint8_t> initialized_rows; // Exactly height values, each 0 or 1.
    // Unknown rows have canonical zero storage, but may NEVER be sampled.
    bool operator==(const SceneGLOverlaySource&) const = default;
};
struct SceneGLOverlayQuad {
    SceneGLOverlayPhase phase = SceneGLOverlayPhase::DepthWorld;
    SceneGLOverlayMode mode = SceneGLOverlayMode::Sprite;
    bool depth_test = true; // LEQUAL, read-only D24; Swipe retains its actual sub-layer state.
    bool scissor_enabled = false;
    int32_t scissor_x = 0, scissor_y = 0, scissor_w = 0, scissor_h = 0; // Native top-down.
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0; // Actual post-graphics-window clipping.
    float u0 = 0, v0 = 0, u1 = 0, v1 = 0; // Original normalized nearest/clamp UVs; flips retained.
    float z_ndc = 0;
    std::array<float, 4> colour{1,1,1,1}; // Actual resolved vertex RGBA, not a palette index.
    uint32_t source = NoSceneGLOverlayResource;
    uint32_t colour_row = NoSceneGLOverlayResource;
    uint8_t fade_row = 0;
    bool operator==(const SceneGLOverlayQuad&) const = default;
};
struct SceneGLOverlayPass {
    uint32_t native_width = 0, native_height = 0;
    std::vector<SceneGLOverlaySource> sources;
    // Actual uploaded global palette or UI CLUT rows. Sprite/Fade ignore row
    // alpha; Clut multiplies it by vertex alpha. Colored/Solid use no row.
    std::vector<SceneGLOverlayColourRow> colour_rows;
    std::vector<uint8_t> fade; // Actual 256x64 R8 upload, present iff a Fade draw exists.
    std::vector<SceneGLOverlayQuad> draws; // Executed order, phases monotonic.
    bool operator==(const SceneGLOverlayPass&) const = default;
};

inline void ValidateSceneGLOverlayPass(const SceneGLOverlayPass& pass)
{
    const auto require=[](bool good,const char* reason){if(!good)throw std::invalid_argument(reason);};
    constexpr uint64_t budget=64u*1024u*1024u;
    require(pass.native_width&&pass.native_height&&pass.native_width<=16384&&pass.native_height<=16384
        &&uint64_t(pass.native_width)*pass.native_height*4<=budget,"Invalid native world overlay target");
    require(pass.sources.size()<=256&&pass.colour_rows.size()<=16384&&pass.draws.size()<=262144,
        "World overlay record limit exceeded");
    uint64_t bytes=uint64_t(pass.colour_rows.size())*1024+pass.fade.size();
    for(const auto& source:pass.sources){
        require(source.width&&source.height&&source.width<=16384&&source.height<=16384
            &&source.indices.size()==uint64_t(source.width)*source.height
            &&source.initialized_rows.size()==source.height,"Invalid world overlay source extent");
        bytes+=source.indices.size()+source.initialized_rows.size();
        require(bytes<=budget,"World overlay resources exceed 64 MiB");
        for(uint32_t y=0;y<source.height;++y){
            require(source.initialized_rows[y]<=1,"Invalid world overlay row proof");
            if(!source.initialized_rows[y]){
                const auto first=source.indices.begin()+size_t(y)*source.width;
                require(std::all_of(first,first+source.width,[](uint8_t v){return v==0;}),
                    "Unknown world overlay source row contains data");
            }
        }
    }
    require(bytes<=budget,"World overlay resources exceed 64 MiB");
    bool has_fade=false;unsigned previous_phase=0;
    const auto bounded=[](float value,float limit){return std::isfinite(value)&&std::abs(value)<=limit;};
    for(const auto& q:pass.draws){
        const unsigned phase=unsigned(q.phase),mode=unsigned(q.mode);
        require(phase<=2&&phase>=previous_phase&&mode<=4,"Invalid world overlay mode or phase order");
        previous_phase=phase;
        require((phase!=0||q.depth_test)&&(phase!=1||!q.depth_test),"Invalid world overlay depth state");
        require(bounded(q.x0,32768)&&bounded(q.y0,32768)&&bounded(q.x1,32768)&&bounded(q.y1,32768)
            &&q.x1>q.x0&&q.y1>q.y0&&bounded(q.z_ndc,2),"Invalid world overlay geometry");
        for(float c:q.colour)require(std::isfinite(c)&&c>=0&&c<=1,"Invalid world overlay vertex colour");
        require(q.scissor_enabled
            ? q.scissor_x>=-32768&&q.scissor_x<=32768&&q.scissor_y>=-32768&&q.scissor_y<=32768
                &&q.scissor_w>=0&&q.scissor_w<=32768&&q.scissor_h>=0&&q.scissor_h<=32768
            : q.scissor_x==0&&q.scissor_y==0&&q.scissor_w==0&&q.scissor_h==0,"Invalid world overlay scissor");
        const bool solid=q.mode==SceneGLOverlayMode::Solid;
        const bool coloured=q.mode==SceneGLOverlayMode::Colored;
        const bool fade=q.mode==SceneGLOverlayMode::Fade;
        has_fade|=fade;
        require(fade?q.fade_row<64:q.fade_row==0,"Invalid or unused world overlay fade row");
        require(solid||coloured?q.colour_row==NoSceneGLOverlayResource:q.colour_row<pass.colour_rows.size(),
            "Invalid or unused world overlay colour row");
        if(solid){
            require(q.source==NoSceneGLOverlayResource&&q.u0==0&&q.v0==0&&q.u1==0&&q.v1==0,
                "Unused solid world overlay source");
            continue;
        }
        require(q.source<pass.sources.size(),"Invalid world overlay source reference");
        for(float uv:{q.u0,q.v0,q.u1,q.v1})
            require(std::isfinite(uv)&&uv>=0&&uv<=1,"Invalid normalized world overlay source coordinate");
        const auto& source=pass.sources[q.source];
        // Closed endpoint range also covers mirrored and clipped quads. Include
        // the adjacent binary32 values so a rounded endpoint cannot read an
        // unproved neighbour row. This does not alter shader coordinates.
        const double low=std::nextafter(std::min(q.v0,q.v1),-std::numeric_limits<float>::infinity());
        const double high=std::nextafter(std::max(q.v0,q.v1),std::numeric_limits<float>::infinity());
        const auto row=[&](double v){return uint32_t(std::clamp(std::floor(v*source.height),0.0,double(source.height-1)));};
        const auto first=row(low),last=row(high);
        for(uint32_t y=first;y<=last;++y)
            require(source.initialized_rows[y]!=0,"World overlay sample reaches an uninitialized source row");
    }
    require(pass.fade.size()==(has_fade?16384u:0u),"Missing or unused world overlay fade table");
}
}
