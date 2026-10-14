// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace kfx::render {
enum class IndexedWorldFallbackReason : uint8_t { CustomLens = 0 };
// Producer-owned diagnostic output, before palette expansion and ordinary UI.
// Retained only with KFX_SCENE_TEST_DIR; not a scene-capture wire format.
struct IndexedWorldFallbackFrame {
    IndexedWorldFallbackReason reason = IndexedWorldFallbackReason::CustomLens;
    uint64_t sequence = 0, custom_draw_attempts_before = 0, custom_draw_attempts_after = 0;
    int native_width = 0, native_height = 0;
    int x = 0, y = 0, width = 0, height = 0;
    uint8_t destination_clear_index = 0;
    std::array<uint8_t, 1024> display_palette_rgba{}; // Actual displayed RGB, opaque alpha.
    std::vector<uint8_t> indices; // width*height, top-down; index zero is opaque.
    bool renderer_state_restored = false;
};
}
// Game-thread only. Null unless a completed custom-lens fallback was retained.
std::shared_ptr<const kfx::render::IndexedWorldFallbackFrame> RendererGetIndexedWorldFallbackDiagnostic();
