// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#include "PaletteTests.h"
#include "kfx/renderer/SceneAssets.h"
#include <algorithm>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace kfx::render::dx12 {
namespace {
using Ghost = std::array<uint8_t, 65536>;
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
void Texel(Image& atlas, uint32_t x, uint32_t y, uint8_t index, bool covered = true)
{
    const size_t offset = (size_t(y) * atlas.width + x) * 4;
    atlas.rgba.at(offset) = index;
    atlas.rgba.at(offset + 3) = covered ? 255 : 0;
}
Image Atlas(const Ghost& ghost)
{
    Image atlas{512, 320, std::vector<uint8_t>(512 * 320 * 4)};
    for (uint32_t high = 0; high < 256; ++high) for (uint32_t low = 0; low < 256; ++low) {
        Texel(atlas, low, high, ghost[high * 256 + low]);
        Texel(atlas, 256 + low, high, uint8_t(high));
    }
    // Nonzero indices in holes, covered zero and 255, and asymmetric artwork.
    for (uint32_t y = 0; y < 8; ++y) for (uint32_t x = 0; x < 8; ++x)
        Texel(atlas, 256 + x, 256 + y, uint8_t(x * 17 + y * 29), (x + y) % 4 != 1);
    Texel(atlas, 256, 256, 0); Texel(atlas, 263, 263, 255);
    Texel(atlas, 272, 256, 0); Texel(atlas, 273, 256, 255);
    Texel(atlas, 274, 256, 123, false);
    return atlas;
}
std::vector<uint8_t> Oracle(uint32_t width, uint32_t height, std::vector<uint8_t> indices,
    const Image& atlas, const std::vector<PaletteQuad>& quads, const Ghost& ghost)
{
    for (const auto& q : quads) {
        const int64_t right = std::min<int64_t>(width, int64_t(q.x) + q.width);
        const int64_t bottom = std::min<int64_t>(height, int64_t(q.y) + q.height);
        for (int64_t y = std::max<int64_t>(0, q.y); y < bottom; ++y)
            for (int64_t x = std::max<int64_t>(0, q.x); x < right; ++x) {
                // Nearest texel at the pixel centre, exact integer arithmetic.
                const uint32_t tx = q.texture_x + uint32_t((2 * (x - q.x) + 1) * q.texture_width / (2 * uint64_t(q.width)));
                const uint32_t ty = q.texture_y + uint32_t((2 * (y - q.y) + 1) * q.texture_height / (2 * uint64_t(q.height)));
                const size_t texel = (size_t(ty) * atlas.width + tx) * 4;
                if (!atlas.rgba.at(texel + 3)) continue;
                const uint8_t source = atlas.rgba.at(texel);
                auto& destination = indices.at(size_t(y) * width + x);
                if (q.blend == PaletteBlend::Opaque) destination = source;
                else if (q.blend == PaletteBlend::SourceHigh) destination = ghost[size_t(source) * 256 + destination];
                else destination = ghost[size_t(destination) * 256 + source];
            }
    }
    return indices;
}
struct Runner {
    Device& device;
    PaletteTestResult result;
    bool rov;
    std::vector<std::string> records;
    std::vector<uint8_t> Check(const std::string& name, uint32_t width, uint32_t height,
        const std::vector<uint8_t>& background, const Image& atlas,
        const std::vector<PaletteQuad>& quads, const std::vector<uint8_t>& expected)
    {
        std::vector<uint8_t> fallback;
        for (bool ordered : {false, true}) {
            if (ordered && !rov) continue;
            const std::string path = ordered ? "rov" : "fallback";
            const auto before = device.GetDiagnostics();
            std::string error;
            uint64_t checks = 0;
            try {
                auto actual = device.CompositePalette(width, height, background, atlas, quads, ordered);
                const auto after = device.GetDiagnostics();
                if (after.allocation_count != before.allocation_count || after.allocation_bytes != before.allocation_bytes)
                    throw std::runtime_error("temporary allocations retained: count " + std::to_string(before.allocation_count)
                        + " -> " + std::to_string(after.allocation_count) + ", bytes " + std::to_string(before.allocation_bytes)
                        + " -> " + std::to_string(after.allocation_bytes));
                if (actual.size() != expected.size()) throw std::runtime_error("index readback size mismatch");
                for (size_t i = 0; i < expected.size(); ++i) {
                    if (actual[i] != expected[i]) throw std::runtime_error("index mismatch at (" + std::to_string(i % width)
                        + "," + std::to_string(i / width) + "): expected " + std::to_string(expected[i])
                        + ", got " + std::to_string(actual[i]));
                    ++checks;
                }
                if (ordered && actual != fallback) throw std::runtime_error("ROV/fallback index disagreement");
                if (!ordered) fallback = std::move(actual);
                result.rov_tested |= ordered;
            } catch (const std::exception& e) { error = e.what(); }
            result.indexed_checks += checks;
            records.push_back("{\"case\":" + Json(name) + ",\"path\":" + Json(path)
                + ",\"indexed_checks\":" + std::to_string(checks) + ",\"passed\":" + (error.empty() ? "true" : "false")
                + ",\"allocation_count_before\":" + std::to_string(before.allocation_count)
                + ",\"error\":" + Json(error) + "}");
            if (!error.empty()) throw std::runtime_error("Palette case " + name + " / " + path + ": " + error);
        }
        return fallback;
    }
};
}
PaletteTestResult RunPaletteTests(Device& device, const std::filesystem::path& scene_path,
    const std::filesystem::path& output)
{
    const auto capture = kfx::render::LoadSceneCapture(scene_path);
    const auto diagnostics = device.GetDiagnostics();
    Runner run{device, {}, diagnostics.rasterizer_ordered_views_query_succeeded && diagnostics.rasterizer_ordered_views_supported, {}};
    std::filesystem::create_directories(output);
    std::string sources = "[]";
    auto report = [&](const std::string& error) {
        std::ofstream file(output / "palette.json");
        file << "{\"scope\":\"controlled indexed rectangle composition; not original world-frame parity\","
            << "\"excluded\":[\"world ordering\",\"additive\",\"composite flame overlays\",\"implicit lighting/remaps\"],"
            << "\"scene\":" << Json(scene_path.generic_string()) << ",\"point_sampling\":true,"
            << "\"rov_supported\":" << (run.rov ? "true" : "false") << ",\"rov_tested\":" << (run.result.rov_tested ? "true" : "false")
            << ",\"indexed_checks\":" << run.result.indexed_checks << ",\"preview_sources\":" << sources
            << ",\"passed\":" << (error.empty() ? "true" : "false") << ",\"error\":" << Json(error) << ",\"results\":[";
        for (size_t i = 0; i < run.records.size(); ++i) file << (i ? "," : "") << run.records[i];
        file << "]}\n";
        if (!file) throw std::runtime_error("Cannot write palette.json");
    };
    try {
        Ghost synthetic;
        for (size_t high = 0; high < 256; ++high) for (size_t low = 0; low < 256; ++low)
            synthetic[high * 256 + low] = uint8_t(high * 17 + low * 29 + 7);
        for (bool real : {false, true}) {
            const auto& ghost = real ? capture->assets.ghost : synthetic;
            auto atlas = Atlas(ghost);
            const std::string prefix = real ? "captured/" : "synthetic/";
            std::vector<uint8_t> background(65536), expected(65536);
            for (size_t s = 0; s < 256; ++s) for (size_t d = 0; d < 256; ++d) background[s * 256 + d] = uint8_t(d);
            for (auto blend : {PaletteBlend::Opaque, PaletteBlend::SourceHigh, PaletteBlend::DestinationHigh}) {
                for (size_t s = 0; s < 256; ++s) for (size_t d = 0; d < 256; ++d)
                    expected[s * 256 + d] = blend == PaletteBlend::Opaque ? uint8_t(s)
                        : blend == PaletteBlend::SourceHigh ? ghost[s * 256 + d] : ghost[d * 256 + s];
                run.Check(prefix + "all-pairs-" + std::to_string(uint32_t(blend)), 256, 256, background, atlas,
                    {{0, 0, 256, 256, 256, 0, 256, 256, blend}}, expected);
            }
            background.resize(31 * 23);
            for (size_t i = 0; i < background.size(); ++i) background[i] = uint8_t(i * 13 + 9);
            std::vector<PaletteQuad> layers = {
                {-3, -2, 29, 21, 256, 256, 8, 8, PaletteBlend::SourceHigh},
                {4, 3, 32, 25, 256, 71, 1, 1, PaletteBlend::DestinationHigh},
                {9, 7, 17, 11, 256, 149, 1, 1, PaletteBlend::SourceHigh},
                {0, 18, 31, 3, 272, 256, 1, 1, PaletteBlend::Opaque},
                {0, 0, 3, 3, 273, 256, 1, 1, PaletteBlend::Opaque},
                {27, 0, 4, 3, 274, 256, 1, 1, PaletteBlend::Opaque}};
            const auto forward = Oracle(31, 23, background, atlas, layers, ghost);
            run.Check(prefix + "clipped-three-layers-coverage-strip", 31, 23, background, atlas, layers, forward);
            std::swap(layers[0], layers[1]);
            const auto reverse = Oracle(31, 23, background, atlas, layers, ghost);
            if (forward == reverse) throw std::runtime_error(prefix + "layer reversal is vacuous");
            run.Check(prefix + "reversed-layers", 31, 23, background, atlas, layers, reverse);
            for (uint32_t repeat = 0; repeat < 8; ++repeat)
                run.Check(prefix + "three-layer-repeat-" + std::to_string(repeat), 31, 23, background, atlas, layers, reverse);
            std::vector<uint8_t> odd_background(7 * 9, 81);
            const std::vector<PaletteQuad> downscale = {{1, 1, 3, 5, 256, 256, 8, 8, PaletteBlend::Opaque}};
            run.Check(prefix + "downscale-integer-boundaries-odd-dimensions", 7, 9, odd_background, atlas, downscale,
                Oracle(7, 9, odd_background, atlas, downscale, ghost));
        }
        const auto invalid_atlas = Atlas(synthetic);
        const PaletteQuad valid_quad{0, 0, 1, 1, 256, 0, 1, 1, PaletteBlend::Opaque};
        for (uint32_t case_id = 0; case_id < 5; ++case_id) for (bool ordered : {false, true}) {
            if (ordered && !run.rov) continue;
            auto quad = valid_quad;
            std::vector<uint8_t> background{0};
            if (case_id == 0) background.clear();
            if (case_id == 1) quad.texture_x = invalid_atlas.width;
            if (case_id == 2) quad.blend = static_cast<PaletteBlend>(5);
            if (case_id == 3) quad.width = 0;
            const auto before = device.GetDiagnostics();
            bool rejected = false;
            try { device.CompositePalette(case_id == 4 ? 0 : 1, 1, background, invalid_atlas, {quad}, ordered); }
            catch (const std::invalid_argument&) { rejected = true; }
            const auto after = device.GetDiagnostics();
            const std::string name = "malformed-" + std::to_string(case_id), path = ordered ? "rov" : "fallback";
            const bool passed = rejected && before.allocation_count == after.allocation_count && before.allocation_bytes == after.allocation_bytes;
            run.records.push_back("{\"case\":" + Json(name) + ",\"path\":" + Json(path) + ",\"passed\":" + (passed ? "true" : "false") + "}");
            if (!passed) throw std::runtime_error("Palette case " + name + " / " + path + ": malformed input accepted or allocated resources retained");
        }
        // Select distinct captured artwork; prefer objects carrying each ghost flag.
        struct Selection { const kfx::render::SceneSprite* sprite; uint32_t slot; uint8_t flags; };
        std::vector<Selection> selected;
        for (uint8_t wanted : {uint8_t(16), uint8_t(32), uint8_t(0)}) {
            for (const auto& object : capture->scene.objects) {
                if (wanted && (object.render_flags & 48) != wanted) continue;
                for (const auto& binding : capture->assets.bindings) {
                    if (binding.animation != object.animation || binding.frame != object.frame) continue;
                    const auto& sprite = capture->assets.sprites.at(binding.sprite);
                    if (!sprite.width || !sprite.height || sprite.width > 1024 || sprite.height > 1024) continue;
                    if (std::any_of(selected.begin(), selected.end(), [&](const auto& s) { return s.sprite->pixels == sprite.pixels; })) continue;
                    selected.push_back({&sprite, object.id.slot, object.render_flags}); break;
                }
                if (selected.size() == 2) break;
                if (wanted && !selected.empty() && (selected.back().flags & 48) == wanted) break;
            }
            if (selected.size() == 2) break;
        }
        // Captures without matching objects may still contain two useful sprite assets.
        for (const auto& sprite : capture->assets.sprites) {
            if (selected.size() == 2) break;
            if (!sprite.width || !sprite.height || sprite.width > 1024 || sprite.height > 1024) continue;
            if (std::any_of(selected.begin(), selected.end(), [&](const auto& s) { return s.sprite->pixels == sprite.pixels; })) continue;
            selected.push_back({&sprite, 0, 0});
        }
        if (selected.size() != 2) throw std::runtime_error("Captured preview requires two distinct sprite artworks");
        uint32_t atlas_width = 512, atlas_height = 320;
        for (const auto& s : selected) { atlas_width = std::max(atlas_width, uint32_t(s.sprite->width)); atlas_height += s.sprite->height; }
        const auto fixture = Atlas(capture->assets.ghost);
        Image atlas{atlas_width, atlas_height, std::vector<uint8_t>(size_t(atlas_width) * atlas_height * 4)};
        for (uint32_t y = 0; y < fixture.height; ++y)
            std::copy_n(fixture.rgba.data() + size_t(y) * fixture.width * 4, fixture.width * 4, atlas.rgba.data() + size_t(y) * atlas.width * 4);
        std::vector<PaletteQuad> quads;
        std::ostringstream source_json; source_json << '[';
        uint32_t origin_y = 320;
        for (size_t i = 0; i < selected.size(); ++i) {
            const auto& sprite = *selected[i].sprite;
            for (uint32_t y = 0; y < sprite.height; ++y) for (uint32_t x = 0; x < sprite.width; ++x) {
                const size_t p = (size_t(y) * sprite.width + x) * 2;
                Texel(atlas, x, origin_y + y, sprite.pixels.at(p), sprite.pixels.at(p + 1) != 0);
            }
            const uint32_t extent = std::max(sprite.width, sprite.height);
            const uint32_t w = std::max<uint32_t>(1, uint32_t(sprite.width) * 220 / extent);
            const uint32_t h = std::max<uint32_t>(1, uint32_t(sprite.height) * 220 / extent);
            const auto blend = i ? PaletteBlend::DestinationHigh : PaletteBlend::SourceHigh;
            quads.push_back({int32_t(96 + i * 72), int32_t(42 + i * 24), w, h, 0, origin_y, sprite.width, sprite.height, blend});
            source_json << (i ? "," : "") << "{\"sprite_id\":" << sprite.id << ",\"object_slot\":" << selected[i].slot
                << ",\"captured_render_flags\":" << unsigned(selected[i].flags) << ",\"fixture_operator\":" << uint32_t(blend) << '}';
            origin_y += sprite.height;
        }
        source_json << ']'; sources = source_json.str();
        quads.push_back({180, 76, 142, 150, quads[0].texture_x, quads[0].texture_y, quads[0].texture_width, quads[0].texture_height, PaletteBlend::SourceHigh});
        quads.push_back({0, 260, 480, 18, 272, 256, 1, 1, PaletteBlend::Opaque});
        std::vector<uint8_t> background(480 * 300);
        for (size_t y = 0; y < 300; ++y) for (size_t x = 0; x < 480; ++x) background[y * 480 + x] = uint8_t(24 + (x / 24 + y / 24) % 32);
        const auto expected = Oracle(480, 300, background, atlas, quads, capture->assets.ghost);
        const auto indices = run.Check("captured-artwork-controlled-preview", 480, 300, background, atlas, quads, expected);
        run.result.preview = {480, 300, std::vector<uint8_t>(480 * 300 * 4)};
        for (size_t i = 0; i < indices.size(); ++i) {
            for (size_t c = 0; c < 3; ++c) run.result.preview.rgba[i * 4 + c] = uint8_t(capture->assets.display_palette[indices[i] * 3 + c] * 255 / 63);
            run.result.preview.rgba[i * 4 + 3] = 255;
        }
        report("");
        return std::move(run.result);
    } catch (const std::exception& e) { report(e.what()); throw; }
}
}
