// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#pragma once
#include "OwnedWorldDrawSink.h"
#include "IUIRenderer.h"
#include "ITextRenderer.h"
#include "ICursorLayer.h"
#include <optional>

struct RendererPresentImageDesc;
struct TbHugeSprite;
namespace kfx::render {
// Producer-thread interfaces. Binding captures all sources into the current
// packet once; Draw never re-reads an engine sprite or animation callback.
class OwnedFrameUI final:public IUIRenderer,public ITextRenderer,public ICursorLayer {
public:
    OwnedFrameUI(OwnedRenderAssets& assets,OwnedWorldSpriteSourceCache& sources);
    ~OwnedFrameUI() override;
    bool Bind(OwnedFrameBuilder& builder,OwnedWorldDrawSettings settings);
    void Unbind() noexcept;
    bool Bound() const noexcept {return bool(sink_);}
    bool SetParchmentCapture(bool active) noexcept;
    bool SubmitMapFadeStep(int tick,float step,float warp,bool fading_in,const uint8_t* ghost,const OwnedAssetLease& fade);
    bool SubmitZoomBoxTiles(const uint16_t* ids,int nx,int ny,int x,int y,int w,int h);
    bool HasImagePresents() const noexcept {return image_slot_!=0 || zoom_submitted_;}
    size_t ScratchBytes() const noexcept;
    SpriteHandle ResolveSprite(const TbSprite*) override;
    void RegisterSpriteSheet(const TbSpriteSheet*) override {}
    TbResult SubmitRawSprite(int32_t,int32_t,const TbSprite*,KfxDrawState) override;
    TbResult SubmitRawSpriteOneColour(int32_t,int32_t,const TbSprite*,unsigned char,KfxDrawState) override;
    TbResult SubmitRawSpriteScaled(int32_t,int32_t,const TbSprite*,int32_t,int32_t,KfxDrawState) override;
    TbResult SubmitRawSpriteScaledOneColour(int32_t,int32_t,const TbSprite*,int32_t,int32_t,unsigned char,KfxDrawState) override;
    int SubmitRawSpriteScaledRemap(int32_t,int32_t,const TbSprite*,int32_t,int32_t,const unsigned char*,KfxDrawState) override;
    void SubmitSolidBox(int32_t,int32_t,int32_t,int32_t,uint8_t,KfxDrawState) override;
    void SubmitSlabBackground(int32_t,int32_t,int32_t,int32_t) override;
    void UpdateSlabTexture(const unsigned char*,int) override;
    uint8_t* AcquireMinimapBuffer(int,int,int) override;
    void SubmitMinimap(int,int,int,const int32_t*,const int32_t*) override;
    void SetWorldOverlay(float) override;
    void ClearWorldOverlay() override;
    void SetWorldOverlayFlat(float) override;
    void ClearWorldOverlayFlat() override;
    void SetTopOverlay() override;
    void ClearTopOverlay() override;
    void BeginZoomBoxOverlay(int32_t,int32_t,int32_t,int32_t) override;
    void EndZoomBoxOverlay(int32_t,int32_t,int32_t,int32_t) override;
    void DrawFrontOverlay() override {}
    void SetUICommandBuffers(UICommandBuffers*) override;
    void SetTextCommandBuffers(TextCommandBuffers*) override;
    TbBool DrawTextResized(int32_t,int32_t,int32_t,const char*) override;
    void SubmitPointerSprite(const TbSprite*,int32_t,int32_t,int) override;
    void SubmitKeeperHandSprite(short,short,unsigned short,short,unsigned char,int32_t,TbDrawFlagsMask) override;
    void Draw() override;
    void Clear() override;
    bool PresentImage(const RendererPresentImageDesc&,OwnedFramePhase phase=OwnedFramePhase::ImagePresents);
    bool SubmitLandviewZoom(const unsigned char*,int width,int height,float map_x,float map_y,
        float screen_x,float screen_y,float scale);
    void PresentHugeSprite(const TbHugeSprite*,int32_t,int32_t,int32_t,int32_t);
    // Native indexed image pixels are copied synchronously. Transparent drops
    // index zero; opaque includes it. Composite includes index zero; optional coverage belongs to Transparent.
    bool SubmitIndexedImage(std::span<const uint8_t> bytes,size_t stride,uint32_t width,uint32_t height,
        int32_t x,int32_t y,OwnedFramePhase phase,OwnedImageMode mode=OwnedImageMode::Opaque,
        std::span<const uint8_t> coverage={},size_t coverage_stride=0);
    const char* GetName() const override {return "Owned frame UI";}
private:
    struct Source {OwnedAssetLease lease;uint64_t generation=0,revision=0;};
    OwnedRenderAssets& assets_;
    OwnedWorldSpriteSourceCache& sources_;
    OwnedFrameBuilder* builder_=nullptr;
    std::unique_ptr<OwnedWorldDrawSink> sink_;
    OwnedWorldDrawSettings settings_;
    uint64_t epoch_=0;
    Source slab_,minimap_,zoom_,map_ghost_,map_rgb6_,map_base_;
    std::array<Source,512> zoom_tiles_;
    std::array<uint16_t,512> zoom_tile_ids_{};
    std::array<bool,512> zoom_tile_used_{};
    size_t zoom_tile_victim_=0;
    std::array<uint8_t,768> map_base_palette_{};
    std::vector<uint8_t> map_base_bytes_,map_rgb6_bytes_;
    bool have_map_base_=false,parchment_capture_=false,zoom_capture_=false;
    bool map_was_active_=false,map_submitted_=false;
    uint64_t map_transition_=0;
    bool zoom_submitted_=false;
    std::array<Source,64> images_,coverage_,palettes_;
    size_t image_slot_=0;
    std::vector<uint8_t> slab_bytes_,minimap_bytes_,huge_pixels_,huge_coverage_;
    int slab_dim_=0,minimap_size_=0,minimap_x_=0,minimap_y_=0;
    std::optional<OwnedFrameQuad> pointer_;
    std::optional<OwnedFrameImage> image_overlay_;
    OwnedAssetLease Publish(Source&,uint64_t domain,uint64_t id,OwnedAssetView);
    OwnedFramePhase Phase() const noexcept;
    void Fail(OwnedFrameStatus=OwnedFrameStatus::Incomplete) noexcept;
};
}
