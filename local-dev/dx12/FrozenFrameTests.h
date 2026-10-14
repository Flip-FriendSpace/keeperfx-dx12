// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#pragma once
#include "Dx12Device.h"
#include <filesystem>
#include <functional>

namespace kfx::render::dx12 {
void RunFrozenFrameTests(Device&, const Options&, const std::filesystem::path&,
    const std::function<void(uint32_t,uint32_t)>& resize);
}
