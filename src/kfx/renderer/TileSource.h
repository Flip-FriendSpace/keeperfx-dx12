// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
// Producer-thread view of the actual initialized INDEX8 terrain bank.
#pragma once
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace kfx::render {
inline constexpr uint32_t TileSourceDimension = 32;
inline constexpr uint32_t TileSourceVariationCount = 32;
inline constexpr uint32_t TileSourceTileCount = 1544;

struct TileSourceView {
    // Borrowed, including row gaps; consume/copy synchronously on the producer.
    // No mutable engine address may survive in a sealed packet or asset lease.
    std::span<const uint8_t> bytes;
    uint32_t width = 0, height = 0, row_stride = 0;
    uint32_t level_generation = 0, variation = 0, tile = 0;
    // Stable physical source identity is (mutation_serial, bank_offset).
    // 0/MAX serials cannot be published; animation changes bank_offset only.
    uint64_t mutation_serial = 0;
    size_t bank_offset = 0;
};

// The caller supplies the actual initialized allocation, not a claimed length
// attached to a foreign source pointer. This helper performs no pixel reads.
inline bool ResolveTileSourceBounds(std::span<const uint8_t> initialized_bank,
    const uint8_t* pixels, uint64_t row_stride, TileSourceView& output) noexcept
{
    output = {};
    if (!initialized_bank.data() || initialized_bank.empty() || !pixels
        || row_stride < TileSourceDimension
        || row_stride > std::numeric_limits<uint32_t>::max()) return false;
    const uintptr_t base = reinterpret_cast<uintptr_t>(initialized_bank.data());
    const uintptr_t address = reinterpret_cast<uintptr_t>(pixels);
    if (address < base) return false;
    // Integer addresses are compared before any pointer subtraction/subspan.
    const uintptr_t distance = address - base;
    if (distance >= initialized_bank.size()) return false;
    const size_t offset = static_cast<size_t>(distance);
    const uint64_t row_offset = offset % row_stride;
    if (TileSourceDimension > row_stride - row_offset) return false;
    const uint64_t extent = (TileSourceDimension - 1ull) * row_stride + TileSourceDimension;
    if (extent > initialized_bank.size() - offset) return false;
    output.bytes = initialized_bank.subspan(offset, static_cast<size_t>(extent));
    output.width = output.height = TileSourceDimension;
    output.row_stride = static_cast<uint32_t>(row_stride);
    output.bank_offset = offset;
    return true;
}

// Uses the selected block_ptrs entry exactly, including animation remaps across
// static A/B banks. ready is the existing engine flag, not a stronger proof of
// successful reload. Raw generation may wrap to0; actual bytes must be versioned
// even when generation is unchanged. The nonwrapping bank mutation serial,
// combined with the actual physical offset, identifies immutable source bytes.
// No GL or diagnostic capture dependency.
bool ResolveLogicalTileSource(uint32_t variation, uint32_t tile, TileSourceView& output) noexcept;
}
