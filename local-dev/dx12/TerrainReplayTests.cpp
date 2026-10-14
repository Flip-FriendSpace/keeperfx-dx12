// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#include "TerrainReplayTests.h"
#include "SpriteReplayTests.h"
#include "LegacySpriteOracle.h"
#include "LegacyTerrainOracle.h"
#include "LegacyShadowOracle.h"
#include "LegacyWorldOracle.h"
#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>

namespace kfx::render::dx12 {
namespace {
void Require(bool condition, const char* reason)
{
    if (!condition) throw std::runtime_error(reason);
}
std::string Json(const std::string& value)
{
    std::string result = "\"";
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') { result += '\\'; result += char(c); }
        else result += c < 32 ? '?' : char(c);
    }
    return result + '"';
}
uint64_t Differences(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b)
{
    Require(a.size() == b.size(), "Terrain comparison size mismatch");
    uint64_t count = 0;
    for (size_t i = 0; i < a.size(); ++i) count += a[i] != b[i];
    return count;
}
Image Colour(uint32_t width, uint32_t height, const std::vector<uint8_t>& indices,
    const std::array<uint8_t, 768>& palette)
{
    Require(indices.size() == size_t(width) * height, "Terrain index image size mismatch");
    Image image{width, height, std::vector<uint8_t>(indices.size() * 4)};
    for (size_t p = 0; p < indices.size(); ++p) {
        for (size_t c = 0; c < 3; ++c) image.rgba[p * 4 + c] = uint8_t(palette[indices[p] * 3 + c] * 255 / 63);
        image.rgba[p * 4 + 3] = 255;
    }
    return image;
}
Image Mask(uint32_t width, uint32_t height, const std::vector<uint8_t>& a, const std::vector<uint8_t>& b)
{
    Require(a.size() == size_t(width) * height && b.size() == a.size(), "Terrain mask size mismatch");
    Image image{width, height, std::vector<uint8_t>(a.size() * 4)};
    for (size_t p = 0; p < a.size(); ++p) {
        image.rgba[p * 4] = image.rgba[p * 4 + 2] = a[p] == b[p] ? 0 : 255;
        image.rgba[p * 4 + 3] = 255;
    }
    return image;
}
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
    Require(bool(file), "Cannot retain terrain BMP evidence");
}
void Bytes(const std::filesystem::path& path, const std::vector<uint8_t>& bytes)
{
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    Require(bool(file), "Cannot retain terrain raw-index evidence");
}
void Texel(Image& image, uint32_t x, uint32_t y, uint8_t index)
{
    const size_t p = (size_t(y) * image.width + x) * 4;
    image.rgba.at(p) = index; image.rgba.at(p + 3) = 255;
}
using Upload = IndexedSceneUpload;
const char* Variant(bool quantized) { return quantized ? "vertex-quantized" : "raw-fraction"; }
std::string VariantName(const std::string& name, bool quantized)
{
    // Preserve the existing raw baseline artifact names.
    return quantized ? "vertex-quantized-" + name : name;
}
Upload Pack(const SceneCapture& capture) { return BuildIndexedSceneUpload(capture, false); }
void QuantizeUpload(Upload& upload, const SceneCapture& capture) { QuantizeIndexedSceneUpload(upload, capture); }
struct Runner {
    Device& device;
    std::filesystem::path output;
    bool rov;
    TerrainReplayTestResult result;
    std::vector<std::string> records;
    std::vector<uint8_t> Gpu(uint32_t width, uint32_t height, const std::vector<uint8_t>& background,
        const Image& atlas, const std::vector<PalettePrimitive>& primitives, bool ordered)
    {
        const auto before = device.GetDiagnostics();
        auto pixels = device.CompositeIndexed(width, height, background, atlas, primitives, ordered);
        const auto after = device.GetDiagnostics();
        Require(before.allocation_count == after.allocation_count && before.allocation_bytes == after.allocation_bytes,
            "Mixed replay retained temporary GPU allocations");
        result.rov_tested |= ordered;
        return pixels;
    }
    std::vector<uint8_t> Exact(const std::string& name, uint32_t width, uint32_t height,
        const std::vector<uint8_t>& background, const Image& atlas,
        const std::vector<PalettePrimitive>& primitives, const std::vector<uint8_t>* expected)
    {
        auto serial = Gpu(width, height, background, atlas, primitives, false);
        auto check = [&](const std::vector<uint8_t>& actual, const std::vector<uint8_t>& reference, const char* path) {
            const auto differences = Differences(actual, reference);
            result.indexed_checks += actual.size();
            records.push_back("{\"name\":" + Json(name) + ",\"path\":" + Json(path)
                + ",\"exact_differences\":" + std::to_string(differences) + "}");
            if (differences) {
                Bytes(output / (name + "-" + path + "-actual.idx"), actual);
                Bytes(output / (name + "-" + path + "-expected.idx"), reference);
                Bmp(output / (name + "-" + path + "-failure.bmp"), Mask(width, height, actual, reference));
                throw std::runtime_error("Exact mixed compositor test failed: " + name + "/" + path);
            }
        };
        if (expected) check(serial, *expected, "serial");
        if (rov) {
            const auto ordered = Gpu(width, height, background, atlas, primitives, true);
            check(ordered, serial, "rov-versus-serial");
            if (expected) check(ordered, *expected, "rov");
        }
        return serial;
    }
};
PalettePrimitive Triangle(std::array<PaletteTerrainVertex, 3> points, uint32_t tile_x = 0)
{
    PalettePrimitive primitive;
    primitive.kind = PalettePrimitiveKind::Terrain;
    primitive.terrain.vertices = points;
    primitive.terrain.texture_x = tile_x; primitive.terrain.texture_y = 320;
    primitive.terrain.fade_y = 256;
    return primitive;
}
void Synthetic(Runner& run)
{
    constexpr uint32_t width = 32, height = 32;
    Image atlas{512, 352, std::vector<uint8_t>(size_t(512) * 352 * 4)};
    for (uint32_t high = 0; high < 256; ++high) for (uint32_t low = 0; low < 256; ++low)
        Texel(atlas, low, high, uint8_t(high * 7 + low * 3 + 11));
    for (uint32_t y = 0; y < 64; ++y) for (uint32_t x = 0; x < 256; ++x)
        Texel(atlas, x, y + 256, uint8_t(x * 3 + y * 5));
    for (uint32_t y = 0; y < 32; ++y) for (uint32_t x = 0; x < 32; ++x) {
        Texel(atlas, x, y + 320, uint8_t(x * 17 + y * 29));
        Texel(atlas, x + 32, y + 320, 0);
        Texel(atlas, x + 64, y + 320, 61);
    }
    Texel(atlas, 100, 320, 23);
    const auto triangle = Triangle({{{4,4,-1.25f,33.75f,7.9f}, {21,4,-1.25f,33.75f,7.9f}, {4,20,-1.25f,33.75f,7.9f}}});
    const auto covered = [](uint32_t x, uint32_t y) { return x >= 4 && y >= 4 && 16 * x + 17 * y <= 387; };
    std::vector<uint8_t> background(width * height), expected;
    for (uint32_t y = 0; y < height; ++y) for (uint32_t x = 0; x < width; ++x)
        background[y * width + x] = uint8_t(x * 13 + y * 29 + 81);
    expected = background;
    const uint8_t wrapped = uint8_t(uint8_t(30 * 17 + 1 * 29) * 3 + 7 * 5);
    for (uint32_t y = 0; y < height; ++y) for (uint32_t x = 0; x < width; ++x)
        if (covered(x, y)) expected[y * width + x] = wrapped;
    run.Exact("synthetic-wrap-fade-coverage", width, height, background, atlas, {triangle}, &expected);
    auto reverse = triangle;
    std::swap(reverse.terrain.vertices[1], reverse.terrain.vertices[2]);
    run.Exact("synthetic-reversed-winding", width, height, background, atlas, {reverse}, &expected);
    auto degenerate = triangle;
    degenerate.terrain.vertices[2] = degenerate.terrain.vertices[1];
    run.Exact("synthetic-degenerate", width, height, background, atlas, {degenerate}, &background);
    auto zero = triangle; zero.terrain.texture_x = 32;
    for (auto& p : zero.terrain.vertices) p.shade = 0;
    expected = background;
    for (uint32_t y = 0; y < height; ++y) for (uint32_t x = 0; x < width; ++x)
        if (covered(x, y)) expected[y * width + x] = 0;
    run.Exact("synthetic-opaque-zero", width, height, background, atlas, {zero}, &expected);
    const auto square_a = Triangle({{{2,2,0,0,0},{26,2,0,0,0},{26,26,0,0,0}}}, 64);
    const auto square_b = Triangle({{{2,2,0,0,0},{26,26,0,0,0},{2,26,0,0,0}}}, 64);
    expected = background;
    for (uint32_t y = 2; y < 26; ++y) for (uint32_t x = 2; x < 26; ++x) expected[y * width + x] = 183;
    run.Exact("synthetic-shared-edge-square", width, height, background, atlas, {square_a, square_b}, &expected);
    PalettePrimitive sprite;
    sprite.sprite = {0,0,width,height,100,320,1,1,PaletteBlend::SourceHigh};
    const auto square_value = expected;
    for (auto& index : expected) index = uint8_t(23 * 7 + index * 3 + 11);
    for (uint32_t y = 0; y < height; ++y) for (uint32_t x = 0; x < width; ++x)
        if (covered(x, y)) expected[y * width + x] = wrapped;
    const auto ordered = run.Exact("synthetic-terrain-sprite-terrain", width, height, background, atlas,
        {square_a, square_b, sprite, triangle}, &expected);
    expected = background;
    for (uint32_t y = 0; y < height; ++y) for (uint32_t x = 0; x < width; ++x) {
        const size_t p = y * width + x;
        expected[p] = uint8_t(23 * 7 + expected[p] * 3 + 11);
        if (x >= 2 && x < 26 && y >= 2 && y < 26) expected[p] = square_value[p];
        if (covered(x, y)) expected[p] = wrapped;
    }
    const auto reordered = run.Exact("synthetic-reordered-sprite-terrain-terrain", width, height, background, atlas,
        {sprite, square_a, square_b, triangle}, &expected);
    Require(Differences(ordered, reordered) != 0, "Mixed fixture failed to distinguish painter order");
    for (unsigned bad = 0; bad < 6; ++bad) for (bool ordered_views : {false, true}) {
        if (ordered_views && !run.rov) continue;
        auto invalid = triangle;
        switch (bad) {
        case 0: invalid.terrain.vertices[0].x = std::numeric_limits<float>::quiet_NaN(); break;
        case 1: invalid.terrain.vertices[1].u = std::numeric_limits<float>::infinity(); break;
        case 2: invalid.terrain.vertices[2].shade = -1; break;
        case 3: invalid.terrain.texture_y = atlas.height - 31; break;
        case 4: invalid.terrain.fade_x = atlas.width - 255; break;
        case 5: invalid.kind = static_cast<PalettePrimitiveKind>(255); break;
        }
        const auto before = run.device.GetDiagnostics();
        bool rejected = false;
        try { run.device.CompositeIndexed(width, height, background, atlas, {invalid}, ordered_views); }
        catch (const std::invalid_argument&) { rejected = true; }
        const auto after = run.device.GetDiagnostics();
        Require(rejected && before.allocation_count == after.allocation_count && before.allocation_bytes == after.allocation_bytes,
            "Invalid terrain input was not rejected without retaining allocations");
        run.records.push_back("{\"name\":\"invalid-triangle-" + std::to_string(bad)
            + "\",\"path\":" + Json(ordered_views ? "rov" : "serial") + ",\"rejected\":true}");
    }
}
MixedReference Original(const SceneCapture& capture, const std::vector<uint8_t>& background, bool corrected)
{
    return ReplayLegacyWorld(capture, background, corrected);
}
void DiagnoseShadowBounds(Runner& run, const SceneCapture& capture)
{
    const auto width = uint32_t(capture.scene.camera.viewport_w), height = uint32_t(capture.scene.camera.viewport_h);
    const std::vector<uint8_t> zero(size_t(width) * height);
    Image ramps{256, 320, std::vector<uint8_t>(size_t(256) * 320 * 4)};
    size_t inspected = 0;
    constexpr size_t search_cap = 16;
    std::vector<size_t> candidates;
    for (size_t index = 0; index < capture.trace.raster.shadows.size(); ++index) {
        const auto& shadow = capture.trace.raster.shadows[index];
        const auto& v = shadow.vertices;
        const int64_t area = (int64_t(v[1].x) - v[0].x) * (int64_t(v[2].y) - v[0].y)
            - (int64_t(v[1].y) - v[0].y) * (int64_t(v[2].x) - v[0].x);
        if (area <= 0 || std::max({v[0].x,v[1].x,v[2].x}) <= 0
            || std::min({v[0].x,v[1].x,v[2].x}) >= int32_t(width)
            || std::max({v[0].y,v[1].y,v[2].y}) <= 0
            || std::min({v[0].y,v[1].y,v[2].y}) >= int32_t(height)) continue;
        candidates.push_back(index);
    }
    for (const size_t index : candidates) {
        if (inspected == search_cap) break;
        const auto& shadow = capture.trace.raster.shadows[index];
        const auto& mask = capture.trace.raster.shadow_masks.at(shadow.mask);
        const auto& v = shadow.vertices;
        PalettePrimitive probe;
        probe.kind = PalettePrimitiveKind::Terrain;
        probe.terrain.texture_width = probe.terrain.texture_height = 256;
        probe.terrain.apply_fade = false; probe.terrain.fade_y = 256;
        probe.terrain.clamp_to_vertices = true;
        for (size_t p = 0; p < 3; ++p) probe.terrain.vertices[p] = ConvertIndexedTerrainVertex(v[p], false);
        std::array<std::vector<uint8_t>, 3> gpu;
        for (size_t channel = 0; channel < gpu.size(); ++channel) {
            for (uint32_t y = 0; y < 256; ++y) for (uint32_t x = 0; x < 256; ++x)
                Texel(ramps, x, y, channel == 0 ? 255 : channel == 1 ? uint8_t(x) : uint8_t(y));
            // Failure-only localization keeps the exact full-frame viewport
            // and vertex coordinates; cropping could change raster rounding.
            gpu[channel] = run.Gpu(width, height, zero, ramps, {probe}, false);
        }
        ++inspected;
        size_t bad_count = 0;
        std::ostringstream samples;
        for (size_t p = 0; p < zero.size(); ++p) if (gpu[0][p] && (gpu[1][p] >= mask.width || gpu[2][p] >= mask.height)) {
            if (bad_count < 32) {
                if (bad_count) samples << ',';
                samples << "{\"x\":" << p % width << ",\"y\":" << p / width
                    << ",\"u\":" << unsigned(gpu[1][p]) << ",\"v\":" << unsigned(gpu[2][p]) << '}';
            }
            ++bad_count;
        }
        if (!bad_count) continue;
        const std::string name = "shadow-bounds-failure-" + std::to_string(index);
        constexpr std::array<const char*, 3> channels{"geometry", "u", "v"};
        for (size_t channel = 0; channel < gpu.size(); ++channel)
            Bytes(run.output / (name + "-" + channels[channel] + ".idx"), gpu[channel]);
        std::string oracle_error;
        uint64_t original_geometry = 0;
        try {
            const auto original = InspectLegacyShadowTriangle(shadow, mask, capture.assets, width, height);
            original_geometry = std::count(original.geometric_coverage.begin(), original.geometric_coverage.end(), uint8_t(255));
            Bytes(run.output / (name + "-original-geometry.idx"), original.geometric_coverage);
            Bytes(run.output / (name + "-original-u.idx"), original.sample_u);
            Bytes(run.output / (name + "-original-v.idx"), original.sample_v);
        } catch (const std::exception& error) { oracle_error = error.what(); }
        std::ostringstream row;
        row << "{\"name\":" << Json(name) << ",\"shadow\":" << index << ",\"entry\":" << shadow.entry
            << ",\"mask\":" << shadow.mask << ",\"mask_width\":" << mask.width << ",\"mask_height\":" << mask.height
            << ",\"inspected\":" << inspected << ",\"total_candidates\":" << candidates.size() << ",\"search_cap\":" << search_cap
            << ",\"invalid_sample_count\":" << bad_count
            << ",\"first_32_samples\":[" << samples.str() << "],\"vertices_xyuvs\":[";
        for (size_t p = 0; p < 3; ++p) {
            if (p) row << ',';
            row << '[' << v[p].x << ',' << v[p].y << ',' << v[p].u << ',' << v[p].v << ',' << v[p].s << ']';
        }
        row << "],\"original_probe_passed\":" << (oracle_error.empty() ? "true" : "false")
            << ",\"original_geometric_coverage\":" << original_geometry << ",\"original_error\":" << Json(oracle_error)
            << ",\"rasterComparisonAccepted\":false}";
        run.records.push_back(row.str());
        return;
    }
    run.records.push_back("{\"name\":\"shadow-bounds-failure-localization\",\"inspected\":"
        + std::to_string(inspected) + ",\"total_candidates\":" + std::to_string(candidates.size()) + ",\"search_cap\":"
        + std::to_string(search_cap) + ",\"invalid_sample_count\":0,\"explanation\":\"Independent shadow UV probes did not reproduce the compositor error within this bounded search; original failure remains unresolved\"}");
}
void Captured(Runner& run, const SceneCapture& capture, const Upload& upload, bool quantized)
{
    const auto width = uint32_t(capture.scene.camera.viewport_w), height = uint32_t(capture.scene.camera.viewport_h);
    std::vector<uint8_t> background(size_t(width) * height);
    for (unsigned pattern = 0; pattern < 2; ++pattern) {
        for (uint32_t y = 0; y < height; ++y) for (uint32_t x = 0; x < width; ++x)
            background[size_t(y) * width + x] = pattern ? uint8_t(x * 13 + y * 29 + 81) : 81;
        const std::string name = VariantName(pattern ? "captured-pattern" : "captured-flat", quantized);
        std::vector<uint8_t> modern;
        try {
            modern = run.Exact(name, width, height, background, upload.atlas, upload.primitives, nullptr);
        } catch (const std::exception& error) {
            if (std::string(error.what()).find("shadow sample outside") != std::string::npos) {
                try { DiagnoseShadowBounds(run, capture); }
                catch (const std::exception& diagnostic_error) {
                    run.records.push_back("{\"name\":\"shadow-bounds-diagnostic-error\",\"error\":" + Json(diagnostic_error.what()) + "}");
                }
            }
            throw;
        }
        const auto retain = [&](const std::string& suffix, const std::vector<uint8_t>& indices) {
            Bytes(run.output / (name + suffix + ".idx"), indices);
            Bmp(run.output / (name + suffix + ".bmp"), Colour(width, height, indices, capture.assets.display_palette));
        };
        retain("-modern", modern);
        auto original = Original(capture, background, false);
        auto corrected = Original(capture, background, true);
        retain("-original", original.pixels); retain("-corrected-sprites", corrected.pixels);
        Bmp(run.output / (name + "-original-modern-diff.bmp"), Mask(width, height, original.pixels, modern));
        Bmp(run.output / (name + "-corrected-modern-diff.bmp"), Mask(width, height, corrected.pixels, modern));
        Bmp(run.output / (name + "-original-corrected-diff.bmp"), Mask(width, height, original.pixels, corrected.pixels));
        std::ostringstream row;
        row << "{\"name\":" << Json(name) << ",\"vertex_attributes\":" << Json(Variant(quantized))
            << ",\"original_modern_differences\":" << Differences(original.pixels, modern)
            << ",\"corrected_modern_differences\":" << Differences(corrected.pixels, modern)
            << ",\"original_corrected_differences\":" << Differences(original.pixels, corrected.pixels)
            << ",\"rasterComparisonAccepted\":false,\"original_guard_changes\":[";
        for (size_t i = 0; i < original.guard_changes.size(); ++i) {
            const auto& change = original.guard_changes[i];
            if (i) row << ',';
            row << "{\"command\":" << change.command << ",\"offset\":" << change.offset
                << ",\"before\":" << unsigned(change.before) << ",\"after\":" << unsigned(change.after) << '}';
        }
        row << "]}"; run.records.push_back(row.str());
        if (!pattern && quantized) run.result.preview = Colour(width, height, modern, capture.assets.display_palette);
    }
}
void InspectShadows(Runner& run, const SceneCapture& capture, const Upload& upload)
{
    const auto width = uint32_t(capture.scene.camera.viewport_w), height = uint32_t(capture.scene.camera.viewport_h);
    const auto& shadows = capture.trace.raster.shadows;
    std::vector<size_t> candidates;
    for (size_t i = 0; i < shadows.size(); ++i) {
        const auto& v = shadows[i].vertices;
        const int64_t area = (int64_t(v[1].x) - v[0].x) * (int64_t(v[2].y) - v[0].y)
            - (int64_t(v[1].y) - v[0].y) * (int64_t(v[2].x) - v[0].x);
        const auto xmin = std::min({v[0].x,v[1].x,v[2].x}), xmax = std::max({v[0].x,v[1].x,v[2].x});
        const auto ymin = std::min({v[0].y,v[1].y,v[2].y}), ymax = std::max({v[0].y,v[1].y,v[2].y});
        if (area > 0 && std::max(0, xmin) < std::min(int32_t(width), xmax)
            && std::max(0, ymin) < std::min(int32_t(height), ymax)) candidates.push_back(i);
    }
    std::vector<size_t> selected;
    if (!candidates.empty()) selected = {candidates.front(), candidates[candidates.size() / 2], candidates.back()};
    std::sort(selected.begin(), selected.end());
    selected.erase(std::unique(selected.begin(), selected.end()), selected.end());
    run.records.push_back("{\"name\":\"shadow-selection\",\"policy\":\"First, middle and last positive-area shadows with a nonempty clipped input bounding box; independent of oracle and GPU output\",\"candidates\":"
        + std::to_string(candidates.size()) + ",\"selected\":" + std::to_string(selected.size()) + "}");
    for (const size_t index : selected) {
        const auto& shadow = shadows[index];
        const auto& mask = capture.trace.raster.shadow_masks.at(shadow.mask);
        const std::string name = "shadow-" + std::to_string(index);
        // Original mode 2 probes validate every sampled address before mode 10
        // reads this mask. No original pixels or scanlines are GPU input.
        const auto legacy = InspectLegacyShadowTriangle(shadow, mask, capture.assets, width, height);
        const std::vector<uint8_t> zero(size_t(width) * height);
        Image ramps{256, 320, std::vector<uint8_t>(size_t(256) * 320 * 4)};
        PalettePrimitive probe;
        probe.kind = PalettePrimitiveKind::Terrain;
        probe.terrain.texture_width = probe.terrain.texture_height = 256;
        probe.terrain.apply_fade = false;
        probe.terrain.clamp_to_vertices = true;
        probe.terrain.fade_y = 256;
        // The shadow's explicit positive winding was checked above. Terrain
        // probes otherwise use exactly its raw UVs and hardware geometry.
        for (size_t p = 0; p < 3; ++p) probe.terrain.vertices[p] = ConvertIndexedTerrainVertex(shadow.vertices[p], false);
        std::array<std::vector<uint8_t>, 3> gpu;
        constexpr std::array<const char*, 3> channels{"geometric-coverage", "u", "v"};
        const std::array<const std::vector<uint8_t>*, 3> old{&legacy.geometric_coverage, &legacy.sample_u, &legacy.sample_v};
        for (size_t channel = 0; channel < channels.size(); ++channel) {
            for (uint32_t y = 0; y < 256; ++y) for (uint32_t x = 0; x < 256; ++x)
                Texel(ramps, x, y, channel == 0 ? 255 : channel == 1 ? uint8_t(x) : uint8_t(y));
            gpu[channel] = run.Exact(name + "-" + channels[channel], width, height, zero, ramps, {probe}, nullptr);
            Bytes(run.output / (name + "-original-" + channels[channel] + ".idx"), *old[channel]);
            Bytes(run.output / (name + "-modern-" + channels[channel] + ".idx"), gpu[channel]);
            Bmp(run.output / (name + "-" + channels[channel] + "-diff.bmp"), Mask(width, height, *old[channel], gpu[channel]));
        }
        std::vector<uint8_t> background(zero.size()), covered(zero.size());
        for (uint32_t y = 0; y < height; ++y) for (uint32_t x = 0; x < width; ++x)
            background[size_t(y) * width + x] = uint8_t(x * 13 + y * 29 + 81);
        auto addressed = background;
        for (size_t p = 0; p < zero.size(); ++p) if (gpu[0][p]) {
            Require(gpu[0][p] == 255, "Shadow geometric coverage is not binary");
            if (gpu[1][p] >= mask.width || gpu[2][p] >= mask.height)
                throw std::runtime_error(name + " GPU sample outside initialized mask at screen "
                    + std::to_string(p % width) + "," + std::to_string(p / width) + " UV "
                    + std::to_string(gpu[1][p]) + "," + std::to_string(gpu[2][p]) + " extent "
                    + std::to_string(mask.width) + "x" + std::to_string(mask.height));
            covered[p] = mask.coverage.at(size_t(gpu[2][p]) * mask.width + gpu[1][p]);
            if (covered[p]) addressed[p] = capture.assets.fade.at(size_t(shadow.shade) * 256 + background[p]);
        }
        PalettePrimitive primitive;
        primitive.kind = PalettePrimitiveKind::Shadow;
        primitive.shadow = upload.shadows.at(index);
        const auto modern = run.Exact(name + "-masked-destination-fade", width, height, background,
            upload.atlas, {primitive}, &addressed);
        const auto original = ReplayLegacyShadowTriangle(shadow, mask, capture.assets, width, height, background);
        Bytes(run.output / (name + "-original-masked-coverage.idx"), legacy.coverage);
        Bytes(run.output / (name + "-modern-masked-coverage.idx"), covered);
        Bmp(run.output / (name + "-masked-coverage-diff.bmp"), Mask(width, height, legacy.coverage, covered));
        Bytes(run.output / (name + "-original.idx"), original);
        Bytes(run.output / (name + "-modern.idx"), modern);
        Bmp(run.output / (name + "-original.bmp"), Colour(width, height, original, capture.assets.display_palette));
        Bmp(run.output / (name + "-modern.bmp"), Colour(width, height, modern, capture.assets.display_palette));
        Bmp(run.output / (name + "-index-diff.bmp"), Mask(width, height, original, modern));
        uint64_t old_geometry = 0, new_geometry = 0, uv_differences = 0, unexplained = 0;
        for (size_t p = 0; p < zero.size(); ++p) {
            old_geometry += legacy.geometric_coverage[p] != 0;
            new_geometry += gpu[0][p] != 0;
            const bool common = legacy.geometric_coverage[p] && gpu[0][p];
            const bool uv = common && (legacy.sample_u[p] != gpu[1][p] || legacy.sample_v[p] != gpu[2][p]);
            uv_differences += uv;
            if (common && !uv) unexplained += legacy.coverage[p] != covered[p] || original[p] != modern[p];
            if (!legacy.geometric_coverage[p] && !gpu[0][p]) unexplained += original[p] != modern[p];
        }
        std::ostringstream row;
        row << "{\"name\":" << Json(name) << ",\"vertex_attributes\":\"raw-fraction\",\"mask\":" << shadow.mask
            << ",\"mask_width\":" << mask.width << ",\"mask_height\":" << mask.height << ",\"shade\":" << unsigned(shadow.shade)
            << ",\"winding_policy\":\"Positive screen-space signed area; original rounded thin setup may reject\""
            << ",\"original_geometric_coverage\":" << old_geometry << ",\"modern_geometric_coverage\":" << new_geometry
            << ",\"geometric_coverage_differences\":" << Differences(legacy.geometric_coverage, gpu[0])
            << ",\"common_geometry_uv_differences\":" << uv_differences
            << ",\"masked_coverage_differences\":" << Differences(legacy.coverage, covered)
            << ",\"same_samples_unexplained_index_differences\":" << unexplained
            << ",\"index_differences\":" << Differences(original, modern) << ",\"rasterComparisonAccepted\":false}";
        run.records.push_back(row.str());
        Require(unexplained == 0, "Isolated shadow output differs despite identical geometry/U/V/mask/fade");
    }
}
void InspectSelected(Runner& run, const SceneCapture& capture, const Upload& upload, bool quantized)
{
    const auto width = uint32_t(capture.scene.camera.viewport_w), height = uint32_t(capture.scene.camera.viewport_h);
    const size_t count = capture.trace.raster.triangles.size();
    std::vector<size_t> selected{0, count / 2, count - 1};
    std::sort(selected.begin(), selected.end());
    selected.erase(std::unique(selected.begin(), selected.end()), selected.end());
    // At most three isolated commands, selected independently of their result.
    // No oracle-generated pixels, coverage or scanlines are uploaded to DX12.
    for (const size_t index : selected) {
        const auto& triangle = capture.trace.raster.triangles[index];
        const auto legacy = InspectLegacyTerrainTriangle(triangle, capture.assets, width, height);
        const std::string name = VariantName("triangle-" + std::to_string(index), quantized);
        const std::vector<uint8_t> background(size_t(width) * height);
        Image diagnostics{256, 352, std::vector<uint8_t>(size_t(256) * 352 * 4)};
        PalettePrimitive primitive;
        primitive.kind = PalettePrimitiveKind::Terrain;
        primitive.terrain = upload.triangles[index];
        primitive.terrain.texture_x = 0; primitive.terrain.texture_y = 320;
        primitive.terrain.fade_x = 0; primitive.terrain.fade_y = 256;
        std::array<std::vector<uint8_t>, 4> gpu;
        constexpr std::array<const char*, 4> channels{"coverage", "u", "v", "shade"};
        const std::array<const std::vector<uint8_t>*, 4> old{&legacy.coverage, &legacy.sample_u, &legacy.sample_v, &legacy.shade};
        for (size_t channel = 0; channel < channels.size(); ++channel) {
            for (uint32_t y = 0; y < 32; ++y) for (uint32_t x = 0; x < 32; ++x)
                Texel(diagnostics, x, 320 + y, channel == 0 ? 255 : channel == 1 ? uint8_t(x) : channel == 2 ? uint8_t(y) : 0);
            for (uint32_t y = 0; y < 64; ++y) for (uint32_t x = 0; x < 256; ++x)
                Texel(diagnostics, x, 256 + y, channel == 3 ? uint8_t(y) : uint8_t(x));
            gpu[channel] = run.Exact(name + "-" + channels[channel], width, height, background, diagnostics, {primitive}, nullptr);
            if (quantized && channel == 0) {
                auto raw = primitive;
                for (size_t p = 0; p < 3; ++p) raw.terrain.vertices[p] = ConvertIndexedTerrainVertex(triangle.vertices[p], false);
                // A controlled attribute-only variant must preserve every covered
                // pixel. Compare both GPU paths; no original-C coverage is input.
                run.Exact(name + "-raw-versus-quantized-coverage", width, height, background,
                    diagnostics, {raw}, &gpu[channel]);
            }
            Bytes(run.output / (name + "-original-" + channels[channel] + ".idx"), *old[channel]);
            Bytes(run.output / (name + "-modern-" + channels[channel] + ".idx"), gpu[channel]);
            Bmp(run.output / (name + "-" + channels[channel] + "-diff.bmp"), Mask(width, height, *old[channel], gpu[channel]));
        }
        primitive.terrain = upload.triangles[index];
        std::vector<uint8_t> independently_addressed(background.size());
        for (size_t p = 0; p < background.size(); ++p) if (gpu[0][p]) {
            Require(gpu[0][p] == 255 && gpu[1][p] < 32 && gpu[2][p] < 32 && gpu[3][p] < 64,
                "GPU diagnostic sample outside palette bounds");
            const auto texel = capture.assets.texture_pixels.at(triangle.texture_offset + gpu[2][p] * 256 + gpu[1][p]);
            independently_addressed[p] = capture.assets.fade[gpu[3][p] * 256 + texel];
        }
        const auto modern = run.Exact(name + "-raw-table-addressing", width, height, background,
            upload.atlas, {primitive}, &independently_addressed);
        Bytes(run.output / (name + "-original.idx"), legacy.indices);
        Bytes(run.output / (name + "-modern.idx"), modern);
        Bmp(run.output / (name + "-original.bmp"), Colour(width, height, legacy.indices, capture.assets.display_palette));
        Bmp(run.output / (name + "-modern.bmp"), Colour(width, height, modern, capture.assets.display_palette));
        Bmp(run.output / (name + "-index-diff.bmp"), Mask(width, height, legacy.indices, modern));
        uint64_t old_coverage = 0, new_coverage = 0, coverage_only = 0, uv_only = 0, shade_only = 0, both = 0, unexplained = 0;
        for (size_t p = 0; p < background.size(); ++p) {
            old_coverage += legacy.coverage[p] != 0; new_coverage += gpu[0][p] != 0;
            if (bool(legacy.coverage[p]) != bool(gpu[0][p])) { ++coverage_only; continue; }
            if (!legacy.coverage[p]) continue;
            const bool uv = legacy.sample_u[p] != gpu[1][p] || legacy.sample_v[p] != gpu[2][p];
            const bool shade = legacy.shade[p] != gpu[3][p];
            uv_only += uv && !shade; shade_only += shade && !uv; both += uv && shade;
            unexplained += !uv && !shade && legacy.indices[p] != modern[p];
        }
        const auto& a = triangle.vertices[0]; const auto& b = triangle.vertices[1]; const auto& c = triangle.vertices[2];
        const int64_t area = (int64_t(b.x) - a.x) * (int64_t(c.y) - a.y) - (int64_t(b.y) - a.y) * (int64_t(c.x) - a.x);
        std::ostringstream row;
        row << "{\"name\":" << Json(name) << ",\"vertex_attributes\":" << Json(Variant(quantized))
            << ",\"signed_area_twice\":" << area
            << ",\"winding_policy\":\"Both windings; original Gpoly sorts by Y and has no area-sign cull\""
            << ",\"original_covered\":" << old_coverage << ",\"modern_covered\":" << new_coverage
            << ",\"coverage_differences\":" << coverage_only << ",\"common_coverage_uv_only\":" << uv_only
            << ",\"common_coverage_shade_only\":" << shade_only << ",\"common_coverage_uv_and_shade\":" << both
            << ",\"same_samples_unexplained_index_differences\":" << unexplained
            << ",\"index_differences\":" << Differences(legacy.indices, modern)
            << ",\"rasterComparisonAccepted\":false}";
        run.records.push_back(row.str());
        Require(unexplained == 0, "Isolated terrain palette output differs despite identical coverage/U/V/shade");
    }
}
}
TerrainReplayTestResult RunTerrainReplayTests(Device& device,
    const std::filesystem::path& scene_path, const std::filesystem::path& output)
{
    std::filesystem::create_directories(output);
    Runner run{device, output, device.GetDiagnostics().rasterizer_ordered_views_supported, {}, {}};
    std::string failure;
    size_t triangle_count = 0, sprite_count = 0, primitive_count = 0, clipped_count = 0;
    size_t shadow_count = 0, mask_count = 0, excluded_shadow_count = 0;
    try {
        Synthetic(run);
        const auto capture = LoadSceneCapture(scene_path);
        Require(capture->format_version >= 4 && capture->format_version <= 6 && capture->trace.complete && capture->trace.backend == 0,
            "Mixed terrain replay requires a complete software KFXC v4-v6 trace");
        Require(!capture->trace.raster.triangles.empty(), "Capture has no owned terrain triangles");
        triangle_count = capture->trace.raster.triangles.size(); sprite_count = capture->trace.draws.size();
        shadow_count = capture->trace.raster.shadows.size(); mask_count = capture->trace.raster.shadow_masks.size();
        excluded_shadow_count = capture->trace.raster.excluded_shadow_entries;
        auto upload = Pack(*capture);
        primitive_count = upload.primitives.size(); clipped_count = upload.clipped_sprites;
        Captured(run, *capture, upload, false);
        InspectSelected(run, *capture, upload, false);
        InspectShadows(run, *capture, upload);
        QuantizeUpload(upload, *capture);
        Captured(run, *capture, upload, true);
        InspectSelected(run, *capture, upload, true);
    } catch (const std::exception& error) { failure = error.what(); }
    std::ofstream report(output / "terrain-replay.json");
    report << "{\"schema\":1,\"passed\":" << (failure.empty() ? "true" : "false")
        << ",\"scene\":" << Json(scene_path.generic_string()) << ",\"error\":" << Json(failure)
        << ",\"rasterComparisonAccepted\":false,\"visual_review\":\"pending\""
        << ",\"preview_vertex_attributes\":\"vertex-quantized\""
        << ",\"vertex_attribute_variants\":[\"raw-fraction\",\"vertex-quantized\"]"
        << ",\"quantization_comparison\":\"Only Gpoly vertex U/V/S conversion changes from raw 16.16 fractions to signed value >> 16. Captured data, X/Y, command order, atlas and shader remain identical. Selected GPU coverage and ROV/serial output must match exactly. Neither variant is automatically accepted against original C.\""
        << ",\"scope\":\"Captured Gpoly mode 5 triangles, ordinary sprites, v5 owned-mask shadows and v6 spinning-key sprites in producer order; excludes uncaptured terrain modes, excluded shadow entries, UI and full-world parity\""
        << ",\"comparison_policy\":\"GPU ROV/serial and analytic palette/order fixtures require exact equality. Original C versus hardware coverage and affine interpolation are recorded without tolerance or acceptance. Corrected C changes only the documented sprite row-copy defect.\""
        << ",\"sprite_rle\":\"Canonical encoding of owned index/coverage; original packet boundaries were not captured\""
        << ",\"rov_supported\":" << (run.rov ? "true" : "false")
        << ",\"rov_tested\":" << (run.result.rov_tested ? "true" : "false")
        << ",\"indexed_checks\":" << run.result.indexed_checks << ",\"triangles\":" << triangle_count
        << ",\"sprites\":" << sprite_count << ",\"gpu_primitives\":" << primitive_count
        << ",\"shadows\":" << shadow_count << ",\"shadow_masks\":" << mask_count
        << ",\"excluded_shadow_entries\":" << excluded_shadow_count
        << ",\"clipped_sprites\":" << clipped_count << ",\"cases\":[";
    for (size_t i = 0; i < run.records.size(); ++i) { if (i) report << ','; report << run.records[i]; }
    report << "]}\n";
    Require(bool(report), "Cannot write terrain replay report");
    if (!failure.empty()) throw std::runtime_error("Terrain replay failed: " + failure);
    return run.result;
}
}
