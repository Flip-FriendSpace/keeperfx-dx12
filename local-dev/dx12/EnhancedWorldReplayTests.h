// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#pragma once
#include "Dx12Device.h"
#include <filesystem>
namespace kfx::render::dx12 {
struct EnhancedWorldReplayTestResult {
    FrozenGpuFrame frame; // V10 viewport crop; V11 full native composition, ready for presentation.
    Image preview; // Explicit diagnostic readback only.
    uint64_t compared_pixels=0,differing_bytes=0;
    bool exact=false;
};
EnhancedWorldReplayTestResult RunEnhancedWorldReplayTests(Device& device,
    const std::filesystem::path& scene_path,const std::filesystem::path& output,
    EnhancedWorldRasterOrigin raster_origin=EnhancedWorldRasterOrigin::ViewportLocal,
    EnhancedFadeAddressing fade_addressing=EnhancedFadeAddressing::Sampler);
}
