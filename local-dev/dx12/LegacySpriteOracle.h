// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
// Diagnostic adapter for the original software sprite rasterizer.
#pragma once
#include "kfx/renderer/SceneAssets.h"

namespace kfx::render::dx12 {
struct LegacySpriteOracleCopyAccess {
    size_t command = 0;
    int64_t source_offset = 0, destination_offset = 0; // Relative to window origin, at native pitch.
    uint32_t length = 0;
};
struct LegacySpriteOracleGuardChange {
    size_t command = 0;
    int64_t offset = 0; // Byte offset relative to viewport start, can be negative.
    uint8_t before = 0, after = 0;
};
struct LegacySpriteOracleResult {
    std::vector<uint8_t> pixels;
    // Observed byte changes, not a count of stores (same-value stores are
    // invisible to canaries). Every entry is independently verified to vanish
    // with only the known mirrored row-copy correction on identical input.
    std::vector<LegacySpriteOracleGuardChange> guard_changes;
    bool available = true;
    std::string unavailable_reason;
    std::vector<LegacySpriteOracleCopyAccess> unavailable_reads;
};
// v8-only native target adapter. Background/output contain pitch*height bytes;
// only the window is authoritative. Reads beyond that window are unavailable,
// never replaced by fabricated UI/padding data. The mixed adapter preserves
// this surface across ordered runs and crops it only at the end.
LegacySpriteOracleResult ReplayLegacySpritesNative(const SceneCapture& capture,
    const std::vector<uint8_t>& background, std::span<const SceneSpriteDraw> draws, bool corrected);
// Retains diagnostic evidence of the original's known one-byte left overrun.
// Other guard changes fail. Both vector-returning APIs below remain strict.
LegacySpriteOracleResult ReplayLegacySpritesDetailed(const SceneCapture& capture,
    const std::vector<uint8_t>& background);
LegacySpriteOracleResult ReplayLegacySpritesDetailed(const SceneCapture& capture,
    const std::vector<uint8_t>& background, std::span<const SceneSpriteDraw> draws);
// Returns viewport-sized palette indices. Throws on malformed or unsafe legacy
// scale state; never changes captured assets or invokes the game simulation.
std::vector<uint8_t> ReplayLegacySprites(const SceneCapture& capture,
    const std::vector<uint8_t>& background);
// Explicit command span supports focused fixtures and prefix comparisons.
// Validates raster inputs without requiring world/object identity fixtures.
std::vector<uint8_t> ReplayLegacySprites(const SceneCapture& capture,
    const std::vector<uint8_t>& background, std::span<const SceneSpriteDraw> draws);
// Prospective visual repair reference: only the mirrored solid/remap row-copy
// off-by-one is corrected in two build-generated original routines. Keep and
// report original-oracle comparisons alongside this explicitly distinct result.
std::vector<uint8_t> ReplayCorrectedLegacySprites(const SceneCapture& capture,
    const std::vector<uint8_t>& background);
std::vector<uint8_t> ReplayCorrectedLegacySprites(const SceneCapture& capture,
    const std::vector<uint8_t>& background, std::span<const SceneSpriteDraw> draws);
}
