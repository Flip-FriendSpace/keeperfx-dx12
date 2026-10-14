// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#include "pre_inc.h"
#include "kfx/renderer/OwnedWorldViewRenderer.h"
#include "kfx/renderer/RendererThread.h"
#include "kfx/renderer/ir/WorldCommands.h"
#include <stdexcept>
#include "post_inc.h"

namespace kfx::render {
OwnedFrameBuilder& OwnedWorldViewRenderer::BoundFrame()
{
    ASSERT_GAME_THREAD();
    if (!builder_ || !*builder_)
        throw std::logic_error("Owned world submission requires a live bound frame");
    return *builder_;
}

void OwnedWorldViewRenderer::BindFrame(OwnedFrameBuilder* builder)
{
    ASSERT_GAME_THREAD();
    if (builder) {
        if (builder_) {
            builder_->Fail(OwnedFrameStatus::InvalidInput);
            builder->Fail(OwnedFrameStatus::InvalidInput);
            throw std::logic_error("Unbind the previous owned world frame before rebinding");
        }
        if (!*builder)
            throw std::invalid_argument("Cannot bind an empty owned frame builder");
    } else if (builder_ && !ended_) {
        builder_->Fail(OwnedFrameStatus::Incomplete);
    }
    builder_ = builder;
    view_started_ = view_attempted_ = view_populated_ = false;
    lens_submitted_ = ended_ = rejected_ = false;
    SetSceneSnapshot({});
}

void OwnedWorldViewRenderer::BeginWorldPass(int width, int height, int viewport_x, int viewport_y)
{
    auto& frame = BoundFrame();
    if (ended_ || view_started_) {
        Unsupported(OwnedFramePhase::World);
        return;
    }
    view_started_ = true;
    if (width <= 0 || height <= 0
        || !frame.SetWorldViewport(viewport_x, viewport_y, uint32_t(width), uint32_t(height))) {
        rejected_ = true;
        frame.Fail(OwnedFrameStatus::InvalidInput);
        return;
    }
    if (!rejected_) driver_.BeginView(width, height);
}

void OwnedWorldViewRenderer::Populate(WorldDrawView view, Camera* camera)
{
    auto& frame = BoundFrame();
    if (ended_ || !view_started_ || view_attempted_) {
        Unsupported(OwnedFramePhase::World);
        return;
    }
    view_attempted_ = true;
    if (rejected_) return;
    if (view == WorldDrawView::Front && !camera) {
        rejected_ = true;
        frame.Fail(OwnedFrameStatus::InvalidInput);
        return;
    }
    try {
        view_populated_ = driver_.Populate(frame, view, camera);
    } catch (...) {
        rejected_ = true;
        frame.Fail(OwnedFrameStatus::Incomplete);
        throw;
    }
    if (!view_populated_) rejected_ = true;
}

void OwnedWorldViewRenderer::DrawIsometricView()
{
    Populate(WorldDrawView::Isometric, nullptr);
}

void OwnedWorldViewRenderer::DrawFrontView(Camera* camera)
{
    Populate(WorldDrawView::Front, camera);
}

void OwnedWorldViewRenderer::SubmitPossessionLens(const IRWorldLensCmd& command)
{
    auto& frame = BoundFrame();
    if (ended_ || lens_submitted_ || (command.active && !view_populated_)) {
        Unsupported(OwnedFramePhase::ResolveWorldCapture);
        return;
    }
    lens_submitted_ = true;
    if (rejected_) return;
    if (!driver_.PopulateLens(frame, command)) rejected_ = true;
}

void OwnedWorldViewRenderer::EndWorldProduction()
{
    auto& frame = BoundFrame();
    if (ended_) return;
    if (view_started_ && !view_populated_) {
        rejected_ = true;
        frame.Fail(OwnedFrameStatus::Incomplete);
    } else if (!view_started_) {
        // The complete submission window contained no world view (e.g. menus).
        // A successful traversal finishes these same phases in its owned sink.
        frame.FinishPhase(OwnedFramePhase::World);
        frame.FinishPhase(OwnedFramePhase::WorldSprites);
        frame.FinishPhase(OwnedFramePhase::WorldFlatOverlay);
    }
    if (!lens_submitted_) {
        const IRWorldLensCmd inactive;
        lens_submitted_ = true;
        if (!driver_.PopulateLens(frame, inactive)) rejected_ = true;
    }
    // Clear, capture begin, swipe, UI exclusion and all other composition
    // phases belong to the parent backend and are deliberately not completed.
    ended_ = true;
}

void OwnedWorldViewRenderer::SetPaletteSource(const uint8_t* palette)
{
    ASSERT_GAME_THREAD();
    if (!palette) throw std::invalid_argument("Missing owned sprite palette");
    driver_.SetSpritePalette({palette, 768});
}

void OwnedWorldViewRenderer::SetDisplayPalette(std::span<const uint8_t> rgb8)
{
    ASSERT_GAME_THREAD();
    driver_.SetDisplayPalette(rgb8);
}

void OwnedWorldViewRenderer::Unsupported(OwnedFramePhase phase)
{
    auto& frame = BoundFrame();
    rejected_ = true;
    frame.Unsupported(phase);
}

void OwnedWorldViewRenderer::BeginWorldSpriteCapture(int32_t)
{
    Unsupported(OwnedFramePhase::WorldSprites);
}

void OwnedWorldViewRenderer::SubmitKeeperSprite(const SpriteScale*, const unsigned char*,
    int, int, int32_t, unsigned int, const unsigned char*, int32_t)
{
    Unsupported(OwnedFramePhase::WorldSprites);
}

int OwnedWorldViewRenderer::ResolveDeferredWorld()
{
    Unsupported(OwnedFramePhase::World);
    return 0;
}

void OwnedWorldViewRenderer::ReexecuteDeferredWorld()
{
    Unsupported(OwnedFramePhase::World);
}

void OwnedWorldViewRenderer::MarkDeferredWorldAsLensCapture()
{
    Unsupported(OwnedFramePhase::ResolveWorldCapture);
}

void OwnedWorldViewRenderer::SetWorldCommandBuffers(WorldCommandBuffers* commands)
{
    if (commands) Unsupported(OwnedFramePhase::World);
}

void OwnedWorldViewRenderer::ExecuteFromIR(const WorldCommandBuffers&)
{
    Unsupported(OwnedFramePhase::World);
}
}
