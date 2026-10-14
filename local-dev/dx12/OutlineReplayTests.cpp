// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#include "OutlineReplayTests.h"
#include "OutlineGLOracle.h"
#include "kfx/renderer/SceneAssets.h"
#include <algorithm>
#include <fstream>
#include <map>
#include <stdexcept>

namespace kfx::render::dx12 {
namespace {
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
std::string Json(const std::string& value)
{
    std::string out = "\"";
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') out += '\\';
        out += c < 32 ? '?' : char(c);
    }
    return out + '"';
}
void Bmp(const std::filesystem::path& path, const Image& image)
{
    std::ofstream file(path, std::ios::binary);
    const auto word = [&](uint16_t n) { file.put(char(n)); file.put(char(n >> 8)); };
    const auto dword = [&](uint32_t n) { word(uint16_t(n)); word(uint16_t(n >> 16)); };
    file.put('B'); file.put('M'); dword(uint32_t(54 + image.rgba.size()));
    word(0); word(0); dword(54); dword(40); dword(image.width); dword(image.height);
    word(1); word(32); dword(0); dword(uint32_t(image.rgba.size()));
    dword(2835); dword(2835); dword(0); dword(0);
    for (uint32_t y = image.height; y-- > 0;) for (uint32_t x = 0; x < image.width; ++x) {
        const size_t p = (size_t(y) * image.width + x) * 4;
        file.put(char(image.rgba[p + 2])); file.put(char(image.rgba[p + 1]));
        file.put(char(image.rgba[p])); file.put(char(255));
    }
    Require(bool(file), "Cannot retain outline bitmap");
}
void Bytes(const std::filesystem::path& path, const Image& image)
{
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(image.rgba.data()), std::streamsize(image.rgba.size()));
    Require(bool(file), "Cannot retain outline RGBA");
}
struct Packed {
    Image atlas;
    std::vector<OutlineDepthTriangle> terrain;
    std::vector<OutlinePassDraw> outlines, composition;
    uint32_t glow_omitted = 0, border_sources = 0;
};
Packed Pack(const SceneCapture& capture)
{
    Packed result;
    const auto& trace = capture.trace.outline;
    std::map<uint32_t, std::pair<uint32_t, uint32_t>> locations;
    uint32_t x = 0, y = 0, row_height = 0;
    for (const auto& sprite : trace.sprites) {
        const auto source_id = capture.trace.draws[sprite.draw].sprite;
        if (locations.contains(source_id)) continue;
        const auto& source = capture.assets.sprites[source_id];
        if (x + source.width > 1024) { x = 0; y += row_height; row_height = 0; }
        locations.emplace(source_id, std::pair{x, y});
        row_height = std::max<uint32_t>(row_height, source.height); x += source.width;
    }
    const uint32_t height = std::max<uint32_t>(1, y + row_height);
    Require(height <= 16384, "Outline coverage atlas exceeds diagnostic budget");
    result.atlas = {1024, height, std::vector<uint8_t>(size_t(1024) * height * 4)};
    for (const auto& [source_id, location] : locations) {
        const auto& source = capture.assets.sprites[source_id];
        for (uint32_t sy = 0; sy < source.height; ++sy) for (uint32_t sx = 0; sx < source.width; ++sx) {
            const size_t a = (size_t(location.second + sy) * 1024 + location.first + sx) * 4;
            const size_t b = (size_t(sy) * source.width + sx) * 2;
            result.atlas.rgba[a] = source.pixels[b];
            result.atlas.rgba[a + 1] = source.pixels[b + 1];
        }
    }
    for (const auto& triangle : trace.occluders) {
        OutlineDepthTriangle output;
        for (size_t i = 0; i < 3; ++i) {
            const auto& v = triangle.vertices[i]; output.vertices[i] = {v.x,v.y,v.z,v.w};
        }
        result.terrain.push_back(output);
    }
    for (const auto& sprite : trace.sprites) result.border_sources += sprite.source_border_covered;
    for (const auto& command : trace.commands) {
        const auto& sprite = trace.sprites[command.sprite];
        const auto& submitted = capture.trace.draws[sprite.draw];
        const auto& source = capture.assets.sprites[submitted.sprite];
        // Glow has index-dependent support and a separate reverse-subtract
        // phase. It is explicitly outside this coverage-only comparison.
        if (command.kind == SceneOutlineCommandKind::GlowDarken || sprite.additive) {
            ++result.glow_omitted; continue;
        }
        OutlinePassDraw draw;
        draw.x = sprite.dst_x; draw.y = sprite.dst_y; draw.width = sprite.dst_w; draw.height = sprite.dst_h;
        draw.phase_x = sprite.phase_x; draw.phase_y = sprite.phase_y;
        draw.step_x = sprite.step_x; draw.step_y = sprite.step_y;
        draw.mask_x = locations.at(submitted.sprite).first; draw.mask_y = locations.at(submitted.sprite).second;
        draw.source_width = source.width; draw.source_height = source.height; draw.content_height = submitted.content_height;
        draw.flip = (sprite.flags & 1) != 0;
        draw.premultiplied = sprite.path == SceneOutlineExecutionPath::Instanced;
        if (command.kind == SceneOutlineCommandKind::Outline) {
            draw.kind = trace.mode == 2 ? OutlinePassKind::Edge : OutlinePassKind::Silhouette;
            draw.z_ndc = sprite.outline_z_ndc; draw.colour = sprite.owner_rgba;
            result.outlines.push_back(draw);
        } else {
            draw.kind = OutlinePassKind::VisibleCoverage; draw.z_ndc = sprite.z_ndc;
            // This declared colour verifies normal-pass coverage and ordering;
            // it is not a substitute for the normal sprite CLUT or glow shader.
            draw.colour = {0.125f, 0.625f, 0.375f, sprite.source_alpha};
        }
        result.composition.push_back(draw);
    }
    return result;
}
}

