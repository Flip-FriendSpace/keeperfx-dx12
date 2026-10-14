// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#pragma once
#include "IRenderer.h"
#include <memory>

// Optional Windows x64 development backend. All engine submissions are resolved
// on the game thread into immutable packets before the asynchronous GPU call.
class RendererDX12 final : public IRenderer {
public:
    RendererDX12();
    ~RendererDX12() override;
    bool Init() override;
    void Shutdown() override;
    const char* GetName() const override { return "DX12"; }
    void SetDisplayPalette(const unsigned char*) override;
    void ClearScreen(unsigned char) override;
    void PresentFrame() override;
    bool BeginFrame() override;
    void EndFrame() override;
    bool CanDraw() const override;
    bool CanPresent() const override;
    BackendCapabilities GetCapabilities() const override { return {1}; }
    bool PresentImage(const RendererPresentImageDesc*) override;
    void PresentHugeSprite(const TbHugeSprite*,int32_t,int32_t,int32_t,int32_t) override;
    void SubmitZoomBoxTiles(const uint16_t*,int,int,int,int,int,int) override;
    bool SubmitLandviewZoom(const unsigned char*,int,int,float,float,float,float,float) override;
    bool ScheduleScreenshot(const char*,int) override;
    bool WriteDiagnostics(const char*) override;
    bool SetDiagnosticReaderHold(bool) override;
    ITextRenderer* GetTextRenderer() override;
    IUIRenderer* GetUIRenderer() override;
    ICursorLayer* GetCursorLayer() override;
    IWorldViewRenderer* GetWorldViewRenderer() override;
    void SubmitMapFadeStep(int,float,float,bool,const unsigned char*) override;
    void BeginOverlayCapture(OverlayCaptureKind) override;
    void EndOverlayCapture(OverlayCaptureKind) override;
    bool MapFadeSupportsNativeResolution() const override { return true; }
    bool BeginIndexedWorldFallback(int,int,unsigned char&) override;
    bool SubmitIndexedWorldFallback(const RendererPresentImageDesc*) override;
    bool GetIndexedWorldFallbackPalette(std::array<uint8_t,1024>&) const override;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
