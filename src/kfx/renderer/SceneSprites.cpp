// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#include "kfx/renderer/SceneSprites.h"
#include "kfx/renderer/KeeperSpriteResolution.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <stdexcept>
#include <tuple>

namespace kfx::render {
SceneSpriteMesh BuildSceneSprites(const SceneCapture& capture, const TerrainMesh& terrain)
{
    if (!ValidateSceneCapture(capture) || capture.assets.presentation.size() != capture.scene.objects.size())
        throw std::runtime_error("Sprite placement needs a v2 capture with resolved presentation metadata");
    CapturedCamera camera(capture.scene.camera);
    SceneSpriteMesh result;
    const auto terrain_atlas = BuildTerrainAtlas(capture, terrain);
    auto& atlas = result.atlas;
    atlas.width = std::max(2048u, terrain_atlas.width); atlas.height = terrain_atlas.height;
    atlas.origins = terrain_atlas.origins;
    atlas.rgba.resize(size_t(atlas.width)*atlas.height*4);
    for (uint32_t y = 0; y < terrain_atlas.height; ++y)
        std::copy_n(&terrain_atlas.rgba[size_t(y)*terrain_atlas.width*4], size_t(terrain_atlas.width)*4,
            &atlas.rgba[size_t(y)*atlas.width*4]);
    std::map<uint32_t, const SceneSpritePresentation*> presentation;
    for (const auto& p : capture.assets.presentation) presentation.emplace(p.id.slot, &p);
    std::map<std::tuple<uint16_t,uint8_t,uint8_t>, const SceneSpriteBinding*> bindings;
    for (const auto& b : capture.assets.bindings) bindings.emplace(std::make_tuple(b.animation,b.frame,b.direction), &b);
    std::vector<const SceneObject*> objects;
    for (const auto& o : capture.scene.objects) objects.push_back(&o);
    std::sort(objects.begin(),objects.end(),[](auto a, auto b){ return a->id.slot < b->id.slot; });
    uint32_t ox = 1, oy = atlas.height+1, row_height = 0;
    for (const auto* object : objects) {
        const auto& o = *object;
        // Destination-palette transparency and spinning keys
        // need dedicated passes; never silently turn them into solid rectangles.
        if ((o.render_flags & 0x30) || o.draw_class != 2) { ++result.deferred_effects; continue; }
        auto p = camera.Project(o.position.x/256.0,o.position.y/256.0,o.position.z/256.0);
        if (!p.in_front || !o.size) { ++result.clipped; continue; }
        const auto& appearance = *presentation.at(o.id.slot);
        int angle = o.yaw;
        if (appearance.rotates) angle += appearance.angle_offset-capture.scene.camera.yaw;
        const uint8_t direction = ((angle+128)&2047)/256;
        const auto& binding = *bindings.at({o.animation,o.frame,direction});
        const bool flip = appearance.rotates && (angle&2047) > 1151 && (angle&2047) < 1919;
        const auto& sprite = capture.assets.sprites[binding.sprite];
        if (!sprite.width || !sprite.height) { ++result.empty; continue; }
        // Legacy sprite scale is quantized to 1/32 of a source pixel.
        const double scale = std::floor(p.pixels_per_unit*o.size)
            / (32u << keeper_sprite_resolution_shift(sprite.flags));
        if (scale <= 0) { ++result.clipped; continue; }
        const double x_origin = flip ? -std::floor((binding.origin_width+binding.origin_x)*scale)
            : std::floor(binding.origin_x*scale);
        const double crop_x = flip ? sprite.frame_width-sprite.frame_x-sprite.width : sprite.frame_x;
        const double left = x_origin + std::floor(crop_x*scale);
        const double top = std::floor(binding.origin_y*scale) + std::floor(sprite.frame_y*scale);
        const double width = std::floor(sprite.width*scale), height = std::floor(sprite.height*scale);
        if (width <= 0 || height <= 0 || p.x+left+width < camera.source.viewport_x || p.y+top+height < camera.source.viewport_y
            || p.x+left >= camera.source.viewport_x+camera.source.viewport_w || p.y+top >= camera.source.viewport_y+camera.source.viewport_h) {
            ++result.clipped; continue;
        }
        if (ox+sprite.width+1 > atlas.width) { ox = 1; oy += row_height+2; row_height = 0; }
        row_height = std::max<uint32_t>(row_height,sprite.height);
        atlas.height = oy+row_height+1;
        if (atlas.height > 16384 || uint64_t(atlas.width)*atlas.height*4 > 64*1024*1024)
            throw std::runtime_error("Sprite atlas budget exceeded");
        atlas.rgba.resize(size_t(atlas.width)*atlas.height*4,0);
        int shade = std::min<int>(63,appearance.lightness>>8);
        if (capture.scene.camera.view == SceneView::Possession) {
            const double far = capture.scene.camera.view_distance*256.0;
            if (p.depth > far*.75) shade = int(shade*std::clamp((far-p.depth)/(far*.25),0.0,1.0));
        }
        for (uint32_t y = 0; y < sprite.height; ++y) for (uint32_t x = 0; x < sprite.width; ++x) {
            const auto index = sprite.pixels[(size_t(y)*sprite.width+x)*2];
            const auto remapped = (o.render_flags&12) ? capture.assets.ghost[o.tint*256+index]
                : shade == 32 ? index : capture.assets.fade[shade*256+index];
            auto* dst = &atlas.rgba[(size_t(oy+y)*atlas.width+ox+x)*4];
            for (size_t channel = 0; channel < 3; ++channel) dst[channel] = capture.assets.display_palette[remapped*3+channel]*255/63;
            dst[3] = sprite.pixels[(size_t(y)*sprite.width+x)*2+1];
        }
        SceneSpriteQuad quad; quad.id = o.id; quad.sprite = binding.sprite; quad.direction = direction; quad.flip = flip;
        // CCW on screen, on an upright plane through the world anchor.
        // Coverage discard prevents holes from hiding the terrain.
        const double dx[] = {left,left+width,left+width,left};
        const double dy[] = {top+height,top+height,top,top};
        for (size_t i = 0; i < 4; ++i) {
            const auto v = camera.OffsetOnSpritePlane(o.position,dx[i],dy[i]);
            const bool right = i == 1 || i == 2;
            quad.vertices[i] = {v[0],v[1],v[2], float(ox+(right != flip ? sprite.width : 0)),
                float(oy+(i<2 ? sprite.height : 0))};
        }
        result.quads.push_back(quad); ox += sprite.width+2;
    }
    // Sprite UVs are normalized only after the final atlas height is known.
    for (auto& quad : result.quads) for (auto& v : quad.vertices) { v.u /= atlas.width; v.v /= atlas.height; }
    return result;
}
}
