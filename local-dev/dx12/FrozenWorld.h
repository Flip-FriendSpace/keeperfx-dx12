// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#pragma once
#include "Dx12Device.h"
#include <filesystem>

namespace kfx::render::dx12 {
// Builds and retains the GPU result. Explicit readbacks only supply separate
// comparison evidence; presentation uses the returned resident frame directly.
FrozenGpuFrame PrepareFrozenWorld(Device&, const std::filesystem::path& capture,
    const std::filesystem::path& output);
}
