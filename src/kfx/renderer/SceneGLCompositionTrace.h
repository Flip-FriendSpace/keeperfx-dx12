// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
// Executed world overlays and possession composition, before ordinary UI.
#pragma once
#include "kfx/renderer/SceneGLOverlayTrace.h"
#include "kfx/renderer/SceneGLLensPass.h"

namespace kfx::render {
// V11 combines the world, original-extent overlay atlases and native lens tables.
// A 4K RG16 remap plus one UI atlas needs this aggregate headroom. Each child
// remains limited to 64 MiB; captures through V10 retain their 64 MiB aggregate.
inline constexpr uint64_t SceneGLCompositionOwnedByteLimit = 96ull * 1024 * 1024;
struct SceneGLUploadStamp {
    uint64_t generation = 0, revision = 0;
    bool operator==(const SceneGLUploadStamp&) const = default;
};
enum class SceneGLLensOperation : uint8_t {
    None = 0, Copy = 1, Remap = 2, Mist = 3, Overlay = 4, NotDrawn = 5
};
struct SceneGLLensTrace {
    bool requested_active = false, capture_began = false;
    SceneGLLensOperation operation = SceneGLLensOperation::None;
    // Original IR request: None=0, Mist=1, Displacement=2, Flyeye=3, Overlay=4.
    uint8_t requested_type = 0;
    bool fallback_copy = false;
    SceneGLLensPass pass;
    // Actual resource incarnation/upload revision, never GL names or pointers.
    // Fixed order: palette, fade, RGB6 lookup, density, remap, overlay.
    std::array<SceneGLUploadStamp, 6> resources{};
    bool operator==(const SceneGLLensTrace&) const = default;
};
struct SceneGLCompositionTrace {
    bool present = false, complete = false;
    // 1: depth overlays, 2: flat overlays, 4: swipe, 8: lens resolution.
    // Empty phases still complete. Publication requires all four bits.
    uint8_t completed_phases = 0;
    std::array<uint32_t, 3> phase_quad_counts{};
    std::array<float, 4> screen_clear_rgba{};
    SceneGLOverlayPass overlays;
    SceneGLLensTrace lens;
    bool operator==(const SceneGLCompositionTrace&) const = default;
};
}
