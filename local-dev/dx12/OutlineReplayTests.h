// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#pragma once
#include "Dx12Device.h"
#include <filesystem>

namespace kfx::render::dx12 {
struct OutlineReplayTestResult {
    Image preview;
    uint64_t compared_pixels = 0;
};
OutlineReplayTestResult RunOutlineReplayTests(Device& device,
    const std::filesystem::path& scene_path, const std::filesystem::path& output);
}
