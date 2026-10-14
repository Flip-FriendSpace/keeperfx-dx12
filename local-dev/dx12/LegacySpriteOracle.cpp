// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#include "LegacySpriteOracle.h"
#include "LegacySpriteOracleShim.h"
#include <algorithm>
#include <atomic>
#include <limits>
#include <stdexcept>
#include <string>

namespace kfx::render::dx12 {
namespace {
std::atomic_flag active = ATOMIC_FLAG_INIT;
struct Invocation {
    Invocation()
    {
        if (active.test_and_set()) throw std::runtime_error("Legacy sprite oracle is already running");
    }
    ~Invocation() { LegacySpriteOracleClear(); active.clear(); }
};
void Require(bool condition, const char* reason)
{
    if (!condition) throw std::runtime_error(std::string("Legacy sprite oracle: ") + reason);
}
void ValidateAxis(int32_t origin, int32_t source, int32_t destination,
    int32_t offset, int32_t content, int32_t capacity)
{
    Require(source > 0 && source <= capacity, "frame exceeds original scale table");
    Require(offset >= 0 && content > 0 && int64_t(offset) + content <= source,
        "content lies outside initialized scale table");
    Require(destination >= 0 && destination <= 32767, "destination exceeds signed 16.16 range");
    Require(origin >= -32768 && origin <= 32767, "origin exceeds signed 16.16 range");
    // Windows C long is 32 bits. Validate the entire initialized frame, not
    // only its content crop, before entering the original 16.16 arithmetic.
    const int64_t factor = (int64_t(destination) * 65536) / source;
    const int64_t first = int64_t(origin) * 65536 + factor / 2;
    const int64_t last = first + int64_t(source) * factor;
    Require(first >= std::numeric_limits<int32_t>::min()
        && last <= std::numeric_limits<int32_t>::max(), "scale accumulator would overflow");
}
std::vector<uint8_t> EncodeRle(const SceneSprite& sprite)
{
    Require(sprite.width && sprite.height && sprite.width <= 1024 && sprite.height <= 1024
        && sprite.pixels.size() == size_t(sprite.width) * sprite.height * 2,
        "invalid source image");
    std::vector<uint8_t> result;
    result.reserve(sprite.pixels.size() + sprite.height);
    for (uint32_t y = 0; y < sprite.height; ++y) {
        const size_t row = size_t(y) * sprite.width * 2;
        uint32_t end = sprite.width;
        for (uint32_t x = 0; x < sprite.width; ++x)
            Require(sprite.pixels[row + x * 2 + 1] == 0
                || sprite.pixels[row + x * 2 + 1] == 255, "invalid source coverage");
        // Short rows are legal. Omit transparent tails: the original skip
        // code dereferences the next scale entry, including after a skip.
        while (end && !sprite.pixels[row + (end - 1) * 2 + 1]) --end;
        for (uint32_t x = 0; x < end;) {
            const bool covered = sprite.pixels[row + x * 2 + 1] != 0;
            const uint32_t start = x++;
            while (x < end && x - start < 127
                && (sprite.pixels[row + x * 2 + 1] != 0) == covered) ++x;
            const int run = static_cast<int>(x - start);
            result.push_back(static_cast<uint8_t>(covered ? run : -run));
            if (covered) for (uint32_t p = start; p < x; ++p)
                result.push_back(sprite.pixels[row + p * 2]);
        }
        result.push_back(0);
    }
    return result;
}
}
std::vector<uint8_t> ReplayLegacySprites(const SceneCapture& capture,
    const std::vector<uint8_t>& background)
{
    Require(capture.format_version >= 3 && capture.trace.complete
        && ValidateSceneCapture(capture), "complete validated KFXC v3 capture required");
    return ReplayLegacySprites(capture, background, capture.trace.draws);
}

static LegacySpriteOracleResult ReplayOracle(const SceneCapture& capture,
    const std::vector<uint8_t>& background, std::span<const SceneSpriteDraw> draws,
    bool corrected, bool detailed = false)
{
    Invocation invocation;
    const auto width = capture.scene.camera.viewport_w;
    const auto height = capture.scene.camera.viewport_h;
    Require(width > 0 && width <= 16384 && height > 0 && height <= 16384
        && background.size() == size_t(width) * height, "invalid viewport/background");
    Require(capture.assets.additive.size() == 65536, "missing additive table");
    Require(draws.size() <= 65536, "too many sprite commands");

    // Flipped clipped rows can transiently form pointers one pixel/row outside
    // the viewport. Keep those within owned storage and detect actual writes.
    const size_t guard = size_t(width) * 2 + 16;
    constexpr uint8_t sentinel = 0xa7;
    std::vector<uint8_t> storage(background.size() + 2 * guard, sentinel);
    std::copy(background.begin(), background.end(), storage.begin() + guard);
    LegacySpriteOracleTarget(storage.data() + guard, width, height);
    LegacySpriteOracleResult output;
    std::vector<uint8_t> guard_before(guard * 2);
    std::vector<std::vector<uint8_t>> encoded(capture.assets.sprites.size());
    for (size_t i = 0; i < draws.size(); ++i) {
        const auto& draw = draws[i];
        Require(draw.sprite < capture.assets.sprites.size(), "invalid sprite reference");
        const auto& sprite = capture.assets.sprites[draw.sprite];
        const auto& s = draw.scale;
        Require(s.window_w == width && s.window_h == height, "scale window differs from viewport");
        Require(draw.content_height > 0 && draw.content_height <= sprite.height, "invalid source row crop");
        Require(draw.flags <= 65535 && draw.has_remap == bool(draw.flags & 0x0800)
            && draw.blend == ResolveSceneSpriteBlend(draw.flags, draw.alpha), "inconsistent draw state");
        ValidateAxis(s.frame_x, s.frame_src_w, s.frame_dst_w,
            s.content_x, sprite.width, LegacySpriteOracleXSteps());
        ValidateAxis(s.frame_y, s.frame_src_h, s.frame_dst_h,
            s.content_y, draw.content_height, LegacySpriteOracleYSteps());
        if (encoded[draw.sprite].empty()) encoded[draw.sprite] = EncodeRle(sprite);
        std::copy_n(storage.begin(), guard, guard_before.begin());
        std::copy_n(storage.end() - guard, guard, guard_before.begin() + guard);
        const bool row_copy_candidate = !draw.alpha && (draw.flags & 0x0001)
            && (s.frame_dst_w > s.frame_src_w || s.frame_dst_h > s.frame_src_h)
            && (draw.blend == SceneSpriteBlend::Opaque || draw.blend == SceneSpriteBlend::Remap);
        // Only diagnostic original replay snapshots eligible commands. If a
        // canary changes, rerun the corrected C on exactly this preimage.
        std::vector<uint8_t> preimage;
        if (detailed && row_copy_candidate) preimage = storage;
        const int result = LegacySpriteOracleDraw(&s, encoded[draw.sprite].data(),
            sprite.width, draw.content_height, static_cast<uint16_t>(draw.flags), draw.alpha, corrected,
            draw.remap.data(), capture.assets.ghost.data(), capture.assets.additive.data());
        Require(result == 0, "original rasterizer failed");
        const auto guards_match = [&](const std::vector<uint8_t>& candidate) {
            return std::equal(candidate.begin(), candidate.begin() + guard, guard_before.begin())
                && std::equal(candidate.end() - guard, candidate.end(), guard_before.begin() + guard);
        };
        if (!guards_match(storage)) {
            if (!detailed || corrected || !row_copy_candidate)
                throw std::runtime_error("Legacy sprite oracle: renderer changed guard outside viewport at command "
                    + std::to_string(i));
            LegacySpriteOracleTarget(preimage.data() + guard, width, height);
            const int corrected_result = LegacySpriteOracleDraw(&s, encoded[draw.sprite].data(),
                sprite.width, draw.content_height, static_cast<uint16_t>(draw.flags), draw.alpha, true,
                draw.remap.data(), capture.assets.ghost.data(), capture.assets.additive.data());
            LegacySpriteOracleTarget(storage.data() + guard, width, height);
            Require(corrected_result == 0 && guards_match(preimage),
                "guard change not eliminated by the isolated row-copy correction");
            for (size_t g = 0; g < guard * 2; ++g) {
                const size_t position = g < guard ? g : storage.size() - guard + g - guard;
                if (storage[position] == guard_before[g]) continue;
                const int64_t offset = static_cast<int64_t>(position) - static_cast<int64_t>(guard);
                // This defect can copy only the byte immediately left of the
                // top row outside the target. Never accept broader corruption.
                Require(offset == -1, "unexpected guard corruption location");
                output.guard_changes.push_back({i, offset, guard_before[g], storage[position]});
            }
        }
    }
    output.pixels.assign(storage.begin() + guard, storage.end() - guard);
    return output;
}
std::vector<uint8_t> ReplayLegacySprites(const SceneCapture& capture,
    const std::vector<uint8_t>& background, std::span<const SceneSpriteDraw> draws)
{
    return ReplayOracle(capture, background, draws, false).pixels;
}
LegacySpriteOracleResult ReplayLegacySpritesNative(const SceneCapture& capture,
    const std::vector<uint8_t>& background, std::span<const SceneSpriteDraw> draws, bool corrected)
{
    Invocation invocation;
    const auto& t = capture.target;
    Require(capture.format_version == 8 && capture.trace.backend == 0
        && (t.kind == SceneIndexedTargetKind::Screen || t.kind == SceneIndexedTargetKind::LensSource)
        && t.width && t.height && t.width <= t.pitch && t.pitch <= 16384 && t.height <= 16384
        && uint64_t(t.pitch) * t.height <= 64 * 1024 * 1024
        && background.size() == size_t(t.pitch) * t.height
        && t.window_x >= 0 && t.window_y >= 0 && t.window_w > 0 && t.window_h > 0
        && int64_t(t.window_x) + t.window_w <= t.width && int64_t(t.window_y) + t.window_h <= t.height
        && t.window_w == capture.scene.camera.viewport_w && t.window_h == capture.scene.camera.viewport_h,
        "invalid native target/background");
    Require(capture.assets.additive.size() == 65536 && draws.size() <= 65536,
        "missing additive table or too many sprite commands");
    const size_t guard = size_t(t.pitch) * 2 + 16;
    const size_t origin = size_t(t.window_y) * t.pitch + t.window_x;
    std::vector<uint8_t> storage(background.size() + guard * 2, 0xa7);
    std::copy(background.begin(), background.end(), storage.begin() + guard);
    const auto bind = [&](std::vector<uint8_t>& image) {
        LegacySpriteOracleNativeTarget(image.data() + guard, t.pitch, t.height,
            t.window_x, t.window_y, t.window_w, t.window_h, uint32_t(guard));
    };
    const auto outside = [&](size_t position) {
        const int64_t offset = int64_t(position) - int64_t(guard + origin);
        return offset < 0 || offset / t.pitch >= t.window_h || offset % t.pitch >= t.window_w;
    };
    LegacySpriteOracleResult output;
    std::vector<std::vector<uint8_t>> encoded(capture.assets.sprites.size());
    for (size_t i = 0; i < draws.size(); ++i) {
        const auto& draw = draws[i];
        Require(draw.sprite < capture.assets.sprites.size(), "invalid sprite reference");
        const auto& sprite = capture.assets.sprites[draw.sprite];
        const auto& s = draw.scale;
        Require(s.window_w == t.window_w && s.window_h == t.window_h, "scale window differs from native viewport");
        Require(draw.content_height > 0 && draw.content_height <= sprite.height, "invalid source row crop");
        Require(draw.flags <= 65535 && draw.has_remap == bool(draw.flags & 0x0800)
            && draw.blend == ResolveSceneSpriteBlend(draw.flags, draw.alpha), "inconsistent draw state");
        ValidateAxis(s.frame_x, s.frame_src_w, s.frame_dst_w, s.content_x, sprite.width, LegacySpriteOracleXSteps());
        ValidateAxis(s.frame_y, s.frame_src_h, s.frame_dst_h, s.content_y, draw.content_height, LegacySpriteOracleYSteps());
        if (encoded[draw.sprite].empty()) encoded[draw.sprite] = EncodeRle(sprite);
        const auto before = storage;
        bind(storage);
        const auto submit = [&](bool repair) {
            return LegacySpriteOracleDraw(&s, encoded[draw.sprite].data(), sprite.width, draw.content_height,
                static_cast<uint16_t>(draw.flags), draw.alpha, repair, draw.remap.data(),
                capture.assets.ghost.data(), capture.assets.additive.data());
        };
        Require(submit(corrected) == 0, "original rasterizer failed");
        LegacySpriteOracleCopyAccess access{i, 0, 0, 0};
        const int failure = LegacySpriteOracleCopyFailure(&access.source_offset, &access.destination_offset, &access.length);
        Require(failure != 2, "native row copy escaped owned allocation");
        if (failure) {
            Require(!corrected, "corrected native row copy reads outside owned viewport");
            output.available = false;
            output.unavailable_reason = "Original sprite command " + std::to_string(i)
                + " reads uncaptured bytes outside the viewport (native source offset "
                + std::to_string(access.source_offset) + ", length " + std::to_string(access.length) + ")";
            output.unavailable_reads.push_back(access);
            return output; // No partially rendered image is an original reference.
        }
        std::vector<size_t> changed;
        for (size_t p = 0; p < storage.size(); ++p)
            if (storage[p] != before[p] && outside(p)) changed.push_back(p);
        if (!changed.empty()) {
            const bool candidate = !draw.alpha && (draw.flags & 1)
                && (s.frame_dst_w > s.frame_src_w || s.frame_dst_h > s.frame_src_h)
                && (draw.blend == SceneSpriteBlend::Opaque || draw.blend == SceneSpriteBlend::Remap);
            Require(!corrected && candidate, "unexpected native write outside viewport");
            auto repaired = before;
            bind(repaired);
            Require(submit(true) == 0, "corrected native proof draw failed");
            int64_t source = 0, destination = 0; uint32_t length = 0;
            Require(!LegacySpriteOracleCopyFailure(&source, &destination, &length), "corrected proof reads uncaptured bytes");
            for (size_t p = 0; p < repaired.size(); ++p)
                Require(repaired[p] == before[p] || !outside(p), "native outside write not eliminated by isolated correction");
            for (const auto p : changed) {
                const int64_t offset = int64_t(p) - int64_t(guard + origin);
                Require(offset >= -1 && (offset + 1) % t.pitch == 0 && (offset + 1) / t.pitch < t.window_h,
                    "unexpected native row-copy corruption location");
                output.guard_changes.push_back({i, offset, before[p], storage[p]});
            }
        }
    }
    output.pixels.assign(storage.begin() + guard, storage.end() - guard);
    return output;
}
std::vector<uint8_t> ReplayCorrectedLegacySprites(const SceneCapture& capture,
    const std::vector<uint8_t>& background)
{
    Require(capture.format_version >= 3 && capture.trace.complete
        && ValidateSceneCapture(capture), "complete validated KFXC v3 capture required");
    return ReplayOracle(capture, background, capture.trace.draws, true).pixels;
}
std::vector<uint8_t> ReplayCorrectedLegacySprites(const SceneCapture& capture,
    const std::vector<uint8_t>& background, std::span<const SceneSpriteDraw> draws)
{
    return ReplayOracle(capture, background, draws, true).pixels;
}
LegacySpriteOracleResult ReplayLegacySpritesDetailed(const SceneCapture& capture,
    const std::vector<uint8_t>& background)
{
    Require(capture.format_version >= 3 && capture.trace.complete
        && ValidateSceneCapture(capture), "complete validated KFXC v3 capture required");
    return ReplayOracle(capture, background, capture.trace.draws, false, true);
}
LegacySpriteOracleResult ReplayLegacySpritesDetailed(const SceneCapture& capture,
    const std::vector<uint8_t>& background, std::span<const SceneSpriteDraw> draws)
{
    return ReplayOracle(capture, background, draws, false, true);
}
}
