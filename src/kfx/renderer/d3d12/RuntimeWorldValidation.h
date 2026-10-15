// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#pragma once
#include "RuntimeWorld.h"
#include "kfx/renderer/SpriteRasterBounds.h"
#include <cmath>
#include <stdexcept>
#include <string>

namespace kfx::render::dx12 {
inline void ValidateRuntimeWorldFrame(const RuntimeWorldFrame& p)
{
    const auto require = [](bool value, const char* why) { if (!value) throw std::invalid_argument(why); };
    require(p.native_width && p.native_height && p.native_width <= 16384 && p.native_height <= 16384
        && uint64_t(p.native_width) * p.native_height <= 16ull * 1024 * 1024
        && p.viewport_x >= 0 && p.viewport_y >= 0 && p.viewport_width && p.viewport_height
        && uint64_t(p.viewport_x) + p.viewport_width <= p.native_width
        && uint64_t(p.viewport_y) + p.viewport_height <= p.native_height, "Invalid runtime world viewport");
    require(p.terrain.size() <= 262144 && p.shadows.size() <= 65536 && p.sprites.size() <= 196608 && p.overlays.size() <= 131072 && p.post_composition.size() <= 131072 && p.cursor_sprites.size() <= 16384,
        "Runtime world command count exceeded");
    require((uint64_t(p.terrain.size()) * 3 + uint64_t(p.shadows.size()) * 6) * 40
        + uint64_t(p.sprites.size() + p.cursor_sprites.size()) * 600 + uint64_t(p.overlays.size() + p.post_composition.size()) * 216 <= 32ull * 1024 * 1024, "Runtime world geometry exceeds32MiB");
    for (float v : p.clear_rgba) require(std::isfinite(v) && v >= 0 && v <= 1, "Invalid runtime clear colour");
    for (float v : p.palette_transform) require(std::isfinite(v) && std::abs(v) <= 1024, "Invalid runtime palette transform");
    const auto& s = p.shade_settings;
    for (float v : s) require(std::isfinite(v), "Invalid runtime shade settings");
    require((s[0] == 0 || s[0] == 1) && s[1] >= 0 && s[1] <= 1 && s[2] >= 0.1f && s[2] <= 3
        && s[3] >= 0.1f && s[3] <= 3 && (s[4] == 0 || s[4] == 1)
        && (s[5] == 0 || s[5] == 1 || s[5] == 2) && s[6] >= 0.1f && s[6] <= 5
        && s[7] >= 0 && s[7] <= 1 && (s[8] == 0 || s[8] == 1)
        && std::isfinite(p.time) && std::abs(p.time) <= 16777216
        && (p.fade_addressing == EnhancedFadeAddressing::Sampler || p.fade_addressing == EnhancedFadeAddressing::TexelLoad),
        "Invalid runtime world shade state");
    for (const auto& triangle : p.terrain) for (const auto& v : triangle.vertices) {
        const auto& q = v.position;
        require(std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z) && std::isfinite(q.w)
            && std::abs(q.x) <= 65536 && std::abs(q.y) <= 65536 && std::abs(q.z) <= 16
            && q.w >= 1 && q.w <= 2147483648.0f && std::floor(q.w) == q.w, "Invalid runtime terrain position");
        require(std::isfinite(v.u) && std::isfinite(v.v) && std::abs(v.u) <= 32768 && std::abs(v.v) <= 32768
            && std::isfinite(v.shade) && std::abs(v.shade) <= 1024
            && std::isfinite(v.stl_x) && std::isfinite(v.stl_y)
            && std::abs(v.stl_x) <= 65536 && std::abs(v.stl_y) <= 65536, "Invalid runtime terrain attributes");
    }
    const auto source = [&](const auto& draw, const RuntimeAssetLease& lease) {
        const auto desc = lease.Description();
        require(draw.source_width && draw.source_height && draw.source_width <= 1024 && draw.source_height <= 1024
            && uint64_t(draw.mask_x) + draw.source_width <= desc.width
            && uint64_t(draw.mask_y) + draw.source_height <= desc.height
            && std::isfinite(draw.z_ndc) && std::abs(draw.z_ndc) <= 16, "Invalid runtime source rectangle");
    };
    for (const auto& shadow : p.shadows) {
        const auto& draw = shadow.draw; source(draw, shadow.source);
        require(std::isfinite(draw.darken) && draw.darken >= 0 && draw.darken <= 1, "Invalid runtime shadow darken");
        for (const auto& v : draw.vertices)
            require(std::isfinite(v.x) && std::isfinite(v.y) && std::abs(v.x) <= 16777216 && std::abs(v.y) <= 16777216
                && std::isfinite(v.u) && std::isfinite(v.v) && std::abs(v.u) <= 32768 && std::abs(v.v) <= 32768,
                "Invalid runtime shadow vertex");
    }
    for (const auto* list : {&p.sprites,&p.cursor_sprites}) for (const auto& sprite : *list) {
        const auto& draw = sprite.draw; source(draw, sprite.source);
        const bool cursor=list==&p.cursor_sprites;
        const auto visible_x=SpriteRasterBounds(draw.x,draw.width,cursor?p.native_width:p.viewport_width);
        const auto visible_y=SpriteRasterBounds(draw.y,draw.height,cursor?p.native_height:p.viewport_height);
        const bool visible=visible_x.Visible()&&visible_y.Visible();
        const bool mapping_valid=uint32_t(draw.kind) <= 5 && draw.width > 0 && draw.height > 0
            && draw.step_x>0 && draw.step_y>0
            && (!visible || (SpriteRasterMappingSafe(visible_x,draw.phase_x,draw.step_x)
                && SpriteRasterMappingSafe(visible_y,draw.phase_y,draw.step_y)))
            && draw.content_height && draw.content_height <= draw.source_height;
        if(!mapping_valid) throw std::invalid_argument("Invalid runtime sprite mapping: list="
            + std::string(list==&p.cursor_sprites?"cursor":"world") + " index=" + std::to_string(&sprite-list->data())
            + " pass=" + std::to_string(uint32_t(draw.kind))
            + " dst=" + std::to_string(draw.x) + "," + std::to_string(draw.y) + "," + std::to_string(draw.width) + "," + std::to_string(draw.height)
            + " phase=" + std::to_string(draw.phase_x) + "," + std::to_string(draw.phase_y)
            + " step=" + std::to_string(draw.step_x) + "," + std::to_string(draw.step_y)
            + " source=" + std::to_string(draw.source_width) + "," + std::to_string(draw.source_height)
            + " contentHeight=" + std::to_string(draw.content_height)
            + " viewport=" + std::to_string(p.viewport_width) + "," + std::to_string(p.viewport_height));
        const bool normal = draw.kind == OutlinePassKind::NormalSprite;
        require(!(draw.texture_fallback && draw.premultiplied) && std::isfinite(draw.source_alpha)
            && draw.source_alpha >= 0 && draw.source_alpha <= 1
            && (normal ? draw.lookup_row == 0 : draw.lookup_row == NoSpriteColourRow)
            && (normal || draw.source_alpha == 1), "Invalid runtime sprite colour path");
        for (float v : draw.colour) require(std::isfinite(v) && v >= 0 && v <= 1, "Invalid runtime sprite colour");
    }
    const auto bounded = [](float value, float limit) { return std::isfinite(value) && std::abs(value) <= limit; };
    const auto validate_overlay = [&](const WorldOverlayQuad& q) {
        require(unsigned(q.mode)<=4,"Invalid runtime UI mode");
        require(bounded(q.x0,32768) && bounded(q.y0,32768) && bounded(q.x1,32768) && bounded(q.y1,32768)
            && q.x1 > q.x0 && q.y1 > q.y0 && bounded(q.z_ndc,2), "Invalid runtime overlay geometry");
        for (float c : q.colour) require(std::isfinite(c) && c >= 0 && c <= 1, "Invalid runtime overlay colour");
        require(q.scissor_enabled
            ? q.scissor_x >= -32768 && q.scissor_x <= 32768 && q.scissor_y >= -32768 && q.scissor_y <= 32768
                && q.scissor_w >= 0 && q.scissor_w <= 32768 && q.scissor_h >= 0 && q.scissor_h <= 32768
            : q.scissor_x == 0 && q.scissor_y == 0 && q.scissor_w == 0 && q.scissor_h == 0,
            "Invalid runtime overlay scissor");
        const bool solid = q.mode == WorldOverlayMode::Solid;
        const bool coloured = q.mode == WorldOverlayMode::Colored;
        require(q.mode == WorldOverlayMode::Fade ? q.fade_row < 64 : q.fade_row == 0, "Invalid runtime overlay fade row");
        require(solid || coloured ? q.colour_row == NoWorldOverlayResource : q.colour_row == 0,
            "Runtime overlay colour row must be lease-local");
        if (solid) {
            require(q.source == NoWorldOverlayResource && q.u0 == 0 && q.v0 == 0 && q.u1 == 0 && q.v1 == 0,
                "Unused runtime solid overlay source");
        } else {
            require(q.source == 0, "Runtime overlay source must be lease-local");
            for (float uv : {q.u0,q.v0,q.u1,q.v1})
                require(std::isfinite(uv) && uv >= 0 && uv <= 1, "Invalid runtime overlay UV");
        }
    };
    unsigned previous_phase=0;
    for(const auto& overlay:p.overlays) {
        const auto& q=overlay.draw;const unsigned phase=unsigned(q.phase);
        require(phase<=2&&phase>=previous_phase,"Invalid runtime overlay phase order");previous_phase=phase;
        require((phase!=0||q.depth_test)&&(phase!=1||!q.depth_test),"Invalid runtime overlay depth state");
        validate_overlay(q);
    }
    previous_phase=0;
    for(const auto& item:p.post_composition) {
        const auto& q=item.draw;const unsigned phase=unsigned(item.phase),kind=unsigned(item.kind);
        require(phase<=unsigned(RuntimePostPhase::Cursor)&&phase>=previous_phase&&kind<=unsigned(RuntimePostKind::QuadOpaque),"Invalid post-world phase or kind");previous_phase=phase;
        const bool parchment=item.phase>=RuntimePostPhase::ParchmentWorld&&item.phase<=RuntimePostPhase::ParchmentFront;
        const bool depth=item.phase==RuntimePostPhase::ParchmentWorld;
        require(q.depth_test==depth&&q.phase==(depth?WorldOverlayPhase::DepthWorld:WorldOverlayPhase::FlatWorld),"Invalid post-world UI depth/phase");
        require(!parchment||(p.map_fade&&p.map_fade->capture_pending),"Parchment commands require capture");
        validate_overlay(q);
        const bool zoom=item.kind==RuntimePostKind::ImageZoom;
        for(float v:item.zoom) require(std::isfinite(v)&&(zoom?std::abs(v)<=16777216.0f:v==0),"Invalid landview zoom mapping");
        if(zoom) require(q.x0==0&&q.y0==0&&q.x1==float(p.native_width)&&q.y1==float(p.native_height),"Invalid landview zoom destination");
        if(item.kind==RuntimePostKind::QuadOpaque) {
            require((item.phase==RuntimePostPhase::ZoomBoxes||item.phase==RuntimePostPhase::OverheadMap)
                &&q.mode==WorldOverlayMode::Sprite&&q.colour==std::array<float,4>{1,1,1,1}&&!item.coverage.Valid(),"Invalid opaque indexed quad");
        }else if(item.kind==RuntimePostKind::Quad) {
            require(item.phase!=RuntimePostPhase::ImagePresents&&item.phase!=RuntimePostPhase::ImagePresentOverlay&&!item.coverage.Valid(),"Invalid post-world UI phase or coverage");
        }else {
            const bool base=item.kind==RuntimePostKind::ImageOpaque||item.kind==RuntimePostKind::ImageComposite||item.kind==RuntimePostKind::ImageZoom;
            require(item.phase==(base?RuntimePostPhase::ImagePresents:RuntimePostPhase::ImagePresentOverlay),"Invalid image-present phase");
            require(q.mode==WorldOverlayMode::Sprite&&q.u0==0&&q.v0==0&&q.u1==1&&q.v1==1
                &&q.colour==std::array<float,4>{1,1,1,1}&&!q.scissor_enabled,"Invalid image-present geometry or tint");
            require(item.kind==RuntimePostKind::ImageCoverage ? item.coverage.Valid()&&item.coverage.Description()==item.source.Description() : !item.coverage.Valid(),"Invalid image-present coverage extent");
        }
    }
    for(const auto& sprite:p.cursor_sprites)
        require(sprite.draw.kind==OutlinePassKind::NormalSprite||sprite.draw.kind==OutlinePassKind::GlowAdd||sprite.draw.kind==OutlinePassKind::GlowDarken,"Invalid cursor sprite pass");
    if(p.map_fade) {
        const auto& m=*p.map_fade;
        require(m.transition_id&&m.transition_id!=UINT64_MAX&&std::isfinite(m.step)&&m.step>=0&&m.step<=32
            &&std::isfinite(m.warp_scale)&&m.warp_scale>0&&m.warp_scale<=2.5f,"Invalid runtime map-fade state");
    }
    if (p.lens) {
        const auto& lens = *p.lens;
        require(unsigned(lens.kind) <= unsigned(LensPassKind::Overlay), "Invalid runtime lens operation");
        require(lens.source_x >= 0 && uint32_t(lens.source_x) < p.native_width
            && lens.destination_x >= 0 && lens.destination_y >= 0 && lens.viewport_width && lens.viewport_height
            && uint64_t(lens.destination_x) + lens.viewport_width <= p.native_width
            && uint64_t(lens.destination_y) + lens.viewport_height <= p.native_height,
            "Invalid runtime lens native source or destination");
        const bool mist = lens.kind == LensPassKind::Mist;
        const bool overlay = lens.kind == LensPassKind::Overlay;
        for (float value : lens.mist_offsets)
            require(std::isfinite(value) && (mist ? value >= 0 && value < 256 : value == 0), "Invalid runtime mist offsets");
        require(mist ? lens.mist_lightness <= 63 : lens.mist_lightness == 0, "Invalid runtime mist lightness");
        require(overlay ? lens.overlay_alpha <= 256 : lens.overlay_alpha == 0, "Invalid runtime lens overlay alpha");
        for (float value : lens.clear_rgba)
            require(std::isfinite(value) && value >= 0 && value <= 1, "Invalid runtime lens target clear");
    }
}
}
