// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#include "LegacyShadowOracle.h"
#include "LegacyTerrainOracleShim.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <stdexcept>
#include <string>

namespace kfx::render::dx12 {
namespace {
std::atomic_flag active = ATOMIC_FLAG_INIT;
struct Invocation {
    Invocation() { if (active.test_and_set()) throw std::runtime_error("Legacy shadow oracle already running"); }
    ~Invocation() { active.clear(); }
};
void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(std::string("Legacy shadow oracle: ") + message);
}
void ValidateTriangle(const SceneShadowTriangle& triangle)
{
    Require(triangle.shade >= 16 && triangle.shade <= 31, "shade outside active shadow range 16..31");
    for (const auto& p : triangle.vertices) {
        Require(p.x >= -16383 && p.x <= 16383 && p.y >= -16383 && p.y <= 16383,
            "position outside bounded original fixed-point range");
        Require(p.u >= -4096 * 65536 && p.u <= 4096 * 65536 && p.v >= -4096 * 65536 && p.v <= 4096 * 65536,
            "UV outside bounded original arithmetic range");
        Require(p.s == 0, "ignored vertex S must be canonical zero");
    }
    for (size_t i = 0; i < 3; ++i) for (size_t j = i + 1; j < 3; ++j) {
        const auto& a = triangle.vertices[i]; const auto& b = triangle.vertices[j];
        Require(int64_t(a.x) - b.x >= -16383 && int64_t(a.x) - b.x <= 16383
            && int64_t(a.y) - b.y >= -16383 && int64_t(a.y) - b.y <= 16383,
            "triangle extent outside bounded original arithmetic range");
    }
}
void ValidateMask(const SceneShadowMask& mask)
{
    Require(mask.width && mask.width <= 256 && mask.height && mask.height <= 256
        && mask.coverage.size() == size_t(mask.width) * mask.height, "invalid initialized mask extent");
    Require(std::all_of(mask.coverage.begin(), mask.coverage.end(), [](uint8_t x) { return x == 0 || x == 255; }),
        "shadow mask is not binary initialized coverage");
}
struct Frame {
    uint32_t width, height;
    size_t guard;
    std::vector<uint8_t> storage;
    Frame(uint32_t w, uint32_t h) : width(w), height(h), guard(32768 + size_t(w) * 2),
        storage(size_t(w) * h + guard * 2, 0xa7)
    {
        std::fill(storage.begin() + guard, storage.end() - guard, 0);
    }
    uint8_t* Data() { return storage.data() + guard; }
    std::vector<uint8_t> Pixels() const { return {storage.begin() + guard, storage.end() - guard}; }
    void Check() const
    {
        Require(std::all_of(storage.begin(), storage.begin() + guard, [](uint8_t x) { return x == 0xa7; })
            && std::all_of(storage.end() - guard, storage.end(), [](uint8_t x) { return x == 0xa7; }),
            "original shadow rasterizer changed a framebuffer guard");
    }
};
struct Bounds { int left, top, right, bottom; };
Bounds Box(const SceneShadowTriangle& triangle, uint32_t width, uint32_t height)
{
    Bounds b{triangle.vertices[0].x, triangle.vertices[0].y, triangle.vertices[0].x, triangle.vertices[0].y};
    for (const auto& p : triangle.vertices) {
        b.left = std::min(b.left, p.x); b.right = std::max(b.right, p.x);
        b.top = std::min(b.top, p.y); b.bottom = std::max(b.bottom, p.y);
    }
    b.left = std::clamp(b.left - 1, 0, int(width)); b.right = std::clamp(b.right + 1, 0, int(width));
    b.top = std::clamp(b.top - 1, 0, int(height)); b.bottom = std::clamp(b.bottom + 1, 0, int(height));
    return b;
}
void Draw(const SceneShadowTriangle& triangle, Frame& frame,
    const uint8_t* texture, const uint8_t* fade, bool probe)
{
    std::array<LegacyTerrainPoint, 3> points{};
    for (size_t i = 0; i < 3; ++i) {
        const auto& p = triangle.vertices[i]; points[i] = {p.x,p.y,p.u,p.v,0};
    }
    const int status = LegacyShadowOracleDrawTrig(points.data(), frame.Data(), frame.width, frame.height,
        texture, fade, triangle.shade, probe ? 1 : 0);
    Require(status != -2, "original Trig has undefined texture steps for zero rounded horizontal extent");
    Require(status != -3, "original Trig kernel is already running");
    Require(status == 0, "original Trig changed its scan-buffer guard");
    frame.Check();
}
struct Replay {
    uint32_t width, height;
    Frame target, geometry, u, v;
    std::array<uint8_t, 65536> solid{}, u_ramp{}, v_ramp{}, mask{};
    std::array<uint8_t, 16384> fade{}, mark{};
    Replay(uint32_t w, uint32_t h, const SceneAssets& assets, const std::vector<uint8_t>& background)
        : width(w), height(h), target(w,h), geometry(w,h), u(w,h), v(w,h)
    {
        std::copy(background.begin(), background.end(), target.Data());
        fade = assets.fade; mark.fill(255); solid.fill(255);
        for (size_t y = 0; y < 256; ++y) for (size_t x = 0; x < 256; ++x) {
            u_ramp[y * 256 + x] = uint8_t(x); v_ramp[y * 256 + x] = uint8_t(y);
        }
    }
    void LoadMask(const SceneShadowMask& source)
    {
        ValidateMask(source);
        mask.fill(0);
        for (uint32_t y = 0; y < source.height; ++y)
            std::copy_n(source.coverage.begin() + size_t(y) * source.width, source.width, mask.begin() + size_t(y) * 256);
    }
    void Probe(const SceneShadowTriangle& triangle, const SceneShadowMask& source)
    {
        ValidateTriangle(triangle);
        const auto box = Box(triangle, width, height);
        // Original Trig clips scanlines inside the projected vertex bounds.
        // Clear only this conservative rectangle; U/V are overwritten at
        // every currently covered pixel, so their other values are irrelevant.
        for (int y = box.top; y < box.bottom; ++y)
            std::fill(geometry.Data() + size_t(y) * width + box.left,
                geometry.Data() + size_t(y) * width + box.right, 0);
        Draw(triangle, geometry, solid.data(), fade.data(), true);
        Draw(triangle, u, u_ramp.data(), fade.data(), true);
        Draw(triangle, v, v_ramp.data(), fade.data(), true);
        for (int y = box.top; y < box.bottom; ++y) for (int x = box.left; x < box.right; ++x) {
            const size_t p = size_t(y) * width + x;
            if (!geometry.Data()[p]) continue;
            if (u.Data()[p] >= source.width || v.Data()[p] >= source.height)
                throw std::runtime_error("Legacy shadow oracle: uninitialized mask sample at viewport ("
                    + std::to_string(x) + "," + std::to_string(y) + "), UV ("
                    + std::to_string(u.Data()[p]) + "," + std::to_string(v.Data()[p]) + "), initialized extent "
                    + std::to_string(source.width) + "x" + std::to_string(source.height));
        }
    }
    void Submit(const SceneShadowTriangle& triangle, const SceneShadowMask& source)
    {
        Probe(triangle, source);
        // Only after geometric address validation may original mode 10 read
        // the owned mask. Zero padding is storage, never parity evidence.
        Draw(triangle, target, mask.data(), fade.data(), false);
    }
};
void ValidateTarget(uint32_t width, uint32_t height, size_t bytes)
{
    Require(width && height && width <= 4096 && height <= 4096 && bytes == size_t(width) * height,
        "invalid viewport/background");
}
}
std::vector<uint8_t> ReplayLegacyShadowTriangle(const SceneShadowTriangle& triangle,
    const SceneShadowMask& mask, const SceneAssets& assets,
    uint32_t width, uint32_t height, const std::vector<uint8_t>& background)
{
    Invocation invocation;
    ValidateTarget(width, height, background.size());
    Replay replay(width, height, assets, background);
    replay.LoadMask(mask); replay.Submit(triangle, mask);
    return replay.target.Pixels();
}
std::vector<uint8_t> ReplayLegacyShadowTriangles(std::span<const SceneShadowTriangle> triangles,
    std::span<const SceneShadowMask> masks, const SceneAssets& assets,
    uint32_t width, uint32_t height, const std::vector<uint8_t>& background)
{
    Invocation invocation;
    ValidateTarget(width, height, background.size());
    Require(triangles.size() <= 262144 && masks.size() <= 65536, "shadow command or mask count exceeds bound");
    Replay replay(width, height, assets, background);
    size_t loaded = masks.size();
    for (size_t command = 0; command < triangles.size(); ++command) {
        try {
            const auto& triangle = triangles[command];
            Require(triangle.mask < masks.size(), "invalid shadow mask reference");
            const auto& mask = masks[triangle.mask];
            if (loaded != triangle.mask) { replay.LoadMask(mask); loaded = triangle.mask; }
            replay.Submit(triangle, mask);
        } catch (const std::exception& error) {
            throw std::runtime_error("Legacy shadow command " + std::to_string(command) + ": " + error.what());
        }
    }
    return replay.target.Pixels();
}
LegacyShadowInspection InspectLegacyShadowTriangle(const SceneShadowTriangle& triangle,
    const SceneShadowMask& mask, const SceneAssets& assets, uint32_t width, uint32_t height)
{
    Invocation invocation;
    ValidateTarget(width, height, size_t(width) * height);
    Replay replay(width, height, assets, std::vector<uint8_t>(size_t(width) * height));
    replay.LoadMask(mask); replay.Submit(triangle, mask);
    LegacyShadowInspection result;
    result.indices = replay.target.Pixels(); result.geometric_coverage = replay.geometry.Pixels();
    result.sample_u = replay.u.Pixels(); result.sample_v = replay.v.Pixels();
    Frame coverage(width,height);
    Draw(triangle, coverage, replay.mask.data(), replay.mark.data(), false);
    result.coverage = coverage.Pixels();
    return result;
}
}
