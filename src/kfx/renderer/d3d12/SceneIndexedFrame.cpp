// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#include "SceneIndexedFrame.h"
#include "kfx/renderer/SceneSpriteReplay.h"
#include <algorithm>
#include <map>
#include <stdexcept>

namespace kfx::render::dx12 {
namespace {
void Require(bool condition, const char* reason) { if (!condition) throw std::runtime_error(reason); }
void Texel(Image& image, uint32_t x, uint32_t y, uint8_t index)
{
    const size_t p = (size_t(y) * image.width + x) * 4;
    image.rgba.at(p) = index; image.rgba.at(p + 3) = 255;
}
PaletteTerrainVertex ConvertVertex(const SceneRasterVertex& point, bool quantized)
{
    // C++20 signed right shift matches Gpoly's input truncation, including
    // negative UVs. Wrapping still happens after hardware interpolation.
    const auto attribute = [quantized](int32_t value) {
        return quantized ? float(value >> 16) : float(value) / 65536.0f;
    };
    return {float(point.x), float(point.y), attribute(point.u), attribute(point.v), attribute(point.s)};
}
struct AtlasBuilder {
    static constexpr uint32_t Width = 4096, MaxHeight = 4096;
    Image image{Width, 256, std::vector<uint8_t>(size_t(Width) * 256 * 4)};
    uint32_t x = 0, y = 256, row_height = 0;
    void Grow(uint32_t end)
    {
        if (end > MaxHeight) throw std::runtime_error("Sprite replay atlas exceeds declared 64 MiB bound");
        if (end > image.height) {
            image.height = end;
            image.rgba.resize(size_t(Width) * end * 4);
        }
    }
    std::pair<uint32_t, uint32_t> Place(uint32_t width, uint32_t height)
    {
        if (!width || !height || width > Width) throw std::runtime_error("Sprite replay atlas rectangle outside bounds");
        if (x + width > Width) { y += row_height; x = row_height = 0; }
        Grow(y + height);
        const auto result = std::make_pair(x, y);
        x += width; row_height = std::max(row_height, height);
        return result;
    }
    void Texel(uint32_t tx, uint32_t ty, uint8_t red, uint8_t green = 0, uint8_t alpha = 255)
    {
        const size_t p = (size_t(ty) * Width + tx) * 4;
        image.rgba.at(p) = red; image.rgba.at(p + 1) = green; image.rgba.at(p + 3) = alpha;
    }
    uint32_t Map(const std::vector<int32_t>& values)
    {
        if (values.size() > Width) throw std::runtime_error("Sprite replay sampling map exceeds 4096 coordinates");
        const auto origin = Place(Width, 1);
        for (uint32_t i = 0; i < values.size(); ++i) {
            if (values[i] < -1 || values[i] > 1023) throw std::runtime_error("Sprite replay invalid sampling coordinate");
            const uint32_t encoded = uint32_t(values[i] + 1);
            Texel(i, origin.second, uint8_t(encoded), uint8_t(encoded >> 8));
        }
        return origin.second;
    }
};
}
PaletteTerrainVertex ConvertIndexedTerrainVertex(const SceneRasterVertex& point, bool quantize)
{
    return ConvertVertex(point, quantize);
}
SpriteReplayUpload BuildSpriteReplayUpload(const SceneCapture& capture, std::span<const SceneSpriteDraw> draws)
{
    if (capture.assets.additive.size() != 65536) throw std::runtime_error("Sprite replay requires captured additive table");
    AtlasBuilder atlas;
    for (uint32_t high = 0; high < 256; ++high) for (uint32_t low = 0; low < 256; ++low) {
        atlas.Texel(low, high, capture.assets.ghost[high * 256 + low]);
        atlas.Texel(256 + low, high, capture.assets.additive[high * 256 + low]);
    }
    std::map<uint32_t, std::pair<uint32_t, uint32_t>> sprites;
    std::map<std::array<uint8_t, 256>, std::pair<uint32_t, uint32_t>> remaps;
    std::vector<SceneSpriteMapping> mappings;
    SpriteReplayUpload result;
    // Raw index/coverage images and raw tables only: CPU does not prescale or
    // preblend the source images submitted to the GPU.
    for (const auto& draw : draws) {
        if (draw.sprite >= capture.assets.sprites.size()) throw std::runtime_error("Sprite replay invalid source reference");
        const auto& sprite = capture.assets.sprites[draw.sprite];
        auto mapping = MapSceneSprite(draw, sprite);
        result.quirk_commands += mapping.legacy_row_copy_quirk;
        result.quirk_by_command.push_back(mapping.legacy_row_copy_quirk);
        result.invisible_commands += mapping.source_x.empty();
        mappings.push_back(std::move(mapping));
        if (!sprites.contains(draw.sprite)) {
            const auto origin = atlas.Place(sprite.width, sprite.height);
            sprites.emplace(draw.sprite, origin);
            for (uint32_t sy = 0; sy < sprite.height; ++sy) for (uint32_t sx = 0; sx < sprite.width; ++sx) {
                const size_t p = (size_t(sy) * sprite.width + sx) * 2;
                atlas.Texel(origin.first + sx, origin.second + sy, sprite.pixels.at(p), 0, sprite.pixels.at(p + 1));
            }
        }
        if (draw.blend == SceneSpriteBlend::Remap && !remaps.contains(draw.remap)) {
            const auto origin = atlas.Place(256, 1);
            remaps.emplace(draw.remap, origin);
            for (uint32_t i = 0; i < 256; ++i) atlas.Texel(origin.first + i, origin.second, draw.remap[i]);
        }
    }
    std::map<std::vector<int32_t>, uint32_t> maps;
    for (size_t i = 0; i < draws.size(); ++i) {
        const auto& draw = draws[i]; const auto& mapping = mappings[i];
        result.command_quads.push_back(mapping.source_x.empty() ? -1 : int32_t(result.quads.size()));
        if (mapping.source_x.empty()) continue;
        auto map_row = [&](const std::vector<int32_t>& values) {
            const auto found = maps.find(values);
            if (found != maps.end()) return found->second;
            const auto row = atlas.Map(values); maps.emplace(values, row); return row;
        };
        const auto origin = sprites.at(draw.sprite);
        const auto& sprite = capture.assets.sprites[draw.sprite];
        PaletteQuad quad{mapping.x, mapping.y, uint32_t(mapping.source_x.size()), uint32_t(mapping.source_y.size()),
            origin.first, origin.second, sprite.width, sprite.height, PaletteBlend::Opaque};
        quad.mapped = true; quad.map_x_row = map_row(mapping.source_x); quad.map_y_row = map_row(mapping.source_y);
        switch (draw.blend) {
        case SceneSpriteBlend::Opaque: break;
        case SceneSpriteBlend::GhostSourceHigh: quad.blend = PaletteBlend::SourceHigh; break;
        case SceneSpriteBlend::GhostDestinationHigh: quad.blend = PaletteBlend::DestinationHigh; break;
        case SceneSpriteBlend::Additive: quad.blend = PaletteBlend::Additive; quad.lookup_x = 256; break;
        case SceneSpriteBlend::Remap: {
            quad.blend = PaletteBlend::Remap;
            const auto table = remaps.at(draw.remap); quad.lookup_x = table.first; quad.lookup_y = table.second; break;
        }
        default: throw std::runtime_error("Sprite replay unsupported blend operator");
        }
        result.quads.push_back(quad);
    }
    result.atlas = std::move(atlas.image);
    return result;
}
IndexedSceneUpload BuildIndexedSceneUpload(const SceneCapture& capture, bool quantize_vertices)
{
    Require(capture.format_version >= 4 && capture.format_version <= 8 && capture.trace.backend == 0
        && ValidateSceneCapture(capture), "Indexed world requires a complete validated software capture");
    auto sprites = BuildSpriteReplayUpload(capture, capture.trace.draws);
    IndexedSceneUpload upload;
    upload.excluded_sprite_calls = capture.trace.excluded_sprite_calls;
    upload.excluded_shadow_entries = capture.trace.raster.excluded_shadow_entries;
    for (const auto& entry : capture.trace.entries) {
        Require(entry.kind < upload.bucket_counts.size(), "Unknown world bucket kind");
        ++upload.bucket_counts[entry.kind];
    }
    upload.atlas = std::move(sprites.atlas);
    auto& image = upload.atlas;
    const uint32_t fade_y = image.height;
    const uint32_t tiles_y = fade_y + 64;
    std::map<uint32_t, std::pair<uint32_t, uint32_t>> tiles;
    auto grow = [&](uint32_t height) {
        Require(height <= 16384 && uint64_t(image.width) * height * 4 <= 64 * 1024 * 1024,
            "Mixed replay atlas exceeds 64 MiB bound");
        image.height = height; image.rgba.resize(size_t(image.width) * height * 4);
    };
    grow(tiles_y);
    for (uint32_t y = 0; y < 64; ++y) for (uint32_t x = 0; x < 256; ++x)
        Texel(image, x, fade_y + y, capture.assets.fade[y * 256 + x]);
    for (const auto& triangle : capture.trace.raster.triangles) {
        Require(triangle.rasterizer == SceneRasterizer::Gpoly && triangle.mode == 5,
            "Terrain replay supports Gpoly mode 5 only; Trig mode 7 requires a separate qualified implementation");
        Require(uint64_t(triangle.texture_offset) + 31 * 256 + 32 <= capture.assets.texture_pixels.size(),
            "Captured terrain texture lies outside the owned bank");
        if (!tiles.contains(triangle.texture_offset)) {
            const uint32_t ordinal = uint32_t(tiles.size()), per_row = image.width / 32;
            const uint32_t x = (ordinal % per_row) * 32, y = tiles_y + (ordinal / per_row) * 32;
            if (y + 32 > image.height) grow(y + 32);
            tiles.emplace(triangle.texture_offset, std::make_pair(x, y));
            for (uint32_t ty = 0; ty < 32; ++ty) for (uint32_t tx = 0; tx < 32; ++tx)
                Texel(image, x + tx, y + ty, capture.assets.texture_pixels[triangle.texture_offset + ty * 256 + tx]);
        }
        const auto origin = tiles.at(triangle.texture_offset);
        PaletteTriangle converted;
        converted.texture_x = origin.first; converted.texture_y = origin.second; converted.fade_y = fade_y;
        for (size_t p = 0; p < 3; ++p) {
            const auto& point = triangle.vertices[p];
            converted.vertices[p] = ConvertVertex(point, false);
        }
        upload.triangles.push_back(converted);
    }
    std::vector<std::pair<uint32_t, uint32_t>> masks;
    uint32_t mask_x = 0, mask_y = image.height, mask_row_height = 0;
    for (const auto& mask : capture.trace.raster.shadow_masks) {
        Require(mask.width && mask.width <= 256 && mask.height && mask.height <= 256
            && mask.coverage.size() == size_t(mask.width) * mask.height, "Invalid captured shadow mask extent");
        if (mask_x + mask.width > image.width) { mask_y += mask_row_height; mask_x = mask_row_height = 0; }
        if (mask_y + mask.height > image.height) grow(mask_y + mask.height);
        masks.emplace_back(mask_x, mask_y);
        for (uint32_t y = 0; y < mask.height; ++y) for (uint32_t x = 0; x < mask.width; ++x) {
            const auto value = mask.coverage[size_t(y) * mask.width + x];
            Require(value == 0 || value == 255, "Captured shadow mask is not binary");
            Texel(image, mask_x + x, mask_y + y, value);
        }
        mask_x += mask.width; mask_row_height = std::max(mask_row_height, uint32_t(mask.height));
    }
    for (const auto& shadow : capture.trace.raster.shadows) {
        const auto origin = masks.at(shadow.mask);
        const auto& mask = capture.trace.raster.shadow_masks.at(shadow.mask);
        PaletteShadowTriangle converted;
        converted.mask_x = origin.first; converted.mask_y = origin.second;
        converted.mask_width = mask.width; converted.mask_height = mask.height;
        converted.fade_y = fade_y; converted.shade = shadow.shade;
        for (size_t i = 0; i < 3; ++i) {
            const auto& p = shadow.vertices[i];
            converted.vertices[i] = {float(p.x),float(p.y),float(p.u) / 65536.0f,float(p.v) / 65536.0f};
        }
        upload.shadows.push_back(converted);
    }
    for (const auto& command : capture.trace.raster.commands) {
        PalettePrimitive primitive;
        if (command.kind == SceneDrawCommandKind::Triangle) {
            primitive.kind = PalettePrimitiveKind::Terrain;
            primitive.terrain = upload.triangles.at(command.index);
        } else if (command.kind == SceneDrawCommandKind::Sprite) {
            const int32_t index = sprites.command_quads.at(command.index);
            if (index < 0) { ++upload.clipped_sprites; continue; }
            primitive.sprite = sprites.quads.at(size_t(index));
        } else if (command.kind == SceneDrawCommandKind::Shadow) {
            primitive.kind = PalettePrimitiveKind::Shadow;
            primitive.shadow = upload.shadows.at(command.index);
        } else throw std::runtime_error("Unknown mixed command kind");
        upload.primitives.push_back(primitive);
    }
    Require(!upload.primitives.empty(), "Captured mixed stream is empty after clipping");
    if (quantize_vertices) QuantizeIndexedSceneUpload(upload, capture);
    return upload;
}
void QuantizeIndexedSceneUpload(IndexedSceneUpload& upload, const SceneCapture& capture)
{
    // Reuse the same bounded atlas and command storage. Only the three vertex
    // attributes change; positions, texture selection and painter order stay put.
    for (size_t i = 0; i < upload.triangles.size(); ++i)
        for (size_t p = 0; p < 3; ++p)
            upload.triangles[i].vertices[p] = ConvertVertex(capture.trace.raster.triangles.at(i).vertices[p], true);
    size_t next_triangle = 0;
    for (auto& primitive : upload.primitives)
        if (primitive.kind == PalettePrimitiveKind::Terrain)
            primitive.terrain = upload.triangles.at(next_triangle++);
    Require(next_triangle == upload.triangles.size(), "Quantized variant changed triangle command count");
}
}
