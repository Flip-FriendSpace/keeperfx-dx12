// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#include "pre_inc.h"
#include "kfx/renderer/TileSource.h"
#include "kfx/renderer/RendererThread.h"
#include "engine_textures.h"
#include "post_inc.h"

namespace kfx::render {
bool ResolveLogicalTileSource(uint32_t variation, uint32_t tile, TileSourceView& output) noexcept
{
    ASSERT_GAME_THREAD();
    output = {};
    static_assert(TEXTURE_VARIATIONS_COUNT == TileSourceVariationCount);
    static_assert(TEXTURE_BLOCKS_COUNT == TileSourceTileCount);
    if (!level_textures_ready || texture_bank_mutation_serial == 0
        || texture_bank_mutation_serial == UINT64_MAX || variation >= TEXTURE_VARIATIONS_COUNT
        || tile >= TEXTURE_BLOCKS_COUNT || block_dimension != TileSourceDimension
        || block_count_per_row <= 0
        || uint64_t(block_count_per_row) > std::numeric_limits<uint32_t>::max() / TileSourceDimension)
        return false;
    const size_t index = size_t(variation) * TEXTURE_BLOCKS_COUNT + tile;
    const uint64_t stride = uint64_t(block_count_per_row) * TileSourceDimension;
    // block_mem is a fixed declared array. Static initialization and the loader's
    // full-bank memset initialize every byte, including absent optional banks.
    const std::span<const uint8_t> bank(block_mem, sizeof(block_mem));
    if (!ResolveTileSourceBounds(bank, block_ptrs[index], stride, output)) return false;
    output.mutation_serial = texture_bank_mutation_serial;
    output.level_generation = level_textures_generation;
    output.variation = variation;
    output.tile = tile;
    return true;
}
}
