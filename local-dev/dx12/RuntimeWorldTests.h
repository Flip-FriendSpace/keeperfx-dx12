// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#pragma once
#include "Dx12Device.h"
#include <filesystem>
namespace kfx::render::dx12 {
struct RuntimeWorldTestResult {
    Image preview;
    uint64_t checks=0,rgba_byte_checks=0,submitted_frames=0;
    uint32_t cases=0;
};
// Retained capture adaptation is diagnostic-only and happens once. Overlays,
// lenses, temporal engine updates and live producer integration remain pending.
RuntimeWorldTestResult RunRuntimeWorldTests(Device&,const Options&,
    const std::filesystem::path& capture,const std::filesystem::path& output);
}
