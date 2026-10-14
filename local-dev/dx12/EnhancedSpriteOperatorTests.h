// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#pragma once
#include "Dx12Device.h"
#include <filesystem>

namespace kfx::render::dx12 {
// Strict analytic failures throw. Every GL blend-byte difference is retained
// and makes the return value false; it is never silently tolerated.
bool RunEnhancedSpriteOperatorTests(Device&, const std::filesystem::path& output);
}
