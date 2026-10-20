// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
// KeeperFX renderer performance changes: 2026-10-19.
#pragma once
#include "OwnedRenderFrame.h"
#include "KeeperSpriteDarkenCache.h"
#include "WorldDrawProducer.h"
#include "WorldSubmissionCapture.h"
#include <array>
#include <memory>

struct TbSprite;
struct AsianFont;

namespace kfx::render {
struct OwnedWorldDrawSettings {
    uint64_t terrain_domain=1,sprite_domain=2,colour_row_domain=3,ui_source_domain=5,ui_colour_row_domain=6;
    uint64_t dbc_domain_base=7; // Sixteen independent loaded-font identities.
    OwnedAssetLease palette; // Exact opaque RGBA sprite/base palette.
    OwnedAssetLease ui_palette; // Exact display RGBA palette for normal UI.
    uint8_t outline_mode=0; // 0 none, 1 silhouette, 2 edge; explicit frame state.
    float outline_alpha=0;
    int bucket_count=0;
};
// Persistent producer bookkeeping: exact remap identities, not hashes. Palette
// changes start a new row generation. At most 256 mappings plus identity; no
// per-frame revision or allocation on an unchanged source.
class OwnedWorldSpriteSourceCache {
public:
    static constexpr size_t MaxColourRows=256;
private:
    struct Rows {
        OwnedAssetKey palette_key_{};
        uint64_t epoch_=0,row_generation_=0;
        size_t remaps_=0;
        std::array<std::array<uint8_t,256>,MaxColourRows> remap_{};
    };
    Rows sprite_,ui_;
    KeeperSpriteDarkenCache darken_;
    uint64_t font_epoch_=0;
    std::array<uint64_t,16> font_stamps_{};
    friend class OwnedWorldDrawSink;
};
class OwnedWorldUI;
class OwnedWorldDrawSink final : public WorldDrawSink, public WorldSubmissionCapture {
public:
    OwnedWorldDrawSink(OwnedFrameBuilder& builder,OwnedRenderAssets& assets,
        OwnedWorldSpriteSourceCache& sources,OwnedWorldDrawSettings settings);
    ~OwnedWorldDrawSink() override;
    // Install scoped normal callbacks for exactly one traversal. No engine lookup
    // or callback is retained by the sealed packet or repeated by a render retry.
    bool Produce(WorldDrawView view,std::span<BasicQ* const> buckets,
        const WorldDrawProjection& projection,int pixel_scale,Camera* camera);
    void BeginBucket(int bucket) override;
    void Triangle(const WorldDrawTriangle& vertices) override;
    void Sprite(WorldSpriteDispatch mode,BucketKindJontySprite& sprite,Camera* camera) override;
    void Shadow(const BucketKindCreatureShadow& shadow) override;
    void Overlay(WorldOverlayDispatch kind,WorldOverlayDepth depth,float z_ndc,BasicQ& entry) override;
    void BeginSpriteBucket(int32_t bucket) override;
    void CaptureKeeperSprite(const SpriteScale* scale,const uint8_t* data,int width,int height,
        int32_t content_height,unsigned flags,const uint8_t* remap,int32_t sprite_id,
        int owner,bool wants_outline,uint8_t alpha) override;
    IUIRenderer& UI() override;
    TbBool DrawTextResized(int32_t,int32_t,int32_t,const char*) override;
    void UnsupportedUI() noexcept;
    // Caller owns an active ScopedWorldSubmissionCapture and exception-safe End.
    // Only Swipe is supported outside the ordinary world traversal.
    bool BeginExternalOverlay(OwnedFramePhase phase) noexcept;
    void EndExternalOverlay() noexcept;
    void SetParchmentCapture(bool active) noexcept { parchment_capture_=active; }
private:
    OwnedFrameBuilder& builder_;
    OwnedRenderAssets& assets_;
    OwnedWorldSpriteSourceCache& sources_;
    OwnedWorldDrawSettings settings_;
    std::unique_ptr<IUIRenderer> ui_;
    float sprite_z_=0;
    uint32_t source_animation_=0;
    uint32_t source_class_=0,source_model_=0;
    bool cursor_capture_=false;
    bool parchment_capture_=false,zoom_box_capture_=false;
    bool dispatch_active_=false,external_overlay_=false;
    OwnedFramePhase previous_overlay_phase_=OwnedFramePhase::WorldSprites;
    OwnedFramePhase overlay_phase_=OwnedFramePhase::WorldSprites;
    OwnedAssetLease ColourRow(const uint8_t* remap,bool ui=false);
    OwnedAssetLease UISource(const TbSprite* sprite);
    OwnedAssetLease DbcSource(const AsianFont* font,uint32_t codepoint);
    friend class OwnedWorldUI;
    friend class OwnedFrameUI;
    OwnedAssetLease KeeperSource(const uint8_t* data,int width,int height,int32_t sprite_id);
};
}
