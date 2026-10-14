// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstddef>
#include <span>
#include <stdexcept>

namespace kfx::render::dx12 {
struct RuntimeLightmapRect {
    uint32_t x = 0, y = 0, width = 0, height = 0;
    bool Valid() const noexcept
    {
        return x < 511 && y < 511 && width && height && width <= 511 - x && height <= 511 - y;
    }
};
struct RuntimeLightmapUpdatePlan {
    std::array<RuntimeLightmapRect, 256> rectangles{};
    uint32_t count = 0;
    uint64_t upload_bytes = 0;
    bool bounding_box_fallback = false;
};

// Compare the selected GPU image's immutable baseline, never just the last
// published revision. Empty baseline means an uninitialized image. Results are
// deterministic: horizontal changed runs merge vertically only when x/width
// match. If the 256-rectangle bound is exceeded, upload the dirty bounding box,
// including its unchanged interior. The scan and result allocate no storage.
inline RuntimeLightmapUpdatePlan PlanRuntimeLightmapUpdates(std::span<const uint8_t> baseline,
    std::span<const uint8_t> target)
{
    constexpr uint32_t side = 511;
    constexpr uint64_t bytes = uint64_t(side) * side * 2;
    if (target.size() != bytes || (!baseline.empty() && baseline.size() != bytes))
        throw std::invalid_argument("Runtime lightmap snapshots must be511x511 R16");
    RuntimeLightmapUpdatePlan result;
    if (baseline.empty()) {
        result.rectangles[0] = {0, 0, side, side}; result.count = 1; result.upload_bytes = bytes;
        return result;
    }
    uint32_t left = side, top = side, right = 0, bottom = 0;
    const auto changed = [&](uint32_t x, uint32_t y) {
        const std::size_t offset = (std::size_t(y) * side + x) * 2;
        return baseline[offset] != target[offset] || baseline[offset + 1] != target[offset + 1];
    };
    for (uint32_t y = 0; y < side; ++y) {
        for (uint32_t x = 0; x < side;) {
            if (!changed(x, y)) { ++x; continue; }
            const uint32_t first = x++;
            while (x < side && changed(x, y)) ++x;
            const uint32_t width = x - first;
            left = std::min(left, first); top = std::min(top, y);
            right = std::max(right, x); bottom = y + 1;
            if (result.bounding_box_fallback) continue;
            bool merged = false;
            for (uint32_t i = 0; i < result.count; ++i) {
                auto& rect = result.rectangles[i];
                if (rect.x == first && rect.width == width && rect.y + rect.height == y) {
                    ++rect.height; merged = true; break;
                }
            }
            if (!merged) {
                if (result.count == result.rectangles.size()) result.bounding_box_fallback = true;
                else result.rectangles[result.count++] = {first, y, width, 1};
            }
        }
    }
    if (result.bounding_box_fallback) {
        result.rectangles[0] = {left, top, right - left, bottom - top}; result.count = 1;
    }
    for (uint32_t i = 0; i < result.count; ++i)
        result.upload_bytes += uint64_t(result.rectangles[i].width) * result.rectangles[i].height * 2;
    return result;
}
} // namespace kfx::render::dx12
