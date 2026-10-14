// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#include "OwnedWorldOverlayCapture.h"
#include <utility>

namespace kfx::render {
OwnedWorldOverlayCapture::OwnedWorldOverlayCapture(OwnedFrameBuilder& builder,OwnedRenderAssets& assets,
    OwnedWorldSpriteSourceCache& sources,OwnedWorldDrawSettings settings)
    :builder_(builder),owned_sink_(std::make_unique<OwnedWorldDrawSink>(builder,assets,sources,std::move(settings))),sink_(owned_sink_.get())
{
    Begin();
}
OwnedWorldOverlayCapture::OwnedWorldOverlayCapture(OwnedFrameBuilder& builder,OwnedWorldDrawSink& sink) noexcept
    :builder_(builder),sink_(&sink)
{
    Begin();
}
void OwnedWorldOverlayCapture::Begin() noexcept
{
    routing_.emplace(static_cast<WorldSubmissionCapture&>(*this));
    if(!routing_->Active()||!sink_->BeginExternalOverlay(OwnedFramePhase::Swipe)) {
        routing_.reset();builder_.Unsupported(OwnedFramePhase::Swipe);return;
    }
    active_=true;
}
OwnedWorldOverlayCapture::~OwnedWorldOverlayCapture()
{
    if(active_) {
        builder_.Unsupported(OwnedFramePhase::Swipe);
        sink_->EndExternalOverlay();
        routing_.reset();
    }
}
bool OwnedWorldOverlayCapture::Complete() noexcept
{
    if(!active_)return false;
    sink_->EndExternalOverlay();routing_.reset();active_=false;
    // FinishPhase never overwrites a source failure or Unsupported status.
    builder_.FinishPhase(OwnedFramePhase::Swipe);return true;
}
IUIRenderer& OwnedWorldOverlayCapture::UI() {return sink_->UI();}
TbBool OwnedWorldOverlayCapture::DrawTextResized(int32_t x,int32_t y,int32_t units,const char* text)
{
    // GL redirects only UI into its Swipe buffer; normal text remains GameUI.
    return sink_->DrawTextResized(x,y,units,text);
}
void OwnedWorldOverlayCapture::BeginSpriteBucket(int32_t)
{
    builder_.Unsupported(OwnedFramePhase::Swipe);
}
void OwnedWorldOverlayCapture::CaptureKeeperSprite(const SpriteScale*,const uint8_t*,int,int,int32_t,
    unsigned,const uint8_t*,int32_t,int,bool,uint8_t)
{
    // A world keeper-sprite callback is not an ordinary UI swipe submission.
    builder_.Unsupported(OwnedFramePhase::Swipe);
}
}
