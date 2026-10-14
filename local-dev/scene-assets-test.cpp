// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#include "kfx/renderer/SceneAssets.h"
#include <algorithm>
#include <cassert>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <iterator>
#include <limits>
#include <cmath>
#include <tuple>

using namespace kfx::render;
namespace {
SceneCapture Fixture()
{
    SceneCapture c;
    auto& s = c.scene;
    s.epoch = 0x123456789abcdef0ull; s.sequence = 19; s.game_turn = -20;
    s.map_width = s.map_height = 2; s.texture_generation = 7; s.interpolation = .25f;
    s.camera.viewport_w = 3840; s.camera.viewport_h = 2160; s.camera.position = {-256, 512, 99};
    s.camera.rotation[7] = -4567; s.camera.has_rotation = true;
    s.wibble[1].offset = {1, -2, 3}; s.lava_scroll = -2.5f;
    s.cube_materials.resize(1); s.top_cubes = {0}; s.ceiling_textures = {0};
    SceneColumn column; column.revealed = true; column.texture_variation = 17; column.light[3] = 333;
    s.columns.push_back(column);
    SceneObject o; o.id = {0x8877665544332211ull, 2}; o.animation = 52; o.frame = 3; o.yaw = -55;
    s.objects.push_back(o);
    c.assets.presentation.push_back({o.id,12345,768,true});
    c.assets.projection_reference.push_back({{-123,456,789},333,-444,true});
    SceneLight light; light.id = {55, 1}; light.position = {512, 256, -128}; light.radius = 123;
    s.lights.push_back(light);
    s.texture_dimension = 32; s.texture_row_stride = 256; s.texture_frames.assign(32 * 1544, 0);
    c.assets.texture_pixels.resize(8192);
    for (size_t i = 0; i < 8192; ++i) c.assets.texture_pixels[i] = i % 256;
    for (size_t i = 0; i < 768; ++i) c.assets.base_palette[i] = c.assets.display_palette[i] = i % 64;
    for (size_t i = 0; i < c.assets.fade.size(); ++i) c.assets.fade[i] = i % 253;
    c.assets.ghost[999] = 201;
    SceneSprite sprite; sprite.id = 400; sprite.width = sprite.height = 2; sprite.offset_x = -1;
    const uint8_t rle[] = {2, 0, 17, 0, 255, 1, 9, 0};
    assert(DecodeSceneSprite(rle, 2, 2, sprite.pixels));
    c.assets.sprites.push_back(sprite);
    for (uint8_t d = 0; d < 8; ++d) c.assets.bindings.push_back({52, 3, d, 0, d >= 5, 99, 88, -22, -33});
    return c;
}
SceneCapture FixtureV3()
{
    auto c = Fixture();
    c.format_version = 3;
    c.scene.objects[0].draw_class = 2;
    c.assets.additive.resize(65536);
    for (size_t i = 0; i < c.assets.additive.size(); ++i) c.assets.additive[i] = uint8_t((i * 13 + i / 256) % 256);
    c.trace.complete = true; c.trace.backend = 1; c.trace.excluded_sprite_calls = 29;
    // Equal buckets retain their actual traversal order; kind is raw evidence.
    c.trace.entries = {{4097, 2}, {4096, 2}, {4096, 2}, {2000, 2}, {1000, 2}, {0, 2}, {0, 255}, {0, 127}};
    const uint32_t flags[] = {0x80000001u, 0x080c, 0x000c, 0x0008, 0, 0x080c, 0x0004};
    const SceneSpriteBlend blends[] = {SceneSpriteBlend::Opaque, SceneSpriteBlend::Remap,
        SceneSpriteBlend::GhostSourceHigh, SceneSpriteBlend::GhostDestinationHigh,
        SceneSpriteBlend::Additive, SceneSpriteBlend::Additive, SceneSpriteBlend::GhostSourceHigh};
    for (unsigned i = 0; i < 7; ++i) {
        SceneSpriteDraw draw;
        draw.object = c.scene.objects[0].id; draw.entry = i ? i - 1 : 0; draw.sprite = 0;
        draw.scale = {-17 - int32_t(i), 23 + int32_t(i), 99, 88, 177 + int32_t(i), 166, -22, -33, 3840, 2160};
        draw.content_height = i % 2 ? 1 : 2; draw.flags = flags[i]; draw.alpha = i == 4 || i == 5;
        draw.has_remap = (draw.flags & 0x0800) != 0; draw.blend = blends[i];
        if (draw.has_remap) for (size_t n = 0; n < draw.remap.size(); ++n) draw.remap[n] = uint8_t(255 - n);
        c.trace.draws.push_back(draw);
    }
    return c;
}
void AssertDrawEqual(const SceneSpriteDraw& a, const SceneSpriteDraw& b)
{
    assert(a.object == b.object && a.entry == b.entry && a.sprite == b.sprite);
    const auto& x = a.scale; const auto& y = b.scale;
    assert(x.frame_x == y.frame_x && x.frame_y == y.frame_y && x.frame_src_w == y.frame_src_w && x.frame_src_h == y.frame_src_h
        && x.frame_dst_w == y.frame_dst_w && x.frame_dst_h == y.frame_dst_h && x.content_x == y.content_x && x.content_y == y.content_y
        && x.window_w == y.window_w && x.window_h == y.window_h);
    assert(a.content_height == b.content_height && a.flags == b.flags && a.alpha == b.alpha && a.has_remap == b.has_remap
        && a.blend == b.blend && a.remap == b.remap);
}
void RejectCapture(const SceneCapture& c)
{
    assert(!ValidateSceneCapture(c));
    bool rejected = false;
    try { (void)EncodeSceneCapture(c); } catch (const std::exception&) { rejected = true; }
    assert(rejected);
}
void SetU32(std::vector<uint8_t>& bytes, size_t position, uint32_t value)
{
    for (int i = 0; i < 4; ++i) bytes[position + i] = uint8_t(value >> (i * 8));
}
void Rechecksum(std::vector<uint8_t>& bytes)
{
    uint32_t crc = ~0u;
    for (size_t p = 24; p < bytes.size(); ++p) {
        crc ^= bytes[p];
        for (int i = 0; i < 8; ++i) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
    }
    crc = ~crc;
    for (int i = 0; i < 4; ++i) bytes[12 + i] = uint8_t(crc >> (i * 8));
}
void Reject(const std::vector<uint8_t>& bytes)
{
    bool rejected = false;
    try { (void)DecodeSceneCapture(bytes); } catch (const std::exception&) { rejected = true; }
    assert(rejected);
}
std::vector<uint8_t> CheckV3()
{
    auto original = FixtureV3();
    assert(ValidateSceneCapture(original));
    const auto bytes = EncodeSceneCapture(original);
    const auto retained = DecodeSceneCapture(bytes);
    assert(retained->format_version == 3 && EncodeSceneCapture(*retained) == bytes);
    assert(retained->assets.additive == original.assets.additive);
    assert(retained->trace.complete && retained->trace.backend == 1 && retained->trace.excluded_sprite_calls == 29);
    assert(retained->trace.entries.size() == original.trace.entries.size() && retained->trace.draws.size() == original.trace.draws.size());
    for (size_t i = 0; i < original.trace.entries.size(); ++i) {
        assert(retained->trace.entries[i].bucket == original.trace.entries[i].bucket);
        assert(retained->trace.entries[i].kind == original.trace.entries[i].kind);
    }
    for (size_t i = 0; i < original.trace.draws.size(); ++i) AssertDrawEqual(retained->trace.draws[i], original.trace.draws[i]);
    // Retaining a decoded capture survives producer table/frame/identity reuse.
    original.assets.additive.assign(65536, 99); original.trace.draws[1].remap.fill(77);
    original.trace.entries.clear(); original.trace.draws.clear(); original.scene.objects[0].id.incarnation++;
    original.scene.epoch++; original.assets.sprites.clear();
    assert(EncodeSceneCapture(*retained) == bytes && retained->assets.additive[1] == 13 && retained->trace.draws[1].remap[0] == 255);
    auto reject = [&](auto change) { auto invalid = *retained; change(invalid); RejectCapture(invalid); };
    reject([](auto& c) { c.assets.additive.clear(); });
    reject([](auto& c) { c.assets.additive.pop_back(); });
    reject([](auto& c) { c.assets.additive.push_back(0); });
    reject([](auto& c) { c.assets.presentation.clear(); });
    reject([](auto& c) { c.trace.complete = false; });
    reject([](auto& c) { c.trace.backend = 2; });
    reject([](auto& c) { c.trace.entries[0].bucket = 4098; });
    reject([](auto& c) { c.trace.entries.back().bucket = -1; });
    reject([](auto& c) { c.trace.entries[2].bucket = 4097; });
    reject([](auto& c) { c.trace.entries.resize(262145); });
    reject([](auto& c) { c.trace.draws.resize(65537, c.trace.draws[0]); });
    reject([](auto& c) { c.trace.draws[0].object.incarnation++; });
    reject([](auto& c) { c.trace.draws[0].object.slot++; });
    reject([](auto& c) { c.scene.objects[0].draw_class = 3; });
    reject([](auto& c) { c.trace.draws[0].entry = uint32_t(c.trace.entries.size()); });
    reject([](auto& c) { c.trace.entries.clear(); });
    reject([](auto& c) { c.trace.entries[0].kind = 0; }); // Terrain cannot own an ordinary sprite draw.
    reject([](auto& c) { c.trace.draws[0].entry = 1; }); // Next draw still references entry zero.
    reject([](auto& c) { c.trace.draws[0].sprite = uint32_t(c.assets.sprites.size()); });
    reject([](auto& c) { c.trace.draws[0].content_height = 0; });
    reject([](auto& c) { c.trace.draws[0].content_height = 3; });
    reject([](auto& c) { c.assets.sprites[0].width = 0; c.assets.sprites[0].pixels.clear(); });
    reject([](auto& c) { c.trace.draws[1].has_remap = false; });
    reject([](auto& c) { c.trace.draws[0].has_remap = true; });
    reject([](auto& c) { c.trace.draws[0].remap[255] = 1; });
    reject([](auto& c) { c.trace.draws[0].blend = SceneSpriteBlend(255); });
    for (size_t i = 0; i < retained->trace.draws.size(); ++i)
        reject([&](auto& c) { c.trace.draws[i].blend = SceneSpriteBlend((unsigned(c.trace.draws[i].blend) + 1) % 5); });
    for (auto member : {&SpriteScale::frame_src_w, &SpriteScale::frame_src_h}) {
        for (int32_t value : {0, -1, 65536}) reject([&](auto& c) { c.trace.draws[0].scale.*member = value; });
    }
    for (auto member : {&SpriteScale::frame_dst_w, &SpriteScale::frame_dst_h}) {
        for (int32_t value : {-1, 16777217}) reject([&](auto& c) { c.trace.draws[0].scale.*member = value; });
    }
    for (auto member : {&SpriteScale::frame_x, &SpriteScale::frame_y}) {
        for (int32_t value : {-16777217, 16777217}) reject([&](auto& c) { c.trace.draws[0].scale.*member = value; });
    }
    for (auto member : {&SpriteScale::content_x, &SpriteScale::content_y}) {
        for (int32_t value : {-65536, 65536}) reject([&](auto& c) { c.trace.draws[0].scale.*member = value; });
    }
    for (auto member : {&SpriteScale::window_w, &SpriteScale::window_h}) {
        for (int32_t value : {0, -1, 16385}) reject([&](auto& c) { c.trace.draws[0].scale.*member = value; });
    }
    auto valid = *retained; valid.trace.backend = 0;
    valid.trace.draws[0].scale = {-16777216, 16777216, 1, 65535, 0, 16777216, -65535, 65535, 1, 16384};
    assert(ValidateSceneCapture(valid)); // Bounds, clipping and zero destination size remain legitimate evidence.
    valid.trace.draws[0].scale = {16777216, -16777216, 65535, 1, 16777216, 0, 65535, -65535, 16384, 1};
    assert(ValidateSceneCapture(valid));
    auto empty = *retained; empty.trace.entries.clear(); empty.trace.draws.clear();
    assert(ValidateSceneCapture(empty)); // A complete walk can have no attempted visible entries.
    valid.scene.objects.clear(); valid.assets.presentation.clear(); valid.trace.entries.clear(); valid.trace.draws.clear();
    assert(ValidateSceneCapture(valid) && EncodeSceneCapture(*DecodeSceneCapture(EncodeSceneCapture(valid))) == EncodeSceneCapture(valid));
    for (uint32_t version : {1u, 2u}) {
        auto legacy = Fixture(); legacy.format_version = version;
        if (version == 1) { legacy.assets.presentation.clear(); legacy.assets.projection_reference.clear(); }
        for (unsigned field = 0; field < 6; ++field) {
            auto invalid = legacy;
            if (field == 0) invalid.assets.additive.push_back(0);
            if (field == 1) invalid.trace.complete = true;
            if (field == 2) invalid.trace.backend = 1;
            if (field == 3) invalid.trace.excluded_sprite_calls = 1;
            if (field == 4) invalid.trace.entries.push_back({});
            if (field == 5) invalid.trace.draws.push_back({});
            RejectCapture(invalid);
        }
    }
    // Check every overlapping operator combination without deriving expected values from the resolver.
    for (uint32_t combination = 0; combination < 8; ++combination) {
        const uint32_t flags = ((combination & 1) ? 0x0800 : 0) | ((combination & 2) ? 4 : 0) | ((combination & 4) ? 8 : 0);
        const auto expected = (combination & 1) ? SceneSpriteBlend::Remap : (combination & 2) ? SceneSpriteBlend::GhostSourceHigh
            : (combination & 4) ? SceneSpriteBlend::GhostDestinationHigh : SceneSpriteBlend::Opaque;
        assert(ResolveSceneSpriteBlend(flags, false) == expected);
        assert(ResolveSceneSpriteBlend(flags | 0x80000001u, false) == expected);
        assert(ResolveSceneSpriteBlend(flags, true) == SceneSpriteBlend::Additive);
    }
    auto prefix = *retained; prefix.format_version = 2; prefix.assets.additive.clear(); prefix.trace = {};
    const auto v2 = EncodeSceneCapture(prefix);
    const size_t extension = v2.size();
    assert(std::equal(v2.begin() + 24, v2.end(), bytes.begin() + 24));
    const size_t additive_count = extension;
    const size_t trace_start = additive_count + 4 + 65536;
    const size_t entry_count = trace_start + 6;
    const size_t draw_count = entry_count + 4 + retained->trace.entries.size() * 5;
    const size_t first_draw = draw_count + 4;
    auto mutate = [&](size_t position, uint8_t value) { auto bad = bytes; bad[position] = value; Rechecksum(bad); Reject(bad); };
    mutate(trace_start, 0); mutate(trace_start, 2); mutate(trace_start + 1, 2);
    mutate(first_draw + 68, 2); mutate(first_draw + 69, 2); mutate(first_draw + 70, 255);
    mutate(first_draw + 71, 1); // Absent remap must be all zero.
    mutate(first_draw, uint8_t(bytes[first_draw] + 1)); // Stale object incarnation.
    mutate(first_draw + 16, 1); // Sprite one does not exist.
    mutate(first_draw + 60, 0); mutate(first_draw + 60, 3); // Source-row clip bounds.
    mutate(first_draw + 69, 1); // Remap presence contradicts raw flags.
    mutate(first_draw + 70, unsigned(SceneSpriteBlend::Remap)); // Valid enum, wrong effective operator.
    mutate(first_draw + 65, 8); // Raw remap flag requires a table even under blend precedence.
    for (auto [position, value] : {std::pair{entry_count + 4, 4098u}, std::pair{entry_count + 14, 4097u},
        std::pair{first_draw + 12, 6u}, std::pair{first_draw + 12, 1u}}) {
        auto bad = bytes; SetU32(bad, position, value); Rechecksum(bad); Reject(bad);
    }
    for (uint32_t version : {1u, 2u}) {
        auto bad = bytes; SetU32(bad, 4, version); Reject(bad); // An older version must not silently discard the extension.
    }
    for (auto [position, count] : {std::pair{additive_count, 65537u}, std::pair{entry_count, 262145u}, std::pair{draw_count, 65537u}}) {
        auto bad = bytes;
        // Leave enough remaining bytes to prove the count cap fires before resize, independently of EOF.
        bad.resize(std::max(bad.size(), position + 4 + count));
        SetU32(bad, position, count); SetU32(bad, 8, uint32_t(bad.size() - 24)); Rechecksum(bad); Reject(bad);
    }
    for (size_t cut : {extension, trace_start - 1, trace_start + 1, entry_count + 2, draw_count + 2, first_draw + 100, bytes.size() - 1}) {
        std::vector<uint8_t> bad(bytes.begin(), bytes.begin() + cut);
        Reject(bad); SetU32(bad, 8, uint32_t(bad.size() - 24)); Rechecksum(bad); Reject(bad);
    }
    for (size_t position : {extension + 123, entry_count + 4, first_draw + 100, bytes.size() - 1}) {
        auto bad = bytes; bad[position] ^= 1; Reject(bad); // New fields are checksum protected.
    }
    return bytes;
}
std::vector<uint8_t> CheckV4()
{
    auto c = FixtureV3(); c.format_version = 4; c.trace.backend = 0;
    c.trace.entries = {{4097, 0}, {4096, 2}, {3000, 1}, {2000, 2}, {1000, 6}};
    c.trace.draws.resize(2); c.trace.draws[0].entry = 1; c.trace.draws[1].entry = 3;
    for (uint32_t entry : {0u, 2u, 4u, 4u}) {
        SceneTerrainTriangle triangle;
        triangle.entry = entry; triangle.mode = 5;
        triangle.vertices = {{{-4, 3, -1234567, 65539, 17 * 65536 + 9},
            {21, 7, 90007, -200011, 18 * 65536 + 3}, {8, 31, 400001, 800009, 19 * 65536 + 1}}};
        if (entry == 2) {
            triangle.rasterizer = SceneRasterizer::Trig; triangle.mode = 7; triangle.colour = 23;
            for (auto& vertex : triangle.vertices) vertex.s = 0;
        }
        c.trace.raster.triangles.push_back(triangle);
    }
    c.trace.raster.commands = {{SceneDrawCommandKind::Triangle, 0}, {SceneDrawCommandKind::Sprite, 0},
        {SceneDrawCommandKind::Triangle, 1}, {SceneDrawCommandKind::Sprite, 1},
        {SceneDrawCommandKind::Triangle, 2}, {SceneDrawCommandKind::Triangle, 3}};
    assert(ValidateSceneCapture(c));
    const auto bytes = EncodeSceneCapture(c);
    const auto retained = DecodeSceneCapture(bytes);
    assert(retained->format_version == 4 && EncodeSceneCapture(*retained) == bytes);
    const auto& raster = retained->trace.raster;
    assert(raster.triangles.size() == 4 && raster.commands.size() == 6);
    const uint32_t expected_entries[] = {0, 2, 4, 4};
    const uint32_t expected_indices[] = {0, 0, 1, 1, 2, 3};
    const SceneDrawCommandKind expected_kinds[] = {SceneDrawCommandKind::Triangle, SceneDrawCommandKind::Sprite,
        SceneDrawCommandKind::Triangle, SceneDrawCommandKind::Sprite, SceneDrawCommandKind::Triangle, SceneDrawCommandKind::Triangle};
    for (size_t i = 0; i < 4; ++i) {
        const auto& t = raster.triangles[i];
        assert(t.entry == expected_entries[i] && t.texture_offset == 0 && t.mode == (i == 1 ? 7 : 5));
        assert(t.colour == (i == 1 ? 23 : 0) && t.rasterizer == (i == 1 ? SceneRasterizer::Trig : SceneRasterizer::Gpoly));
        assert(t.vertices[0].x == -4 && t.vertices[1].v == -200011 && t.vertices[2].u == 400001);
        assert(t.vertices[0].s == (i == 1 ? 0 : 17 * 65536 + 9));
    }
    for (size_t i = 0; i < 6; ++i) assert(raster.commands[i].kind == expected_kinds[i] && raster.commands[i].index == expected_indices[i]);
    c.trace.raster.triangles[0].vertices[0].u = 0; c.trace.raster.commands.clear();
    assert(EncodeSceneCapture(*retained) == bytes); // All new arrays are owned.
    auto reject = [&](auto change) { auto invalid = *retained; change(invalid); RejectCapture(invalid); };
    reject([](auto& x) { x.trace.backend = 1; });
    reject([](auto& x) { x.scene.texture_row_stride = 128; });
    reject([](auto& x) { x.trace.raster.triangles[0].entry = 1; });
    reject([](auto& x) { x.trace.raster.triangles[0].rasterizer = SceneRasterizer(2); });
    reject([](auto& x) { x.trace.raster.triangles[0].mode = 7; });
    reject([](auto& x) { x.trace.raster.triangles[0].colour = 1; });
    reject([](auto& x) { x.trace.raster.triangles[1].colour = 64; });
    reject([](auto& x) { x.trace.raster.triangles[1].vertices[2].s = 1; });
    reject([](auto& x) { x.trace.raster.triangles[0].vertices[0].s = -1; });
    reject([](auto& x) { x.trace.raster.triangles[0].vertices[0].s = 4194304; });
    reject([](auto& x) { x.trace.raster.triangles[0].vertices[0].x = 16777217; });
    reject([](auto& x) { x.trace.raster.triangles[0].vertices[0].y = -16777217; });
    reject([](auto& x) { x.trace.raster.triangles[0].texture_offset = 1; }); // Not a captured bank-frame origin.
    reject([](auto& x) { x.assets.texture_pixels.resize(7967); });
    reject([](auto& x) { x.trace.raster.triangles[1].texture_offset = 8192; x.scene.texture_frames[1] = 8192; });
    reject([](auto& x) { x.trace.entries[4].kind = 0; }); // Front pair cannot become a standard single triangle.
    reject([](auto& x) { x.trace.raster.triangles[3].entry = 2; }); // Payload order regression.
    reject([](auto& x) { x.trace.raster.commands[0].kind = SceneDrawCommandKind(2); });
    reject([](auto& x) { x.trace.raster.commands[0].index = 1; });
    reject([](auto& x) { x.trace.raster.commands[5].index = 2; }); // Duplicate payload, missing triangle three.
    reject([](auto& x) { std::swap(x.trace.raster.commands[0], x.trace.raster.commands[1]); });
    reject([](auto& x) { x.trace.raster.commands.pop_back(); });
    reject([](auto& x) { x.trace.raster.triangles.resize(262145); });
    reject([](auto& x) { x.trace.raster.commands.resize(327681); });
    auto boundary = *retained;
    boundary.trace.raster.triangles[0].vertices[0] = {-16777216, 16777216, -2147483647 - 1, 2147483647, 4194303};
    boundary.trace.raster.triangles[1].colour = 63;
    assert(ValidateSceneCapture(boundary)); // Raw UV low bits/range are evidence, not restricted by consumer arithmetic.
    const auto boundary_bytes = EncodeSceneCapture(boundary);
    assert(EncodeSceneCapture(*DecodeSceneCapture(boundary_bytes)) == boundary_bytes);
    auto full_uv = *retained;
    full_uv.trace.raster.triangles[1].colour = 32;
    RejectCapture(full_uv); // Actual Trig7 colour32 dispatches md02: full 8-bit U/V, no fade lookup.
    full_uv.assets.texture_pixels.resize(65536);
    assert(ValidateSceneCapture(full_uv)); // Exactly the independently required 256*256 byte address span.
    const auto full_uv_bytes = EncodeSceneCapture(full_uv);
    const auto full_uv_retained = DecodeSceneCapture(full_uv_bytes);
    assert(full_uv_retained->assets.texture_pixels.size() == 65536 && full_uv_retained->trace.raster.triangles[1].colour == 32);
    assert(EncodeSceneCapture(*full_uv_retained) == full_uv_bytes);
    full_uv.assets.texture_pixels.resize(65535); RejectCapture(full_uv);
    auto masked_v = *retained; masked_v.trace.raster.triangles[1].colour = 31;
    assert(masked_v.assets.texture_pixels.size() == 8192 && ValidateSceneCapture(masked_v));
    for (uint32_t divisions : {2u, 3u, 4u, 6u, 16u}) {
        auto near = *retained;
        const auto triangle = near.trace.raster.triangles[1];
        near.trace.raster.triangles.erase(near.trace.raster.triangles.begin() + 1);
        near.trace.raster.triangles.insert(near.trace.raster.triangles.begin() + 1, divisions, triangle);
        near.trace.raster.commands = {{SceneDrawCommandKind::Triangle, 0}, {SceneDrawCommandKind::Sprite, 0}};
        for (uint32_t i = 0; i < divisions; ++i) near.trace.raster.commands.push_back({SceneDrawCommandKind::Triangle, 1 + i});
        near.trace.raster.commands.push_back({SceneDrawCommandKind::Sprite, 1});
        near.trace.raster.commands.push_back({SceneDrawCommandKind::Triangle, divisions + 1});
        near.trace.raster.commands.push_back({SceneDrawCommandKind::Triangle, divisions + 2});
        if (divisions == 3) RejectCapture(near); // Three fragments is not a producer subdivision shape.
        else assert(ValidateSceneCapture(near) && EncodeSceneCapture(*DecodeSceneCapture(EncodeSceneCapture(near))) == EncodeSceneCapture(near));
    }
    for (uint32_t version : {1u, 2u, 3u}) {
        auto older = version == 3 ? FixtureV3() : Fixture(); older.format_version = version;
        if (version == 1) { older.assets.presentation.clear(); older.assets.projection_reference.clear(); }
        auto invalid = older; invalid.trace.raster.triangles.push_back(raster.triangles[0]); RejectCapture(invalid);
        invalid = older; invalid.trace.raster.commands.push_back(raster.commands[0]); RejectCapture(invalid);
    }
    auto prefix = *retained; prefix.format_version = 3; prefix.trace.raster = {};
    const size_t triangle_count = EncodeSceneCapture(prefix).size();
    const size_t first_triangle = triangle_count + 4;
    const size_t command_count = first_triangle + 4 * 71;
    const size_t first_command = command_count + 4;
    auto wire = [&](size_t offset, uint32_t value, bool byte = false) {
        auto bad = bytes;
        if (byte) bad[offset] = uint8_t(value); else SetU32(bad, offset, value);
        Rechecksum(bad); Reject(bad);
    };
    wire(first_triangle, 5); wire(first_triangle + 4, 2, true); wire(first_triangle + 5, 7, true);
    wire(first_triangle + 6, 1, true); wire(first_triangle + 7, 1);
    wire(first_triangle + 11 + 16, 4194304); wire(first_triangle + 71 + 11 + 16, 1);
    wire(first_command, 2, true); wire(first_command + 1, 1); wire(first_command + 5 * 5 + 1, 2);
    for (auto [offset, excessive] : {std::pair{triangle_count, 262145u}, std::pair{command_count, 327681u}}) {
        auto bad = bytes; SetU32(bad, offset, excessive); Rechecksum(bad); Reject(bad);
    }
    for (size_t cut : {triangle_count, first_triangle + 13, command_count + 2, bytes.size() - 1}) {
        auto bad = std::vector<uint8_t>(bytes.begin(), bytes.begin() + cut);
        SetU32(bad, 8, uint32_t(bad.size() - 24)); Rechecksum(bad); Reject(bad);
    }
    return bytes;
}
std::vector<uint8_t> CheckV5(const std::vector<uint8_t>& v4)
{
    auto c = *DecodeSceneCapture(v4); c.format_version = 5; c.scene.objects[0].class_id = 5;
    c.trace.entries = {{4097,0},{4096,2},{3000,1},{2500,3},{2000,2},{1000,6},{0,3}};
    c.trace.draws[1].entry = 4;
    c.trace.raster.triangles[2].entry = c.trace.raster.triangles[3].entry = 5;
    auto& r = c.trace.raster; r.excluded_shadow_entries = 1;
    r.shadow_masks.push_back({4,3,{0,255,0,255,255,0,255,0,0,255,255,0}});
    SceneShadowTriangle a; a.caster = c.scene.objects[0].id; a.entry = 3; a.mask = 0; a.shade = 23;
    a.vertices = {{{-5,4,0,0,0},{9,4,3*65536,0,0},{9,15,3*65536,2*65536,0}}};
    auto b = a; b.vertices = {{a.vertices[0],a.vertices[2],{-5,15,0,2*65536,0}}};
    r.shadows = {a,b};
    r.commands = {{SceneDrawCommandKind::Triangle,0},{SceneDrawCommandKind::Sprite,0},{SceneDrawCommandKind::Triangle,1},
        {SceneDrawCommandKind::Shadow,0},{SceneDrawCommandKind::Shadow,1},{SceneDrawCommandKind::Sprite,1},
        {SceneDrawCommandKind::Triangle,2},{SceneDrawCommandKind::Triangle,3}};
    assert(ValidateSceneCapture(c));
    const auto bytes = EncodeSceneCapture(c); const auto retained = DecodeSceneCapture(bytes);
    assert(retained->format_version == 5 && EncodeSceneCapture(*retained) == bytes);
    const auto& shadow = retained->trace.raster;
    assert(shadow.excluded_shadow_entries == 1 && shadow.shadow_masks.size() == 1 && shadow.shadows.size() == 2);
    assert(shadow.shadow_masks[0].width == 4 && shadow.shadow_masks[0].height == 3
        && shadow.shadow_masks[0].coverage == std::vector<uint8_t>({0,255,0,255,255,0,255,0,0,255,255,0}));
    assert(shadow.shadows[0].caster == retained->scene.objects[0].id && shadow.shadows[1].entry == 3
        && shadow.shadows[1].mask == 0 && shadow.shadows[1].shade == 23 && shadow.shadows[1].vertices[1].u == 3*65536);
    const uint32_t indices[] = {0,0,1,0,1,1,2,3};
    const SceneDrawCommandKind kinds[] = {SceneDrawCommandKind::Triangle,SceneDrawCommandKind::Sprite,SceneDrawCommandKind::Triangle,
        SceneDrawCommandKind::Shadow,SceneDrawCommandKind::Shadow,SceneDrawCommandKind::Sprite,SceneDrawCommandKind::Triangle,SceneDrawCommandKind::Triangle};
    for (size_t i = 0; i < 8; ++i) assert(shadow.commands[i].index == indices[i] && shadow.commands[i].kind == kinds[i]);
    c.trace.raster.shadow_masks[0].coverage.assign(12,0); c.trace.raster.shadows.clear();
    assert(EncodeSceneCapture(*retained) == bytes);
    auto reject = [&](auto change) { auto invalid = *retained; change(invalid); RejectCapture(invalid); };
    reject([](auto& x) { x.trace.raster.excluded_shadow_entries = 0; });
    reject([](auto& x) { x.trace.raster.shadow_masks[0].width = 0; });
    reject([](auto& x) { x.trace.raster.shadow_masks[0].height = 65535; });
    reject([](auto& x) { x.trace.raster.shadow_masks[0].width = 257; });
    reject([](auto& x) { x.trace.raster.shadow_masks[0].coverage.pop_back(); });
    reject([](auto& x) { x.trace.raster.shadow_masks[0].coverage[0] = 1; });
    reject([](auto& x) { x.trace.raster.shadows[0].caster.incarnation++; });
    reject([](auto& x) { x.scene.objects[0].class_id = 4; });
    reject([](auto& x) { x.trace.raster.shadows[1].mask = 1; });
    reject([](auto& x) { x.trace.raster.shadows[1].entry = 6; });
    reject([](auto& x) { x.trace.raster.shadows[0].shade = 15; });
    reject([](auto& x) { x.trace.raster.shadows[0].shade = 32; });
    reject([](auto& x) { x.trace.raster.shadows[1].shade = 24; });
    reject([](auto& x) { x.trace.raster.shadows[1].vertices[0].s = 1; });
    reject([](auto& x) { x.trace.raster.shadows[1].vertices[0].x++; });
    reject([](auto& x) { x.trace.raster.shadows[1].vertices[1].u++; });
    reject([](auto& x) { x.trace.raster.shadows[0].vertices[1].u = -1; });
    reject([](auto& x) { x.trace.raster.shadows[0].vertices[1].u = 3*65536+1; });
    reject([](auto& x) { x.trace.raster.shadows[0].vertices[1].v = 2*65536+1; });
    reject([](auto& x) { x.trace.raster.shadows[0].vertices[0].x = 16777217; });
    reject([](auto& x) { x.trace.raster.commands[4].index = 0; });
    reject([](auto& x) { std::swap(x.trace.raster.commands[3],x.trace.raster.commands[5]); });
    reject([](auto& x) { x.trace.raster.shadow_masks.resize(32769); });
    reject([](auto& x) { x.trace.raster.shadows.resize(65537); });
    for (uint8_t shade : {uint8_t(16),uint8_t(31)}) {
        auto valid = *retained; valid.trace.raster.shadows[0].shade = valid.trace.raster.shadows[1].shade = shade;
        assert(ValidateSceneCapture(valid));
    }
    auto budget = *retained;
    budget.trace.raster.shadow_masks.assign(1024,SceneShadowMask{256,256,std::vector<uint8_t>(65536,255)});
    for (uint32_t mask = 1; mask < 1024; ++mask) {
        budget.trace.entries.push_back({0,3});
        auto first = a, second = b; first.entry = second.entry = 6+mask; first.mask = second.mask = mask;
        budget.trace.raster.shadows.push_back(first); budget.trace.raster.shadows.push_back(second);
        budget.trace.raster.commands.push_back({SceneDrawCommandKind::Shadow,mask*2});
        budget.trace.raster.commands.push_back({SceneDrawCommandKind::Shadow,mask*2+1});
    }
    RejectCapture(budget); // 64 MiB of valid masks plus eight owned sprite bytes exceeds the shared budget.
    for (uint32_t version : {3u,4u}) {
        auto older = version == 4 ? *DecodeSceneCapture(v4) : FixtureV3();
        auto invalid = older; invalid.trace.raster.excluded_shadow_entries = 1; RejectCapture(invalid);
        invalid = older; invalid.trace.raster.shadow_masks.push_back(shadow.shadow_masks[0]); RejectCapture(invalid);
        invalid = older; invalid.trace.raster.shadows.push_back(shadow.shadows[0]); RejectCapture(invalid);
    }
    // The v5 extension is after the unchanged v4 triangle/command wire shapes.
    const size_t extension = bytes.size() - (4 + 4 + 2 + 2 + 4 + 12 + 4 + 2*81);
    const size_t mask_count = extension + 4, first_mask = mask_count + 4;
    const size_t shadows_count = first_mask + 8 + 12, first_shadow = shadows_count + 4;
    auto wire = [&](size_t offset, uint32_t value, bool byte = false) {
        auto bad = bytes; if (byte) bad[offset] = uint8_t(value); else SetU32(bad,offset,value);
        Rechecksum(bad); Reject(bad);
    };
    wire(extension,0); wire(mask_count,32769); wire(first_mask,0,true); wire(first_mask+4,11); wire(first_mask+8,1,true);
    wire(shadows_count,65537); wire(first_shadow,0); wire(first_shadow+12,7); wire(first_shadow+16,1);
    wire(first_shadow+20,15,true); wire(first_shadow+21+16,1); wire(first_shadow+81+21,100);
    for (size_t cut : {extension,first_mask+7,first_shadow+40,bytes.size()-1}) {
        auto bad = std::vector<uint8_t>(bytes.begin(),bytes.begin()+cut);
        SetU32(bad,8,uint32_t(bad.size()-24)); Rechecksum(bad); Reject(bad);
    }
    return bytes;
}
std::vector<uint8_t> CheckV6(const std::vector<uint8_t>& v5)
{
    auto c = *DecodeSceneCapture(v5); c.format_version = 6;
    c.scene.camera.view = SceneView::Overhead; c.scene.camera.view_mode = 2;
    auto key = c.scene.objects[0]; key.id = {123,3}; key.draw_class = 6; key.model = 65535;
    c.scene.objects.push_back(key); c.assets.presentation.push_back({key.id,8192,0,false});
    c.trace.entries[1].kind = 9; c.trace.draws[0].object = key.id;
    assert(ValidateSceneCapture(c));
    const auto bytes = EncodeSceneCapture(c); const auto retained = DecodeSceneCapture(bytes);
    assert(retained->format_version == 6 && EncodeSceneCapture(*retained) == bytes);
    assert(retained->trace.draws[0].object.slot == 3 && retained->trace.draws[1].object.slot == 2);
    assert(retained->trace.entries[1].kind == 9 && retained->scene.objects[1].draw_class == 6 && retained->scene.objects[1].model == 65535);
    assert(retained->trace.raster.commands[1].kind == SceneDrawCommandKind::Sprite && retained->trace.raster.commands[1].index == 0
        && retained->trace.raster.commands[5].kind == SceneDrawCommandKind::Sprite && retained->trace.raster.commands[5].index == 1);
    for (auto [view,mode] : {std::pair{SceneView::Overhead,2},std::pair{SceneView::Overhead,8},std::pair{SceneView::Front,5}}) {
        auto valid = *retained; valid.scene.camera.view = view; valid.scene.camera.view_mode = uint8_t(mode);
        assert(ValidateSceneCapture(valid) && EncodeSceneCapture(*DecodeSceneCapture(EncodeSceneCapture(valid))) == EncodeSceneCapture(valid));
    }
    auto reject = [&](auto change) { auto invalid = *retained; change(invalid); RejectCapture(invalid); };
    reject([](auto& x) { x.trace.entries[1].kind = 2; }); // (key draw class, ordinary bucket) is not allowlisted.
    reject([](auto& x) { x.trace.draws[0].object = x.scene.objects[0].id; }); // (ordinary draw class, key bucket).
    reject([](auto& x) { x.trace.draws[0].object.incarnation++; });
    reject([](auto& x) { x.trace.draws[0].object.slot = 99; });
    reject([](auto& x) { x.scene.objects[1].draw_class = 7; });
    reject([](auto& x) { x.scene.camera.view = SceneView::Possession; x.scene.camera.view_mode = 6; });
    reject([](auto& x) { x.scene.camera.view_mode = 5; });
    reject([](auto& x) { x.scene.camera.view = SceneView::Front; x.scene.camera.view_mode = 2; });
    reject([](auto& x) { x.trace.raster.commands[5].index = 0; });
    reject([](auto& x) { x.trace.raster.commands.erase(x.trace.raster.commands.begin()+1); });
    reject([](auto& x) { std::swap(x.trace.raster.commands[1],x.trace.raster.commands[5]); });
    reject([](auto& x) { std::swap(x.trace.raster.commands[1],x.trace.raster.commands[2]); }); // Per-kind order survives; painter entries regress.
    for (uint32_t version : {3u,4u,5u}) {
        auto invalid = *retained; invalid.format_version = version;
        if (version < 5) { invalid.trace.raster.excluded_shadow_entries = 0; invalid.trace.raster.shadow_masks.clear(); invalid.trace.raster.shadows.clear();
            std::erase_if(invalid.trace.raster.commands,[](const auto& command) { return command.kind == SceneDrawCommandKind::Shadow; }); }
        if (version < 4) invalid.trace.raster = {};
        RejectCapture(invalid);
    }
    // Possession may retain a raw key bucket, but cannot submit its sprite.
    auto suppressed = *retained; suppressed.scene.camera.view = SceneView::Possession; suppressed.scene.camera.view_mode = 6;
    suppressed.trace.draws.erase(suppressed.trace.draws.begin());
    suppressed.trace.raster.commands.erase(suppressed.trace.raster.commands.begin()+1);
    for (auto& command : suppressed.trace.raster.commands) if (command.kind == SceneDrawCommandKind::Sprite) command.index = 0;
    assert(ValidateSceneCapture(suppressed));
    // Prove the version bump changes semantics, not the v5 payload schema.
    auto ordinary = *retained; ordinary.scene.objects[1].draw_class = 2; ordinary.trace.entries[1].kind = 2;
    const auto wire6 = EncodeSceneCapture(ordinary); ordinary.format_version = 5; const auto wire5 = EncodeSceneCapture(ordinary);
    assert(wire5.size() == wire6.size() && std::equal(wire5.begin()+24,wire5.end(),wire6.begin()+24));
    auto unknown = bytes; SetU32(unknown,4,10); Reject(unknown);
    return bytes;
}
void CheckOutlineDepthPolicies(const SceneCapture& fixture)
{
    auto c = fixture;
    c.trace.entries = {{0,0},{0,2}}; c.trace.draws.resize(1); c.trace.draws[0].entry = 1;
    auto& o = c.trace.outline; o.occluders.resize(1); o.sprites.resize(1); o.commands.resize(2);
    auto stored = [](double value) { volatile float f = float(value); return f; };
    const float bias32 = stored(.002);
    auto third = [](float first, float second) {
        float value = first;
        do { value = std::nextafter(value, std::numeric_limits<float>::infinity()); } while (value == first || value == second);
        return value;
    };
    // Independent binary32 materialization of the two production evaluation
    // policies, including division before subtraction on SSE and deferred x87
    // materialization. Every legal bucket must accept both exact encodings.
    for (uint32_t bucket = 0; bucket <= 4097; ++bucket) {
        const float terrain[] = {stored(double(stored(double(2*bucket)/4097))-1), stored(double(2*bucket)/4097-1)};
        const float sprite[] = {stored(double(stored(double(int32_t(2*bucket)-1)/4097))-1), stored(double(int32_t(2*bucket)-1)/4097-1)};
        c.trace.entries[0].bucket = c.trace.entries[1].bucket = int32_t(bucket);
        o.sprites[0].sort_key = uint64_t(bucket)<<32;
        for (unsigned policy = 0; policy < 2; ++policy) {
            for (auto& vertex : o.occluders[0].vertices) vertex.z = terrain[policy];
            o.sprites[0].z_ndc = sprite[policy];
            const float biases[] = {stored(double(sprite[policy])+double(bias32)), stored(double(sprite[policy])+.002)};
            for (float bias : biases) { o.sprites[0].outline_z_ndc = bias; assert(ValidateSceneCapture(c)); }
            o.sprites[0].outline_z_ndc = third(biases[0],biases[1]);
            assert(!ValidateSceneCapture(c)); // No adjacent third representation or epsilon allowance.
        }
        o.sprites[0].outline_z_ndc = stored(double(sprite[1])+.002);
        o.occluders[0].vertices[0].z = third(terrain[0],terrain[1]);
        assert(!ValidateSceneCapture(c)); o.occluders[0].vertices[0].z = terrain[1];
        o.sprites[0].z_ndc = third(sprite[0],sprite[1]);
        assert(!ValidateSceneCapture(c));
    }
}
std::vector<uint8_t> CheckV7(const std::vector<uint8_t>& v6)
{
    auto c = FixtureV3(); c.format_version = 7;
    c.trace.entries = {{4097,0},{2049,2},{2049,2},{2049,2},{0,6}};
    c.trace.draws.clear();
    for (uint32_t i = 0; i < 3; ++i) {
        SceneSpriteDraw draw; draw.object = c.scene.objects[0].id; draw.entry = i+1;
        draw.scale = {10+int32_t(i)*8,20,2,2,4,4,0,0,3840,2160};
        draw.content_height = i == 1 ? 1 : 2; draw.flags = i == 1 ? 8 : i == 2 ? 0x4000 : 0;
        draw.alpha = i == 2;
        draw.blend = i == 1 ? SceneSpriteBlend::GhostDestinationHigh : i == 2 ? SceneSpriteBlend::Additive : SceneSpriteBlend::Opaque;
        c.trace.draws.push_back(draw);
    }
    auto& o = c.trace.outline; o.present = o.complete = true; o.mode = 2; o.alpha = .625f;
    o.palette_transform = {1,0,0,0,1,0,0,0,1};
    // Bucket endpoints and midpoint give independently known exact NDC depths.
    o.occluders = {{0, {{{-.5f,-.75f,1,1},{.5f,-.75f,1,257},{0,.75f,1,65536}}}},
        {4, {{{-1,-1,-1,2},{1,-1,-1,3},{1,1,-1,4}}}},
        {4, {{{-1,-1,-1,2},{1,1,-1,4},{-1,1,-1,5}}}}};
    for (uint32_t i = 0; i < 3; ++i) {
        SceneOutlineSprite sprite; sprite.draw = i;
        // Two source texels enlarged to four pixels have a one-pixel centre offset.
        sprite.dst_x = 11+int32_t(i)*8; sprite.dst_y = 21; sprite.dst_w = 4; sprite.dst_h = i == 1 ? 2 : 4;
        sprite.step_x = sprite.step_y = 131072; sprite.z_ndc = 0; sprite.outline_z_ndc = .002f;
        sprite.source_alpha = i == 1 ? 2.0f/3.0f : 1;
        sprite.sort_key = (uint64_t(2049)<<32)|i; sprite.flags = c.trace.draws[i].flags; sprite.owner = 0;
        sprite.wants_outline = i != 2; sprite.additive = sprite.darkens = i == 2;
        sprite.source_border_covered = true; sprite.path = SceneOutlineExecutionPath(i);
        if (i != 2) sprite.owner_rgba = {.125f,.5f,.75f,.625f};
        o.sprites.push_back(sprite);
    }
    o.commands = {{SceneOutlineCommandKind::Outline,0,0},{SceneOutlineCommandKind::Sprite,0,0},
        {SceneOutlineCommandKind::Outline,1,3},{SceneOutlineCommandKind::Sprite,1,3},
        {SceneOutlineCommandKind::Sprite,2,7},{SceneOutlineCommandKind::GlowDarken,2,7}};
    assert(ValidateSceneCapture(c));
    const auto bytes = EncodeSceneCapture(c); const auto retained = DecodeSceneCapture(bytes);
    assert(retained->format_version == 7 && EncodeSceneCapture(*retained) == bytes);
    const auto& outline = retained->trace.outline;
    assert(outline.occluders.size() == 3 && outline.sprites.size() == 3 && outline.commands.size() == 6);
    assert(outline.occluders[0].vertices[1].w == 257 && outline.occluders[0].vertices[2].w == 65536);
    assert(outline.sprites[1].dst_x == 19 && outline.sprites[1].dst_h == 2 && outline.sprites[1].source_border_covered);
    const SceneOutlineCommandKind kinds[] = {SceneOutlineCommandKind::Outline,SceneOutlineCommandKind::Sprite,
        SceneOutlineCommandKind::Outline,SceneOutlineCommandKind::Sprite,SceneOutlineCommandKind::Sprite,SceneOutlineCommandKind::GlowDarken};
    const uint32_t refs[] = {0,0,1,1,2,2}, groups[] = {0,0,3,3,7,7};
    for (size_t i = 0; i < 6; ++i) assert(outline.commands[i].kind == kinds[i] && outline.commands[i].sprite == refs[i] && outline.commands[i].group == groups[i]);
    c.trace.outline.occluders.clear(); c.trace.outline.sprites[0].owner_rgba.fill(0); c.trace.outline.commands.clear();
    assert(EncodeSceneCapture(*retained) == bytes); // Extension owns data independently of producer storage.
    CheckOutlineDepthPolicies(*retained);
    auto reject = [&](auto change) { auto invalid = *retained; change(invalid); RejectCapture(invalid); };
    reject([](auto& x) { x.trace.outline.present = false; });
    reject([](auto& x) { x.trace.outline.complete = false; });
    reject([](auto& x) { x.trace.outline.mode = 3; });
    reject([](auto& x) { x.trace.outline.alpha = 1.01f; });
    reject([](auto& x) { x.trace.outline.alpha = std::numeric_limits<float>::quiet_NaN(); });
    reject([](auto& x) { x.trace.outline.palette_transform[0] = 1025; });
    reject([](auto& x) { x.trace.outline.palette_transform[0] = std::numeric_limits<float>::infinity(); });
    reject([](auto& x) { x.trace.backend = 0; });
    reject([](auto& x) { x.trace.raster.excluded_shadow_entries = 1; });
    reject([](auto& x) { x.trace.raster.triangles.push_back({}); });
    reject([](auto& x) { x.trace.outline.occluders[0].entry = 1; });
    reject([](auto& x) { x.trace.outline.occluders.pop_back(); });
    reject([](auto& x) { std::swap(x.trace.outline.occluders[0],x.trace.outline.occluders[1]); });
    for (float w : {0.0f,.5f,1.5f,4294967296.0f,std::numeric_limits<float>::infinity()})
        reject([&](auto& x) { x.trace.outline.occluders[0].vertices[0].w = w; });
    reject([](auto& x) { x.trace.outline.occluders[0].vertices[0].x = 67108864; });
    reject([](auto& x) { x.trace.outline.occluders[0].vertices[0].z = 0; });
    reject([](auto& x) { x.trace.outline.sprites[0].draw = 3; });
    reject([](auto& x) { x.trace.outline.sprites[1].draw = 0; });
    reject([](auto& x) { std::swap(x.trace.outline.sprites[0],x.trace.outline.sprites[1]); });
    reject([](auto& x) { x.trace.outline.sprites.pop_back(); });
    for (auto member : {&SceneOutlineSprite::dst_x,&SceneOutlineSprite::dst_y,&SceneOutlineSprite::dst_w,&SceneOutlineSprite::dst_h,
        &SceneOutlineSprite::phase_x,&SceneOutlineSprite::phase_y,&SceneOutlineSprite::step_x,&SceneOutlineSprite::step_y})
        reject([&](auto& x) { ++(x.trace.outline.sprites[0].*member); });
    reject([](auto& x) { x.trace.draws[0].scale.frame_dst_w = 65536; }); // Step exceeds signed 16.16 storage.
    reject([](auto& x) { x.assets.sprites[0].width = 257; x.assets.sprites[0].pixels.resize(257*2*2); });
    reject([](auto& x) { x.trace.outline.sprites[0].flags = 1; });
    reject([](auto& x) { x.trace.outline.sprites[0].z_ndc = .1f; });
    reject([](auto& x) { x.trace.outline.sprites[0].outline_z_ndc = 0; });
    reject([](auto& x) { x.trace.outline.sprites[1].source_alpha = 1; });
    reject([](auto& x) { x.trace.outline.sprites[0].sort_key ^= uint64_t(1)<<32; });
    reject([](auto& x) { x.trace.outline.sprites[0].owner = -1; });
    reject([](auto& x) { x.trace.outline.sprites[0].source_border_covered = false; });
    reject([](auto& x) { x.trace.outline.sprites[0].owner_rgba[3] = .5f; });
    reject([](auto& x) { x.trace.outline.sprites[2].owner_rgba[0] = .5f; });
    reject([](auto& x) { x.scene.camera.view = SceneView::Possession; });
    reject([](auto& x) { x.trace.outline.sprites[0].path = SceneOutlineExecutionPath(3); });
    reject([](auto& x) { x.trace.outline.sprites[2].darkens = false; });
    reject([](auto& x) { x.trace.outline.sprites[2].path = SceneOutlineExecutionPath::Instanced; });
    reject([](auto& x) { x.trace.outline.commands[0].kind = SceneOutlineCommandKind(3); });
    reject([](auto& x) { x.trace.outline.commands[0].sprite = 3; });
    reject([](auto& x) { x.trace.outline.commands.erase(x.trace.outline.commands.begin()); });
    reject([](auto& x) { x.trace.outline.commands[1].kind = SceneOutlineCommandKind::Outline; });
    reject([](auto& x) { x.trace.outline.commands[1].group = 1; });
    reject([](auto& x) { x.trace.outline.commands[2].group = x.trace.outline.commands[3].group = 0; });
    reject([](auto& x) { std::swap(x.trace.outline.commands[0],x.trace.outline.commands[1]); });
    reject([](auto& x) { std::swap(x.trace.outline.commands[4],x.trace.outline.commands[5]); });
    reject([](auto& x) { x.trace.outline.commands.pop_back(); });
    reject([](auto& x) { x.trace.outline.occluders.resize(262145); });
    reject([](auto& x) { x.trace.outline.sprites.resize(65537); });
    reject([](auto& x) { x.trace.outline.commands.resize(196609); });
    auto valid = *retained;
    valid.trace.outline.occluders[0].vertices[0].w = 2147483648.0f;
    assert(ValidateSceneCapture(valid));
    valid = *retained; valid.trace.outline.sprites[2].path = SceneOutlineExecutionPath::Instanced;
    valid.trace.outline.sprites[2].darkens = false; valid.trace.outline.commands.pop_back();
    assert(ValidateSceneCapture(valid)); // Covered zero is opaque, but not a black-glow palette code.
    valid.assets.sprites[0].pixels[6] = 50; // Full-source glow is outside the clipped top row.
    valid.trace.draws[2].content_height = 1; valid.trace.outline.sprites[2].dst_h = 2;
    valid.trace.outline.sprites[2].darkens = true; valid.trace.outline.commands.push_back({SceneOutlineCommandKind::GlowDarken,2,7});
    assert(ValidateSceneCapture(valid));
    valid.assets.sprites[0].pixels[7] = 0; RejectCapture(valid); // Uncovered index50 cannot trigger darken.
    valid = *retained; valid.trace.outline.mode = 0;
    for (auto& state : valid.trace.outline.sprites) state.owner_rgba.fill(0);
    std::erase_if(valid.trace.outline.commands,[](const auto& command) { return command.kind == SceneOutlineCommandKind::Outline; });
    assert(ValidateSceneCapture(valid));
    valid = *retained; valid.assets.sprites[0].pixels = {0,255,17,0,0,0,9,0};
    for (auto& state : valid.trace.outline.sprites) state.source_border_covered = false;
    assert(ValidateSceneCapture(valid)); // Fully owned border coverage, independent of zero palette index.
    auto software = *DecodeSceneCapture(v6); software.format_version = 7;
    const auto absent = EncodeSceneCapture(software);
    assert(absent.size() == v6.size()+1 && absent.back() == 0 && std::equal(v6.begin()+24,v6.end(),absent.begin()+24));
    assert(EncodeSceneCapture(*DecodeSceneCapture(absent)) == absent);
    for (uint32_t version = 1; version <= 6; ++version) {
        auto old = version < 3 ? Fixture() : version == 3 ? FixtureV3() : *DecodeSceneCapture(v6);
        old.format_version = version;
        if (version == 1) { old.assets.presentation.clear(); old.assets.projection_reference.clear(); }
        if (version >= 4) {
            old.scene.objects[1].draw_class = 2; old.trace.entries[1].kind = 2;
            if (version == 4) { old.trace.raster.excluded_shadow_entries = 0; old.trace.raster.shadow_masks.clear(); old.trace.raster.shadows.clear();
                std::erase_if(old.trace.raster.commands,[](const auto& command) { return command.kind == SceneDrawCommandKind::Shadow; }); }
        }
        assert(ValidateSceneCapture(old));
        old.trace.outline.alpha = .5f; RejectCapture(old);
        auto invalid = *retained; invalid.format_version = version; RejectCapture(invalid);
    }
    // The extension begins after the unchanged v6 body; counts are checked before allocation.
    auto prefix = *retained; prefix.format_version = 3; prefix.trace.outline = {};
    const size_t extension = EncodeSceneCapture(prefix).size()+20; // empty triangle/command + shadow tail
    const size_t occluders = extension+43, sprites = occluders+4+3*52, commands = sprites+4+3*82;
    assert(bytes[extension] == 1 && bytes.size() == commands+4+6*9);
    for (auto [offset,count] : {std::pair{occluders,262145u},std::pair{sprites,65537u},std::pair{commands,196609u}}) {
        auto bad = bytes; SetU32(bad,offset,count); Rechecksum(bad); Reject(bad);
    }
    for (size_t offset : {extension,extension+1,sprites+4+61,sprites+4+64}) {
        auto bad = bytes; bad[offset] = 2; Rechecksum(bad); Reject(bad);
    }
    for (size_t cut : {extension,extension+2,occluders+11,sprites+31,commands+7,bytes.size()-1}) {
        auto bad = std::vector<uint8_t>(bytes.begin(),bytes.begin()+cut);
        SetU32(bad,8,uint32_t(bad.size()-24)); Rechecksum(bad); Reject(bad);
    }
    auto unknown = bytes; SetU32(unknown,4,10); Reject(unknown);
    return bytes;
}
std::pair<std::vector<uint8_t>,std::vector<uint8_t>> CheckV8(const std::vector<uint8_t>& v5, const std::vector<uint8_t>& v7)
{
    auto c = *DecodeSceneCapture(v5); c.format_version = 8;
    c.scene.camera.viewport_x = 753; c.scene.camera.viewport_w = 3087;
    for (auto& draw : c.trace.draws) draw.scale.window_w = 3087;
    c.target = {SceneIndexedTargetKind::Screen,173,0,4096,2160,4112,753,0,3087,2160};
    assert(ValidateSceneCapture(c));
    const auto bytes = EncodeSceneCapture(c); const auto retained = DecodeSceneCapture(bytes);
    assert(retained->format_version == 8 && retained->target == c.target && EncodeSceneCapture(*retained) == bytes);
    const uint8_t expected[] = {1,173,0,0, 0,16,0,0, 112,8,0,0, 16,16,0,0,
        241,2,0,0, 0,0,0,0, 15,12,0,0, 112,8,0,0};
    const size_t tail = bytes.size()-32;
    assert(bytes[tail-1] == 0 && std::equal(std::begin(expected),std::end(expected),bytes.begin()+tail));
    auto prefix = c; prefix.format_version = 7; prefix.target = {};
    const auto older = EncodeSceneCapture(prefix);
    assert(older.size()+32 == bytes.size() && std::equal(older.begin()+24,older.end(),bytes.begin()+24));
    c.target.clear_index = 99; c.target.width = 999;
    assert(EncodeSceneCapture(*retained) == bytes); // Target proof is owned with the immutable capture.
    auto reject = [&](auto change) { auto invalid = *retained; change(invalid); RejectCapture(invalid); };
    reject([](auto& x) { x.target = {}; });
    reject([](auto& x) { x.target.kind = SceneIndexedTargetKind(3); });
    reject([](auto& x) { x.trace.backend = 1; });
    reject([](auto& x) { x.trace.complete = false; });
    reject([](auto& x) { x.trace.outline.present = true; });
    reject([](auto& x) { x.target.lens_id = 1; });
    reject([](auto& x) { x.target.width = 0; });
    reject([](auto& x) { x.target.height = 0; });
    reject([](auto& x) { x.target.width = 16385; x.target.pitch = 16385; });
    reject([](auto& x) { x.target.height = 16385; });
    reject([](auto& x) { x.target.pitch = 4095; });
    reject([](auto& x) { x.target.pitch = 16385; });
    reject([](auto& x) { x.target.width = x.target.pitch = 16384; x.target.height = 4097; });
    reject([](auto& x) { x.target.window_x = -1; });
    reject([](auto& x) { x.target.window_y = -1; });
    reject([](auto& x) { x.target.window_w = 0; });
    reject([](auto& x) { x.target.window_h = -1; });
    reject([](auto& x) { x.target.window_x = 1010; x.scene.camera.viewport_x = 1010; }); // Logical width, not pitch padding.
    reject([](auto& x) { x.target.window_y = 1; x.scene.camera.viewport_y = 1; });
    reject([](auto& x) { x.scene.camera.viewport_w++; });
    reject([](auto& x) { x.scene.camera.viewport_x = 752; });
    for (uint8_t clear : {uint8_t(0),uint8_t(255)}) { auto valid = *retained; valid.target.clear_index = clear; assert(ValidateSceneCapture(valid)); }
    auto boundary = *retained; boundary.target.width = boundary.target.pitch = 16384; boundary.target.height = 4096;
    assert(ValidateSceneCapture(boundary)); // Exactly 64 MiB, without allocating or owning padding pixels.
    auto lens = *retained; lens.scene.camera.view = SceneView::Possession; lens.scene.camera.view_mode = 6;
    lens.scene.camera.lens = 2256; // Projection lens differs from the actual lens-source identity.
    lens.scene.camera.viewport_x = 16; lens.scene.camera.viewport_y = 12; lens.scene.camera.viewport_w = 3000; lens.scene.camera.viewport_h = 2000;
    for (auto& draw : lens.trace.draws) { draw.scale.window_w = 3000; draw.scale.window_h = 2000; }
    lens.target = {SceneIndexedTargetKind::LensSource,0,18,3072,2048,3072,16,12,3000,2000};
    assert(ValidateSceneCapture(lens));
    const auto lens_bytes = EncodeSceneCapture(lens); const auto lens_retained = DecodeSceneCapture(lens_bytes);
    assert(lens_retained->target == lens.target && EncodeSceneCapture(*lens_retained) == lens_bytes);
    for (uint16_t id : {uint16_t(1),uint16_t(255)}) { auto valid = lens; valid.target.lens_id = id; assert(ValidateSceneCapture(valid)); }
    auto no_lens = lens; no_lens.target.kind = SceneIndexedTargetKind::Screen; no_lens.target.clear_index = 144; no_lens.target.lens_id = 0;
    assert(ValidateSceneCapture(no_lens) && DecodeSceneCapture(EncodeSceneCapture(no_lens))->target == no_lens.target);
    for (unsigned field = 0; field < 6; ++field) {
        auto invalid = lens;
        if (field == 0) invalid.target.clear_index = 1;
        if (field == 1) invalid.target.lens_id = 256;
        if (field == 2) invalid.target.pitch++;
        if (field == 3) invalid.scene.camera.view = SceneView::Overhead;
        if (field == 4) invalid.scene.camera.view = SceneView::Front;
        if (field == 5) invalid.target.lens_id = 0;
        RejectCapture(invalid);
    }
    auto gl = *DecodeSceneCapture(v7); gl.format_version = 8;
    gl.target = {SceneIndexedTargetKind::Screen,0,0,3840,2160,3840,0,0,3840,2160};
    RejectCapture(gl); // V8 cannot smuggle a valid v7 GL extension into the software target format.
    for (uint32_t version = 1; version <= 7; ++version) {
        auto old = version < 3 ? Fixture() : version == 3 ? FixtureV3() : *DecodeSceneCapture(v5); old.format_version = version;
        if (version == 1) { old.assets.presentation.clear(); old.assets.projection_reference.clear(); }
        if (version == 4) { old.trace.raster.excluded_shadow_entries = 0; old.trace.raster.shadow_masks.clear(); old.trace.raster.shadows.clear();
            std::erase_if(old.trace.raster.commands,[](const auto& command) { return command.kind == SceneDrawCommandKind::Shadow; }); }
        assert(ValidateSceneCapture(old));
        for (unsigned field = 0; field < 10; ++field) {
            auto invalid = old;
            if (field == 0) invalid.target.kind = SceneIndexedTargetKind::Screen;
            if (field == 1) invalid.target.clear_index = 1;
            if (field == 2) invalid.target.lens_id = 1;
            if (field == 3) invalid.target.width = 1;
            if (field == 4) invalid.target.height = 1;
            if (field == 5) invalid.target.pitch = 1;
            if (field == 6) invalid.target.window_x = 1;
            if (field == 7) invalid.target.window_y = 1;
            if (field == 8) invalid.target.window_w = 1;
            if (field == 9) invalid.target.window_h = 1;
            RejectCapture(invalid);
        }
    }
    for (auto [offset,value] : {std::pair{tail+4,0u},std::pair{tail+8,16385u},std::pair{tail+12,4095u},
        std::pair{tail+16,0xffffffffu},std::pair{tail+24,0u},std::pair{tail+24,3088u}}) {
        auto bad = bytes; SetU32(bad,offset,value); Rechecksum(bad); Reject(bad);
    }
    for (size_t cut : {tail,tail+1,tail+3,tail+17,bytes.size()-1}) {
        auto bad = std::vector<uint8_t>(bytes.begin(),bytes.begin()+cut);
        SetU32(bad,8,uint32_t(bad.size()-24)); Rechecksum(bad); Reject(bad);
    }
    for (size_t position : {tail,tail+1,tail+12,bytes.size()-1}) { auto bad = bytes; bad[position] ^= 1; Reject(bad); }
    auto unknown = bytes; SetU32(unknown,4,10); Reject(unknown);
    return {bytes,lens_bytes};
}
std::vector<uint8_t> CheckV9(const std::vector<uint8_t>& v7, const std::vector<uint8_t>& v8, const std::vector<uint8_t>& v5)
{
    auto c = *DecodeSceneCapture(v7); c.format_version = 9;
    auto& o = c.trace.outline;
    c.trace.entries = {{4097,0},{2049,2},{2049,2},{2049,2},{2049,2},{2049,2},{2049,2},{0,6}};
    o.occluders[1].entry = o.occluders[2].entry = 7;
    for (unsigned i = 1; i < 6; ++i) {
        if (i >= 3) { c.trace.draws.push_back(c.trace.draws[0]); o.sprites.push_back(o.sprites[0]); }
        auto& draw = c.trace.draws[i]; auto& state = o.sprites[i];
        draw.entry = i+1; draw.scale.frame_x = 10+int32_t(i)*8;
        state.draw = i; state.dst_x = 11+int32_t(i)*8; state.sort_key = (uint64_t(2049)<<32)|i;
        if (i == 1 || i == 3 || i == 5) {
            draw.flags |= 0x0800; draw.has_remap = true; draw.blend = SceneSpriteBlend::Remap;
            for (unsigned index = 0; index < 256; ++index) draw.remap[index] = uint8_t(255-index);
            state.flags = draw.flags;
        }
        if (i >= 3) {
            state.wants_outline = false; state.owner_rgba.fill(0);
            state.path = i == 3 ? SceneOutlineExecutionPath::AtlasFallback : SceneOutlineExecutionPath::TextureFallback;
            o.commands.push_back({SceneOutlineCommandKind::Sprite,i,2*i+3});
        }
    }
    for (unsigned row = 0; row < 2; ++row) {
        std::array<uint8_t,1024> colours{};
        for (unsigned index = 0; index < 256; ++index) {
            colours[index*4] = uint8_t(index*(row?7:3)+(row?19:7));
            colours[index*4+1] = uint8_t(row?index*3:255-index);
            colours[index*4+2] = uint8_t(row?255-index:127); colours[index*4+3] = 255;
        }
        o.colour_rows.push_back(colours);
    }
    o.colour_bindings = {{0,SceneOutlineColourResolution::ClutIdentity},{1,SceneOutlineColourResolution::ClutRemap},
        {UINT32_MAX,SceneOutlineColourResolution::Glow},{0,SceneOutlineColourResolution::ClutCapacityIdentity},
        {0,SceneOutlineColourResolution::TextureIdentity},{1,SceneOutlineColourResolution::TextureRemap}};
    assert(ValidateSceneCapture(c));
    const auto bytes = EncodeSceneCapture(c); const auto retained = DecodeSceneCapture(bytes);
    assert(retained->format_version == 9 && retained->target == SceneIndexedTarget{} && EncodeSceneCapture(*retained) == bytes);
    const auto& colours = retained->trace.outline;
    assert(colours.colour_rows.size() == 2 && colours.colour_bindings.size() == 6);
    const uint32_t rows[] = {0,1,UINT32_MAX,0,0,1}; const unsigned reasons[] = {1,2,0,3,4,5};
    for (size_t i = 0; i < 6; ++i) assert(colours.colour_bindings[i].lookup_row == rows[i] && unsigned(colours.colour_bindings[i].resolution) == reasons[i]);
    assert(colours.colour_rows[0][0] == 7 && colours.colour_rows[0][1] == 255 && colours.colour_rows[0][2] == 127
        && colours.colour_rows[0][3] == 255 && colours.colour_rows[1][255*4+2] == 0);
    o.colour_rows[0].fill(0); o.colour_bindings.clear(); assert(EncodeSceneCapture(*retained) == bytes);
    auto reject = [&](auto change) { auto invalid = *retained; change(invalid); RejectCapture(invalid); };
    reject([](auto& x) { x.trace.backend = 0; });
    reject([](auto& x) { x.trace.outline.present = false; });
    reject([](auto& x) { x.target.clear_index = 1; });
    reject([](auto& x) { x.target.kind = SceneIndexedTargetKind::Screen; });
    reject([](auto& x) { x.trace.outline.colour_rows.resize(16385); });
    reject([](auto& x) { x.trace.outline.colour_rows[1] = x.trace.outline.colour_rows[0]; });
    reject([](auto& x) { x.trace.outline.colour_rows.push_back({}); }); // Unique but unused row.
    reject([](auto& x) { x.trace.outline.colour_bindings.pop_back(); });
    reject([](auto& x) { x.trace.outline.colour_bindings.push_back({}); });
    reject([](auto& x) { x.trace.outline.colour_bindings[0].lookup_row = 1; }); // Noncanonical first use.
    reject([](auto& x) { x.trace.outline.colour_bindings[0].lookup_row = 2; });
    reject([](auto& x) { x.trace.outline.colour_bindings[0].lookup_row = UINT32_MAX; });
    reject([](auto& x) { x.trace.outline.colour_bindings[2].lookup_row = 0; });
    reject([](auto& x) { x.trace.outline.colour_bindings[0].resolution = SceneOutlineColourResolution::Glow; });
    reject([](auto& x) { x.trace.outline.colour_bindings[2].resolution = SceneOutlineColourResolution::TextureIdentity; });
    reject([](auto& x) { x.trace.outline.colour_bindings[0].resolution = SceneOutlineColourResolution(6); });
    reject([](auto& x) { x.trace.outline.colour_bindings[0].resolution = SceneOutlineColourResolution::ClutRemap; });
    reject([](auto& x) { x.trace.outline.colour_bindings[1].resolution = SceneOutlineColourResolution::ClutIdentity; });
    reject([](auto& x) { x.trace.outline.colour_bindings[1].resolution = SceneOutlineColourResolution::TextureRemap; });
    reject([](auto& x) { x.trace.outline.colour_bindings[4].resolution = SceneOutlineColourResolution::ClutIdentity; });
    reject([](auto& x) { x.trace.outline.colour_bindings[5].resolution = SceneOutlineColourResolution::ClutCapacityIdentity; });
    reject([](auto& x) { x.trace.outline.colour_rows[0][3] = 254; });
    reject([](auto& x) { x.trace.outline.colour_rows[0].fill(0); }); // Texture fallback cannot consume transparent CLUT alpha.
    reject([](auto& x) { x.trace.outline.colour_rows[1].fill(0); }); // Remap rows cannot use uninitialized identity data.
    auto valid = *retained; valid.assets.display_palette.fill(0);
    assert(ValidateSceneCapture(valid)); // Effective cached rows may differ from the current palette.
    valid = *retained; valid.trace.outline.colour_bindings[1].lookup_row = 0;
    assert(ValidateSceneCapture(valid)); // Row one is still first-used later by texture remap.
    valid = *retained; valid.trace.outline.colour_bindings[1].resolution = SceneOutlineColourResolution::ClutCapacityIdentity;
    assert(ValidateSceneCapture(valid)); // Requested remap can resolve to capacity identity.
    valid = *retained; valid.trace.outline.colour_rows.push_back({}); valid.trace.outline.colour_bindings[3].lookup_row = 2;
    assert(ValidateSceneCapture(valid)); // Capacity identity admits the entirely zero initialized CLUT row.
    valid.trace.outline.colour_rows[2][0] = 1; RejectCapture(valid);
    valid = *retained; valid.trace.outline.colour_rows[0].fill(0);
    valid.trace.outline.colour_rows.push_back(colours.colour_rows[0]); valid.trace.outline.colour_bindings[4].lookup_row = 2;
    assert(ValidateSceneCapture(valid)); // Identity CLUT zero is valid; texture identity has its own opaque bytes.
    valid = *retained; valid.trace.draws[0].scale.frame_dst_w = 5;
    valid.trace.outline.sprites[0].dst_w = 5; valid.trace.outline.sprites[0].phase_x = 16384; valid.trace.outline.sprites[0].step_x = 163840;
    assert(ValidateSceneCapture(valid));
    for (uint32_t version = 1; version <= 8; ++version) {
        auto old = version < 3 ? Fixture() : version == 3 ? FixtureV3() : version == 7 ? *DecodeSceneCapture(v7)
            : version == 8 ? *DecodeSceneCapture(v8) : *DecodeSceneCapture(v5); old.format_version = version;
        if (version == 1) { old.assets.presentation.clear(); old.assets.projection_reference.clear(); }
        if (version == 4) { old.trace.raster.excluded_shadow_entries = 0; old.trace.raster.shadow_masks.clear(); old.trace.raster.shadows.clear();
            std::erase_if(old.trace.raster.commands,[](const auto& command) { return command.kind == SceneDrawCommandKind::Shadow; }); }
        assert(ValidateSceneCapture(old));
        auto invalid = old; invalid.trace.outline.colour_rows.push_back(colours.colour_rows[0]); RejectCapture(invalid);
        invalid = old; invalid.trace.outline.colour_bindings.push_back({}); RejectCapture(invalid);
    }
    auto software = *DecodeSceneCapture(v8); software.format_version = 9; RejectCapture(software);
    const size_t tail = bytes.size()-(4+2*1024+4+6*5), bindings = tail+4+2*1024;
    assert(std::all_of(bytes.begin()+tail-32,bytes.begin()+tail,[](uint8_t value) { return value == 0; }));
    for (auto [offset,value] : {std::pair{tail,16385u},std::pair{bindings,65537u},std::pair{bindings,5u},std::pair{bindings+4,1u}}) {
        auto bad = bytes; SetU32(bad,offset,value); Rechecksum(bad); Reject(bad);
    }
    for (size_t cut : {tail,tail+4+1023,bindings+2,bytes.size()-1}) {
        auto bad = std::vector<uint8_t>(bytes.begin(),bytes.begin()+cut); SetU32(bad,8,uint32_t(bad.size()-24)); Rechecksum(bad); Reject(bad);
    }
    for (size_t position : {tail+4,bindings+4,bytes.size()-1}) { auto bad = bytes; bad[position] ^= 1; Reject(bad); }
    auto unknown = bytes; SetU32(unknown,4,10); Reject(unknown);
    return bytes;
}
std::vector<uint8_t> CheckV10(const std::vector<uint8_t>& v9, const std::vector<uint8_t>& v8)
{
    auto c = *DecodeSceneCapture(v9); c.format_version = 10;
    c.scene.objects[0].class_id = 5;
    c.trace.entries.insert(c.trace.entries.end(), {{0,3},{0,3},{0,3}});
    auto& w = c.trace.gl_world;
    w.present = w.complete = true; w.native_width = 3840; w.native_height = 2160;
    w.viewport_width = 3840; w.viewport_height = 2160; w.clear_rgba = {.125f,.25f,.75f,1};
    w.shade_settings = {0,.25f,1.5f,.75f,1,2,.5f,.625f,1};
    for (unsigned i = 0; i < 256; ++i) {
        w.palette_rgba[i*4] = uint8_t(i+7); w.palette_rgba[i*4+1] = uint8_t(i*3+17);
        w.palette_rgba[i*4+2] = uint8_t(255-i); w.palette_rgba[i*4+3] = 255;
    }
    for (size_t i = 0; i < w.fade.size(); ++i) w.fade[i] = uint8_t(i*7+i/256);
    w.lightmap.resize(511*511);
    for (size_t i = 0; i < w.lightmap.size(); ++i) w.lightmap[i] = uint16_t(i*17);
    for (unsigned version = 0; version < 2; ++version) {
        std::array<uint8_t,1024> tile{};
        for (unsigned i = 0; i < tile.size(); ++i) tile[i] = uint8_t(i*3+version*11);
        w.tiles.push_back(tile);
    }
    // Two tile versions and a real missing-tile sentinel, parallel to the
    // independent three occluders; repeated bytes need no texture identity.
    for (uint32_t tile : {0u,UINT32_MAX,1u}) {
        SceneGLTerrainTriangle triangle; triangle.tile = tile;
        triangle.vertices = {{{-4,31,.5f,-1,-1},{32,.25f,64,511,0},{.125f,1,-32,.5f,511}}};
        w.terrain.push_back(triangle);
    }
    w.shadow_sources = {{4,3,{0,255,0,255,255,255,0,0,0,0,255,255}},{1,1,{255}}};
    for (uint32_t i = 0; i < 3; ++i) {
        SceneGLShadow shadow; shadow.caster = c.scene.objects[0].id; shadow.entry = 8+i; shadow.source = i == 1 ? 1 : 0;
        const float u = i == 1 ? 0 : 3.0f/256, v = i == 1 ? 0 : 2.0f/256;
        shadow.vertices = {{{10,20,0,0},{30,20,u,0},{30,40,u,v},{10,40,0,v}}};
        shadow.z_ndc = -1; shadow.darken = .375f; w.shadows.push_back(shadow);
    }
    assert(ValidateSceneCapture(c));
    const auto bytes = EncodeSceneCapture(c); const auto retained = DecodeSceneCapture(bytes);
    assert(retained->format_version == 10 && retained->trace.gl_world == w && EncodeSceneCapture(*retained) == bytes);
    const auto& actual = retained->trace.gl_world;
    assert(actual.terrain[0].tile == 0 && actual.terrain[1].tile == UINT32_MAX && actual.terrain[2].tile == 1);
    assert(actual.tiles[0][0] == 0 && actual.tiles[1][0] == 11 && actual.lightmap[510] == 8670);
    assert(actual.shadows[0].entry == 8 && actual.shadows[1].source == 1 && actual.shadows[2].source == 0);
    assert(actual.shadows[0].vertices[2].u == 3.0f/256 && actual.shadows[0].vertices[2].v == 2.0f/256);
    w.tiles[0].fill(99); w.shadow_sources[0].coverage.clear(); w.palette_rgba.fill(0);
    assert(EncodeSceneCapture(*retained) == bytes); // Applied state/resources are owned, not producer aliases.
    auto reject = [&](auto change) { auto invalid = *retained; change(invalid); RejectCapture(invalid); };
    reject([](auto& x) { x.trace.gl_world.present = false; });
    reject([](auto& x) { x.trace.gl_world.complete = false; });
    reject([](auto& x) { x.trace.backend = 0; });
    reject([](auto& x) { x.trace.gl_world.target_kind = SceneGLWorldTargetKind(2); });
    reject([](auto& x) { x.trace.gl_world.target_kind = SceneGLWorldTargetKind::LensSource; });
    reject([](auto& x) { x.target.clear_index = 1; });
    reject([](auto& x) { x.trace.gl_world.native_width = 0; });
    reject([](auto& x) { x.trace.gl_world.native_height = 16385; });
    reject([](auto& x) { x.trace.gl_world.native_width = 16385; });
    reject([](auto& x) { x.trace.gl_world.native_width = x.trace.gl_world.native_height = 4097; });
    reject([](auto& x) { x.trace.gl_world.viewport_x = -1; });
    reject([](auto& x) { x.trace.gl_world.viewport_y = 1; x.scene.camera.viewport_y = 1; });
    reject([](auto& x) { x.trace.gl_world.viewport_width = 0; });
    reject([](auto& x) { x.trace.gl_world.viewport_height = -1; });
    reject([](auto& x) { x.trace.gl_world.viewport_width--; });
    reject([](auto& x) { x.trace.gl_world.clear_rgba[0] = -.01f; });
    reject([](auto& x) { x.trace.gl_world.clear_rgba[1] = 1.01f; });
    reject([](auto& x) { x.trace.gl_world.clear_rgba[2] = std::numeric_limits<float>::infinity(); });
    reject([](auto& x) { x.trace.gl_world.clear_rgba[3] = 0; });
    reject([](auto& x) { x.trace.gl_world.palette_rgba[3] = 254; });
    const float bad_settings[] = {.5f,-.01f,.09f,3.01f,2,3,5.01f,1.01f,.5f};
    for (size_t i = 0; i < 9; ++i) {
        reject([&](auto& x) { x.trace.gl_world.shade_settings[i] = bad_settings[i]; });
        reject([&](auto& x) { x.trace.gl_world.shade_settings[i] = std::numeric_limits<float>::quiet_NaN(); });
    }
    reject([](auto& x) { x.trace.gl_world.time = .5f; });
    reject([](auto& x) { x.trace.gl_world.lightmap.pop_back(); });
    reject([](auto& x) { x.trace.gl_world.lightmap.push_back(1); });
    reject([](auto& x) { x.trace.gl_world.tiles.resize(65537); });
    reject([](auto& x) { x.trace.gl_world.tiles[1] = x.trace.gl_world.tiles[0]; });
    reject([](auto& x) { x.trace.gl_world.tiles.push_back({}); });
    reject([](auto& x) { x.trace.gl_world.terrain.pop_back(); });
    reject([](auto& x) { x.trace.gl_world.terrain.push_back(x.trace.gl_world.terrain[0]); });
    reject([](auto& x) { x.trace.gl_world.terrain[0].tile = 1; });
    reject([](auto& x) { x.trace.gl_world.terrain[2].tile = 2; });
    reject([](auto& x) { x.trace.gl_world.terrain[2].tile = UINT32_MAX; });
    for (auto member : {&SceneGLTerrainVertex::u,&SceneGLTerrainVertex::v,&SceneGLTerrainVertex::shade,
        &SceneGLTerrainVertex::stl_x,&SceneGLTerrainVertex::stl_y}) {
        reject([&](auto& x) { x.trace.gl_world.terrain[0].vertices[0].*member = std::numeric_limits<float>::infinity(); });
    }
    reject([](auto& x) { x.trace.gl_world.terrain[0].vertices[0].u = 32769; });
    reject([](auto& x) { x.trace.gl_world.terrain[0].vertices[0].v = -32769; });
    reject([](auto& x) { x.trace.gl_world.terrain[0].vertices[0].shade = 1025; });
    reject([](auto& x) { x.trace.gl_world.terrain[0].vertices[0].stl_x = -1.01f; });
    reject([](auto& x) { x.trace.gl_world.terrain[0].vertices[0].stl_y = 511.01f; });
    reject([](auto& x) { x.trace.gl_world.shadow_sources.resize(65537); });
    reject([](auto& x) { x.trace.gl_world.shadow_sources[0].width = 0; });
    reject([](auto& x) { x.trace.gl_world.shadow_sources[0].height = 257; });
    reject([](auto& x) { x.trace.gl_world.shadow_sources[0].coverage.pop_back(); });
    reject([](auto& x) { x.trace.gl_world.shadow_sources[0].coverage[0] = 1; });
    reject([](auto& x) { x.trace.gl_world.shadow_sources[1] = x.trace.gl_world.shadow_sources[0]; });
    reject([](auto& x) { x.trace.gl_world.shadow_sources.push_back({1,1,{0}}); });
    reject([](auto& x) { x.trace.gl_world.shadows.resize(65537); });
    reject([](auto& x) { x.trace.gl_world.shadows.pop_back(); });
    reject([](auto& x) { x.trace.gl_world.shadows.push_back(x.trace.gl_world.shadows[0]); });
    reject([](auto& x) { x.trace.gl_world.shadows[0].caster.incarnation++; });
    reject([](auto& x) { x.trace.gl_world.shadows[0].caster.slot = 99; });
    reject([](auto& x) { x.scene.objects[0].class_id = 4; });
    reject([](auto& x) { x.scene.objects[0].draw_class = 6; });
    reject([](auto& x) { x.trace.gl_world.shadows[0].entry = 7; });
    reject([](auto& x) { x.trace.gl_world.shadows[1].entry = 8; });
    reject([](auto& x) { std::swap(x.trace.gl_world.shadows[0],x.trace.gl_world.shadows[1]); });
    reject([](auto& x) { x.trace.gl_world.shadows[0].source = 1; });
    reject([](auto& x) { x.trace.gl_world.shadows[0].source = 2; });
    reject([](auto& x) { x.trace.gl_world.shadows[0].z_ndc = 0; });
    reject([](auto& x) { x.trace.gl_world.shadows[0].darken = .19f; });
    reject([](auto& x) { x.trace.gl_world.shadows[0].darken = .56f; });
    reject([](auto& x) { x.trace.gl_world.shadows[0].darken = std::numeric_limits<float>::quiet_NaN(); });
    reject([](auto& x) { x.trace.gl_world.shadows[0].vertices[0].x = 16777218; });
    reject([](auto& x) { x.trace.gl_world.shadows[0].vertices[0].y = std::numeric_limits<float>::infinity(); });
    reject([](auto& x) { x.trace.gl_world.shadows[0].vertices[0].u = -.001f; });
    reject([](auto& x) { x.trace.gl_world.shadows[0].vertices[2].v = 3.0f/256; });
    reject([](auto& x) { x.assets.texture_pixels.resize(64*1024*1024); }); // World resources share the old asset budget.
    auto valid = *retained;
    valid.trace.gl_world.native_width = 16384; valid.trace.gl_world.native_height = 1024;
    valid.trace.gl_world.viewport_height = valid.scene.camera.viewport_h = 1024;
    for (auto& draw : valid.trace.draws) draw.scale.window_h = 1024;
    assert(ValidateSceneCapture(valid)); // Exactly 64 MiB RGBA target, no target-pixel allocation.
    valid = *retained; valid.trace.gl_world.tiles.resize(1); valid.trace.gl_world.terrain[2].tile = 0;
    assert(ValidateSceneCapture(valid)); // Reuse a prior tile version without introducing a row.
    valid = *retained; valid.trace.gl_world.shadow_sources[0].width = valid.trace.gl_world.shadow_sources[0].height = 256;
    valid.trace.gl_world.shadow_sources[0].coverage.assign(256*256,0);
    valid.trace.gl_world.shadows[0].vertices[2].u = valid.trace.gl_world.shadows[0].vertices[2].v = 255.0f/256;
    assert(ValidateSceneCapture(valid)); // Binary all-zero mask and full logical source boundary are owned.
    // Rotable0 shadows can use frame endpoints beyond the logical artwork.
    // Only the actual initialized atlas footprint proves those samples owned.
    auto padded = *retained;
    auto& padded_source = padded.trace.gl_world.shadow_sources[0];
    padded_source.width = padded_source.height = 256;
    padded_source.coverage.assign(256*256,0);
    for (unsigned y = 0; y < 38; ++y)
        for (unsigned x = 0; x < 45; ++x) padded_source.coverage[y*256+x] = 255;
    auto& padded_shadow = padded.trace.gl_world.shadows[0];
    padded_shadow.vertices[1].u = padded_shadow.vertices[2].u = 46.0f/256;
    padded_shadow.vertices[2].v = padded_shadow.vertices[3].v = 42.0f/256;
    assert(padded_source.coverage[37*256+44] == 255 && padded_source.coverage[42*256+46] == 0);
    assert(ValidateSceneCapture(padded));
    const auto padded_bytes = EncodeSceneCapture(padded);
    const auto padded_loaded = DecodeSceneCapture(padded_bytes);
    assert(padded_loaded && padded_loaded->trace.gl_world == padded.trace.gl_world
        && EncodeSceneCapture(*padded_loaded) == padded_bytes);
    auto unproved = padded;
    unproved.trace.gl_world.shadow_sources[0] = {45,38,std::vector<uint8_t>(45*38,255)};
    RejectCapture(unproved); // Identical UV cannot borrow uncaptured atlas padding.
    for (auto member : {&SceneGLShadowVertex::u,&SceneGLShadowVertex::v}) {
        auto outside = padded;
        outside.trace.gl_world.shadows[0].vertices[2].*member = 256.0f/256;
        RejectCapture(outside); // Last owned texel is255; endpoint256 is outside.
    }
    valid = *retained; valid.trace.gl_world.clear_rgba = {0,1,0,1}; valid.trace.gl_world.lightmap[0] = 65535;
    valid.trace.gl_world.shade_settings = {1,0,.1f,3,0,0,5,1,0}; assert(ValidateSceneCapture(valid));
    valid = *retained; valid.trace.gl_world.target_kind = SceneGLWorldTargetKind::LensSource;
    valid.scene.camera.view = SceneView::Possession;
    for (auto& sprite : valid.trace.outline.sprites) { sprite.wants_outline = false; sprite.owner_rgba.fill(0); }
    std::erase_if(valid.trace.outline.commands,[](const auto& command) { return command.kind == SceneOutlineCommandKind::Outline; });
    assert(ValidateSceneCapture(valid) && EncodeSceneCapture(*DecodeSceneCapture(EncodeSceneCapture(valid))) == EncodeSceneCapture(valid));
    for (const auto& previous : {v8,v9}) {
        auto old = *DecodeSceneCapture(previous); old.trace.gl_world.present = true; RejectCapture(old);
        old = *DecodeSceneCapture(previous); old.trace.gl_world.lightmap.push_back(0); RejectCapture(old);
        old = *DecodeSceneCapture(previous); old.trace.gl_world.time = 1; RejectCapture(old);
    }
    auto software = *DecodeSceneCapture(v8); software.format_version = 10; RejectCapture(software);
    auto prefix = *retained; prefix.format_version = 9; prefix.trace.gl_world = {};
    const auto old = EncodeSceneCapture(prefix); const size_t tail = old.size();
    assert(std::equal(old.begin()+24,old.end(),bytes.begin()+24));
    const size_t lightmap = tail+17491, tiles = lightmap+4+511*511*2, terrain = tiles+4+2*1024;
    const size_t sources = terrain+4+3*64, shadows = sources+4+8+12+8+1;
    assert(bytes.size() == shadows+4+3*92 && bytes[tail] == 1 && bytes[tail+1] == 1 && bytes[tail+2] == 0);
    assert(bytes[tail+43] == 7 && bytes[tail+44] == 17 && bytes[tail+45] == 255 && bytes[tail+46] == 255);
    for (auto [offset,count] : {std::pair{lightmap,261120u},std::pair{lightmap,261122u},std::pair{tiles,65537u},
        std::pair{terrain,262145u},std::pair{sources,65537u},std::pair{sources+8,65537u},std::pair{shadows,65537u}}) {
        auto bad = bytes; SetU32(bad,offset,count); Rechecksum(bad); Reject(bad);
    }
    for (size_t offset : {tail,tail+1}) { auto bad = bytes; bad[offset] = 2; Rechecksum(bad); Reject(bad); }
    for (size_t cut : {tail,tail+17490,lightmap+9,tiles+7,terrain+63,sources+11,shadows+91,bytes.size()-1}) {
        auto bad = std::vector<uint8_t>(bytes.begin(),bytes.begin()+cut); SetU32(bad,8,uint32_t(bad.size()-24)); Rechecksum(bad); Reject(bad);
    }
    for (size_t offset : {tail+43,lightmap+4,tiles+4,sources+12,bytes.size()-1}) { auto bad = bytes; bad[offset] ^= 1; Reject(bad); }
    auto unknown = bytes; SetU32(unknown,4,12); Reject(unknown);
    return bytes;
}
std::map<std::string, std::vector<uint8_t>> CheckV11(const std::vector<uint8_t>& v10)
{
    auto c = *DecodeSceneCapture(v10); c.format_version = 11;
    auto& composition = c.trace.gl_composition;
    composition.present = composition.complete = true; composition.completed_phases = 15;
    composition.phase_quad_counts = {3,1,1}; composition.screen_clear_rgba = c.trace.gl_world.clear_rgba;
    auto& overlay = composition.overlays;
    overlay.native_width = 3840; overlay.native_height = 2160;
    overlay.sources = {{4,4,{0,1,2,3,4,5,6,7,8,9,10,11,0,0,0,0},{1,1,1,0}},
        {2,2,{0,255,1,4},{1,1}}};
    for (unsigned row = 0; row < 2; ++row) {
        SceneGLOverlayColourRow colour{};
        for (unsigned i = 0; i < colour.size(); ++i) colour[i] = uint8_t(i*7+row*13);
        overlay.colour_rows.push_back(colour);
    }
    overlay.fade.resize(16384);
    for (size_t i = 0; i < overlay.fade.size(); ++i) overlay.fade[i] = uint8_t(i*3+i/256);
    for (unsigned mode = 0; mode < 5; ++mode) {
        SceneGLOverlayQuad quad; quad.mode = SceneGLOverlayMode(mode);
        quad.phase = mode < 3 ? SceneGLOverlayPhase::DepthWorld : mode == 3 ? SceneGLOverlayPhase::FlatWorld : SceneGLOverlayPhase::Swipe;
        quad.depth_test = mode != 3; quad.x0 = -2.5f; quad.y0 = 10; quad.x1 = 64.25f; quad.y1 = 48;
        quad.z_ndc = -.25f; quad.colour = {.25f,.5f,.75f,.625f};
        if (mode != 2) { quad.source = mode == 3 ? 1 : 0; quad.u1 = 1; quad.v1 = mode == 3 ? 1 : .49f; }
        if (mode != 1 && mode != 2) quad.colour_row = mode == 3 ? 1 : 0;
        if (mode == 3) { quad.fade_row = 63; quad.scissor_enabled = true; quad.scissor_x = 7; quad.scissor_y = 9; quad.scissor_w = 45; quad.scissor_h = 39; }
        if (mode == 4) { quad.u0 = 1; quad.u1 = 0; quad.v0 = .49f; quad.v1 = 0; }
        overlay.draws.push_back(quad);
    }
    assert(ValidateSceneCapture(c));
    std::map<std::string,std::vector<uint8_t>> fixtures;
    auto retain = [&](const std::string& name, const SceneCapture& capture) {
        assert(ValidateSceneCapture(capture)); const auto bytes = EncodeSceneCapture(capture);
        const auto decoded = DecodeSceneCapture(bytes);
        assert(decoded->trace.gl_composition == capture.trace.gl_composition && EncodeSceneCapture(*decoded) == bytes);
        fixtures.emplace(name,bytes);
    };
    retain(".v11",c);
    const auto bytes = fixtures.at(".v11"); const size_t tail = v10.size();
    assert(std::equal(v10.begin()+24,v10.end(),bytes.begin()+24));
    const size_t sources = tail+39, rows = sources+4+16+16+4+16+4+2;
    const size_t fade = rows+4+2*1024, draws = fade+4+16384, lens = draws+4+5*81;
    assert(bytes.size() == lens+201 && bytes[tail] == 1 && bytes[tail+2] == 15);
    assert(bytes[draws+4+81+1] == 1 && bytes[draws+4+3*81] == 1 && bytes[draws+4+4*81+2] == 1);
    assert(bytes[sources+4+12+16+4+3] == 0 && bytes[lens+2] == 0);
    auto reject = [&](auto change) { auto bad = c; change(bad); RejectCapture(bad); };
    reject([](auto& x) { x.trace.gl_composition.present = false; });
    reject([](auto& x) { x.trace.gl_composition.complete = false; });
    for (unsigned bit : {1u,2u,4u,8u,16u}) reject([&](auto& x) { x.trace.gl_composition.completed_phases ^= bit; });
    reject([](auto& x) { x.trace.backend = 0; });
    reject([](auto& x) { x.trace.gl_composition.phase_quad_counts[2]++; });
    reject([](auto& x) { x.trace.gl_composition.screen_clear_rgba[0] = 0; });
    reject([](auto& x) { x.trace.gl_composition.screen_clear_rgba[3] = .5f; });
    reject([](auto& x) { x.trace.gl_composition.screen_clear_rgba[1] = std::numeric_limits<float>::quiet_NaN(); });
    reject([](auto& x) { x.trace.gl_composition.overlays.native_width--; });
    reject([](auto& x) { x.trace.gl_composition.overlays.sources.resize(257); });
    reject([](auto& x) { x.trace.gl_composition.overlays.sources[0].width = 0; });
    reject([](auto& x) { x.trace.gl_composition.overlays.sources[0].height = 16385; });
    reject([](auto& x) { x.trace.gl_composition.overlays.sources[0].indices.pop_back(); });
    reject([](auto& x) { x.trace.gl_composition.overlays.sources[0].initialized_rows.pop_back(); });
    reject([](auto& x) { x.trace.gl_composition.overlays.sources[0].initialized_rows[3] = 2; });
    reject([](auto& x) { x.trace.gl_composition.overlays.sources[0].indices[15] = 1; });
    reject([](auto& x) { x.trace.gl_composition.overlays.sources[1] = x.trace.gl_composition.overlays.sources[0]; });
    reject([](auto& x) { x.trace.gl_composition.overlays.sources.push_back({1,1,{0},{1}}); });
    reject([](auto& x) { x.trace.gl_composition.overlays.colour_rows[1] = x.trace.gl_composition.overlays.colour_rows[0]; });
    reject([](auto& x) { x.trace.gl_composition.overlays.colour_rows.push_back({}); });
    reject([](auto& x) { x.trace.gl_composition.overlays.colour_rows.resize(16385); });
    reject([](auto& x) { x.trace.gl_composition.overlays.fade.pop_back(); });
    reject([](auto& x) { x.trace.gl_composition.overlays.draws.resize(262145); });
    reject([](auto& x) { x.trace.gl_composition.overlays.draws[0].mode = SceneGLOverlayMode(5); });
    reject([](auto& x) { x.trace.gl_composition.overlays.draws[4].phase = SceneGLOverlayPhase(3); });
    reject([](auto& x) { x.trace.gl_composition.overlays.draws[0].depth_test = false; });
    reject([](auto& x) { x.trace.gl_composition.overlays.draws[3].depth_test = true; });
    reject([](auto& x) { std::swap(x.trace.gl_composition.overlays.draws[3],x.trace.gl_composition.overlays.draws[4]); });
    reject([](auto& x) { x.trace.gl_composition.overlays.draws[0].scissor_x = 1; });
    reject([](auto& x) { x.trace.gl_composition.overlays.draws[3].scissor_w = -1; });
    reject([](auto& x) { x.trace.gl_composition.overlays.draws[0].x1 = x.trace.gl_composition.overlays.draws[0].x0; });
    reject([](auto& x) { x.trace.gl_composition.overlays.draws[0].y0 = 32769; });
    reject([](auto& x) { x.trace.gl_composition.overlays.draws[0].z_ndc = 2.01f; });
    reject([](auto& x) { x.trace.gl_composition.overlays.draws[0].colour[0] = -1; });
    reject([](auto& x) { x.trace.gl_composition.overlays.draws[0].u0 = std::numeric_limits<float>::infinity(); });
    reject([](auto& x) { x.trace.gl_composition.overlays.draws[0].v1 = .75f; }); // Unknown fourth row.
    reject([](auto& x) { x.trace.gl_composition.overlays.draws[4].v0 = 1; }); // Flipped unknown row.
    reject([](auto& x) { x.trace.gl_composition.overlays.draws[0].source = 1; }); // Valid resource, wrong first use.
    reject([](auto& x) { x.trace.gl_composition.overlays.draws[0].source = 2; });
    reject([](auto& x) { x.trace.gl_composition.overlays.draws[0].colour_row = 1; });
    reject([](auto& x) { x.trace.gl_composition.overlays.draws[0].colour_row = 2; });
    reject([](auto& x) { x.trace.gl_composition.overlays.draws[2].source = 0; });
    reject([](auto& x) { x.trace.gl_composition.overlays.draws[2].v1 = .1f; });
    reject([](auto& x) { x.trace.gl_composition.overlays.draws[1].colour_row = 0; });
    reject([](auto& x) { x.trace.gl_composition.overlays.draws[0].fade_row = 1; });
    reject([](auto& x) { x.trace.gl_composition.overlays.draws[3].fade_row = 64; });
    reject([](auto& x) { x.trace.gl_composition.lens.requested_active = true; });
    reject([](auto& x) { x.trace.gl_composition.lens.requested_type = 1; });
    reject([](auto& x) { x.trace.gl_composition.lens.resources[0] = {1,1}; });
    reject([](auto& x) { x.assets.texture_pixels.resize(64*1024*1024); });
    auto edge = c;
    auto& source = edge.trace.gl_composition.overlays.sources[0]; source.initialized_rows[2] = 0;
    std::fill(source.indices.begin()+8,source.indices.begin()+12,0);
    edge.trace.gl_composition.overlays.draws[0].v1 = std::nextafter(.5f,0.0f); RejectCapture(edge);
    edge.trace.gl_composition.overlays.draws[0].v1 = std::nextafter(std::nextafter(.5f,0.0f),0.0f);
    assert(ValidateSceneCapture(edge)); // The one-neighbour proof catches the otherwise unseen row boundary.
    auto empty = c; empty.trace.gl_composition.overlays.sources.clear(); empty.trace.gl_composition.overlays.colour_rows.clear();
    empty.trace.gl_composition.overlays.fade.clear(); empty.trace.gl_composition.overlays.draws.clear(); empty.trace.gl_composition.phase_quad_counts = {};
    retain(".v11-empty",empty); // Completed empty phases still have a native target and a resolved None lens.
    auto nofade = c; nofade.trace.gl_composition.overlays.draws[3].mode = SceneGLOverlayMode::Sprite;
    nofade.trace.gl_composition.overlays.draws[3].fade_row = 0; nofade.trace.gl_composition.overlays.fade.clear(); assert(ValidateSceneCapture(nofade));
    nofade.trace.gl_composition.overlays.fade.push_back(0); RejectCapture(nofade);
    auto zero = c; zero.trace.gl_world.clear_rgba[0] = 0; zero.trace.gl_composition.screen_clear_rgba[0] = -0.0f;
    assert(ValidateSceneCapture(zero)); // Native float defaults use numeric equality, including signed zero.
    auto lensbase = c; lensbase.trace.gl_world.target_kind = SceneGLWorldTargetKind::LensSource;
    lensbase.scene.camera.view = SceneView::Possession;
    for (auto& sprite : lensbase.trace.outline.sprites) { sprite.wants_outline = false; sprite.owner_rgba.fill(0); }
    std::erase_if(lensbase.trace.outline.commands,[](const auto& command) { return command.kind == SceneOutlineCommandKind::Outline; });
    lensbase.trace.gl_composition.screen_clear_rgba = {0,.5f,0,1};
    lensbase.trace.gl_composition.lens.requested_active = lensbase.trace.gl_composition.lens.capture_began = true;
    auto makeLens = [&](SceneGLLensOperation operation, uint8_t requested) {
        auto result = lensbase; auto& lens = result.trace.gl_composition.lens; lens.operation = operation; lens.requested_type = requested;
        lens.fallback_copy = operation == SceneGLLensOperation::Copy && requested != 0;
        auto& pass = lens.pass; pass.kind = SceneGLLensPassKind(unsigned(operation)-1);
        pass.source_width = pass.target_width = 3840; pass.source_height = pass.target_height = 2160;
        pass.source_x = pass.destination_x = 7; pass.destination_y = 11; pass.viewport_width = 64; pass.viewport_height = 48;
        if (operation == SceneGLLensOperation::Remap) {
            pass.remap_width = pass.remap_height = 2; pass.remap = {{{0,0},{65535,65535},{7,11},{123,456}}}; lens.resources[4] = {3,19};
        }
        if (operation == SceneGLLensOperation::Mist || operation == SceneGLLensOperation::Overlay) {
            pass.palette_rgba.resize(1024); pass.rgb6_lookup.resize(262144);
            for (size_t i = 0; i < pass.palette_rgba.size(); ++i) pass.palette_rgba[i] = uint8_t(i*11);
            for (size_t i = 0; i < pass.rgb6_lookup.size(); ++i) pass.rgb6_lookup[i] = uint8_t(i*3+i/64);
            lens.resources[0] = {2,7}; lens.resources[2] = {2,9};
        }
        if (operation == SceneGLLensOperation::Mist) {
            pass.mist_offsets = {255.75f,0,.5f,128.125f}; pass.mist_lightness = 63;
            pass.fade.resize(16384); pass.density.resize(65536);
            for (size_t i = 0; i < pass.fade.size(); ++i) pass.fade[i] = uint8_t(i*5+i/256);
            for (size_t i = 0; i < pass.density.size(); ++i) pass.density[i] = uint8_t(i+i/256);
            lens.resources[1] = {2,8}; lens.resources[3] = {8,5};
        }
        if (operation == SceneGLLensOperation::Overlay) {
            pass.overlay_width = 3; pass.overlay_height = 2; pass.overlay_alpha = 256; pass.overlay = {0,255,1,2,3,4}; lens.resources[5] = {9,23};
        }
        return result;
    };
    for (auto [name,operation,request] : {std::tuple{".v11-copy",SceneGLLensOperation::Copy,uint8_t(0)},
        std::tuple{".v11-fallback",SceneGLLensOperation::Copy,uint8_t(2)},std::tuple{".v11-remap",SceneGLLensOperation::Remap,uint8_t(3)},
        std::tuple{".v11-mist",SceneGLLensOperation::Mist,uint8_t(1)},std::tuple{".v11-overlay",SceneGLLensOperation::Overlay,uint8_t(4)}}) {
        const auto valid = makeLens(operation,request); retain(name,valid);
        // Corrupt actual wire counts, not only the in-memory validator input;
        // bounded readers must reject these before allocating lookup storage.
        const auto& wire = fixtures.at(name); auto prefix = valid; prefix.format_version = 10; prefix.trace.gl_composition = {};
        const size_t pass_start = EncodeSceneCapture(prefix).size() + (lens-tail) + 101;
        const auto& pass = valid.trace.gl_composition.lens.pass;
        const size_t remap_count = pass_start+66, overlay_count = remap_count+4+pass.remap.size()*4+10;
        const size_t density_count = overlay_count+4+pass.overlay.size(), palette_count = density_count+4+pass.density.size();
        const size_t fade_count = palette_count+4+pass.palette_rgba.size(), rgb6_count = fade_count+4+pass.fade.size();
        for (auto [offset,count] : {std::pair{remap_count,16777217u},std::pair{overlay_count,67108865u},
            std::pair{density_count,65537u},std::pair{palette_count,1025u},std::pair{fade_count,16385u},std::pair{rgb6_count,262145u}}) {
            auto bad = wire; SetU32(bad,offset,count); Rechecksum(bad); Reject(bad);
        }
        auto fail = [&](auto change) { auto bad = valid; change(bad.trace.gl_composition.lens); RejectCapture(bad); };
        fail([](auto& x) { x.capture_began = false; }); fail([](auto& x) { x.operation = SceneGLLensOperation(6); });
        fail([](auto& x) { x.requested_type = 5; }); fail([](auto& x) { x.fallback_copy = !x.fallback_copy; });
        fail([](auto& x) { x.pass.kind = SceneGLLensPassKind(4); }); fail([](auto& x) { x.pass.source_width--; });
        fail([](auto& x) { x.pass.target_height--; }); fail([](auto& x) { x.pass.source_y = 1; });
        fail([](auto& x) { x.pass.source_x++; }); fail([](auto& x) { x.pass.destination_y = -1; });
        fail([](auto& x) { x.pass.viewport_width = 0; }); fail([](auto& x) { x.pass.viewport_height = 2160; });
        const auto& stamps = valid.trace.gl_composition.lens.resources;
        auto maximal_stamp = valid;
        for (auto& stamp : maximal_stamp.trace.gl_composition.lens.resources)
            if (stamp.generation) stamp = {UINT64_MAX,UINT64_MAX};
        assert(ValidateSceneCapture(maximal_stamp)
            && DecodeSceneCapture(EncodeSceneCapture(maximal_stamp))->trace.gl_composition.lens.resources
                == maximal_stamp.trace.gl_composition.lens.resources);
        for (size_t i = 0; i < 6; ++i) {
            fail([&](auto& x) { x.resources[i].generation = stamps[i].generation ? 0 : 1; });
            fail([&](auto& x) { x.resources[i].revision = stamps[i].revision ? 0 : 1; });
        }
        if (operation == SceneGLLensOperation::Mist) {
            fail([](auto& x) { x.requested_type = 2; }); fail([](auto& x) { x.pass.mist_offsets[0] = 256; });
            fail([](auto& x) { x.pass.mist_offsets[1] = -.01f; }); fail([](auto& x) { x.pass.mist_lightness = 64; });
            fail([](auto& x) { x.pass.density.pop_back(); }); fail([](auto& x) { x.pass.fade.pop_back(); });
        } else fail([](auto& x) { x.pass.mist_offsets[0] = .5f; });
        if (operation == SceneGLLensOperation::Remap) {
            fail([](auto& x) { x.requested_type = 1; }); fail([](auto& x) { x.pass.remap_width = 0; });
            fail([](auto& x) { x.pass.remap.pop_back(); });
            auto displacement = valid; displacement.trace.gl_composition.lens.requested_type = 2; assert(ValidateSceneCapture(displacement));
        } else fail([](auto& x) { x.pass.remap_width = 1; });
        if (operation == SceneGLLensOperation::Overlay) {
            fail([](auto& x) { x.requested_type = 3; }); fail([](auto& x) { x.pass.overlay_alpha = 257; });
            fail([](auto& x) { x.pass.overlay_width = 16385; }); fail([](auto& x) { x.pass.overlay.pop_back(); });
            auto transparent = valid; transparent.trace.gl_composition.lens.pass.overlay_alpha = 0; assert(ValidateSceneCapture(transparent));
        } else fail([](auto& x) { x.pass.overlay_alpha = 1; });
        if (operation == SceneGLLensOperation::Mist || operation == SceneGLLensOperation::Overlay) {
            fail([](auto& x) { x.pass.palette_rgba.pop_back(); }); fail([](auto& x) { x.pass.rgb6_lookup.pop_back(); });
        } else fail([](auto& x) { x.pass.palette_rgba.push_back(0); });
    }
    {
        auto combined = makeLens(SceneGLLensOperation::Remap,3);
        auto& sources_owned = combined.trace.gl_composition.overlays.sources;
        sources_owned[0] = {8192,4096,std::vector<uint8_t>(32*1024*1024,0),std::vector<uint8_t>(4096,1)};
        auto& remap_owned = combined.trace.gl_composition.lens.pass;
        remap_owned.remap_width = remap_owned.remap_height = 3072;
        remap_owned.remap.assign(3072*3072,{0,65535});
        // 32 MiB actual source +36 MiB RG16 map exceeds the old shared cap,
        // while both children remain bounded. Retain it for independent JS
        // decoding and both architectures' canonical re-encoding.
        ValidateSceneGLOverlayPass(combined.trace.gl_composition.overlays);
        ValidateSceneGLLensPass(remap_owned); retain(".v11-budget",combined);
        sources_owned[0] = {12288,4096,std::vector<uint8_t>(48*1024*1024,0),std::vector<uint8_t>(4096,1)};
        remap_owned.remap_width = 12288; remap_owned.remap_height = 1024;
        remap_owned.remap.assign(12288*1024,{65535,0});
        // Each 48 MiB child passes independently; adding mandatory world and
        // lookup resources crosses 96 MiB and must reject the composition.
        ValidateSceneGLOverlayPass(combined.trace.gl_composition.overlays);
        ValidateSceneGLLensPass(remap_owned); RejectCapture(combined);
    }
    {
        auto child = makeLens(SceneGLLensOperation::Overlay,4);
        auto& pass = child.trace.gl_composition.lens.pass;
        pass.overlay_width = pass.overlay_height = 8192; pass.overlay.assign(64*1024*1024,0);
        // Image alone is64 MiB, but required palette/RGB6 belongs to the same
        // child; aggregate96 must not accidentally relax this child limit.
        bool rejected = false;
        try { ValidateSceneGLLensPass(pass); } catch (const std::invalid_argument&) { rejected = true; }
        assert(rejected); RejectCapture(child);
    }
    auto notdrawn = c; auto& resolution = notdrawn.trace.gl_composition.lens;
    resolution.requested_active = true; resolution.operation = SceneGLLensOperation::NotDrawn; resolution.requested_type = 4;
    retain(".v11-not-drawn",notdrawn);
    for (uint8_t request = 0; request <= 4; ++request) {
        auto copy = makeLens(SceneGLLensOperation::Copy,request); assert(ValidateSceneCapture(copy));
        notdrawn.trace.gl_composition.lens.requested_type = request; assert(ValidateSceneCapture(notdrawn));
    }
    notdrawn.trace.gl_composition.lens.pass.source_width = 1; RejectCapture(notdrawn);
    notdrawn = lensbase; notdrawn.trace.gl_composition.lens.operation = SceneGLLensOperation::NotDrawn;
    retain(".v11-not-drawn-source",notdrawn);
    for (auto [offset,count] : {std::pair{sources,257u},std::pair{sources+12,67108865u},std::pair{sources+32,16385u},
        std::pair{rows,16385u},std::pair{fade,16385u},std::pair{draws,262145u}}) {
        auto bad = bytes; SetU32(bad,offset,count); Rechecksum(bad); Reject(bad);
    }
    for (size_t offset : {tail,tail+1,lens,lens+1,lens+4,draws+6,draws+7}) {
        auto bad = bytes; bad[offset] = 2; Rechecksum(bad); Reject(bad);
    }
    for (size_t cut : {tail,tail+54,sources+19,rows+1023,fade+16383,draws+80,lens+100,bytes.size()-1}) {
        auto bad = std::vector<uint8_t>(bytes.begin(),bytes.begin()+cut); SetU32(bad,8,uint32_t(bad.size()-24)); Rechecksum(bad); Reject(bad);
    }
    auto unknown = bytes; SetU32(unknown,4,12); Reject(unknown);
    auto old = *DecodeSceneCapture(v10); old.trace.gl_composition.completed_phases = 1; RejectCapture(old);
    return fixtures;
}
void Inspect(const std::filesystem::path& path)
{
    const auto c = LoadSceneCapture(path);
    std::set<unsigned> variations;
    for (const auto& col : c->scene.columns) variations.insert(col.texture_variation);
    std::array<size_t, 5> blends{};
    std::map<uint32_t, unsigned> layers;
    size_t clipped = 0, multi_layer = 0;
    for (const auto& draw : c->trace.draws) {
        ++blends[unsigned(draw.blend)]; ++layers[draw.entry];
        if (draw.content_height < c->assets.sprites[draw.sprite].height) ++clipped;
    }
    for (auto [entry, count] : layers) { (void)entry; if (count > 1) ++multi_layer; }
    std::cout << "{\"version\":" << c->format_version << ",\"traceComplete\":" << (c->trace.complete ? "true" : "false")
        << ",\"traceBackend\":" << unsigned(c->trace.backend) << ",\"entries\":" << c->trace.entries.size()
        << ",\"draws\":" << c->trace.draws.size() << ",\"excluded\":" << c->trace.excluded_sprite_calls
        << ",\"triangles\":" << c->trace.raster.triangles.size() << ",\"commands\":" << c->trace.raster.commands.size()
        << ",\"shadowMasks\":" << c->trace.raster.shadow_masks.size() << ",\"shadows\":" << c->trace.raster.shadows.size()
        << ",\"excludedShadows\":" << c->trace.raster.excluded_shadow_entries
        << ",\"outlinePresent\":" << (c->trace.outline.present ? "true" : "false")
        << ",\"outlineOccluders\":" << c->trace.outline.occluders.size() << ",\"outlineSprites\":" << c->trace.outline.sprites.size()
        << ",\"outlineCommands\":" << c->trace.outline.commands.size()
        << ",\"outlineColourRows\":" << c->trace.outline.colour_rows.size() << ",\"outlineColourBindings\":" << c->trace.outline.colour_bindings.size()
        << ",\"glWorldPresent\":" << (c->trace.gl_world.present ? "true" : "false")
        << ",\"glWorldTiles\":" << c->trace.gl_world.tiles.size() << ",\"glWorldTerrain\":" << c->trace.gl_world.terrain.size()
        << ",\"glWorldShadowSources\":" << c->trace.gl_world.shadow_sources.size() << ",\"glWorldShadows\":" << c->trace.gl_world.shadows.size()
        << ",\"compositionPresent\":" << (c->trace.gl_composition.present ? "true" : "false")
        << ",\"overlaySources\":" << c->trace.gl_composition.overlays.sources.size() << ",\"overlayDraws\":" << c->trace.gl_composition.overlays.draws.size()
        << ",\"lensOperation\":" << unsigned(c->trace.gl_composition.lens.operation)
        << ",\"targetKind\":" << unsigned(c->target.kind) << ",\"targetClearIndex\":" << unsigned(c->target.clear_index)
        << ",\"targetLensId\":" << c->target.lens_id << ",\"targetSize\":[" << c->target.width << ',' << c->target.height
        << ',' << c->target.pitch << "],\"targetWindow\":[" << c->target.window_x << ',' << c->target.window_y << ',' << c->target.window_w << ',' << c->target.window_h << ']'
        << ",\"blendCounts\":{\"opaque\":" << blends[0] << ",\"remap\":" << blends[1]
        << ",\"ghostSourceHigh\":" << blends[2] << ",\"ghostDestinationHigh\":" << blends[3] << ",\"additive\":" << blends[4]
        << "},\"clippedDraws\":" << clipped << ",\"multiLayerEntries\":" << multi_layer << ",\"additiveBytes\":" << c->assets.additive.size()
        << ",\"view\":" << unsigned(c->scene.camera.view) << ",\"epoch\":" << c->scene.epoch
        << ",\"sequence\":" << c->scene.sequence << ",\"objects\":" << c->scene.objects.size()
        << ",\"sprites\":" << c->assets.sprites.size() << ",\"bindings\":" << c->assets.bindings.size()
        << ",\"textureBytes\":" << c->assets.texture_pixels.size() << ",\"variations\":[";
    bool comma = false;
    for (auto v : variations) { std::cout << (comma ? "," : "") << v; comma = true; }
    std::cout << "]}\n";
    uint32_t w, h;
    const auto rgba = SceneAssetContactSheet(*c, w, h);
    std::ofstream image(path.string() + ".ppm", std::ios::binary);
    image << "P6\n" << w << ' ' << h << "\n255\n";
    for (size_t i = 0; i < rgba.size(); i += 4) image.write(reinterpret_cast<const char*>(&rgba[i]), 3);
    if (!image) throw std::runtime_error("Contact sheet write failed");
    // Re-encoding after independent file loading must be byte-for-byte stable.
    std::ifstream source(path, std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(source)), {});
    if (EncodeSceneCapture(*c) != bytes) throw std::runtime_error("Noncanonical capture");
}
}
int main(int argc, char** argv)
{
    try {
        if (argc == 3 && std::string(argv[1]) == "--inspect") { Inspect(argv[2]); return 0; }
        if (argc != 2) throw std::runtime_error("Usage: scene-assets-test output-fixture | --inspect capture");
        auto original = Fixture();
        assert(original.format_version == 2 && ValidateSceneCapture(original));
        const auto bytes = EncodeSceneCapture(original);
        const auto retained = DecodeSceneCapture(bytes);
        original.scene.reference_image = "must-not-be-replayed.png";
        assert(EncodeSceneCapture(original) == bytes && retained->scene.reference_image.empty());
        assert(EncodeSceneCapture(*retained) == bytes);
        assert(retained->assets.bindings[0].origin_x == -22 && retained->assets.bindings[0].origin_width == 99);
        assert(retained->assets.presentation[0].lightness == 12345 && retained->assets.presentation[0].angle_offset == 768);
        assert(retained->assets.projection_reference[0].screen_y == -444);
        auto legacy = *retained; legacy.format_version = 1;
        legacy.assets.presentation.clear(); legacy.assets.projection_reference.clear();
        const auto v1 = EncodeSceneCapture(legacy);
        assert(DecodeSceneCapture(v1)->format_version == 1 && EncodeSceneCapture(*DecodeSceneCapture(v1)) == v1);
        assert(v1.size() + 46 == bytes.size() && std::equal(v1.begin() + 24, v1.end(), bytes.begin() + 24));
        const auto v3 = CheckV3();
        const auto v4 = CheckV4();
        const auto v5 = CheckV5(v4);
        const auto v6 = CheckV6(v5);
        const auto v7 = CheckV7(v6);
        const auto [v8,v8_lens] = CheckV8(v5,v7);
        const auto v9 = CheckV9(v7,v8,v5);
        const auto v10 = CheckV10(v9,v8);
        const auto v11 = CheckV11(v10);
        original.assets.texture_pixels.assign(8192, 99); original.assets.sprites.clear(); original.scene.epoch++;
        assert(EncodeSceneCapture(*retained) == bytes); // no producer storage aliasing
        assert(retained->assets.sprites[0].pixels[0] == 0 && retained->assets.sprites[0].pixels[1] == 255);
        assert(retained->assets.sprites[0].pixels[5] == 0); // transparent skip, separate coverage
        std::vector<uint8_t> dst{77};
        for (const auto& bad : std::vector<std::vector<uint8_t>>{{}, {2, 0}, {3, 1, 2, 3, 0}, {253, 0}, {0}}) {
            assert(!DecodeSceneSprite(bad, 2, 2, dst)); assert(dst == std::vector<uint8_t>{77});
        }
        for (size_t cut : {size_t(0), size_t(23), size_t(24), bytes.size() / 2, bytes.size() - 1})
            Reject({bytes.begin(), bytes.begin() + cut});
        for (size_t position : {size_t(0), size_t(4), size_t(8), size_t(12), size_t(16), bytes.size() - 1}) {
            auto bad = bytes; bad[position] ^= 1; Reject(bad);
        }
        auto bad = bytes; bad.push_back(0); Reject(bad);
        bad = bytes; bad[24 + 24] = 255; // corrupt map width, preserving checksum
        bad[24 + 25] = 255; Rechecksum(bad); Reject(bad);
        bad = bytes; bad[24 + 36] = 2; Rechecksum(bad); Reject(bad); // noncanonical bool
        bad = bytes; // excessive column count, rejected before allocation
        for (size_t i = 0; i < 4; ++i) bad[24 + 2210 + i] = 255;
        Rechecksum(bad); Reject(bad);
        auto invalid = *retained;
        invalid.scene.texture_frames[0] = int32_t(invalid.assets.texture_pixels.size() - 1);
        assert(!ValidateSceneCapture(invalid));
        invalid = *retained; invalid.assets.bindings.pop_back(); assert(!ValidateSceneCapture(invalid));
        invalid = *retained; invalid.assets.bindings[0].sprite = 999; assert(!ValidateSceneCapture(invalid));
        invalid = *retained; invalid.assets.presentation[0].id.incarnation++; assert(!ValidateSceneCapture(invalid));
        invalid = *retained; invalid.assets.presentation[0].lightness=65535; assert(!ValidateSceneCapture(invalid));
        invalid = *retained; invalid.assets.projection_reference.resize(4097); assert(!ValidateSceneCapture(invalid));
        invalid = *retained; invalid.assets.sprites[0].pixels[1] = 1; assert(!ValidateSceneCapture(invalid));
        invalid = *retained; invalid.scene.cube_materials[0].textures[0] = 65535; assert(!ValidateSceneCapture(invalid));
        invalid = *retained; invalid.assets.sprites[0].width = 0; invalid.assets.sprites[0].pixels.clear();
        assert(ValidateSceneCapture(invalid)); // intentionally empty effect frame
        auto write = [](const std::filesystem::path& path, const auto& data) {
            std::ofstream file(path, std::ios::binary); file.write(reinterpret_cast<const char*>(data.data()), data.size());
            if (!file) throw std::runtime_error("Fixture write failed");
        };
        write(argv[1], bytes); write(std::string(argv[1]) + ".v1", v1); write(std::string(argv[1]) + ".v3", v3);
        write(std::string(argv[1]) + ".v4", v4);
        write(std::string(argv[1]) + ".v5", v5);
        write(std::string(argv[1]) + ".v6", v6);
        write(std::string(argv[1]) + ".v7", v7);
        write(std::string(argv[1]) + ".v8", v8); write(std::string(argv[1]) + ".v8-lens", v8_lens);
        write(std::string(argv[1]) + ".v9", v9);
        write(std::string(argv[1]) + ".v10", v10);
        for (const auto& [suffix,data] : v11) write(std::string(argv[1]) + suffix,data);
        std::cout << "Scene assets: ownership, RLE coverage/bounds, canonical v1-v11 codec, mixed raster/GL world/outline/composition/lens rejection checks passed ("
            << bytes.size() << " v2 bytes, " << v3.size() << " v3 bytes, " << v4.size() << " v4 bytes, " << v5.size() << " v5 bytes, " << v6.size() << " v6 bytes, " << v7.size() << " v7 bytes, " << v8.size() << " v8 screen bytes, " << v8_lens.size() << " v8 lens bytes, " << v9.size() << " v9 bytes, " << v10.size() << " v10 bytes)\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
