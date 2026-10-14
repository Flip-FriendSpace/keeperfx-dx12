// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#include "kfx/renderer/SceneSpriteReplay.h"
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace kfx::render {
namespace {
// bflib_vidraw.h: max(MAX_SUPPORTED_SPRITE_DIM, MAX_SUPPORTED_SCREEN_*).
constexpr int32_t XSteps = 3840, YSteps = 2160;
constexpr int64_t FixedOne = 65536;

struct Step { int32_t start, length; };

void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

// Arithmetic right shift without relying on signed-shift implementation details.
int64_t FloorFixed(int64_t value)
{
    return value >= 0 ? value / FixedOne : -((-value + FixedOne - 1) / FixedOne);
}

std::vector<Step> MakeSteps(int32_t origin, int32_t source, int32_t destination,
    int32_t window, int32_t capacity, bool vertical)
{
    Require(source > 0 && source <= capacity, "Sprite replay exceeds initialized legacy scaling table");
    Require(destination >= 0 && destination <= 16777216 && origin >= -16777216 && origin <= 16777216,
        "Sprite replay scale outside capture bounds");
    Require(window > 0 && window <= 16384, "Sprite replay window outside capture bounds");
    std::vector<Step> result(source);
    if (!destination) return result;

    // Windows long is 32 bits in BOTH supported architectures. Refuse input
    // whose legacy shifts/additions overflow rather than invent a 64-bit result.
    const int64_t scaled_destination = int64_t(destination) * FixedOne;
    const int64_t scaled_origin = int64_t(origin) * FixedOne;
    const int64_t factor = scaled_destination / source;
    const int64_t base = factor / 2 + scaled_origin;
    Require(scaled_destination <= std::numeric_limits<int32_t>::max()
        && scaled_origin >= std::numeric_limits<int32_t>::min()
        && scaled_origin <= std::numeric_limits<int32_t>::max()
        && base >= std::numeric_limits<int32_t>::min()
        && base + factor * source <= std::numeric_limits<int32_t>::max(),
        "Sprite replay would overflow legacy 16.16 scaling arithmetic");
    const bool clipped = origin < 0 || int64_t(destination) + source + origin >= window;
    int64_t position = FloorFixed(base);
    if (clipped) {
        position = std::min<int64_t>(position, std::max(0, origin));
        // HeightClippedArray additionally clamps the initial running position.
        if (vertical) position = std::clamp<int64_t>(position, 0, window);
    }
    int64_t accumulator = base;
    for (auto& step : result) {
        accumulator += factor;
        const int64_t end = FloorFixed(accumulator);
        const int64_t start_clipped = clipped ? std::clamp<int64_t>(position, 0, window) : position;
        const int64_t end_clipped = clipped ? std::max(start_clipped, std::clamp<int64_t>(end, 0, window)) : end;
        step = {int32_t(start_clipped), int32_t(end_clipped - start_clipped)};
        position = end; // Legacy advances by the UNCLIPPED difference.
    }
    return result;
}

std::vector<int32_t> MapAxis(const std::vector<Step>& steps, int32_t offset,
    int32_t content, int32_t window, bool reverse, bool scale_up, int32_t& origin)
{
    Require(offset >= 0 && int64_t(offset) + content <= int64_t(steps.size()),
        "Sprite replay content references uninitialized legacy scaling steps");
    std::vector<int32_t> mapping(window, -1);
    const int32_t first = offset + (reverse ? content - 1 : 0);
    int32_t destination = steps[first].start + (reverse ? steps[first].length - 1 : 0);
    const int32_t direction = reverse ? -1 : 1;
    for (int32_t source = 0; source < content; ++source) {
        const auto& step = steps[first + direction * source];
        const int32_t count = scale_up ? step.length : int32_t(step.length > 0);
        // The rasterizer advances its output cursor only for nonzero steps.
        // Clipped zero-length rows must not change source traversal or phase.
        for (int32_t i = 0; i < count; ++i) {
            Require(destination >= 0 && destination < window,
                "Sprite replay legacy cursor escaped its graphics window");
            mapping[destination] = source;
            destination += direction;
        }
    }
    const auto begin = std::find_if(mapping.begin(), mapping.end(), [](int32_t value) { return value >= 0; });
    if (begin == mapping.end()) { origin = 0; return {}; }
    const auto end = std::find_if(mapping.rbegin(), mapping.rend(), [](int32_t value) { return value >= 0; }).base();
    origin = int32_t(begin - mapping.begin());
    return {begin, end};
}
}

SceneSpriteMapping MapSceneSprite(const SceneSpriteDraw& draw, const SceneSprite& sprite)
{
    Require(sprite.width > 0 && sprite.height > 0 && sprite.width <= 1024 && sprite.height <= 1024
        && sprite.pixels.size() == size_t(sprite.width) * sprite.height * 2,
        "Sprite replay needs bounded owned index and coverage pixels");
    Require(draw.content_height > 0 && draw.content_height <= sprite.height,
        "Sprite replay source height outside owned sprite");
    Require(draw.has_remap == bool(draw.flags & 0x0800)
        && draw.blend == ResolveSceneSpriteBlend(draw.flags, draw.alpha),
        "Sprite replay blend disagrees with captured draw state");
    const auto& s = draw.scale;
    const auto xsteps = MakeSteps(s.frame_x, s.frame_src_w, s.frame_dst_w, s.window_w, XSteps, false);
    const auto ysteps = MakeSteps(s.frame_y, s.frame_src_h, s.frame_dst_h, s.window_h, YSteps, true);
    const bool scale_up = s.frame_dst_w > s.frame_src_w || s.frame_dst_h > s.frame_src_h;
    SceneSpriteMapping result;
    result.source_x = MapAxis(xsteps, s.content_x, sprite.width, s.window_w, draw.flags & 1, scale_up, result.x);
    result.source_y = MapAxis(ysteps, s.content_y, draw.content_height, s.window_h, draw.flags & 2, scale_up, result.y);
    if (scale_up && (draw.flags & 1)
        && (draw.blend == SceneSpriteBlend::Opaque || draw.blend == SceneSpriteBlend::Remap)) {
        for (int32_t y = 0; y < draw.content_height; ++y) {
            const int32_t step = s.content_y + ((draw.flags & 2) ? draw.content_height - y - 1 : y);
            if (ysteps[step].length <= 1) continue;
            for (int32_t x = 0; x < sprite.width; ++x) {
                if (sprite.pixels[(size_t(y) * sprite.width + x) * 2 + 1]) {
                    result.legacy_row_copy_quirk = true;
                    break;
                }
            }
            if (result.legacy_row_copy_quirk) break;
        }
    }
    if (result.source_x.empty() || result.source_y.empty()) {
        result.x = result.y = 0;
        result.source_x.clear(); result.source_y.clear();
    }
    return result;
}
}
