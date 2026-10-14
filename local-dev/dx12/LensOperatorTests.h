// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#pragma once
#include "Dx12Device.h"
#include <filesystem>

namespace kfx::render::dx12 {
// Standard owned operator qualification only; the source does not claim to be
// a complete captured possession frame with its intervening world overlays.
bool RunLensOperatorTests(Device& device, const std::filesystem::path& output);
}
