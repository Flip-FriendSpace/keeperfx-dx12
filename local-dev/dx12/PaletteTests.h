// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#pragma once
#include "Dx12Device.h"
#include <filesystem>

namespace kfx::render::dx12 {
struct PaletteTestResult {
    Image preview;
    uint64_t indexed_checks = 0;
    bool rov_tested = false;
};
PaletteTestResult RunPaletteTests(Device& device, const std::filesystem::path& scene_path,
    const std::filesystem::path& output);
}
