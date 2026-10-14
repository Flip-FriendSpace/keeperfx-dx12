// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#pragma once
#include "Dx12Device.h"
#include <filesystem>

namespace kfx::render::dx12 {
struct RuntimeResourceTestResult {
    Image preview;
    uint64_t checks = 0;
    uint64_t rgba_byte_checks = 0;
    uint64_t submitted_frames = 0;
    uint32_t cases = 0;
};
// Loads the retained KFXI snapshot once. Synthetic revisions use the same real
// Device runtime entry, never the frozen compositor or callback reconstruction.
RuntimeResourceTestResult RunRuntimeResourceTests(Device&, const Options&,
    const std::filesystem::path& indexed_snapshot, const std::filesystem::path& output);
}
