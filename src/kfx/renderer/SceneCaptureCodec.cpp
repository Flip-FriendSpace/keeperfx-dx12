// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
// KFXC v1-v11: explicit little-endian fields, bounded lengths and payload CRC32.
// This is a local rendering fixture, independent of saves and network state.
#include "kfx/renderer/SceneAssets.h"
#include <bit>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace kfx::render {
namespace {
constexpr size_t MaxFile = 128 * 1024 * 1024;
void Require(bool ok) { if (!ok) throw std::runtime_error("Invalid or unsupported scene capture"); }
uint32_t Crc(std::span<const uint8_t> data)
{
    uint32_t crc = ~0u;
    for (auto b : data) {
        crc ^= b;
        for (int i = 0; i < 8; ++i) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
    }
    return ~crc;
}
struct Writer {
    std::vector<uint8_t> bytes;
    template<class T> void scalar(T v) {
        if constexpr (std::is_same_v<T, bool>) scalar(uint8_t(v));
        else if constexpr (std::is_enum_v<T>) scalar(static_cast<std::underlying_type_t<T>>(v));
        else if constexpr (std::is_same_v<T, float>) scalar(std::bit_cast<uint32_t>(v));
        else if constexpr (std::is_same_v<T, double>) scalar(std::bit_cast<uint64_t>(v));
        else {
            static_assert(std::is_integral_v<T>);
            using U = std::make_unsigned_t<T>;
            U u = static_cast<U>(v);
            Require(bytes.size() <= MaxFile - sizeof(T));
            for (size_t i = 0; i < sizeof(T); ++i) bytes.push_back(uint8_t(u >> (i * 8)));
        }
    }
    template<class T> void count(T&, size_t n, size_t cap, size_t = 1) { Require(n <= cap); scalar(uint32_t(n)); }
};
struct Reader {
    std::span<const uint8_t> bytes;
    size_t pos = 0, allocation = 0;
    template<class T> void scalar(T& v) {
        if constexpr (std::is_same_v<T, bool>) { uint8_t b; scalar(b); Require(b <= 1); v = b != 0; }
        else if constexpr (std::is_enum_v<T>) { std::underlying_type_t<T> e; scalar(e); v = static_cast<T>(e); }
        else if constexpr (std::is_same_v<T, float>) { uint32_t u; scalar(u); v = std::bit_cast<float>(u); }
        else if constexpr (std::is_same_v<T, double>) { uint64_t u; scalar(u); v = std::bit_cast<double>(u); }
        else {
            static_assert(std::is_integral_v<T>);
            Require(sizeof(T) <= bytes.size() - pos);
            using U = std::make_unsigned_t<T>;
            U u = 0;
            for (size_t i = 0; i < sizeof(T); ++i) u |= U(bytes[pos++]) << (i * 8);
            v = std::bit_cast<T>(u);
        }
    }
    template<class T> void count(T& v, size_t, size_t cap, size_t wire_size = 1) {
        uint32_t n; scalar(n);
        Require(wire_size > 0 && n <= cap && n <= (bytes.size() - pos) / wire_size);
        const uint64_t size = uint64_t(n) * sizeof(typename T::value_type);
        Require(size <= MaxFile && allocation <= MaxFile - size);
        allocation += size;
        v.resize(n);
    }
};
template<class A, class... T> void Fields(A& a, T&... values) { (a.scalar(values), ...); }
template<class A, class T, size_t N> void Array(A& a, std::array<T, N>& v) { for (auto& x : v) a.scalar(x); }
template<class A, class T, class F> void Vector(A& a, std::vector<T>& v, size_t cap, F f) {
    a.count(v, v.size(), cap); for (auto& x : v) f(a, x);
}
template<class A, class T> void Scalars(A& a, std::vector<T>& v, size_t cap) {
    Vector(a, v, cap, [](auto& ar, auto& x) { ar.scalar(x); });
}
template<class A> void Position(A& a, ScenePosition& p) { Fields(a, p.x, p.y, p.z); }
template<class A> void Identity(A& a, SceneObjectId& id) { Fields(a, id.incarnation, id.slot); }
template<class A> void Capture(A& a, SceneCapture& c)
{
    auto& s = c.scene;
    Fields(a, s.epoch, s.sequence, s.game_turn, s.player, s.map_width, s.map_height,
        s.texture_generation, s.ambient_light, s.lights_enabled, s.interpolation, s.lava_scroll, s.water_scroll);
    for (auto& w : s.wibble) { Position(a, w.offset); a.scalar(w.lightness_offset); }
    auto& v = s.camera;
    a.scalar(v.view); Position(a, v.position);
    Fields(a, v.yaw, v.pitch, v.roll, v.scaled_zoom, v.horizontal_fov, v.lens,
        v.viewport_x, v.viewport_y, v.viewport_w, v.viewport_h, v.pixel_size, v.view_distance,
        v.view_mode, v.lens_mode, v.short_walls, v.has_rotation);
    Array(a, v.rotation);
    Vector(a, s.columns, 512 * 512, [](auto& ar, auto& x) {
        Fields(ar, x.cell, x.x, x.y); Array(ar, x.cubes);
        Fields(ar, x.solid_mask, x.floor_texture, x.column_flags, x.orientation, x.texture_variation,
            x.map_flags, x.wibble, x.ceiling_subtiles, x.revealed, x.in_view); Array(ar, x.light);
    });
    Vector(a, s.objects, 65536, [](auto& ar, auto& x) {
        Identity(ar, x.id); Position(ar, x.position); Position(ar, x.simulation_position);
        Fields(ar, x.floor_height, x.model, x.animation, x.size, x.yaw, x.pitch,
            x.owner, x.class_id, x.draw_class, x.frame, x.render_flags, x.tint);
    });
    Vector(a, s.lights, 65536, [](auto& ar, auto& x) {
        Identity(ar, x.id); Position(ar, x.position); Position(ar, x.previous_position);
        Fields(ar, x.radius, x.random_intensity, x.previous_random_intensity, x.intensity, x.flags,
            x.reset_interpolation, x.last_turn_moved, x.last_turn_randomized);
    });
    Vector(a, s.cube_materials, 65536, [](auto& ar, auto& x) { Array(ar, x.textures); ar.scalar(x.properties); });
    Scalars(a, s.top_cubes, 65536); Scalars(a, s.ceiling_textures, 65536); Scalars(a, s.texture_frames, 32 * 1544);
    Fields(a, s.texture_dimension, s.texture_row_stride);
    // capture_ms is deliberately not persisted: profiling clocks are not scene content.
    auto& assets = c.assets;
    Array(a, assets.base_palette); Array(a, assets.display_palette); Array(a, assets.fade); Array(a, assets.ghost);
    Scalars(a, assets.texture_pixels, 64 * 1024 * 1024);
    Vector(a, assets.sprites, 65536, [](auto& ar, auto& x) {
        Fields(ar, x.id, x.width, x.height, x.frame_width, x.frame_height, x.frame_x, x.frame_y,
            x.offset_x, x.offset_y, x.shadow_offset, x.flags);
        Scalars(ar, x.pixels, 1024 * 1024 * 2);
    });
    Vector(a, assets.bindings, 65536 * 8, [](auto& ar, auto& x) {
        Fields(ar, x.animation, x.frame, x.direction, x.sprite, x.flip);
        Fields(ar, x.origin_width, x.origin_height, x.origin_x, x.origin_y);
    });
    if (c.format_version >= 2) {
        Vector(a, assets.presentation, 65536, [](auto& ar, auto& x) {
            Identity(ar, x.id); Fields(ar, x.lightness, x.angle_offset, x.rotates);
        });
        Vector(a, assets.projection_reference, 4096, [](auto& ar, auto& x) {
            Position(ar, x.position); Fields(ar, x.screen_x, x.screen_y, x.visible);
        });
    }
    if (c.format_version >= 3) {
        Scalars(a, assets.additive, 65536);
        auto& trace = c.trace;
        Fields(a, trace.complete, trace.backend, trace.excluded_sprite_calls);
        Vector(a, trace.entries, 262144, [](auto& ar, auto& x) { Fields(ar, x.bucket, x.kind); });
        Vector(a, trace.draws, 65536, [](auto& ar, auto& x) {
            Identity(ar, x.object); Fields(ar, x.entry, x.sprite);
            auto& scale = x.scale;
            Fields(ar, scale.frame_x, scale.frame_y, scale.frame_src_w, scale.frame_src_h,
                scale.frame_dst_w, scale.frame_dst_h, scale.content_x, scale.content_y, scale.window_w, scale.window_h);
            Fields(ar, x.content_height, x.flags, x.alpha, x.has_remap, x.blend); Array(ar, x.remap);
        });
    }
    if (c.format_version >= 4) {
        // Append-only extension: versions 1-3 retain their exact wire bytes.
        // Check fixed wire lengths before allocating on decode, independent
        // of compiler padding in these owned in-memory structs.
        auto& raster = c.trace.raster;
        a.count(raster.triangles, raster.triangles.size(), 262144, 71);
        for (auto& triangle : raster.triangles) {
            Fields(a, triangle.entry, triangle.rasterizer, triangle.mode, triangle.colour,
                triangle.texture_offset);
            for (auto& vertex : triangle.vertices)
                Fields(a, vertex.x, vertex.y, vertex.u, vertex.v, vertex.s);
        }
        a.count(raster.commands, raster.commands.size(), 327680, 5);
        for (auto& command : raster.commands) Fields(a, command.kind, command.index);
    }
    if (c.format_version >= 5) {
        auto& raster = c.trace.raster;
        a.scalar(raster.excluded_shadow_entries);
        a.count(raster.shadow_masks, raster.shadow_masks.size(), 32768, 8);
        for (auto& mask : raster.shadow_masks) {
            Fields(a, mask.width, mask.height);
            Scalars(a, mask.coverage, 256 * 256);
        }
        a.count(raster.shadows, raster.shadows.size(), 65536, 81);
        for (auto& shadow : raster.shadows) {
            Identity(a, shadow.caster); Fields(a, shadow.entry, shadow.mask, shadow.shade);
            for (auto& vertex : shadow.vertices)
                Fields(a, vertex.x, vertex.y, vertex.u, vertex.v, vertex.s);
        }
    }
    if (c.format_version >= 7) {
        auto& o = c.trace.outline;
        a.scalar(o.present);
        if (o.present) {
            Fields(a, o.complete, o.mode, o.alpha); Array(a, o.palette_transform);
            a.count(o.occluders, o.occluders.size(), 262144, 52);
            for (auto& triangle : o.occluders) {
                a.scalar(triangle.entry);
                for (auto& vertex : triangle.vertices) Fields(a, vertex.x, vertex.y, vertex.z, vertex.w);
            }
            a.count(o.sprites, o.sprites.size(), 65536, 82);
            for (auto& sprite : o.sprites) {
                Fields(a, sprite.draw, sprite.dst_x, sprite.dst_y, sprite.dst_w, sprite.dst_h,
                    sprite.phase_x, sprite.phase_y, sprite.step_x, sprite.step_y,
                    sprite.z_ndc, sprite.outline_z_ndc, sprite.source_alpha, sprite.sort_key,
                    sprite.flags, sprite.owner, sprite.wants_outline, sprite.additive, sprite.darkens,
                    sprite.source_border_covered, sprite.path);
                Array(a, sprite.owner_rgba);
            }
            a.count(o.commands, o.commands.size(), 196608, 9);
            for (auto& command : o.commands) Fields(a, command.kind, command.sprite, command.group);
        }
    }
    if (c.format_version >= 8) {
        auto& t = c.target;
        Fields(a, t.kind, t.clear_index, t.lens_id, t.width, t.height, t.pitch,
            t.window_x, t.window_y, t.window_w, t.window_h);
    }
    if (c.format_version >= 9) {
        auto& o = c.trace.outline;
        a.count(o.colour_rows, o.colour_rows.size(), 16384, 1024);
        for (auto& row : o.colour_rows) Array(a, row);
        a.count(o.colour_bindings, o.colour_bindings.size(), 65536, 5);
        for (auto& binding : o.colour_bindings) Fields(a, binding.lookup_row, binding.resolution);
    }
    if (c.format_version >= 10) {
        auto& w = c.trace.gl_world;
        a.scalar(w.present);
        if (w.present) {
            Fields(a, w.complete, w.target_kind, w.native_width, w.native_height,
                w.viewport_x, w.viewport_y, w.viewport_width, w.viewport_height);
            Array(a, w.clear_rgba); Array(a, w.palette_rgba); Array(a, w.fade);
            Array(a, w.shade_settings); a.scalar(w.time);
            a.count(w.lightmap, w.lightmap.size(), 511 * 511, 2);
            for (auto& value : w.lightmap) a.scalar(value);
            a.count(w.tiles, w.tiles.size(), 65536, 1024);
            for (auto& tile : w.tiles) Array(a, tile);
            a.count(w.terrain, w.terrain.size(), 262144, 64);
            for (auto& triangle : w.terrain) {
                a.scalar(triangle.tile);
                for (auto& vertex : triangle.vertices) Fields(a, vertex.u, vertex.v, vertex.shade, vertex.stl_x, vertex.stl_y);
            }
            a.count(w.shadow_sources, w.shadow_sources.size(), 65536, 8);
            for (auto& source : w.shadow_sources) {
                Fields(a, source.width, source.height);
                a.count(source.coverage, source.coverage.size(), 65536, 1);
                for (auto& value : source.coverage) a.scalar(value);
            }
            a.count(w.shadows, w.shadows.size(), 65536, 92);
            for (auto& shadow : w.shadows) {
                Identity(a, shadow.caster); Fields(a, shadow.entry, shadow.source);
                for (auto& vertex : shadow.vertices) Fields(a, vertex.x, vertex.y, vertex.u, vertex.v);
                Fields(a, shadow.z_ndc, shadow.darken);
            }
        }
    }
    if (c.format_version >= 11) {
        auto& composition = c.trace.gl_composition;
        a.scalar(composition.present);
        if (composition.present) {
            Fields(a, composition.complete, composition.completed_phases);
            Array(a, composition.phase_quad_counts); Array(a, composition.screen_clear_rgba);
            auto& overlay = composition.overlays;
            Fields(a, overlay.native_width, overlay.native_height);
            a.count(overlay.sources, overlay.sources.size(), 256, 16);
            for (auto& source : overlay.sources) {
                Fields(a, source.width, source.height);
                Scalars(a, source.indices, 64 * 1024 * 1024);
                Scalars(a, source.initialized_rows, 16384);
            }
            a.count(overlay.colour_rows, overlay.colour_rows.size(), 16384, 1024);
            for (auto& row : overlay.colour_rows) Array(a, row);
            Scalars(a, overlay.fade, 16384);
            a.count(overlay.draws, overlay.draws.size(), 262144, 81);
            for (auto& quad : overlay.draws) {
                Fields(a, quad.phase, quad.mode, quad.depth_test, quad.scissor_enabled,
                    quad.scissor_x, quad.scissor_y, quad.scissor_w, quad.scissor_h,
                    quad.x0, quad.y0, quad.x1, quad.y1, quad.u0, quad.v0, quad.u1, quad.v1, quad.z_ndc);
                Array(a, quad.colour);
                Fields(a, quad.source, quad.colour_row, quad.fade_row);
            }
            auto& lens = composition.lens;
            Fields(a, lens.requested_active, lens.capture_began, lens.operation, lens.requested_type, lens.fallback_copy);
            for (auto& stamp : lens.resources) Fields(a, stamp.generation, stamp.revision);
            auto& pass = lens.pass;
            Fields(a, pass.kind, pass.source_width, pass.source_height, pass.source_x, pass.source_y,
                pass.target_width, pass.target_height, pass.destination_x, pass.destination_y,
                pass.viewport_width, pass.viewport_height);
            Array(a, pass.mist_offsets); a.scalar(pass.mist_lightness);
            Fields(a, pass.remap_width, pass.remap_height);
            a.count(pass.remap, pass.remap.size(), 16 * 1024 * 1024, 4);
            for (auto& pixel : pass.remap) Array(a, pixel);
            Fields(a, pass.overlay_width, pass.overlay_height, pass.overlay_alpha);
            Scalars(a, pass.overlay, 64 * 1024 * 1024);
            Scalars(a, pass.density, 256 * 256);
            Scalars(a, pass.palette_rgba, 1024);
            Scalars(a, pass.fade, 16384);
            Scalars(a, pass.rgb6_lookup, 512 * 512);
        }
    }
}
}

