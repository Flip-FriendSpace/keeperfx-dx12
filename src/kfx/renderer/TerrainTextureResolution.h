// SPDX-License-Identifier: GPL-2.0-or-later
// Texture resolution is independent of the original32-texel terrain UV space.
#pragma once
#include <cstdint>
namespace kfx::render {
inline constexpr bool TerrainTextureDimension(uint32_t size) noexcept
{ return size==32 || size==64 || size==128 || size==256; }
inline constexpr uint32_t TerrainTextureScaleCode(uint32_t size) noexcept
{ return size==256?3:size==128?2:size==64?1:0; }
// Runtime pages remain4096 wide, with origins aligned to the legacy32 grid.
// Low24 bits retain the original atlas-cell address; bits24-25 select resolution.
inline constexpr uint32_t TerrainTextureReference(uint32_t x,uint32_t y,uint32_t size) noexcept
{ return (y/32*128+x/32) | (TerrainTextureScaleCode(size)<<24); }
}
