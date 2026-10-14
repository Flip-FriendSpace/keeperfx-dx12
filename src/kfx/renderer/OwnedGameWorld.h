// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#pragma once
#include "OwnedWorldDrawSink.h"
#include "OwnedWorldSources.h"
#include "OwnedWorldLensProducer.h"

namespace kfx::render {
class OwnedWorldOverlayCapture;
// Game-thread world producer for a renderer's current frame builder. The normal
// engine still builds its buckets and performs picking; this class consumes
// those buckets once, resolving every source before the frame can be sealed.
// The caller owns epoch changes and the later swipe/interface phases.
class OwnedGameWorld {
public:
    explicit OwnedGameWorld(OwnedRenderAssets& assets);
    void SetDisplayPalette(std::span<const uint8_t> rgb8);
    void SetSpritePalette(std::span<const uint8_t> rgb6);
    // Call at BeginWorldPass, before the engine generates projected geometry.
    static void BeginView(int width, int height);
    // Menus and loading frames use the same immutable palette/fade sources.
    bool PrepareFrameSources(OwnedFrameBuilder& builder);
    OwnedWorldSpriteSourceCache& SpriteSources() noexcept { return sprites_; }
    const OwnedWorldDrawSettings& DrawSettings() const noexcept { return frame_draw_settings_; }
    // True means traversal finished. Seal()/phase status determines whether the
    // packet is complete; unsupported composition must not be silently omitted.
    bool Populate(OwnedFrameBuilder& builder, WorldDrawView view, Camera* camera);
    // Consume the lens already evaluated by the engine after drawing the world
    // and swipe. A resource retry must never evaluate a game callback again.
    bool PopulateLens(OwnedFrameBuilder& builder, const IRWorldLensCmd& command);
    // The backend retains this scope between the engine's matching swipe
    // begin/end calls and must Complete it before sealing the frame.
    std::unique_ptr<OwnedWorldOverlayCapture> BeginSwipe(OwnedFrameBuilder& builder);
private:
    OwnedRenderAssets& assets_;
    OwnedWorldSources globals_;
    OwnedWorldSpriteSourceCache sprites_;
    OwnedWorldLensProducer lenses_;
    OwnedWorldDrawSettings frame_draw_settings_;
    OwnedAssetLease frame_palette_, frame_fade_;
    std::array<uint8_t,768> frame_base_palette_{};
    uint64_t frame_epoch_=0, frame_sequence_=0;
    std::array<uint8_t,1024> display_palette_{};
    std::array<uint8_t,768> sprite_palette_{};
    bool have_display_palette_=false, have_sprite_palette_=false;
};
}
