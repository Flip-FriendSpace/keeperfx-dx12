// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#include "LegacyTerrainOracle.h"
#include "LegacyTerrainOracleShim.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <stdexcept>
#include <string>

namespace kfx::render::dx12 {
namespace {
std::atomic_flag active = ATOMIC_FLAG_INIT;
constexpr size_t TextureSpan = 65536; // Also covers Trig7 colour-32's full 8-bit V.
constexpr uint32_t ViewportLimit = 4096;
struct Invocation {
    Invocation()
    {
        if (active.test_and_set()) throw std::runtime_error("Legacy terrain oracle is already running");
    }
    ~Invocation() { LegacyTerrainOracleClear(); LegacyTerrainOracleClearTrig(); active.clear(); }
};
void Require(bool condition, const char* reason)
{
    if (!condition) throw std::runtime_error(std::string("Legacy terrain oracle: ") + reason);
}
void Validate(const SceneTerrainTriangle& triangle, const SceneAssets& assets,
    uint32_t width, uint32_t height)
{
    const bool gpoly = triangle.rasterizer == SceneRasterizer::Gpoly && triangle.mode == 5;
    const bool trig = triangle.rasterizer == SceneRasterizer::Trig && triangle.mode == 7;
    Require(gpoly || trig, "only original Gpoly mode 5 and Trig mode 7 are supported");
    Require(!trig || triangle.colour < 64, "Trig constant shade exceeds owned fade rows");
    Require(width && height && width <= ViewportLimit && height <= ViewportLimit,
        "invalid viewport dimensions");
    const size_t texture_span = gpoly ? 31 * 256 + 32 : triangle.colour == 32 ? 65536 : 8192;
    Require(uint64_t(triangle.texture_offset) + texture_span <= assets.texture_pixels.size(),
        "texture reference exceeds owned original 256-byte-pitch bank");
    for (const auto& v : triangle.vertices) {
        // Bound all absolute edge accumulators as well as the differences
        // checked by the original routine. Do not feed it hostile shifts or
        // products and rely on the original's later validation to catch them.
        Require(v.x >= -16383 && v.x <= 16383 && v.y >= -16383 && v.y <= 16383,
            "vertex exceeds safe original fixed-point coordinate range");
        Require(v.u >= -4096 * 65536 && v.u <= 4096 * 65536
            && v.v >= -4096 * 65536 && v.v <= 4096 * 65536,
            "texture coordinate exceeds bounded original arithmetic range");
        Require(gpoly ? (v.s >= 0 && v.s <= 64 * 65536 - 1) : v.s == 0,
            "vertex shade lies outside captured fade rows");
    }
    for (size_t i = 0; i < 3; ++i) for (size_t j = i + 1; j < 3; ++j) {
        const int64_t dx = int64_t(triangle.vertices[i].x) - triangle.vertices[j].x;
        const int64_t dy = int64_t(triangle.vertices[i].y) - triangle.vertices[j].y;
        Require(dx >= -16383 && dx <= 16383 && dy >= -16383 && dy <= 16383,
            "triangle extent exceeds original arithmetic bound");
    }
}
struct Tables {
    // Gpoly extracts an 8-bit shade row. Allocate the entire possible address
    // range even for malformed interpolation; measured rows >=64 then fail.
    std::array<uint8_t, 65536> actual{}, identity{}, shade{};
    std::array<uint8_t, TextureSpan> u{}, v{};
    explicit Tables(const SceneAssets& assets)
    {
        std::copy(assets.fade.begin(), assets.fade.end(), actual.begin());
        for (size_t row = 0; row < 256; ++row) for (size_t index = 0; index < 256; ++index) {
            identity[row * 256 + index] = static_cast<uint8_t>(index);
            shade[row * 256 + index] = static_cast<uint8_t>(row);
        }
        for (size_t y = 0; y < 256; ++y) for (size_t x = 0; x < 256; ++x) {
            u[y * 256 + x] = static_cast<uint8_t>(x);
            v[y * 256 + x] = static_cast<uint8_t>(y);
        }
    }
};
void DrawInto(const SceneTerrainTriangle& triangle, uint32_t width, uint32_t height,
    uint8_t* target, const uint8_t* texture, const uint8_t* fade)
{
    std::array<LegacyTerrainPoint, 3> points{};
    for (size_t i = 0; i < points.size(); ++i) {
        const auto& v = triangle.vertices[i];
        points[i] = {v.x, v.y, v.u, v.v, v.s};
    }
    // A fully below-window triangle would form a far-out initial row pointer
    // although it writes nothing. Skip that provably empty case before C.
    if (!std::all_of(triangle.vertices.begin(), triangle.vertices.end(),
            [height](const auto& v) { return int64_t(v.y) >= height; })) {
        if (triangle.rasterizer == SceneRasterizer::Trig) {
            const int result = LegacyTerrainOracleDrawTrig(points.data(), target, width, height, texture, fade, triangle.colour);
            Require(result != -2, "original Trig has undefined texture steps for zero rounded horizontal extent");
            Require(result == 0, "original Trig changed its scan-buffer guard");
        }
        else LegacyTerrainOracleDraw(points.data(), target, width, height, texture, fade);
    }
}
void CheckGuards(const std::vector<uint8_t>& storage, size_t guard)
{
    Require(std::all_of(storage.begin(), storage.begin() + guard,
            [](uint8_t x) { return x == 0xa7; })
        && std::all_of(storage.end() - guard, storage.end(),
            [](uint8_t x) { return x == 0xa7; }), "original terrain rasterizer changed a framebuffer guard");
}
std::vector<uint8_t> Render(const SceneTerrainTriangle& triangle,
    uint32_t width, uint32_t height, const std::vector<uint8_t>& background,
    const uint8_t* texture, const uint8_t* fade)
{
    Require(background.size() == size_t(width) * height, "background size differs from viewport");
    // The original forms row pointers even for empty clipped spans. Keep
    // those transient pointers inside owned allocation, then detect stores.
    const size_t guard = 32768 + size_t(width) * 2;
    constexpr uint8_t sentinel = 0xa7;
    std::vector<uint8_t> storage(background.size() + 2 * guard, sentinel);
    std::copy(background.begin(), background.end(), storage.begin() + guard);
    DrawInto(triangle, width, height, storage.data() + guard, texture, fade);
    CheckGuards(storage, guard);
    return {storage.begin() + guard, storage.end() - guard};
}
std::vector<uint8_t> MeasureShade(const SceneTerrainTriangle& triangle,
    const SceneAssets& assets, uint32_t width, uint32_t height, const Tables& tables)
{
    // Uncovered zero is valid here: only covered rows can introduce >=64.
    auto shade = Render(triangle, width, height, std::vector<uint8_t>(size_t(width) * height),
        assets.texture_pixels.data() + triangle.texture_offset, tables.shade.data());
    Require(std::all_of(shade.begin(), shade.end(), [](uint8_t x) { return x < 64; }),
        "original interpolation addressed an uncaptured fade row");
    return shade;
}
}
std::vector<uint8_t> ReplayLegacyTerrainTriangle(const SceneTerrainTriangle& triangle,
    const SceneAssets& assets, uint32_t width, uint32_t height,
    const std::vector<uint8_t>& background)
{
    Invocation invocation;
    Validate(triangle, assets, width, height);
    Require(background.size() == size_t(width) * height, "background size differs from viewport");
    const Tables tables(assets);
    if (triangle.rasterizer == SceneRasterizer::Gpoly)
        MeasureShade(triangle, assets, width, height, tables);
    return Render(triangle, width, height, background,
        assets.texture_pixels.data() + triangle.texture_offset, tables.actual.data());
}
std::vector<uint8_t> ReplayLegacyTerrainTriangles(std::span<const SceneTerrainTriangle> triangles,
    const SceneAssets& assets, uint32_t width, uint32_t height,
    const std::vector<uint8_t>& background)
{
    Invocation invocation;
    Require(width && height && width <= ViewportLimit && height <= ViewportLimit
        && background.size() == size_t(width) * height, "invalid viewport/background");
    Require(triangles.size() <= 262144, "too many terrain commands");
    const Tables tables(assets);
    const size_t guard = 32768 + size_t(width) * 2;
    std::vector<uint8_t> target(background.size() + 2 * guard, 0xa7);
    std::copy(background.begin(), background.end(), target.begin() + guard);
    std::vector<uint8_t> shade(target.size(), 0xa7);
    std::fill(shade.begin() + guard, shade.end() - guard, 0);
    for (size_t command = 0; command < triangles.size(); ++command) {
        try {
            const auto& triangle = triangles[command];
            Validate(triangle, assets, width, height);
            const auto* texture = assets.texture_pixels.data() + triangle.texture_offset;
            if (triangle.rasterizer == SceneRasterizer::Gpoly) {
                DrawInto(triangle, width, height, shade.data() + guard, texture, tables.shade.data());
                CheckGuards(shade, guard);
            }
            int left = triangle.vertices[0].x, right = left;
            int top = triangle.vertices[0].y, bottom = top;
            for (const auto& vertex : triangle.vertices) {
                left = std::min(left, vertex.x); right = std::max(right, vertex.x);
                top = std::min(top, vertex.y); bottom = std::max(bottom, vertex.y);
            }
            left = std::clamp(left - 1, 0, int(width));
            right = std::clamp(right + 1, 0, int(width));
            top = std::clamp(top - 1, 0, int(height));
            bottom = std::clamp(bottom + 1, 0, int(height));
            // Previous validated shade values remain <64, so no framebuffer
            // clearing is needed. Gpoly's bounded scanlines lie inside this
            // conservative projected-vertex rectangle; check every new row
            // before the next command could overwrite an invalid sample.
            for (int y = top; y < bottom; ++y) {
                const auto begin = shade.begin() + guard + size_t(y) * width;
                Require(std::all_of(begin + left, begin + right, [](uint8_t x) { return x < 64; }),
                    "original interpolation addressed an uncaptured fade row");
            }
            DrawInto(triangle, width, height, target.data() + guard, texture, tables.actual.data());
            CheckGuards(target, guard);
        } catch (const std::runtime_error& error) {
            throw std::runtime_error("Legacy terrain command " + std::to_string(command) + ": " + error.what());
        }
    }
    return {target.begin() + guard, target.end() - guard};
}
LegacyTerrainInspection InspectLegacyTerrainTriangle(const SceneTerrainTriangle& triangle,
    const SceneAssets& assets, uint32_t width, uint32_t height)
{
    Invocation invocation;
    Validate(triangle, assets, width, height);
    const Tables tables(assets);
    LegacyTerrainInspection inspection;
    if (triangle.rasterizer == SceneRasterizer::Gpoly)
        inspection.shade = MeasureShade(triangle, assets, width, height, tables);
    else inspection.shade.resize(size_t(width) * height);
    std::vector<uint8_t> background(size_t(width) * height);
    const auto* texture = assets.texture_pixels.data() + triangle.texture_offset;
    inspection.indices = Render(triangle, width, height, background, texture, tables.actual.data());
    inspection.sample_u = Render(triangle, width, height, background, tables.u.data(), tables.identity.data());
    inspection.sample_v = Render(triangle, width, height, background, tables.v.data(), tables.identity.data());
    std::fill(background.begin(), background.end(), 255);
    const auto second = Render(triangle, width, height, background, texture, tables.actual.data());
    inspection.coverage.resize(background.size());
    for (size_t i = 0; i < background.size(); ++i) {
        inspection.coverage[i] = inspection.indices[i] == second[i] ? 255 : 0;
        if (triangle.rasterizer == SceneRasterizer::Trig && inspection.coverage[i])
            inspection.shade[i] = triangle.colour; // Captured constant; colour 32 bypasses fade.
    }
    return inspection;
}
}