std::vector<uint8_t> EncodeSceneCapture(const SceneCapture& c)
{
    const char* failure = nullptr;
    if (!ValidateSceneCapture(c, &failure)) throw std::runtime_error(failure);
    Writer payload;
    // The writer never modifies visited fields. Using the same field visitor
    // for both directions prevents read/write schema drift.
    Capture(payload, const_cast<SceneCapture&>(c));
    Require(payload.bytes.size() <= MaxFile - 24);
    Writer file;
    file.scalar(uint32_t(0x4358464b)); // KFXC
    file.scalar(c.format_version); file.scalar(uint32_t(payload.bytes.size()));
    file.scalar(Crc(payload.bytes)); file.scalar(uint64_t(0));
    file.bytes.insert(file.bytes.end(), payload.bytes.begin(), payload.bytes.end());
    return file.bytes;
}
SceneCaptureHandle DecodeSceneCapture(std::span<const uint8_t> bytes)
{
    Require(bytes.size() >= 24 && bytes.size() <= MaxFile);
    Reader header{bytes};
    uint32_t magic, version, size, crc; uint64_t flags;
    Fields(header, magic, version, size, crc, flags);
    // v6 is a semantic extension of the v5 fields: the sprite producer
    // allowlist changes, with no new bytes or relaxed earlier-version rules.
    Require(magic == 0x4358464b && (version >= 1 && version <= 11) && flags == 0 && size == bytes.size() - 24);
    const auto payload = bytes.subspan(24);
    Require(Crc(payload) == crc);
    auto capture = std::make_shared<SceneCapture>();
    capture->format_version = version;
    Reader reader{payload}; Capture(reader, *capture);
    Require(reader.pos == payload.size() && ValidateSceneCapture(*capture));
    return capture;
}
SceneCaptureHandle LoadSceneCapture(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    Require(bool(file));
    const auto size = file.tellg();
    Require(size >= 24 && size <= static_cast<std::streamoff>(MaxFile));
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    file.seekg(0); file.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
    Require(bool(file));
    return DecodeSceneCapture(bytes);
}
void SaveSceneCapture(const std::filesystem::path& path, const SceneCapture& capture)
{
    const auto bytes = EncodeSceneCapture(capture);
    // The caller supplies a unique final path. Publish only a complete file.
    Require(!std::filesystem::exists(path));
    auto temporary = path; temporary += ".tmp";
    Require(!std::filesystem::exists(temporary));
    try {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size()); file.close();
        Require(bool(file));
        std::filesystem::rename(temporary, path);
    } catch (...) {
        std::error_code ec; std::filesystem::remove(temporary, ec); throw;
    }
}
}
