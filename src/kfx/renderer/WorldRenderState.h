// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#pragma once
#include "RendererSettings.h"
#include <array>

namespace kfx::render {
// Uniform order shared by the enhanced world shaders and owned frame packets.
inline std::array<float, 9> WorldShadeSettings(const RendererSettings& settings) noexcept
{
    return {settings.shade_fullbright, settings.shade_ambient,
        settings.shade_scale, settings.shade_gamma,
        static_cast<float>(settings.lighting_mode), static_cast<float>(settings.darkness_mode),
        settings.fog_speed, settings.fog_density, static_cast<float>(settings.tile_filter)};
}
}