OutlineReplayTestResult RunOutlineReplayTests(Device& device,
    const std::filesystem::path& scene_path, const std::filesystem::path& output)
{
    const auto capture = LoadSceneCapture(scene_path);
    Require(capture->format_version == 7 && capture->trace.backend == 1
        && capture->trace.outline.present && capture->trace.outline.complete,
        "Outline replay requires a complete owned v7 OpenGL trace");
    const auto packed = Pack(*capture);
    const auto& camera = capture->scene.camera;
    Image background{uint32_t(camera.viewport_w), uint32_t(camera.viewport_h), {}};
    Require(background.width && background.height && uint64_t(background.width) * background.height <= 16777216,
        "Invalid outline viewport");
    background.rgba.resize(size_t(background.width) * background.height * 4);
    for (size_t i = 0; i < background.rgba.size(); i += 4) {
        background.rgba[i] = 19; background.rgba[i + 1] = 31;
        background.rgba[i + 2] = 47; background.rgba[i + 3] = 255;
    }
    OutlineReplayTestResult result;
    std::vector<std::string> records;
    std::string error;
    const auto compare = [&](const std::string& name, const Image& bg, const std::vector<OutlinePassDraw>& draws) {
        const auto before = device.GetDiagnostics();
        auto actual = device.CompositeOutlines(bg, packed.atlas, packed.terrain, draws);
        const auto after = device.GetDiagnostics();
        Require(before.allocation_count == after.allocation_count && before.allocation_bytes == after.allocation_bytes,
            "Outline diagnostic retained GPU allocations");
        auto original = RenderOutlineGLReference(bg, packed.atlas, packed.terrain, draws);
        Require(actual.rgba.size() == original.rgba.size(), "Outline comparison dimensions differ");
        Bmp(output / (name + "-dx12.bmp"), actual); Bmp(output / (name + "-opengl.bmp"), original);
        Bytes(output / (name + "-dx12.rgba"), actual); Bytes(output / (name + "-opengl.rgba"), original);
        Image mask = bg;
        uint64_t pixels = 0, channels = 0, coverage = 0; unsigned maximum = 0;
        for (size_t i = 0; i < actual.rgba.size(); i += 4) {
            bool different = false, a_changed = false, b_changed = false;
            for (size_t c = 0; c < 4; ++c) {
                const unsigned delta = unsigned(std::abs(int(actual.rgba[i+c]) - int(original.rgba[i+c])));
                maximum = std::max(maximum, delta); channels += delta != 0; different |= delta != 0;
                a_changed |= actual.rgba[i+c] != bg.rgba[i+c]; b_changed |= original.rgba[i+c] != bg.rgba[i+c];
            }
            pixels += different; coverage += a_changed != b_changed;
            mask.rgba[i] = mask.rgba[i + 2] = different ? 255 : 0;
            mask.rgba[i + 1] = 0; mask.rgba[i + 3] = 255;
        }
        Bmp(output / (name + "-difference.bmp"), mask);
        const auto count = uint64_t(bg.width) * bg.height;
        result.compared_pixels += count;
        records.push_back("{\"case\":" + Json(name) + ",\"draws\":" + std::to_string(draws.size())
            + ",\"compared_pixels\":" + std::to_string(count) + ",\"different_pixels\":" + std::to_string(pixels)
            + ",\"different_channels\":" + std::to_string(channels) + ",\"max_channel_delta\":" + std::to_string(maximum)
            + ",\"changed_coverage_disagreement\":" + std::to_string(coverage) + "}");
        return actual;
    };
    try {
        result.preview = compare("outline-owner-colour", background, packed.outlines);
        compare("outline-ordered-coverage", background, packed.composition);
        auto white = packed.outlines;
        for (auto& draw : white) draw.colour = {1,1,1,1};
        Image black = background;
        for (size_t i = 0; i < black.rgba.size(); i += 4)
            black.rgba[i] = black.rgba[i + 1] = black.rgba[i + 2] = 0;
        compare("outline-depth-coverage", black, white);
    } catch (const std::exception& failure) { error = failure.what(); }
    std::ofstream report(output / "outline-replay.json");
    report << "{\"schema\":1,\"passed\":" << (error.empty() ? "true" : "false")
        << ",\"scope\":\"Captured D24 terrain occlusion and owner outline composition; declared flat normal coverage, no terrain colour, shadows or glow\""
        << ",\"capture\":" << Json(scene_path.string()) << ",\"format_version\":7,\"mode\":" << unsigned(capture->trace.outline.mode)
        << ",\"viewport\":[" << background.width << ',' << background.height << "],\"opengl_device\":" << Json(OutlineGLReferenceDevice())
        << ",\"occluders\":" << packed.terrain.size() << ",\"sprites\":" << capture->trace.outline.sprites.size()
        << ",\"outline_draws\":" << packed.outlines.size() << ",\"glow_commands_omitted\":" << packed.glow_omitted
        << ",\"covered_source_borders\":" << packed.border_sources
        << ",\"outside_logical_source_coverage\":\"defined zero; original allocation padding not captured\""
        << ",\"compared_pixels\":" << result.compared_pixels << ",\"rasterComparisonAccepted\":false,\"gate2Accepted\":false,\"cases\":[";
    for (size_t i = 0; i < records.size(); ++i) { if (i) report << ','; report << records[i]; }
    report << "],\"error\":" << Json(error) << "}\n";
    Require(bool(report), "Cannot retain outline replay report");
    if (!error.empty()) throw std::runtime_error(error);
    return result;
}
}
