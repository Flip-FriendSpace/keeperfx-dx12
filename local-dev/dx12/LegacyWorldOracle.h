// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
// Diagnostic original-C mixed painter stream. Never used as GPU input.
#pragma once
#include "LegacySpriteOracle.h"

namespace kfx::render::dx12 {
struct MixedReference {
    std::vector<uint8_t> pixels;
    std::vector<LegacySpriteOracleGuardChange> guard_changes;
    bool available = true;
    std::string unavailable_reason;
    bool native_stride = false;
    std::vector<LegacySpriteOracleCopyAccess> unavailable_reads;
};
MixedReference ReplayLegacyWorld(const SceneCapture&, const std::vector<uint8_t>& background, bool corrected);
}
