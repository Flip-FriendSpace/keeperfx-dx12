// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
// Owned standard enhanced-GL lens operator inputs. Effect selection, animation
// updates, Lua and resource fallback resolution happen before this boundary.
#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace kfx::render {
enum class SceneGLLensPassKind : uint8_t { Copy = 0, Remap = 1, Mist = 2, Overlay = 3 };
struct SceneGLLensPass {
    SceneGLLensPassKind kind = SceneGLLensPassKind::Copy;
    uint32_t source_width = 0, source_height = 0;
    int32_t source_x = 0, source_y = 0;
    uint32_t target_width = 0, target_height = 0;
    int32_t destination_x = 0, destination_y = 0;
    uint32_t viewport_width = 0, viewport_height = 0;
    // Actual submitted primary X/Y and secondary X/Y offsets, before the
    // production shader truncates them to wrapped integer density coordinates.
    std::array<float, 4> mist_offsets{};
    uint8_t mist_lightness = 0;
    uint32_t remap_width = 0, remap_height = 0;
    std::vector<std::array<uint16_t, 2>> remap;
    uint32_t overlay_width = 0, overlay_height = 0;
    uint16_t overlay_alpha = 0; // Actual integer shader uniform, 0..256.
    std::vector<uint8_t> overlay;
    std::vector<uint8_t> density; // Actual uploaded 256x256 R8 mist texture.
    std::vector<uint8_t> palette_rgba; // 256x1 RGBA8, actual uploaded bytes.
    std::vector<uint8_t> fade; // 256x64 R8, required only by Mist.
    std::vector<uint8_t> rgb6_lookup; // 512x512 R8; never regenerated from palette.
    bool operator==(const SceneGLLensPass&) const = default;
};

inline void ValidateSceneGLLensPass(const SceneGLLensPass& pass)
{
    const auto require = [](bool value, const char* reason) {
        if (!value) throw std::invalid_argument(reason);
    };
    const auto image = [](uint32_t width, uint32_t height) {
        return width && height && width <= 16384 && height <= 16384
            && uint64_t(width) * height * 4 <= 64 * 1024 * 1024;
    };
    require(uint32_t(pass.kind) <= uint32_t(SceneGLLensPassKind::Overlay), "Invalid resolved lens operation");
    require(image(pass.source_width, pass.source_height) && image(pass.target_width, pass.target_height),
        "Invalid native lens source or target extent");
    require(pass.source_x >= 0 && uint32_t(pass.source_x) < pass.source_width && pass.source_y == 0,
        "Invalid lens source origin: standard GL reads source row zero");
    require(pass.destination_x >= 0 && pass.destination_y >= 0 && pass.viewport_width && pass.viewport_height
        && uint64_t(pass.destination_x) + pass.viewport_width <= pass.target_width
        && uint64_t(pass.destination_y) + pass.viewport_height <= pass.target_height,
        "Lens destination rectangle exceeds native target");
    const bool mist = pass.kind == SceneGLLensPassKind::Mist;
    const bool overlay = pass.kind == SceneGLLensPassKind::Overlay;
    const bool remap = pass.kind == SceneGLLensPassKind::Remap;
    require(pass.palette_rgba.size() == (mist || overlay ? 1024u : 0u)
        && pass.rgb6_lookup.size() == (mist || overlay ? 512u * 512u : 0u)
        && pass.fade.size() == (mist ? 256u * 64u : 0u)
        && pass.density.size() == (mist ? 256u * 256u : 0u), "Missing or unused lens lookup resource");
    for (float offset : pass.mist_offsets)
        require(std::isfinite(offset) && (mist ? offset >= 0 && offset < 256 : offset == 0),
            "Invalid or unused mist offset");
    require(mist ? pass.mist_lightness <= 63 : pass.mist_lightness == 0, "Invalid or unused mist lightness");
    if (remap) {
        require(image(pass.remap_width, pass.remap_height)
            && pass.remap.size() == uint64_t(pass.remap_width) * pass.remap_height,
            "Invalid RG16UI lens remap extent");
        // Preserve every unsigned upload value, including 65535. Production
        // clamps remapped source coordinates to the native source texture.
    } else require(pass.remap_width == 0 && pass.remap_height == 0 && pass.remap.empty(), "Unused lens remap resource");
    if (overlay) {
        require(pass.overlay_width && pass.overlay_height && pass.overlay_width <= 16384 && pass.overlay_height <= 16384
            && pass.overlay.size() == uint64_t(pass.overlay_width) * pass.overlay_height && pass.overlay_alpha <= 256,
            "Invalid indexed lens overlay");
    } else require(pass.overlay_width == 0 && pass.overlay_height == 0 && pass.overlay_alpha == 0 && pass.overlay.empty(),
        "Unused lens overlay resource");
    const uint64_t bytes = uint64_t(pass.remap.size()) * 4 + pass.overlay.size() + pass.density.size()
        + pass.palette_rgba.size() + pass.fade.size() + pass.rgb6_lookup.size();
    require(bytes <= 64 * 1024 * 1024, "Lens auxiliary resources exceed 64 MiB");
    // With dimensions <=16384, the signed 16.16 scale/products used by the
    // original Mist/Overlay shaders stay below 2^30; source additions <81920.
}

}
