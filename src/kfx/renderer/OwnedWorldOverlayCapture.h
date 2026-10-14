// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#pragma once
#include "OwnedWorldDrawSink.h"
#include <memory>
#include <optional>

namespace kfx::render {
// Normal game-thread Swipe bracket. Construct at BeginOverlayCapture(SWIPE),
// run the engine's existing drawing code once, then call Complete at End.
// The shared source cache is the same one used by the world/UI producer.
// Native positions, clip and draw state are resolved synchronously by its UI
// sink; no sprite, remap, font pointer or callback is retained in the packet.
// Caller must check Active before accepting the bracket. Abandoning a live
// scope, including exception unwinding, rejects the phase rather than treating
// a partial overlay as complete. No draw callback is invoked by this class.
// This slice accepts the normal Swipe GameUI sublayer only. Other UI sublayers
// are explicitly unsupported until their four-pass ordering is implemented.
// Text is captured separately as ordinary GameUI, matching normal GL routing.
class OwnedWorldOverlayCapture final : private WorldSubmissionCapture {
public:
    OwnedWorldOverlayCapture(OwnedFrameBuilder&,OwnedRenderAssets&,
        OwnedWorldSpriteSourceCache&,OwnedWorldDrawSettings);
    // Reuse an existing sink when the frame producer already owns one.
    OwnedWorldOverlayCapture(OwnedFrameBuilder&,OwnedWorldDrawSink&) noexcept;
    ~OwnedWorldOverlayCapture();
    OwnedWorldOverlayCapture(const OwnedWorldOverlayCapture&)=delete;
    OwnedWorldOverlayCapture& operator=(const OwnedWorldOverlayCapture&)=delete;
    bool Active() const noexcept {return active_;}
    bool Complete() noexcept;
private:
    OwnedFrameBuilder& builder_;
    std::unique_ptr<OwnedWorldDrawSink> owned_sink_;
    OwnedWorldDrawSink* sink_=nullptr;
    std::optional<ScopedWorldSubmissionCapture> routing_;
    bool active_=false;
    void Begin() noexcept;
    void BeginSpriteBucket(int32_t) override;
    void CaptureKeeperSprite(const SpriteScale*,const uint8_t*,int,int,int32_t,unsigned,
        const uint8_t*,int32_t,int,bool,uint8_t) override;
    IUIRenderer& UI() override;
    TbBool DrawTextResized(int32_t,int32_t,int32_t,const char*) override;
};
}
