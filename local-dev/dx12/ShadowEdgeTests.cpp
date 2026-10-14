// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#include "ShadowEdgeTests.h"
#include <array>
#include <fstream>
#include <stdexcept>

namespace kfx::render::dx12 {
namespace {
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void Texel(Image& image, uint32_t x, uint32_t y, uint8_t value)
{
    const size_t p = (size_t(y) * image.width + x) * 4;
    image.rgba.at(p) = value; image.rgba.at(p + 3) = 255;
}
void Bytes(const std::filesystem::path& path, const std::vector<uint8_t>& pixels)
{
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(pixels.data()), std::streamsize(pixels.size()));
    Require(bool(file), "Cannot retain shadow edge diagnostic pixels");
}
uint8_t Fade(uint8_t destination, uint8_t shade) { return uint8_t(destination + shade * 7 + 11); }
bool Covered(uint32_t u, uint32_t v) { return u == 0 || ((u * 3 + v * 5) % 11 == 0); }
}
void RunShadowEdgeTests(Device& device, const std::filesystem::path& output)
{
    std::filesystem::create_directories(output);
    // SOFTWARE-20261012-230643-348/level-1/scene-4-684: viewport
    // [753,0,3087,2160]. Submitted coordinates are already window-local.
    constexpr uint32_t width = 3087, height = 2160;
    constexpr uint32_t edge_x = 1943, edge_y = 745;
    constexpr size_t edge = size_t(edge_y) * width + edge_x;
    const bool rov = device.GetDiagnostics().rasterizer_ordered_views_supported;
    uint64_t checks = 0;
    bool rov_tested = false, invalid_rejected = false;
    std::string error;
    std::array<unsigned, 3> edge_probe{};
    unsigned edge_result = 0;
    try {
        const std::vector<uint8_t> zero(size_t(width) * height), background(zero.size(), 81);
        Image atlas{256, 352, std::vector<uint8_t>(size_t(256) * 352 * 4)};
        for (uint32_t shade = 0; shade < 64; ++shade) for (uint32_t index = 0; index < 256; ++index)
            Texel(atlas,index,256+shade,Fade(uint8_t(index),uint8_t(shade)));
        for (uint32_t v = 0; v < 32; ++v) for (uint32_t u = 0; u < 29; ++u)
            Texel(atlas,u,320+v,Covered(u,v)?255:0);
        PalettePrimitive shadow;
        shadow.kind = PalettePrimitiveKind::Shadow;
        shadow.shadow.vertices = {{{1971,846,0,31},{1916,645,0,0},{2019,630,28,0}}};
        shadow.shadow.mask_y = 320; shadow.shadow.mask_width = 29; shadow.shadow.mask_height = 32;
        shadow.shadow.fade_y = 256; shadow.shadow.shade = 29;
        PalettePrimitive probe;
        probe.kind = PalettePrimitiveKind::Terrain;
        probe.terrain.texture_width = probe.terrain.texture_height = 256;
        probe.terrain.apply_fade = false; probe.terrain.clamp_to_vertices = true; probe.terrain.fade_y = 256;
        for (size_t p = 0; p < 3; ++p) {
            const auto& v = shadow.shadow.vertices[p];
            probe.terrain.vertices[p] = {v.x,v.y,v.u,v.v,0};
        }
        auto render = [&](const Image& image, const PalettePrimitive& primitive, const std::vector<uint8_t>& bg, bool ordered) {
            const auto before = device.GetDiagnostics();
            auto pixels = device.CompositeIndexed(width,height,bg,image,{primitive},ordered);
            const auto after = device.GetDiagnostics();
            Require(before.allocation_count == after.allocation_count && before.allocation_bytes == after.allocation_bytes,
                "Shadow edge test retained GPU allocations");
            return pixels;
        };
        auto exact_paths = [&](const char* name, const PalettePrimitive& primitive, const std::vector<uint8_t>& bg) {
            auto serial = render(atlas,primitive,bg,false);
            Bytes(output/(std::string("shadow-edge-")+name+".idx"),serial);
            if (rov) {
                const auto ordered = render(atlas,primitive,bg,true);
                rov_tested = true; checks += serial.size();
                if (serial != ordered) Bytes(output/(std::string("shadow-edge-")+name+"-rov.idx"),ordered);
                Require(serial == ordered,"Shadow edge ROV/serial mismatch");
            }
            return serial;
        };
        std::array<std::vector<uint8_t>, 3> samples;
        constexpr std::array<const char*,3> names{"geometry","u","v"};
        for (size_t channel = 0; channel < 3; ++channel) {
            for (uint32_t v = 0; v < 256; ++v) for (uint32_t u = 0; u < 256; ++u)
                Texel(atlas,u,v,channel == 0 ? 255 : channel == 1 ? uint8_t(u) : uint8_t(v));
            samples[channel] = exact_paths(names[channel],probe,zero);
            edge_probe[channel] = samples[channel][edge];
        }
        // Pixel centre (1943.5,745.5) is exactly the midpoint of the U=0
        // edge. Its affine V is 15.5, so the wrapped integer sample is (0,15).
        Require(edge_probe == std::array<unsigned,3>{255,0,15}, "Captured shadow edge must cover UV (0,15)");
        auto expected = background;
        for (size_t p = 0; p < expected.size(); ++p) if (samples[0][p]) {
            Require(samples[0][p] == 255 && samples[1][p] < 29 && samples[2][p] < 32,
                "Vertex-range shadow probe left initialized mask bounds");
            if (Covered(samples[1][p],samples[2][p])) expected[p] = Fade(background[p],29);
        }
        const auto actual = exact_paths("masked-fade",shadow,background);
        edge_result = actual[edge]; checks += actual.size();
        if (actual != expected) Bytes(output/"shadow-edge-expected.idx",expected);
        Require(actual == expected,"Captured shadow edge mask/fade differs from independent UV lookup");
        Require(actual[edge] == Fade(81,29) && actual[edge] != background[edge],"Known captured edge pixel was not shadowed");
        // A constant, genuinely invalid coordinate is its own vertex range.
        // Vertex-range repair must not become clamping to the mask rectangle.
        auto invalid = shadow;
        for (auto& v : invalid.shadow.vertices) v.u = 29;
        for (bool ordered : {false,true}) {
            if (ordered && !rov) continue;
            bool rejected = false;
            const auto before = device.GetDiagnostics();
            try { device.CompositeIndexed(width,height,background,atlas,{invalid},ordered); }
            catch (const std::runtime_error& failure) {
                rejected = std::string(failure.what()).find("shadow sample outside") != std::string::npos;
                if (!rejected) throw;
            }
            const auto after = device.GetDiagnostics();
            Require(rejected && before.allocation_count == after.allocation_count && before.allocation_bytes == after.allocation_bytes,
                "Constant invalid UV was hidden by shadow edge correction");
        }
        invalid_rejected = true;
    } catch (const std::exception& failure) { error = failure.what(); }
    std::ofstream report(output/"shadow-edge.json");
    report << "{\"schema\":1,\"passed\":" << (error.empty()?"true":"false")
        << ",\"viewport\":[3087,2160],\"source_viewport_origin\":[753,0],\"mask\":[29,32],\"shade\":29"
        << ",\"vertices_xyuv\":[[1971,846,0,31],[1916,645,0,0],[2019,630,28,0]]"
        << ",\"edge_pixel\":[1943,745],\"edge_geometry_uv\":[" << edge_probe[0] << ',' << edge_probe[1] << ',' << edge_probe[2]
        << "],\"edge_output\":" << edge_result << ",\"expected_edge_output\":" << unsigned(Fade(81,29))
        << ",\"constant_bad_uv_rejected\":" << (invalid_rejected?"true":"false")
        << ",\"rov_supported\":" << (rov?"true":"false") << ",\"rov_tested\":" << (rov_tested?"true":"false")
        << ",\"indexed_checks\":" << checks << ",\"rasterComparisonAccepted\":false,\"error\":\"";
    for (char c : error) report << (c == '"' || c == '\\' || c < 32 ? '?' : c);
    report << "\"}\n";
    Require(bool(report),"Cannot retain shadow edge report");
    if (!error.empty()) throw std::runtime_error(error);
}
}
