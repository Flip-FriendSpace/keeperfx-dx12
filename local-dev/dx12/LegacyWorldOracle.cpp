// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#include "LegacyWorldOracle.h"
#include "LegacyTerrainOracle.h"
#include "LegacyShadowOracle.h"
#include <algorithm>
#include <stdexcept>

namespace kfx::render::dx12 {
namespace {
MixedReference ReplayNative(const SceneCapture& capture, const std::vector<uint8_t>& background, bool corrected)
{
    const auto& t = capture.target;
    const size_t width = size_t(t.window_w), height = size_t(t.window_h);
    if (background.size() != width * height) throw std::invalid_argument("Native mixed background differs from viewport");
    // Outside-window logical bytes start at the declared clear and padding at
    // a diagnostic canary. They are NOT claimed to reproduce uncaptured UI.
    // The sprite adapter blocks every source read outside the owned window.
    std::vector<uint8_t> target(size_t(t.pitch) * t.height, 0xa7);
    for (size_t y = 0; y < t.height; ++y)
        std::fill_n(target.begin() + y * t.pitch, t.width, t.clear_index);
    const size_t origin = size_t(t.window_y) * t.pitch + t.window_x;
    const auto scatter = [&](const std::vector<uint8_t>& packed) {
        for (size_t y = 0; y < height; ++y)
            std::copy_n(packed.begin() + y * width, width, target.begin() + origin + y * t.pitch);
    };
    const auto gather = [&] {
        std::vector<uint8_t> packed(width * height);
        for (size_t y = 0; y < height; ++y)
            std::copy_n(target.begin() + origin + y * t.pitch, width, packed.begin() + y * width);
        return packed;
    };
    scatter(background);
    MixedReference result;
    result.native_stride = true;
    const auto& commands = capture.trace.raster.commands;
    for (size_t first = 0; first < commands.size();) {
        size_t end = first + 1;
        while (end < commands.size() && commands[end].kind == commands[first].kind
            && commands[end].index == commands[first].index + end - first) ++end;
        const auto index = commands[first].index;
        if (commands[first].kind == SceneDrawCommandKind::Sprite) {
            const auto span = std::span<const SceneSpriteDraw>(capture.trace.draws).subspan(index, end - first);
            auto output = ReplayLegacySpritesNative(capture, target, span, corrected);
            for (auto change : output.guard_changes) {
                change.command += first; result.guard_changes.push_back(change);
            }
            if (!output.available) {
                result.available = false;
                result.unavailable_reason = "Native mixed run at command " + std::to_string(first) + ": " + output.unavailable_reason;
                for (auto access : output.unavailable_reads) {
                    access.command += first; result.unavailable_reads.push_back(access);
                }
                return result;
            }
            target = std::move(output.pixels);
        } else if (commands[first].kind == SceneDrawCommandKind::Triangle) {
            // These kernels clip to window dimensions; pitch changes only
            // address placement, never interpolation, coverage or table reads.
            const auto span = std::span<const SceneTerrainTriangle>(capture.trace.raster.triangles).subspan(index, end - first);
            scatter(ReplayLegacyTerrainTriangles(span, capture.assets, uint32_t(width), uint32_t(height), gather()));
        } else if (commands[first].kind == SceneDrawCommandKind::Shadow) {
            const auto span = std::span<const SceneShadowTriangle>(capture.trace.raster.shadows).subspan(index, end - first);
            scatter(ReplayLegacyShadowTriangles(span, capture.trace.raster.shadow_masks, capture.assets,
                uint32_t(width), uint32_t(height), gather()));
        } else throw std::runtime_error("Native mixed replay encountered unknown command kind");
        first = end;
    }
    result.pixels = gather();
    return result;
}
}
MixedReference ReplayLegacyWorld(const SceneCapture& capture, const std::vector<uint8_t>& background, bool corrected)
{
    if (!ValidateSceneCapture(capture) || capture.format_version < 4 || capture.trace.backend != 0)
        throw std::invalid_argument("Original mixed replay requires a validated software capture");
    if (capture.format_version >= 8) return ReplayNative(capture, background, corrected);
    MixedReference result;
    result.pixels = background;
    const auto& commands = capture.trace.raster.commands;
    for (size_t first = 0; first < commands.size();) {
        size_t end = first + 1;
        while (end < commands.size() && commands[end].kind == commands[first].kind
            && commands[end].index == commands[first].index + end - first) ++end;
        const auto index = commands[first].index;
        if (commands[first].kind == SceneDrawCommandKind::Triangle) {
            const auto span = std::span<const SceneTerrainTriangle>(capture.trace.raster.triangles).subspan(index, end - first);
            result.pixels = ReplayLegacyTerrainTriangles(span, capture.assets, capture.scene.camera.viewport_w,
                capture.scene.camera.viewport_h, result.pixels);
        } else if (commands[first].kind == SceneDrawCommandKind::Shadow) {
            try {
                const auto span = std::span<const SceneShadowTriangle>(capture.trace.raster.shadows).subspan(index, end - first);
                result.pixels = ReplayLegacyShadowTriangles(span, capture.trace.raster.shadow_masks, capture.assets,
                    capture.scene.camera.viewport_w, capture.scene.camera.viewport_h, result.pixels);
            } catch (const std::exception& error) {
                throw std::runtime_error("Mixed shadow run at command " + std::to_string(first) + ": " + error.what());
            }
        } else if (commands[first].kind == SceneDrawCommandKind::Sprite) {
            const auto span = std::span<const SceneSpriteDraw>(capture.trace.draws).subspan(index, end - first);
            if (corrected) result.pixels = ReplayCorrectedLegacySprites(capture, result.pixels, span);
            else {
                auto output = ReplayLegacySpritesDetailed(capture, result.pixels, span);
                result.pixels = std::move(output.pixels);
                for (auto change : output.guard_changes) {
                    change.command += first; result.guard_changes.push_back(change);
                }
            }
        } else throw std::runtime_error("Original mixed replay encountered an unknown command kind");
        first = end;
    }
    return result;
}
}
