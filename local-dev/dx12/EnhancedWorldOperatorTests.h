// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#pragma once
#include "Dx12Device.h"
#include <filesystem>
namespace kfx::render::dx12 {
bool RunEnhancedWorldOperatorTests(Device& device,const std::filesystem::path& output,
    EnhancedFadeAddressing fade_addressing=EnhancedFadeAddressing::Sampler);
}
