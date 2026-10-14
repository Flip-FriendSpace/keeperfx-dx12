// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#pragma once
#include "OwnedRenderAssets.h"
#include "WorldRenderState.h"
#include <array>

namespace kfx::render {
// Borrowed inputs consumed synchronously on the producer thread. Display RGB8
// and sprite RGB6 palettes are deliberately separate: palette fades may use
// display colours that cannot be represented by six-bit channels.
struct OwnedWorldSourceInputs {
    uint64_t epoch = 0;
    std::span<const uint8_t> display_palette_rgba, sprite_palette_rgb6, base_palette_rgb6, fade;
    std::span<const uint16_t> lightmap;
    uint32_t lightmap_width = 0, lightmap_height = 0;
    RendererSettings settings{};
};
struct OwnedWorldSourceState {
    OwnedAssetLease palette, fade, lightmap, sprite_palette;
    std::array<uint8_t, 768> sprite_palette_rgb6{};
    std::array<float, 9> palette_transform{1,0,0,0,1,0,0,0,1};
    std::array<float, 9> shade_settings{};
    RendererSettings settings{};
};
struct OwnedWorldSourceResult {
    OwnedAssetStatus status = OwnedAssetStatus::InvalidSource;
    OwnedAssetPressure pressure = OwnedAssetPressure::None;
    OwnedWorldSourceState state;
    explicit operator bool() const noexcept
        { return state.palette.Valid() && state.fade.Valid() && state.lightmap.Valid() && state.sprite_palette.Valid(); }
};
// One instance owns four logical IDs in its exclusive catalog domain. The
// latest leases are the exact byte baselines; revisions advance only when those
// bytes change. Partial publication is retained across resource-pressure retries.
// The fixed 511x511x2 scratch image is separate from catalog payload accounting.
class OwnedWorldSources {
public:
    OwnedWorldSources(OwnedRenderAssets& assets, uint64_t domain);
    OwnedWorldSourceResult Update(const OwnedWorldSourceInputs& input) noexcept;
private:
    OwnedRenderAssets& assets_;
    uint64_t domain_, epoch_ = 0;
    std::array<OwnedAssetLease, 4> current_;
    std::array<uint64_t, 4> revisions_{};
    std::array<uint8_t, 511 * 511 * 2> lightmap_bytes_{};
    OwnedAssetResult Publish(size_t slot, OwnedAssetDesc desc, std::span<const uint8_t> bytes) noexcept;
};
}
