// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX remaster changes: 2026-10-16.
// Opt-in presentation controls. No simulation, capture codec or save changes.
#pragma once
#include <cstdint>
namespace kfx::render::dx12 {
enum LightingRemasterFlags : uint32_t {
    SmoothLightCells = 1u,
    BlendPaletteRows = 2u,
    FilterShadowCoverage = 4u,
};
inline constexpr uint32_t LightingRemasterMask = 7u;
inline constexpr bool ValidLightingRemaster(uint32_t flags) noexcept {
    return (flags & ~LightingRemasterMask) == 0;
}
}
