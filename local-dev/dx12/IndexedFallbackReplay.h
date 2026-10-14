// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#pragma once
#include "Dx12Device.h"
#include <filesystem>

namespace kfx::render::dx12 {
// Reads owned post-callback indices once. Readbacks are comparison evidence;
// presentation uses the returned resident index/colour surfaces directly.
FrozenGpuFrame PrepareIndexedFallbackReplay(Device&, const std::filesystem::path& input,
    const std::filesystem::path& output, bool synthetic = false);
std::filesystem::path WriteIndexedFallbackFixture(const std::filesystem::path& output);
}
