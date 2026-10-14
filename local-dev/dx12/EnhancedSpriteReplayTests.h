// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#pragma once
#include "Dx12Device.h"
#include <filesystem>

namespace kfx::render::dx12 {
struct EnhancedSpriteReplayTestResult {
    Image preview; // Diagnostic only: declared background, no terrain colour.
    uint64_t compared_pixels = 0, differing_bytes = 0;
    bool exact = false;
};
EnhancedSpriteReplayTestResult RunEnhancedSpriteReplayTests(Device&,
    const std::filesystem::path& scene_path, const std::filesystem::path& output);
}
