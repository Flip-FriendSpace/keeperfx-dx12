// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
// Game-thread engine interface for the owned world producer.
#pragma once
#include "IWorldViewRenderer.h"
#include "OwnedGameWorld.h"

namespace kfx::render {
class OwnedWorldViewRenderer final : public IWorldViewRenderer {
public:
    // The backend owns the driver, asset catalogue and epoch. The driver must
    // outlive this adapter; no render-thread work is performed here.
    explicit OwnedWorldViewRenderer(OwnedGameWorld& driver) : driver_(driver) {}

    // Borrow exactly one live builder until BindFrame(nullptr). The backend
    // must unbind before moving, sealing or destroying that builder.
    void BindFrame(OwnedFrameBuilder* builder);
    void EndWorldProduction();

    void BeginWorldPass(int width, int height, int viewport_x, int viewport_y) override;
    void DrawIsometricView() override;
    void DrawFrontView(Camera* camera) override;
    void SubmitPossessionLens(const IRWorldLensCmd& command) override;
    void SetPaletteSource(const uint8_t* palette) override;
    void SetDisplayPalette(std::span<const uint8_t> rgb8);
    const char* GetName() const override { return "OWNED-WORLD"; }

    // Normal traversal intercepts these through ScopedWorldSubmissionCapture.
    // Reaching the adapter directly means no owned source capture is active.
    void BeginWorldSpriteCapture(int32_t bucket) override;
    void SubmitKeeperSprite(const SpriteScale* scale, const unsigned char* data,
        int source_width, int source_height, int32_t content_height,
        unsigned int draw_flags, const unsigned char* remap, int32_t sprite_id) override;

    int ResolveDeferredWorld() override;
    void ReexecuteDeferredWorld() override;
    void MarkDeferredWorldAsLensCapture() override;
    void SetWorldCommandBuffers(WorldCommandBuffers* commands) override;
    void ExecuteFromIR(const WorldCommandBuffers& commands) override;

private:
    OwnedGameWorld& driver_;
    OwnedFrameBuilder* builder_ = nullptr;
    bool view_started_ = false, view_attempted_ = false, view_populated_ = false;
    bool lens_submitted_ = false, ended_ = false, rejected_ = false;

    OwnedFrameBuilder& BoundFrame();
    void Populate(WorldDrawView view, Camera* camera);
    void Unsupported(OwnedFramePhase phase);
};
}
