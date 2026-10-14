// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#include "SpriteReplayTests.h"
#include "LegacySpriteOracle.h"
#include "kfx/renderer/SceneSpriteReplay.h"
#include <algorithm>
#include <array>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>

namespace kfx::render::dx12 {
namespace {
std::string Json(const std::string& value)
{
    std::string result = "\"";
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') { result += '\\'; result += char(c); }
        else if (c < 32) result += '?';
        else result += char(c);
    }
    return result + '"';
}
Image Colour(uint32_t width, uint32_t height, const std::vector<uint8_t>& indices,
    const std::array<uint8_t, 768>& palette)
{
    Image image{width, height, std::vector<uint8_t>(size_t(width) * height * 4)};
    if (indices.size() != size_t(width) * height) throw std::runtime_error("Invalid indexed image size");
    for (size_t i = 0; i < indices.size(); ++i) {
        for (size_t c = 0; c < 3; ++c) image.rgba[i * 4 + c] = uint8_t(palette[indices[i] * 3 + c] * 255 / 63);
        image.rgba[i * 4 + 3] = 255;
    }
    return image;
}
// Uncompressed 32-bit BMPs retain full native viewport resolution, including
// explicit pink/black difference masks, without any image-library dependency.
void Bmp(const std::filesystem::path& path, const Image& image)
{
    std::ofstream file(path, std::ios::binary);
    auto word = [&](uint16_t n) { file.put(char(n)); file.put(char(n >> 8)); };
    auto dword = [&](uint32_t n) { word(uint16_t(n)); word(uint16_t(n >> 16)); };
    file.put('B'); file.put('M'); dword(uint32_t(54 + image.rgba.size()));
    word(0); word(0); dword(54); dword(40); dword(image.width); dword(image.height);
    word(1); word(32); dword(0); dword(uint32_t(image.rgba.size()));
    dword(2835); dword(2835); dword(0); dword(0);
    for (uint32_t y = image.height; y-- > 0;) for (uint32_t x = 0; x < image.width; ++x) {
        const size_t p = (size_t(y) * image.width + x) * 4;
        file.put(char(image.rgba[p + 2])); file.put(char(image.rgba[p + 1]));
        file.put(char(image.rgba[p])); file.put(char(255));
    }
    if (!file) throw std::runtime_error("Cannot write " + path.generic_string());
}
void Indices(const std::filesystem::path& path, const std::vector<uint8_t>& indices)
{
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(indices.data()), std::streamsize(indices.size()));
    if (!file) throw std::runtime_error("Cannot retain raw palette indices " + path.generic_string());
}
Image Difference(uint32_t width, uint32_t height, const std::vector<uint8_t>& a,
    const std::vector<uint8_t>& b)
{
    Image image{width, height, std::vector<uint8_t>(size_t(width) * height * 4)};
    for (size_t i = 0; i < a.size(); ++i) {
        image.rgba[i * 4] = image.rgba[i * 4 + 2] = a[i] == b[i] ? 0 : 255;
        image.rgba[i * 4 + 3] = 255;
    }
    return image;
}
uint64_t Differences(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b)
{
    if (a.size() != b.size()) throw std::runtime_error("Index readback size mismatch");
    uint64_t result = 0;
    for (size_t i = 0; i < a.size(); ++i) result += a[i] != b[i];
    return result;
}
using Packed = SpriteReplayUpload;
Packed Pack(const SceneCapture& capture, std::span<const SceneSpriteDraw> draws)
{
    return BuildSpriteReplayUpload(capture, draws);
}
std::vector<uint8_t> Background(uint32_t width, uint32_t height, bool patterned, bool exhaustive)
{
    std::vector<uint8_t> result(size_t(width) * height, 81);
    if (patterned || exhaustive) for (uint32_t y = 0; y < height; ++y) for (uint32_t x = 0; x < width; ++x)
        result[size_t(y) * width + x] = exhaustive ? uint8_t(x)
            : uint8_t(x * 13 + y * 29 + ((x / 7) ^ (y / 5)) * 11);
    return result;
}
struct Runner {
    Device& device;
    const std::filesystem::path& output;
    SpriteReplayTestResult result;
    bool rov, passed = true;
    uint64_t original_differences = 0, gpu_differences = 0, original_guard_changed_bytes = 0;
    std::vector<std::string> records;
    void Check(const std::string& name, const SceneCapture& capture,
        std::span<const SceneSpriteDraw> draws, bool patterned, bool preview = false, bool exhaustive = false)
    {
        const uint32_t width = capture.scene.camera.viewport_w, height = capture.scene.camera.viewport_h;
        std::string stem = std::to_string(records.size()) + "-" + name;
        std::replace(stem.begin(), stem.end(), '/', '-');
        try {
            const auto background = Background(width, height, patterned, exhaustive);
            const auto packed = Pack(capture, draws);
            const auto diagnostic_original = ReplayLegacySpritesDetailed(capture, background, draws);
            const auto& original = diagnostic_original.pixels;
            const auto expected = ReplayCorrectedLegacySprites(capture, background, draws);
            const bool captured_case = name.starts_with("captured/full-command-order-");
            const std::string captured_background = patterned ? "patterned" : "flat";
            if (captured_case)
                Bmp(output / ("sprite-replay-" + captured_background + "-original.bmp"), Colour(width, height, original, capture.assets.display_palette));
            const uint64_t legacy_differences = Differences(original, expected);
            original_differences += legacy_differences;
            original_guard_changed_bytes += diagnostic_original.guard_changes.size();
            if (legacy_differences && !packed.quirk_commands)
                throw std::runtime_error("Corrected oracle differs without a declared mirrored row-copy quirk");
            std::ostringstream guards;
            guards << '[';
            for (size_t i = 0; i < diagnostic_original.guard_changes.size(); ++i) {
                const auto& change = diagnostic_original.guard_changes[i];
                if (change.command >= packed.quirk_by_command.size() || !packed.quirk_by_command[change.command])
                    throw std::runtime_error("Original guard change without that command's declared mirrored row-copy quirk");
                guards << (i ? "," : "") << "{\"command\":" << change.command << ",\"viewport_byte_offset\":" << change.offset
                    << ",\"before\":" << unsigned(change.before) << ",\"after\":" << unsigned(change.after) << '}';
            }
            guards << ']';
            const std::string guard_json = guards.str();
            if (legacy_differences || !diagnostic_original.guard_changes.empty()) {
                Bmp(output / (stem + "-original.bmp"), Colour(width, height, original, capture.assets.display_palette));
                Bmp(output / (stem + "-corrected.bmp"), Colour(width, height, expected, capture.assets.display_palette));
                Bmp(output / (stem + "-original-diff.bmp"), Difference(width, height, original, expected));
                Indices(output / (stem + "-original.idx"), original);
                Indices(output / (stem + "-corrected.idx"), expected);
                std::ofstream guard_file(output / (stem + "-original-guards.json"));
                guard_file << "{\"scope\":\"changed guard bytes observed after each command; not a count of all writes\",\"changes\":"
                    << guard_json << "}\n";
                if (!guard_file) throw std::runtime_error("Cannot retain original oracle guard diagnostics");
            }
            if (packed.quads.empty()) {
                if (expected != background)
                    throw std::runtime_error("Empty mapped command set changed corrected background");
                records.push_back("{\"case\":" + Json(name)
                    + ",\"path\":\"cpu-empty-maps\",\"gpu_tested\":false,\"indexed_checks\":0,\"cpu_indexed_checks\":"
                    + std::to_string(expected.size()) + ",\"passed\":true,\"command_count\":" + std::to_string(draws.size())
                    + ",\"width\":" + std::to_string(width) + ",\"height\":" + std::to_string(height)
                    + ",\"legacy_row_copy_quirk_commands\":" + std::to_string(packed.quirk_commands)
                    + ",\"original_corrected_differences\":" + std::to_string(legacy_differences)
                    + ",\"original_guard_changed_bytes\":" + std::to_string(diagnostic_original.guard_changes.size())
                    + ",\"original_guard_changes\":" + guard_json + ",\"corrected_guard_changed_bytes\":0,\"artifact_stem\":" + Json(stem) + "}");
                return;
            }
            std::vector<uint8_t> serial;
            for (bool ordered : {false, true}) {
                if (ordered && !rov) continue;
                const std::string path = ordered ? "rov" : "serial";
                const auto before = device.GetDiagnostics();
                std::string error;
                uint64_t differences = 0, original_gpu_differences = 0, checks = 0;
                try {
                    auto actual = device.CompositePalette(width, height, background, packed.atlas, packed.quads, ordered);
                    const auto after = device.GetDiagnostics();
                    if (after.allocation_count != before.allocation_count || after.allocation_bytes != before.allocation_bytes)
                        throw std::runtime_error("Sprite replay retained temporary GPU allocations");
                    differences = Differences(actual, expected);
                    original_gpu_differences = Differences(actual, original);
                    checks = expected.size(); result.indexed_checks += checks; gpu_differences += differences;
                    result.rov_tested |= ordered;
                    if (differences) {
                        Bmp(output / (stem + "-" + path + "-actual.bmp"), Colour(width, height, actual, capture.assets.display_palette));
                        Bmp(output / (stem + "-" + path + "-expected.bmp"), Colour(width, height, expected, capture.assets.display_palette));
                        Bmp(output / (stem + "-" + path + "-diff.bmp"), Difference(width, height, actual, expected));
                        Indices(output / (stem + "-" + path + "-actual.idx"), actual);
                        Indices(output / (stem + "-" + path + "-expected.idx"), expected);
                        const auto first = std::mismatch(actual.begin(), actual.end(), expected.begin());
                        const size_t index = size_t(first.first - actual.begin());
                        error = "Index mismatch at (" + std::to_string(index % width) + "," + std::to_string(index / width)
                            + "): expected " + std::to_string(expected[index]) + ", got " + std::to_string(actual[index]);
                    }
                    if (ordered && actual != serial) {
                        if (!error.empty()) error += "; ";
                        error += "ROV/serial disagreement";
                    }
                    if (preview && !ordered) {
                        result.preview = Colour(width, height, actual, capture.assets.display_palette);
                        Bmp(output / "sprite-replay-preview.bmp", result.preview);
                    }
                    if (captured_case && !ordered)
                        Bmp(output / ("sprite-replay-" + captured_background + "-actual.bmp"), Colour(width, height, actual, capture.assets.display_palette));
                    if (!ordered) serial = std::move(actual);
                } catch (const std::exception& e) { error = e.what(); }
                passed &= error.empty();
                records.push_back("{\"case\":" + Json(name) + ",\"path\":" + Json(path)
                    + ",\"background\":" + Json(exhaustive ? "exhaustive-x-index-ramp" : patterned ? "declared-index-pattern" : "flat-index-81")
                    + ",\"width\":" + std::to_string(width) + ",\"height\":" + std::to_string(height)
                    + ",\"command_count\":" + std::to_string(draws.size()) + ",\"quad_count\":" + std::to_string(packed.quads.size())
                    + ",\"invisible_commands\":" + std::to_string(packed.invisible_commands)
                    + ",\"legacy_row_copy_quirk_commands\":" + std::to_string(packed.quirk_commands)
                    + ",\"original_corrected_differences\":" + std::to_string(legacy_differences)
                    + ",\"original_guard_changed_bytes\":" + std::to_string(diagnostic_original.guard_changes.size())
                    + ",\"original_guard_changes\":" + guard_json + ",\"corrected_guard_changed_bytes\":0"
                    + ",\"gpu_original_differences\":" + std::to_string(original_gpu_differences)
                    + ",\"gpu_corrected_differences\":" + std::to_string(differences)
                    + ",\"atlas_bytes\":" + std::to_string(packed.atlas.rgba.size())
                    + ",\"indexed_checks\":" + std::to_string(checks) + ",\"passed\":" + (error.empty() ? "true" : "false")
                    + ",\"artifact_stem\":" + Json(stem) + ",\"error\":" + Json(error) + "}");
            }
        } catch (const std::exception& e) {
            passed = false;
            records.push_back("{\"case\":" + Json(name) + ",\"passed\":false,\"error\":" + Json(e.what()) + "}");
        }
    }
};
SceneCapture Synthetic(const SceneCapture& source, uint32_t width = 96, uint32_t height = 72)
{
    SceneCapture result;
    result.format_version = 3;
    result.scene.camera.viewport_w = width; result.scene.camera.viewport_h = height;
    result.assets.display_palette = source.assets.display_palette;
    result.assets.additive.resize(65536);
    for (uint32_t high = 0; high < 256; ++high) for (uint32_t low = 0; low < 256; ++low) {
        result.assets.ghost[high * 256 + low] = uint8_t(high * 17 + low * 29 + 7);
        result.assets.additive[high * 256 + low] = uint8_t(high * 31 + low * 11 + 19);
    }
    SceneSprite sprite;
    sprite.width = 9; sprite.height = 7; sprite.frame_width = 13; sprite.frame_height = 10;
    sprite.pixels.resize(size_t(sprite.width) * sprite.height * 2);
    for (uint32_t y = 0; y < sprite.height; ++y) for (uint32_t x = 0; x < sprite.width; ++x) {
        const size_t p = (size_t(y) * sprite.width + x) * 2;
        sprite.pixels[p] = uint8_t(x * 17 + y * 41);
        sprite.pixels[p + 1] = (x + 2 * y) % 5 == 2 ? 0 : 255;
    }
    sprite.pixels[0] = 0; sprite.pixels[1] = 255;
    result.assets.sprites.push_back(std::move(sprite));
    return result;
}
SceneSpriteDraw Draw(const SceneCapture& capture, uint32_t flags, bool alpha,
    int32_t x = 12, int32_t y = 11, int32_t width = 13, int32_t height = 10)
{
    SceneSpriteDraw draw;
    const auto& sprite = capture.assets.sprites[0];
    draw.content_height = sprite.height;
    draw.scale = {x, y, sprite.frame_width, sprite.frame_height, width, height, 2, 1,
        capture.scene.camera.viewport_w, capture.scene.camera.viewport_h};
    draw.flags = flags; draw.alpha = alpha; draw.has_remap = flags & 0x0800;
    draw.blend = ResolveSceneSpriteBlend(flags, alpha);
    if (draw.has_remap) for (uint32_t i = 0; i < 256; ++i) draw.remap[i] = uint8_t(i * 37 + 23);
    return draw;
}
void SyntheticCases(Runner& run, const SceneCapture& source)
{
    const auto fixture = Synthetic(source);
    std::vector<SceneSpriteDraw> precedence;
    for (bool alpha : {false, true}) for (uint32_t flags : {0u, 4u, 8u, 12u, 0x800u, 0x804u, 0x808u, 0x80cu}) {
        auto draw = Draw(fixture, flags, alpha);
        precedence.push_back(draw);
        run.Check("synthetic/blend-precedence-" + std::to_string(flags) + "-alpha-" + std::to_string(alpha), fixture, {&draw, 1}, true);
    }
    run.Check("synthetic/ordered-sixteen-overlaps", fixture, precedence, true);
    const auto forward = ReplayCorrectedLegacySprites(fixture, Background(96, 72, true, false), precedence);
    std::reverse(precedence.begin(), precedence.end());
    const auto reverse = ReplayCorrectedLegacySprites(fixture, Background(96, 72, true, false), precedence);
    if (forward == reverse) throw std::runtime_error("Synthetic command-order reversal is vacuous");
    run.Check("synthetic/reversed-sixteen-overlaps", fixture, precedence, true);
    struct Geometry { const char* name; int32_t x, y, width, height, crop; };
    const std::array<Geometry, 8> geometries = {{
        {"native-offset", 12, 11, 13, 10, 7}, {"upscale", 12, 11, 29, 23, 7},
        {"downscale", 12, 11, 5, 4, 7}, {"mixed-axis", 12, 11, 26, 5, 7},
        {"left-top-clipped", -8, -5, 29, 23, 7}, {"right-bottom-clipped", 84, 61, 29, 23, 7},
        {"source-row-crop", 12, 11, 29, 23, 3}, {"offscreen", 110, 80, 13, 10, 7}}};
    for (const auto& geometry : geometries) for (uint32_t flip = 0; flip < 4; ++flip)
        for (uint32_t blend : {0u, 4u, 8u, 0x800u, 0x10000u}) {
            auto draw = Draw(fixture, (blend & 0xffff) | flip, blend == 0x10000,
                geometry.x, geometry.y, geometry.width, geometry.height);
            draw.content_height = geometry.crop;
            run.Check("synthetic/" + std::string(geometry.name) + "-flip-" + std::to_string(flip)
                + "-blend-" + std::to_string(blend), fixture, {&draw, 1}, true);
        }
    auto holes = fixture;
    for (size_t i = 1; i < holes.assets.sprites[0].pixels.size(); i += 2) holes.assets.sprites[0].pixels[i] = 0;
    auto hole_draw = Draw(holes, 4, false, 2, 2, 29, 23);
    run.Check("synthetic/all-coverage-holes", holes, {&hole_draw, 1}, true);
    // Original mirrored upscale copying can touch an adjacent background
    // pixel even when every source X step is clipped away. Corrected sampling
    // must leave the declared background unchanged; no GPU draw is claimed.
    for (int32_t x : {-50, 110}) for (uint32_t flip : {1u, 3u}) for (uint32_t blend : {0u, 0x800u}) {
        auto draw = Draw(fixture, blend | flip, false, x, 11, 29, 23);
        run.Check("synthetic/fully-x-clipped-y-visible-" + std::to_string(x) + "-flip-" + std::to_string(flip)
            + "-blend-" + std::to_string(blend), fixture, {&draw, 1}, true);
    }
    auto pairs = Synthetic(source, 256, 256);
    auto& sprite = pairs.assets.sprites[0];
    sprite.width = sprite.height = sprite.frame_width = sprite.frame_height = 256;
    sprite.pixels.resize(256 * 256 * 2);
    for (uint32_t y = 0; y < 256; ++y) for (uint32_t x = 0; x < 256; ++x) {
        sprite.pixels[(size_t(y) * 256 + x) * 2] = uint8_t(y);
        sprite.pixels[(size_t(y) * 256 + x) * 2 + 1] = 255;
    }
    // The declared pattern includes every destination index in every source
    // row. This exercises all 65,536 additive and ghost operand pairs.
    for (uint32_t blend : {0u, 4u, 8u, 0x800u, 0x10000u}) {
        auto draw = Draw(pairs, blend & 0xffff, blend == 0x10000, 0, 0, 256, 256);
        draw.scale.content_x = draw.scale.content_y = 0;
        run.Check("synthetic/all-index-pairs-blend-" + std::to_string(blend), pairs, {&draw, 1}, false, false, true);
    }
}
void MalformedCases(Runner& run, const SceneCapture& source)
{
    const auto capture = Synthetic(source);
    const auto draw = Draw(capture, 0, false);
    const auto packed = Pack(capture, {&draw, 1});
    const auto background = Background(96, 72, true, false);
    for (uint32_t id = 0; id < 4; ++id) for (bool ordered : {false, true}) {
        if (ordered && !run.rov) continue;
        auto atlas = packed.atlas;
        auto quad = packed.quads.at(0);
        if (id == 0) quad.map_x_row = atlas.height;
        if (id == 1) {
            const size_t p = size_t(quad.map_x_row) * atlas.width * 4;
            const uint32_t encoded = quad.texture_width + 1;
            atlas.rgba[p] = uint8_t(encoded); atlas.rgba[p + 1] = uint8_t(encoded >> 8);
        }
        if (id == 2) { quad.blend = PaletteBlend::Remap; quad.lookup_x = atlas.width - 255; }
        if (id == 3) { quad.blend = PaletteBlend::Additive; quad.lookup_y = atlas.height - 255; }
        const auto before = run.device.GetDiagnostics();
        bool rejected = false;
        std::string error;
        try { run.device.CompositePalette(96, 72, background, atlas, {quad}, ordered); }
        catch (const std::invalid_argument&) { rejected = true; }
        catch (const std::exception& e) { error = e.what(); }
        const auto after = run.device.GetDiagnostics();
        const bool passed = rejected && error.empty() && before.allocation_count == after.allocation_count
            && before.allocation_bytes == after.allocation_bytes;
        run.passed &= passed;
        run.records.push_back("{\"case\":\"malformed-mapped-" + std::to_string(id) + "\",\"path\":"
            + Json(ordered ? "rov" : "serial") + ",\"rejected_before_allocation\":" + (passed ? "true" : "false")
            + ",\"passed\":" + (passed ? "true" : "false") + ",\"error\":" + Json(error) + "}");
    }
}
}
SpriteReplayTestResult RunSpriteReplayTests(Device& device,
    const std::filesystem::path& scene_path, const std::filesystem::path& output)
{
    std::filesystem::create_directories(output);
    const auto diagnostics = device.GetDiagnostics();
    Runner run{device, output, {}, diagnostics.rasterizer_ordered_views_query_succeeded
        && diagnostics.rasterizer_ordered_views_supported, true, 0, 0, 0, {}};
    std::string sources = "[]";
    uint32_t producer_backend = 0, excluded_calls = 0;
    auto report = [&](const std::string& error) {
        std::ofstream file(output / "sprite-replay.json");
        file << "{\"scope\":\"ordered captured sprites only on declared indexed backgrounds; no full world-frame parity\","
            << "\"comparison\":\"actual original C sprite routines; corrected C variant only fixes mirrored opaque/remap repeated-row background-copy overrun\","
            << "\"original_row_copy_quirk\":\"v3 does not preserve original RLE run packetization; original oracle reconstructs canonical RLE and retains all original/corrected differences\","
            << "\"original_guard_observations\":\"changed guard bytes per command are retained with offsets and values; corrected C must preserve guards; observations do not count writes that preserve byte values\","
            << "\"raw_index_artifacts\":\".idx files are one palette-index byte per pixel in row-major viewport order; dimensions are in each result\","
            << "\"excluded\":[\"terrain destination indices and interleaving\",\"shadows\",\"UI\",\"spinning keys\",\"normal-game OpenGL filtering\",\"live gameplay and performance\"],"
            << "\"unsupported\":[\"legacy scale-table capacity overflow\",\"uninitialized content offsets\",\"signed Win32 16.16 overflow\",\"atlas larger than 64 MiB\",\"sampling map longer than 4096\"],"
            << "\"scene\":" << Json(scene_path.generic_string()) << ",\"rov_supported\":" << (run.rov ? "true" : "false")
            << ",\"producer_backend\":" << producer_backend << ",\"excluded_sprite_calls\":" << excluded_calls
            << ",\"rov_tested\":" << (run.result.rov_tested ? "true" : "false") << ",\"indexed_checks\":" << run.result.indexed_checks
            << ",\"gpu_corrected_differences\":" << run.gpu_differences << ",\"original_corrected_differences\":" << run.original_differences
            << ",\"original_guard_changed_bytes\":" << run.original_guard_changed_bytes
            << ",\"original_exact_parity\":" << (run.original_differences == 0 && run.original_guard_changed_bytes == 0 && run.passed && error.empty() ? "true" : "false")
            << ",\"passed\":" << (run.passed && error.empty() ? "true" : "false") << ",\"error\":" << Json(error)
            << ",\"source_commands\":" << sources << ",\"results\":[";
        for (size_t i = 0; i < run.records.size(); ++i) file << (i ? "," : "") << run.records[i];
        file << "]}\n";
        if (!file) throw std::runtime_error("Cannot write sprite-replay.json");
    };
    try {
        const auto capture = LoadSceneCapture(scene_path);
        if (capture->format_version < 3 || !capture->trace.complete || !ValidateSceneCapture(*capture))
            throw std::runtime_error("Complete validated sprite command capture required");
        if (capture->trace.draws.empty()) throw std::runtime_error("Capture has no submitted sprite commands");
        producer_backend = capture->trace.backend;
        excluded_calls = capture->trace.excluded_sprite_calls;
        std::ostringstream source_json;
        source_json << '[';
        for (size_t i = 0; i < capture->trace.draws.size(); ++i) {
            const auto& draw = capture->trace.draws[i];
            source_json << (i ? "," : "") << "{\"ordinal\":" << i << ",\"entry\":" << draw.entry
                << ",\"object_slot\":" << draw.object.slot << ",\"sprite\":" << draw.sprite << ",\"flags\":" << draw.flags
                << ",\"alpha\":" << (draw.alpha ? "true" : "false") << ",\"operator\":" << unsigned(draw.blend)
                << ",\"content_height\":" << draw.content_height << '}';
        }
        source_json << ']'; sources = source_json.str();
        SyntheticCases(run, *capture);
        MalformedCases(run, *capture);
        run.Check("captured/full-command-order-flat", *capture, capture->trace.draws, false, true);
        run.Check("captured/full-command-order-patterned", *capture, capture->trace.draws, true);
        if (!run.passed) throw std::runtime_error("Sprite replay comparisons failed; retained per-case errors and native-resolution differences in sprite-replay.json");
        report("");
        return std::move(run.result);
    } catch (const std::exception& e) { report(e.what()); throw; }
}
}
