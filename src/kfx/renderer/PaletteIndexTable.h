// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#pragma once
#include <cstddef>
#include <cstdint>
#include <span>

namespace kfx::render {
inline constexpr size_t PaletteIndexTableBytes=512u*512u;
using PaletteNearestColour=uint8_t(*)(const uint8_t*,uint8_t,uint8_t,uint8_t);
// Exactly the normal renderer's sixteen-sample RGB6 fallback grid. The engine
// callback supplies LbPaletteFindColour, including all of its tie-breaking rules.
void BuildPaletteIndexBase(std::span<uint8_t> table,std::span<const uint8_t> palette_rgb6,PaletteNearestColour nearest);
// Display palette overrides: rounded8->6 quantization and lowest duplicate
// index. Keys are returned so callers can restore just these entries next time.
void ApplyPaletteIndexOverrides(std::span<uint8_t> table,std::span<const uint8_t> palette_rgba,std::span<uint32_t> keys);
}
