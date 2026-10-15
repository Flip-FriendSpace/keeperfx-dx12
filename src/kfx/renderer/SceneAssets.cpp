// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#include "kfx/renderer/SceneAssets.h"
#include <map>
#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>
#include <tuple>

namespace kfx::render {
namespace {
float Binary32(float value)
{
    // x87 may keep even float expressions in extended precision. Expected
    // wire values must have the same binary32 rounding as the stored fields.
    volatile float stored = value;
    return stored;
}
float X87Depth(int32_t bucket, bool sprite)
{
    // The original x86 producer can keep quotient/subtraction in x87 until
    // its final float store. Explicit binary64 temporaries reproduce that
    // stored result for every bucket 0..4097 at both x87 precision settings.
    // This is a second exact production representation, never an epsilon.
    volatile double quotient = (2.0 * double(bucket) - (sprite ? 1.0 : 0.0)) / 4097.0;
    volatile double depth = quotient - 1.0;
    return Binary32(float(depth));
}
float X87OutlineDepth(float depth)
{
    // GCC's x87 evaluation can also retain the .002f literal at extended
    // precision. Always bias the actual stored source Z, whichever producer
    // representation it has, rather than replacing it with canonical SSE Z.
    volatile double bias = 0.002;
    volatile double result = double(depth) + bias;
    return Binary32(float(result));
}
bool OutlineAxis(int32_t origin, int32_t src, int32_t dst, int32_t window,
    int32_t offset, int32_t count, SpriteScaleAxis& out)
{
    out = {0, 0, 0, 1};
    if (dst <= 0) return true;
    const int64_t step = int64_t(dst) * 65536 / src;
    if (step > INT32_MAX) return false;
    if (step <= 0) return true;
    const int64_t base = step / 2 + int64_t(origin) * 65536 + int64_t(offset) * step;
    int64_t start = base >> 16;
    const int64_t end = (base + int64_t(count) * step) >> 16;
    if (!offset && (origin < 0 || int64_t(dst) + src + origin >= window))
        start = std::min(start, int64_t(std::max(origin, 0)));
    if (end <= start) return true;
    const int64_t phase = base - start * 65536;
    if (start < INT32_MIN || start > INT32_MAX || end - start > INT32_MAX
        || phase < INT32_MIN || phase > INT32_MAX) return false;
    out = {int32_t(start), int32_t(end - start), int32_t(phase), int32_t(step)};
    return true;
}

bool ValidateOutline(const SceneCapture& c)
{
    const auto& o = c.trace.outline;
    if (c.format_version < 9 && (!o.colour_rows.empty() || !o.colour_bindings.empty())) return false;
    if (c.format_version >= 9 && (c.trace.backend != 1 || !o.present)) return false;
    if (!o.present) {
        return !o.complete && !o.mode && o.alpha == 0 && o.occluders.empty() && o.sprites.empty()
            && o.commands.empty() && std::all_of(o.palette_transform.begin(), o.palette_transform.end(),
                [](float v) { return v == 0; }) && !(c.format_version >= 7 && c.trace.backend == 1);
    }
    if ((c.format_version != 7 && c.format_version < 9) || c.trace.backend != 1 || !o.complete || o.mode > 2
        || !std::isfinite(o.alpha) || o.alpha < 0 || o.alpha > 1
        || o.occluders.size() > 262144 || o.sprites.size() > 65536 || o.commands.size() > 196608) return false;
    for (float value : o.palette_transform) if (!std::isfinite(value) || std::abs(value) > 1024) return false;
    const auto& raster = c.trace.raster;
    if (!raster.triangles.empty() || !raster.commands.empty() || !raster.shadow_masks.empty()
        || !raster.shadows.empty() || raster.excluded_shadow_entries) return false;
    std::vector<uint32_t> triangles(c.trace.entries.size());
    uint32_t previous_entry = 0;
    for (const auto& triangle : o.occluders) {
        if (triangle.entry >= c.trace.entries.size() || triangle.entry < previous_entry) return false;
        previous_entry = triangle.entry;
        const auto& entry = c.trace.entries[triangle.entry];
        if (entry.kind != 0 && entry.kind != 1 && entry.kind != 6) return false;
        const float z = Binary32(Binary32(2.0f * float(entry.bucket) / 4097.0f) - 1.0f);
        const float x87_z = X87Depth(entry.bucket, false);
        for (const auto& v : triangle.vertices)
            if (!std::isfinite(v.x) || !std::isfinite(v.y) || std::abs(v.x) > 33554432
                || std::abs(v.y) > 33554432 || (v.z != z && v.z != x87_z) || !std::isfinite(v.w)
                || v.w < 1 || v.w > 2147483648.0f || std::trunc(v.w) != v.w) return false;
        ++triangles[triangle.entry];
    }
    for (size_t i = 0; i < triangles.size(); ++i) {
        const auto kind = c.trace.entries[i].kind;
        if (((kind == 0 || kind == 1) && triangles[i] != 1) || (kind == 6 && triangles[i] != 2)) return false;
    }
    std::map<uint32_t, const SceneObject*> objects;
    for (const auto& object : c.scene.objects) objects.emplace(object.id.slot, &object);
    std::vector<uint8_t> seen_draw(c.trace.draws.size()), outlines(o.sprites.size()), mains(o.sprites.size()), darkens(o.sprites.size());
    std::vector<uint32_t> sprite_groups(o.sprites.size(), UINT32_MAX);
    uint32_t previous_sprite_draw = 0;
    for (const auto& sprite : o.sprites) {
        if (sprite.draw >= c.trace.draws.size() || sprite.draw < previous_sprite_draw || seen_draw[sprite.draw]++
            || sprite.path > SceneOutlineExecutionPath::TextureFallback) return false;
        previous_sprite_draw = sprite.draw;
        const auto& draw = c.trace.draws[sprite.draw];
        const auto& source = c.assets.sprites[draw.sprite];
        const auto& scale = draw.scale;
        SpriteScaleAxis x, y;
        if (source.width > 1024 || source.height > 1024
            || !OutlineAxis(scale.frame_x, scale.frame_src_w, scale.frame_dst_w, scale.window_w, scale.content_x, source.width, x)
            || !OutlineAxis(scale.frame_y, scale.frame_src_h, scale.frame_dst_h, scale.window_h, scale.content_y, draw.content_height, y)
            || x.dst_len <= 0 || y.dst_len <= 0 || sprite.dst_x != x.dst_start || sprite.dst_y != y.dst_start
            || sprite.dst_w != x.dst_len || sprite.dst_h != y.dst_len || sprite.phase_x != x.phase || sprite.phase_y != y.phase
            || sprite.step_x != x.step || sprite.step_y != y.step || sprite.flags != draw.flags) return false;
        const auto bucket = c.trace.entries[draw.entry].bucket;
        const float z = Binary32(Binary32(2.0f * (float(bucket) - 0.5f) / 4097.0f) - 1.0f);
        const float x87_z = X87Depth(bucket, true);
        const float outline_z = Binary32(sprite.z_ndc + Binary32(0.002f));
        const float x87_outline_z = X87OutlineDepth(sprite.z_ndc);
        const float alpha = Binary32((draw.flags & 0x8000) ? 0.75f : (draw.flags & 4) ? 1.0f / 3.0f : (draw.flags & 8) ? 2.0f / 3.0f : 1.0f);
        if ((sprite.z_ndc != z && sprite.z_ndc != x87_z)
            || (sprite.outline_z_ndc != outline_z && sprite.outline_z_ndc != x87_outline_z) || sprite.source_alpha != alpha
            || sprite.sort_key >> 32 != uint32_t(bucket) || sprite.additive != bool(draw.flags & 0x4000)
            || (sprite.darkens && !sprite.additive)
            || sprite.owner != int8_t(objects.at(draw.object.slot)->owner)
            || (sprite.wants_outline && c.scene.camera.view == SceneView::Possession)) return false;
        bool border = false;
        for (unsigned sy = 0; sy < source.height; ++sy)
            border |= source.pixels[(size_t(sy) * source.width + source.width - 1) * 2 + 1] != 0;
        for (unsigned sx = 0; sx < source.width; ++sx)
            border |= source.pixels[(size_t(source.height - 1) * source.width + sx) * 2 + 1] != 0;
        if (border != sprite.source_border_covered) return false;
        const bool outlined = o.mode && sprite.wants_outline && !sprite.additive;
        for (float value : sprite.owner_rgba) if (!std::isfinite(value) || value < 0 || value > 1 || (!outlined && value != 0)) return false;
        if (outlined && sprite.owner_rgba[3] != o.alpha) return false;
        if (sprite.additive && sprite.path != SceneOutlineExecutionPath::Instanced && !sprite.darkens) return false;
        if (sprite.additive && sprite.path == SceneOutlineExecutionPath::Instanced) {
            bool black_glow = false;
            for (size_t i = 0; i < source.pixels.size(); i += 2)
                black_glow |= source.pixels[i + 1] && source.pixels[i] >= 50 && source.pixels[i] <= 56;
            if (sprite.darkens != black_glow) return false;
        }
    }
    if (c.format_version >= 9) {
        if (o.colour_rows.size() > 16384 || o.colour_bindings.size() != o.sprites.size()) return false;
        std::set<std::array<uint8_t, 1024>> unique_rows;
        for (const auto& row : o.colour_rows) if (!unique_rows.insert(row).second) return false;
        size_t used_rows = 0;
        for (size_t i = 0; i < o.sprites.size(); ++i) {
            const auto& sprite = o.sprites[i]; const auto& binding = o.colour_bindings[i];
            const auto& draw = c.trace.draws[sprite.draw];
            const bool remap = draw.has_remap && (draw.flags & 0x0800);
            const auto resolution = binding.resolution;
            if (sprite.additive) {
                if (resolution != SceneOutlineColourResolution::Glow || binding.lookup_row != UINT32_MAX) return false;
                continue;
            }
            if (binding.lookup_row >= o.colour_rows.size() || binding.lookup_row > used_rows) return false;
            if (binding.lookup_row == used_rows) ++used_rows;
            if (sprite.path == SceneOutlineExecutionPath::TextureFallback) {
                if (resolution != (remap ? SceneOutlineColourResolution::TextureRemap : SceneOutlineColourResolution::TextureIdentity)) return false;
            } else if (remap) {
                if (resolution != SceneOutlineColourResolution::ClutRemap && resolution != SceneOutlineColourResolution::ClutCapacityIdentity) return false;
            } else if (resolution != SceneOutlineColourResolution::ClutIdentity) return false;
            const auto& row = o.colour_rows[binding.lookup_row];
            bool opaque = true;
            for (size_t j = 3; j < row.size(); j += 4) opaque &= row[j] == 255;
            const bool identity = resolution == SceneOutlineColourResolution::ClutIdentity
                || resolution == SceneOutlineColourResolution::ClutCapacityIdentity;
            if (!opaque && !(identity && std::all_of(row.begin(), row.end(), [](uint8_t b) { return b == 0; }))) return false;
        }
        if (used_rows != o.colour_rows.size()) return false;
    }
    // Every admitted GL draw with a nonempty mapping must occur exactly once.
    // Invalid oversized source uploads are rejected, not silently omitted.
    for (size_t i = 0; i < seen_draw.size(); ++i) {
        const auto& draw = c.trace.draws[i]; const auto& scale = draw.scale;
        const auto& source = c.assets.sprites[draw.sprite]; SpriteScaleAxis x, y;
        if (source.width > 1024 || source.height > 1024
            || !OutlineAxis(scale.frame_x, scale.frame_src_w, scale.frame_dst_w, scale.window_w, scale.content_x, source.width, x)
            || !OutlineAxis(scale.frame_y, scale.frame_src_h, scale.frame_dst_h, scale.window_h, scale.content_y, draw.content_height, y)
            || bool(seen_draw[i]) != (x.dst_len > 0 && y.dst_len > 0)) return false;
    }
    uint32_t group = 0, group_sprite = 0;
    SceneOutlineExecutionPath path = SceneOutlineExecutionPath::Instanced;
    bool first = true, main_phase = false;
    uint32_t previous_main_draw = 0, previous_outline_draw = 0;
    for (size_t i = 0; i < o.commands.size(); ++i) {
        const auto& command = o.commands[i];
        if (command.sprite >= o.sprites.size() || command.kind > SceneOutlineCommandKind::GlowDarken) return false;
        const auto& sprite = o.sprites[command.sprite];
        if (sprite_groups[command.sprite] != UINT32_MAX && sprite_groups[command.sprite] != command.group) return false;
        sprite_groups[command.sprite] = command.group;
        if (first || command.group != group) {
            if (!first && command.group <= group) return false;
            group = command.group; path = sprite.path; group_sprite = command.sprite; main_phase = false; first = false;
        }
        if (path != sprite.path || (path != SceneOutlineExecutionPath::Instanced && command.sprite != group_sprite)) return false;
        if (command.kind == SceneOutlineCommandKind::Outline) {
            if (main_phase || outlines[command.sprite]++ || !o.mode || !sprite.wants_outline || sprite.additive
                || sprite.draw < previous_outline_draw) return false;
            previous_outline_draw = sprite.draw;
        } else if (command.kind == SceneOutlineCommandKind::Sprite) {
            if (mains[command.sprite]++ || sprite.draw < previous_main_draw) return false;
            previous_main_draw = sprite.draw; main_phase = true;
        } else {
            if (darkens[command.sprite]++ || !sprite.darkens || !i
                || o.commands[i - 1].kind != SceneOutlineCommandKind::Sprite
                || o.commands[i - 1].sprite != command.sprite || o.commands[i - 1].group != group) return false;
        }
    }
    for (size_t i = 0; i < o.sprites.size(); ++i) {
        const auto& sprite = o.sprites[i];
        if (mains[i] != 1 || bool(outlines[i]) != (o.mode && sprite.wants_outline && !sprite.additive)
            || bool(darkens[i]) != sprite.darkens) return false;
    }
    return true;
}
uint64_t CompositionResourceBytes(const SceneGLCompositionTrace& composition)
{
    const auto& overlay = composition.overlays;
    uint64_t bytes = uint64_t(overlay.colour_rows.size()) * 1024 + overlay.fade.size();
    for (const auto& source : overlay.sources) bytes += source.indices.size() + source.initialized_rows.size();
    const auto& pass = composition.lens.pass;
    return bytes + uint64_t(pass.remap.size()) * 4 + pass.overlay.size() + pass.density.size()
        + pass.palette_rgba.size() + pass.fade.size() + pass.rgb6_lookup.size();
}
bool ValidateGLComposition(const SceneCapture& c, const char** failure)
{
    const auto reject = [failure](const char* reason) { if (failure) *failure = reason; return false; };
    const auto& composition = c.trace.gl_composition;
    if (c.format_version < 11) return composition == SceneGLCompositionTrace{};
    const auto& world = c.trace.gl_world;
    const auto& overlay = composition.overlays;
    if (!composition.present || !composition.complete || composition.completed_phases != 15
        || c.trace.backend != 1 || !world.present || !world.complete
        || overlay.native_width != world.native_width || overlay.native_height != world.native_height)
        return reject("Invalid GL composition completion, backend or native extent");
    for (float channel : composition.screen_clear_rgba)
        if (!std::isfinite(channel) || channel < 0 || channel > 1) return reject("Invalid GL composition screen clear channel");
    if (composition.screen_clear_rgba[3] != 1
        || (world.target_kind == SceneGLWorldTargetKind::Screen && composition.screen_clear_rgba != world.clear_rgba))
        return reject("GL composition screen/world clear mismatch");
    try { ValidateSceneGLOverlayPass(overlay); }
    catch (const std::invalid_argument&) { return reject("Invalid GL composition overlay pass"); }
    std::array<uint32_t, 3> counts{};
    size_t used_sources = 0, used_rows = 0;
    for (const auto& quad : overlay.draws) {
        ++counts[size_t(quad.phase)];
        if (quad.source != NoSceneGLOverlayResource) {
            if (quad.source > used_sources) return reject("GL overlay source first-use order mismatch");
            if (quad.source == used_sources) ++used_sources;
        }
        if (quad.colour_row != NoSceneGLOverlayResource) {
            if (quad.colour_row > used_rows) return reject("GL overlay colour-row first-use order mismatch");
            if (quad.colour_row == used_rows) ++used_rows;
        }
    }
    if (counts != composition.phase_quad_counts || used_sources != overlay.sources.size()
        || used_rows != overlay.colour_rows.size()) return reject("GL overlay phase counts or resource usage mismatch");
    std::set<std::array<uint8_t, 1024>> rows;
    for (const auto& row : overlay.colour_rows) if (!rows.insert(row).second) return reject("Duplicate GL overlay colour row");
    for (size_t i = 0; i < overlay.sources.size(); ++i)
        for (size_t j = 0; j < i; ++j) if (overlay.sources[i] == overlay.sources[j]) return reject("Duplicate GL overlay source");

    const auto& lens = composition.lens;
    if (lens.operation > SceneGLLensOperation::NotDrawn || lens.requested_type > 4
        || lens.capture_began != (world.target_kind == SceneGLWorldTargetKind::LensSource))
        return reject("GL lens operation or capture target mismatch");
    if (!lens.requested_active) return lens == SceneGLLensTrace{} || reject("Inactive GL lens has nonzero state");
    if (lens.operation == SceneGLLensOperation::None) return reject("Active GL lens has no resolved operation");
    if (lens.operation == SceneGLLensOperation::NotDrawn) {
        return !lens.fallback_copy && lens.pass == SceneGLLensPass{}
            && lens.resources == std::array<SceneGLUploadStamp, 6>{};
    }
    if (!lens.capture_began || uint8_t(lens.pass.kind) != uint8_t(lens.operation) - 1
        || lens.fallback_copy != (lens.operation == SceneGLLensOperation::Copy && lens.requested_type != 0)) return false;
    if ((lens.operation == SceneGLLensOperation::Remap && lens.requested_type != 2 && lens.requested_type != 3)
        || (lens.operation == SceneGLLensOperation::Mist && lens.requested_type != 1)
        || (lens.operation == SceneGLLensOperation::Overlay && lens.requested_type != 4)) return false;
    const auto& pass = lens.pass;
    try { ValidateSceneGLLensPass(pass); } catch (const std::invalid_argument&) { return false; }
    if (pass.source_width != world.native_width || pass.source_height != world.native_height
        || pass.target_width != world.native_width || pass.target_height != world.native_height
        || pass.source_x != pass.destination_x) return false;
    const bool mist = lens.operation == SceneGLLensOperation::Mist;
    const bool indexed_overlay = lens.operation == SceneGLLensOperation::Overlay;
    const std::array<bool, 6> consumed{mist || indexed_overlay, mist, mist || indexed_overlay,
        mist, lens.operation == SceneGLLensOperation::Remap, indexed_overlay};
    for (size_t i = 0; i < consumed.size(); ++i) {
        const auto& stamp = lens.resources[i];
        if (consumed[i] ? !stamp.generation || !stamp.revision : stamp != SceneGLUploadStamp{}) return false;
    }
    return true;
}
bool ValidateGLWorld(const SceneCapture& c, const char** failure)
{
    const auto reject = [failure](const char* reason) { if (failure) *failure = reason; return false; };
    const auto& w = c.trace.gl_world;
    if (c.format_version < 10) return w == SceneGLWorldTrace{};
    if (!w.present || !w.complete || c.trace.backend != 1 || !c.trace.outline.complete
        || w.target_kind > SceneGLWorldTargetKind::LensSource
        || !w.native_width || !w.native_height || w.native_width > 16384 || w.native_height > 16384
        || uint64_t(w.native_width) * w.native_height * 4 > 64 * 1024 * 1024
        || w.viewport_x < 0 || w.viewport_y < 0 || w.viewport_width <= 0 || w.viewport_height <= 0
        || int64_t(w.viewport_x) + w.viewport_width > w.native_width
        || int64_t(w.viewport_y) + w.viewport_height > w.native_height
        || w.viewport_x != c.scene.camera.viewport_x || w.viewport_y != c.scene.camera.viewport_y
        || w.viewport_width != c.scene.camera.viewport_w || w.viewport_height != c.scene.camera.viewport_h
        || (w.target_kind == SceneGLWorldTargetKind::LensSource && c.scene.camera.view != SceneView::Possession)
        || w.lightmap.size() != 511 * 511 || w.tiles.size() > 65536 || w.shadow_sources.size() > 65536
        || w.shadows.size() > 65536 || w.terrain.size() != c.trace.outline.occluders.size()
        || w.time != 0) return reject("Invalid GL world extent, target, completion or table length");
    for (float v : w.clear_rgba) if (!std::isfinite(v) || v < 0 || v > 1) return reject("Invalid GL world clear channel");
    if (w.clear_rgba[3] != 1) return reject("Invalid GL world clear opacity");
    for (size_t i = 3; i < w.palette_rgba.size(); i += 4) if (w.palette_rgba[i] != 255) return reject("Invalid GL world palette opacity");
    const auto& s = w.shade_settings;
    for (float v : s) if (!std::isfinite(v)) return reject("Nonfinite GL world shading setting");
    if ((s[0] != 0 && s[0] != 1) || s[1] < 0 || s[1] > 1
        || s[2] < Binary32(0.1f) || s[2] > 3 || s[3] < Binary32(0.1f) || s[3] > 3
        || (s[4] != 0 && s[4] != 1) || (s[5] != 0 && s[5] != 1 && s[5] != 2)
        || s[6] < Binary32(0.1f) || s[6] > 5 || s[7] < 0 || s[7] > 1
        || (s[8] != 0 && s[8] != 1)) return reject("Invalid GL world shading setting range");
    uint64_t bytes = c.assets.texture_pixels.size() + c.assets.additive.size()
        + sizeof(c.assets.base_palette) + sizeof(c.assets.display_palette) + sizeof(c.assets.fade) + sizeof(c.assets.ghost)
        + uint64_t(c.trace.outline.colour_rows.size()) * 1024 + sizeof(w.palette_rgba) + sizeof(w.fade)
        + uint64_t(w.lightmap.size()) * 2 + uint64_t(w.tiles.size()) * 1024;
    for (const auto& source : c.assets.sprites) bytes += source.pixels.size();
    std::set<std::array<uint8_t, 1024>> unique_tiles;
    for (const auto& tile : w.tiles) if (!unique_tiles.insert(tile).second) return reject("Duplicate GL world tile");
    size_t used_tiles = 0;
    for (const auto& triangle : w.terrain) {
        if (triangle.tile != SceneGLMissingTile) {
            if (triangle.tile >= w.tiles.size() || triangle.tile > used_tiles) return reject("GL world tile reference or first-use order mismatch");
            if (triangle.tile == used_tiles) ++used_tiles;
        }
        for (const auto& v : triangle.vertices) {
            if (!std::isfinite(v.u) || !std::isfinite(v.v) || std::abs(v.u) > 32768 || std::abs(v.v) > 32768
                || !std::isfinite(v.shade) || std::abs(v.shade) > 1024
                || !std::isfinite(v.stl_x) || !std::isfinite(v.stl_y)
                || v.stl_x < -1 || v.stl_y < -1 || v.stl_x > 511 || v.stl_y > 511)
                return reject("Invalid GL terrain UV, shade or lightmap coordinate");
        }
    }
    if (used_tiles != w.tiles.size()) return reject("Unused GL world tile");
    std::set<std::tuple<uint16_t, uint16_t, std::vector<uint8_t>>> unique_sources;
    for (const auto& source : w.shadow_sources) {
        if (!source.width || !source.height || source.width > 512 || source.height > 512
            || source.coverage.size() != size_t(source.width) * source.height
            || !unique_sources.emplace(source.width, source.height, source.coverage).second)
            return reject("Invalid or duplicate GL shadow source");
        for (uint8_t value : source.coverage) if (value != 0 && value != 255) return reject("Nonbinary GL shadow coverage");
        bytes += source.coverage.size();
    }
    if (bytes > 64 * 1024 * 1024) return reject("GL world resource budget exceeded");
    const uint64_t aggregate_limit = c.format_version >= 11
        ? SceneGLCompositionOwnedByteLimit : 64 * 1024 * 1024;
    if (CompositionResourceBytes(c.trace.gl_composition) > aggregate_limit - bytes) return reject("GL composition aggregate resource budget exceeded");
    std::map<uint32_t, const SceneObject*> objects;
    for (const auto& object : c.scene.objects) objects.emplace(object.id.slot, &object);
    std::vector<uint8_t> seen(c.trace.entries.size());
    uint32_t previous_entry = 0;
    size_t used_sources = 0;
    for (const auto& shadow : w.shadows) {
        if (shadow.entry >= seen.size() || shadow.entry < previous_entry || seen[shadow.entry]++
            || c.trace.entries[shadow.entry].kind != 3 || shadow.source >= w.shadow_sources.size()
            || shadow.source > used_sources) return reject("GL shadow entry or source order mismatch");
        previous_entry = shadow.entry;
        if (shadow.source == used_sources) ++used_sources;
        const auto object = objects.find(shadow.caster.slot);
        if (object == objects.end() || object->second->id != shadow.caster
            || object->second->class_id != 5 || object->second->draw_class != 2) return reject("Invalid GL shadow caster identity");
        const auto bucket = c.trace.entries[shadow.entry].bucket;
        const float z = Binary32(Binary32(2.0f * float(bucket) / 4097.0f) - 1.0f);
        if ((shadow.z_ndc != z && shadow.z_ndc != X87Depth(bucket, false))
            || !std::isfinite(shadow.darken) || shadow.darken < Binary32(0.2f) || shadow.darken > Binary32(0.55f))
            return reject("Invalid GL shadow depth or darkness");
        const auto& source = w.shadow_sources[shadow.source];
        for (const auto& v : shadow.vertices)
            if (!std::isfinite(v.x) || !std::isfinite(v.y) || std::abs(v.x) > 16777216 || std::abs(v.y) > 16777216
                || !std::isfinite(v.u) || !std::isfinite(v.v) || v.u < 0 || v.v < 0
                || v.u > float(source.width - 1) / 256 || v.v > float(source.height - 1) / 256)
                return reject("Invalid GL shadow position or source coordinate");
    }
    if (used_sources != w.shadow_sources.size()) return reject("Unused GL shadow source");
    for (size_t i = 0; i < seen.size(); ++i) if ((c.trace.entries[i].kind == 3) != bool(seen[i]))
        return reject("GL shadow bucket has no executed shadow quad");
    return true;
}
}

SceneSpriteBlend ResolveSceneSpriteBlend(uint32_t flags, bool alpha)
{
    if (alpha) return SceneSpriteBlend::Additive;
    if (flags & 0x0800) return SceneSpriteBlend::Remap;
    if (flags & 0x0004) return SceneSpriteBlend::GhostSourceHigh;
    if (flags & 0x0008) return SceneSpriteBlend::GhostDestinationHigh;
    return SceneSpriteBlend::Opaque;
}

bool DecodeSceneSprite(std::span<const uint8_t> src, uint16_t w, uint16_t h, std::vector<uint8_t>& pixels)
{
    if (!w || !h || w > 1024 || h > 1024) return false;
    std::vector<uint8_t> decoded(size_t(w) * h * 2);
    size_t p = 0;
    for (unsigned y = 0; y < h; ++y) {
        unsigned x = 0;
        for (;;) {
            if (p == src.size()) return false;
            const int cmd = src[p] < 128 ? src[p] : int(src[p]) - 256;
            ++p;
            if (!cmd) break; // Short rows have transparent tails.
            const unsigned count = cmd < 0 ? -cmd : cmd;
            if (count > w - x) return false;
            if (cmd > 0) {
                if (count > src.size() - p) return false;
                for (unsigned i = 0; i < count; ++i) {
                    decoded[(size_t(y) * w + x + i) * 2] = src[p++];
                    decoded[(size_t(y) * w + x + i) * 2 + 1] = 255;
                }
            }
            x += count;
        }
    }
    pixels = std::move(decoded);
    return true;
}

bool ValidateSceneCapture(const SceneCapture& c, const char** failure)
{
    if (failure) *failure = "Invalid scene assets, projection or raster trace";
    if (c.format_version < 1 || c.format_version > 11) return false;
    if (c.format_version != 8) {
        if (c.target != SceneIndexedTarget{}) return false;
    } else {
        const auto& t = c.target;
        const auto& camera = c.scene.camera;
        if (c.trace.backend != 0 || !c.trace.complete || c.trace.outline.present
            || (t.kind != SceneIndexedTargetKind::Screen && t.kind != SceneIndexedTargetKind::LensSource)
            || !t.width || !t.height || t.width > 16384 || t.height > 16384
            || t.pitch < t.width || t.pitch > 16384 || uint64_t(t.pitch) * t.height > 64 * 1024 * 1024
            || t.window_x < 0 || t.window_y < 0 || t.window_w <= 0 || t.window_h <= 0
            || int64_t(t.window_x) + t.window_w > t.width || int64_t(t.window_y) + t.window_h > t.height
            || t.window_x != camera.viewport_x || t.window_y != camera.viewport_y
            || t.window_w != camera.viewport_w || t.window_h != camera.viewport_h) return false;
        if (t.kind == SceneIndexedTargetKind::Screen) {
            if (t.lens_id != 0) return false;
        } else if (t.clear_index != 0 || t.pitch != t.width || t.lens_id == 0 || t.lens_id > 255
            || camera.view != SceneView::Possession) return false;
    }
    if (c.format_version == 1 && (!c.assets.presentation.empty() || !c.assets.projection_reference.empty())) return false;
    if (c.format_version < 3 && (!c.assets.additive.empty() || c.trace.complete || c.trace.backend
        || c.trace.excluded_sprite_calls || !c.trace.entries.empty() || !c.trace.draws.empty())) return false;
    if (c.format_version < 4 && (!c.trace.raster.triangles.empty() || !c.trace.raster.commands.empty())) return false;
    if (c.format_version < 5 && (c.trace.raster.excluded_shadow_entries
        || !c.trace.raster.shadow_masks.empty() || !c.trace.raster.shadows.empty())) return false;
    if (c.format_version >= 3 && c.assets.presentation.size() != c.scene.objects.size()) return false;
    if (c.assets.projection_reference.size() > 4096) return false;
    if (!c.assets.presentation.empty()) {
        if (c.assets.presentation.size() != c.scene.objects.size()) return false;
        std::map<uint32_t, SceneObjectId> objects;
        for (const auto& o : c.scene.objects) objects.emplace(o.id.slot, o.id);
        for (const auto& p : c.assets.presentation) {
            auto found = objects.find(p.id.slot);
            if (found == objects.end() || found->second != p.id || p.lightness > 16639 || p.angle_offset < 0 || p.angle_offset > 1792) return false;
            objects.erase(found);
        }
    }
    const auto& s = c.scene;
    const auto& a = c.assets;
    if (!ValidateWorldScene(s) || s.columns.empty() || s.camera.view > SceneView::Possession
        || !std::isfinite(s.interpolation) || !std::isfinite(s.lava_scroll) || !std::isfinite(s.water_scroll)
        || s.texture_dimension != 32 || s.texture_row_stride < 32 || s.texture_row_stride > 32768
        || s.texture_frames.size() != 32 * 1544 || a.texture_pixels.empty()
        || a.texture_pixels.size() > 64 * 1024 * 1024) return false;
    for (auto offset : s.texture_frames) {
        if (offset < -1) return false;
        if (offset >= 0 && uint64_t(offset) + uint64_t(s.texture_row_stride) * 31 + 32 > a.texture_pixels.size()) return false;
    }
    for (auto p : a.base_palette) if (p > 63) return false;
    for (auto p : a.display_palette) if (p > 63) return false;
    for (const auto& m : s.cube_materials) for (auto t : m.textures) if (t >= 1544) return false;
    for (auto t : s.ceiling_textures) if (t >= 1544) return false;
    for (auto cube : s.top_cubes) if (cube < -1 || cube >= int(s.cube_materials.size())) return false;
    for (const auto& col : s.columns) {
        const size_t base = size_t(col.texture_variation) * 1544;
        if (col.floor_texture >= 1544 || s.texture_frames[base + col.floor_texture] < 0) return false;
        for (auto cube : col.cubes) for (auto t : s.cube_materials[cube].textures)
            if (s.texture_frames[base + t] < 0) return false;
    }
    std::set<uint32_t> ids;
    size_t bytes = 0;
    for (const auto& sprite : a.sprites) {
        if (!ids.insert(sprite.id).second
            || sprite.width > 1024 || sprite.height > 1024
            || sprite.pixels.size() != size_t(sprite.width) * sprite.height * 2) return false;
        bytes += sprite.pixels.size();
        if (bytes > 64 * 1024 * 1024) return false;
        for (size_t i = 1; i < sprite.pixels.size(); i += 2)
            if (sprite.pixels[i] != 0 && sprite.pixels[i] != 255) return false;
    }
    std::set<std::tuple<uint16_t, uint8_t, uint8_t>> bindings;
    for (const auto& b : a.bindings) {
        if (b.direction >= 8 || b.sprite >= a.sprites.size()
            || !bindings.emplace(b.animation, b.frame, b.direction).second) return false;
    }
    for (const auto& o : s.objects) for (uint8_t d = 0; d < 8; ++d)
        if (!bindings.contains({o.animation, o.frame, d})) return false;
    if (c.format_version >= 3) {
        const auto& trace = c.trace;
        if (a.additive.size() != 65536 || !trace.complete || trace.backend > 1
            || trace.entries.size() > 262144 || trace.draws.size() > 65536) return false;
        int32_t previous_bucket = 4097;
        for (const auto& entry : trace.entries) {
            if (entry.bucket < 0 || entry.bucket > previous_bucket) return false;
            previous_bucket = entry.bucket;
        }
        std::map<uint32_t, const SceneObject*> objects;
        for (const auto& o : s.objects) objects.emplace(o.id.slot, &o);
        uint32_t previous_entry = 0;
        for (const auto& draw : trace.draws) {
            if (draw.entry >= trace.entries.size() || draw.entry < previous_entry || draw.sprite >= a.sprites.size()) return false;
            previous_entry = draw.entry;
            auto found = objects.find(draw.object.slot);
            if (found == objects.end() || found->second->id != draw.object) return false;
            const auto kind = trace.entries[draw.entry].kind;
            const auto draw_class = found->second->draw_class;
            const bool ordinary = draw_class == 2 && kind == SceneOrdinarySpriteKind;
            const bool spinning_key = c.format_version >= 6 && draw_class == 6 && kind == SceneSpinningKeySpriteKind;
            if (!ordinary && !spinning_key) return false;
            // These are the two actual key dispatch paths. Possession can
            // retain an empty key bucket, but cannot submit a key sprite.
            if (spinning_key && !((s.camera.view == SceneView::Overhead
                    && (s.camera.view_mode == 2 || s.camera.view_mode == 8))
                || (s.camera.view == SceneView::Front && s.camera.view_mode == 5))) return false;
            const auto& sprite = a.sprites[draw.sprite];
            const auto& scale = draw.scale;
            if (!sprite.width || !sprite.height || draw.content_height <= 0 || draw.content_height > sprite.height
                || scale.frame_src_w <= 0 || scale.frame_src_w > 65535 || scale.frame_src_h <= 0 || scale.frame_src_h > 65535
                || scale.frame_dst_w < 0 || scale.frame_dst_w > 16777216 || scale.frame_dst_h < 0 || scale.frame_dst_h > 16777216
                || scale.window_w <= 0 || scale.window_w > 16384 || scale.window_h <= 0 || scale.window_h > 16384
                || scale.frame_x < -16777216 || scale.frame_x > 16777216 || scale.frame_y < -16777216 || scale.frame_y > 16777216
                || scale.content_x < -65535 || scale.content_x > 65535 || scale.content_y < -65535 || scale.content_y > 65535
                || draw.has_remap != bool(draw.flags & 0x0800)
                || draw.blend != ResolveSceneSpriteBlend(draw.flags, draw.alpha)) return false;
            if (!draw.has_remap && std::any_of(draw.remap.begin(), draw.remap.end(), [](uint8_t x) { return x != 0; })) return false;
        }
    }
    if (c.format_version >= 4 && !(c.format_version >= 7 && c.trace.backend == 1)) {
        const auto& trace = c.trace;
        const auto& raster = trace.raster;
        if (trace.backend != 0 || s.texture_row_stride != 256
            || raster.triangles.size() > 262144 || raster.commands.size() > 327680
            || raster.commands.size() != raster.triangles.size() + trace.draws.size() + raster.shadows.size()) return false;
        std::set<uint32_t> textures;
        for (auto offset : s.texture_frames) if (offset >= 0) textures.insert(uint32_t(offset));
        std::vector<uint32_t> counts(trace.entries.size());
        uint32_t previous_entry = 0;
        for (const auto& triangle : raster.triangles) {
            if (triangle.entry >= trace.entries.size() || triangle.entry < previous_entry) return false;
            previous_entry = triangle.entry;
            const auto kind = trace.entries[triangle.entry].kind;
            if (kind != 0 && kind != 1 && kind != 6) return false;
            size_t texture_span;
            if (triangle.rasterizer == SceneRasterizer::Gpoly && triangle.mode == 5) {
                texture_span = 31 * 256 + 32;
                if (triangle.colour != 0) return false; // This mode ignores vec_colour.
            } else if (triangle.rasterizer == SceneRasterizer::Trig && triangle.mode == 7 && kind == 1) {
                // Colour 32 dispatches mode 2, which also retains 8-bit V.
                texture_span = triangle.colour == 32 ? 256 * 256 : 32 * 256;
                if (triangle.colour >= 64) return false;
            } else return false;
            if (!textures.contains(triangle.texture_offset)
                || uint64_t(triangle.texture_offset) + texture_span > a.texture_pixels.size()) return false;
            for (const auto& vertex : triangle.vertices) {
                // Keep projected inputs, including degenerate/offscreen triangles;
                // consumers separately enforce their rasterizer's safe arithmetic.
                if (vertex.x < -16777216 || vertex.x > 16777216
                    || vertex.y < -16777216 || vertex.y > 16777216) return false;
                if (triangle.rasterizer == SceneRasterizer::Trig) {
                    if (vertex.s != 0) return false; // Never capture uninitialized mode-7 S.
                } else if (vertex.s < 0 || vertex.s > 64 * 65536 - 1) return false;
            }
            ++counts[triangle.entry];
        }
        for (size_t i = 0; i < trace.entries.size(); ++i) {
            const auto kind = trace.entries[i].kind;
            const auto count = counts[i];
            if (kind == 0 && count != 1) return false;
            if (kind == 6 && count != 2) return false;
            if (kind == 1 && count != 1 && count != 2 && count != 4 && count != 6 && count != 16) return false;
        }
        if (c.format_version >= 5) {
            if (raster.shadow_masks.size() > 32768 || raster.shadows.size() > 65536
                || raster.shadows.size() != raster.shadow_masks.size() * 2) return false;
            for (const auto& mask : raster.shadow_masks) {
                if (!mask.width || !mask.height || mask.width > 256 || mask.height > 256
                    || mask.coverage.size() != size_t(mask.width) * mask.height) return false;
                bytes += mask.coverage.size();
                if (bytes > 64 * 1024 * 1024 || std::any_of(mask.coverage.begin(), mask.coverage.end(),
                    [](uint8_t value) { return value != 0 && value != 255; })) return false;
            }
            std::map<uint32_t, const SceneObject*> casters;
            for (const auto& object : s.objects) casters.emplace(object.id.slot, &object);
            const auto same_vertex = [](const SceneRasterVertex& left, const SceneRasterVertex& right) {
                return left.x == right.x && left.y == right.y && left.u == right.u && left.v == right.v && left.s == right.s;
            };
            uint32_t previous_shadow_entry = 0;
            for (size_t i = 0; i < raster.shadows.size(); ++i) {
                const auto& shadow = raster.shadows[i];
                if (shadow.entry >= trace.entries.size() || trace.entries[shadow.entry].kind != 3
                    || shadow.mask != i / 2 || shadow.shade < 16 || shadow.shade > 31) return false;
                const auto found = casters.find(shadow.caster.slot);
                if (found == casters.end() || found->second->id != shadow.caster
                    || found->second->draw_class != 2 || found->second->class_id != 5) return false;
                if (i % 2) {
                    const auto& first = raster.shadows[i - 1];
                    if (shadow.entry != first.entry || shadow.caster != first.caster || shadow.shade != first.shade
                        || !same_vertex(first.vertices[0], shadow.vertices[0])
                        || !same_vertex(first.vertices[2], shadow.vertices[1])) return false;
                } else if (i && shadow.entry <= previous_shadow_entry) return false;
                previous_shadow_entry = shadow.entry;
                const auto& mask = raster.shadow_masks[shadow.mask];
                for (const auto& vertex : shadow.vertices) {
                    if (vertex.x < -16777216 || vertex.x > 16777216
                        || vertex.y < -16777216 || vertex.y > 16777216 || vertex.s != 0
                        || vertex.u < 0 || vertex.u > int32_t(mask.width - 1) * 65536
                        || vertex.v < 0 || vertex.v > int32_t(mask.height - 1) * 65536) return false;
                }
                ++counts[shadow.entry];
            }
            uint32_t excluded = 0;
            for (size_t i = 0; i < trace.entries.size(); ++i) if (trace.entries[i].kind == 3) {
                if (!counts[i]) ++excluded;
                else if (counts[i] != 2) return false;
            }
            if (excluded != raster.excluded_shadow_entries) return false;
        }
        size_t next_sprite = 0, next_triangle = 0, next_shadow = 0;
        previous_entry = 0;
        for (const auto& command : raster.commands) {
            uint32_t entry;
            if (command.kind == SceneDrawCommandKind::Sprite) {
                if (command.index != next_sprite || next_sprite >= trace.draws.size()) return false;
                entry = trace.draws[next_sprite++].entry;
            } else if (command.kind == SceneDrawCommandKind::Triangle) {
                if (command.index != next_triangle || next_triangle >= raster.triangles.size()) return false;
                entry = raster.triangles[next_triangle++].entry;
            } else if (command.kind == SceneDrawCommandKind::Shadow && c.format_version >= 5) {
                if (command.index != next_shadow || next_shadow >= raster.shadows.size()) return false;
                entry = raster.shadows[next_shadow++].entry;
            } else return false;
            if (entry < previous_entry) return false;
            previous_entry = entry;
        }
        if (next_sprite != trace.draws.size() || next_triangle != raster.triangles.size()
            || next_shadow != raster.shadows.size()) return false;
    }
    if (!ValidateOutline(c)) {
        if (failure) *failure = "Invalid captured outline trace";
        return false;
    }
    if (failure) *failure = "Invalid captured GL world trace";
    if (!ValidateGLWorld(c, failure)) return false;
    if (failure) *failure = "Invalid captured GL composition trace";
    if (!ValidateGLComposition(c, failure)) return false;
    if (failure) *failure = nullptr;
    return true;
}

std::vector<uint8_t> SceneAssetContactSheet(const SceneCapture& c, uint32_t& w, uint32_t& h)
{
    if (!ValidateSceneCapture(c)) throw std::runtime_error("Invalid scene contact sheet");
    // Sorted (variation, tile) entries, followed by unique resolved sprites.
    std::set<std::pair<uint8_t, uint16_t>> tiles;
    for (const auto& col : c.scene.columns) {
        tiles.emplace(col.texture_variation, col.floor_texture);
        for (auto cube : col.cubes) for (auto t : c.scene.cube_materials[cube].textures)
            tiles.emplace(col.texture_variation, t);
    }
    w = 1024;
    const size_t tile_count = std::min<size_t>(tiles.size(), 512);
    const size_t sprite_count = std::min<size_t>(c.assets.sprites.size(), 128);
    const size_t tile_rows = (tile_count + 15) / 16;
    h = static_cast<uint32_t>((tile_rows + (sprite_count + 7) / 8) * 128);
    h = std::max(h, 128u);
    std::vector<uint8_t> rgba(size_t(w) * h * 4, 255);
    for (uint32_t y = 0; y < h; ++y) for (uint32_t x = 0; x < w; ++x) {
        auto* p = &rgba[(size_t(y) * w + x) * 4];
        p[0] = p[1] = p[2] = ((x / 8 + y / 8) & 1) ? 38 : 24;
    }
    auto put = [&](uint32_t x, uint32_t y, uint8_t index) {
        auto* dst = &rgba[(size_t(y) * w + x) * 4];
        for (int k = 0; k < 3; ++k) dst[k] = c.assets.display_palette[index * 3 + k] * 255 / 63;
    };
    size_t n = 0;
    for (auto [variation, tile] : tiles) {
        if (n == tile_count) break;
        const size_t offset = c.scene.texture_frames[variation * 1544 + tile];
        for (uint32_t y = 0; y < 64; ++y) for (uint32_t x = 0; x < 64; ++x)
            put((n % 16) * 64 + x, (n / 16) * 128 + y,
                c.assets.texture_pixels[offset + (y / 2) * c.scene.texture_row_stride + x / 2]);
        ++n;
    }
    for (n = 0; n < sprite_count; ++n) {
        const auto& s = c.assets.sprites[n];
        const uint32_t sw = std::min<uint32_t>(s.width, 128), sh = std::min<uint32_t>(s.height, 128);
        for (uint32_t y = 0; y < sh; ++y) for (uint32_t x = 0; x < sw; ++x) {
            const auto p = (size_t(y * s.height / sh) * s.width + x * s.width / sw) * 2;
            if (s.pixels[p + 1]) put((n % 8) * 128 + x, (tile_rows + n / 8) * 128 + y, s.pixels[p]);
        }
    }
    return rgba;
}
}
