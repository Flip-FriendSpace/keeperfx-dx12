// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#pragma once
#include "OwnedRenderAssets.h"
#include "WorldDrawMath.h"
#include <array>
#include <memory>
#include <span>
#include <vector>

namespace kfx::render {
// Exact RenderGraph execution order. A phase is never inferred empty merely
// because its producer has not yet been ported.
enum class OwnedFramePhase : uint8_t {
    Clear, PopulateUI, BeginWorldCapture, World, WorldSprites, WorldFlatOverlay,
    Swipe, ResolveWorldCapture, LensPaletteUIExclusion, CaptureMapFadeWorld,
    ImagePresents, OverheadMap, PiPCaptures, GameUI, ResolveMapFade, ZoomBoxes,
    FrontOverlay, Text, ImagePresentOverlay, Cursor, DevTools, Screenshot, ScrgbLift, Count,
};
enum class OwnedPhaseState : uint8_t { Unvisited, Empty, Recorded, Unsupported };
enum class OwnedFrameStatus : uint8_t { Ready, WouldBlock, Incomplete, InvalidInput, CommandLimit };
struct OwnedFrameInfo {
    uint64_t epoch = 0, sequence = 0;
    uint32_t native_width = 0, native_height = 0;
    int32_t viewport_x = 0, viewport_y = 0;
    uint32_t viewport_width = 0, viewport_height = 0;
    std::array<float,4> clear_rgba{0,0,0,1};
    std::array<float,9> shade_settings{0,0,1,1,0,1,1,0.4f,0};
    std::array<float,9> palette_transform{1,0,0,0,1,0,0,0,1};
    float time = 0;
    bool clear_uses_display_palette=false;
    uint8_t clear_palette_index=0;
};
struct OwnedWorldTerrain {
    WorldDrawTriangle vertices{};
    OwnedAssetLease tile; // Required proven source; no hidden missing-texture fallback.
    bool emissive_lava=false; // Explicit authenticated material; default unchanged.
};
// Resolved neutral draw records; no engine pointer, sprite handle or physical
// atlas address survives. Coordinates and phases retain producer order.
enum class OwnedSpritePass : uint8_t { Silhouette, Edge, VisibleCoverage, Normal, GlowAdd, GlowDarken };
struct OwnedWorldSprite {
    OwnedFramePhase phase=OwnedFramePhase::WorldSprites;
    OwnedSpritePass pass = OwnedSpritePass::Normal;
    int32_t x=0,y=0,width=0,height=0,phase_x=0,phase_y=0,step_x=65536,step_y=65536;
    uint32_t source_width=0,source_height=0,content_height=0;
    bool flip=false,premultiplied=true,texture_fallback=false;
    float z_ndc=0,source_alpha=1;
    std::array<float,4> colour{};
    OwnedAssetLease source,colour_row;
};
struct OwnedShadowVertex { float x=0,y=0,u=0,v=0; };
struct OwnedWorldShadow {
    std::array<OwnedShadowVertex,4> vertices{};
    float z_ndc=0,darken=0;
    uint32_t source_width=0,source_height=0;
    OwnedAssetLease source;
};
enum class OwnedQuadMode : uint8_t { Sprite, Colored, Solid, Fade, Clut, OpaqueIndexed };
enum class OwnedQuadPaletteSource : uint8_t { Fixed, Display, DisplayRemap, DisplayIndex };
struct OwnedFrameQuad {
    bool parchment_capture=false; // Separate redirected UI, resolved immediately before map-fade.
    OwnedFramePhase phase=OwnedFramePhase::GameUI;
    OwnedQuadMode mode=OwnedQuadMode::Sprite;
    bool depth_test=false,scissor_enabled=false;
    int32_t scissor_x=0,scissor_y=0,scissor_w=0,scissor_h=0;
    float x0=0,y0=0,x1=0,y1=0,u0=0,v0=0,u1=0,v1=0,z_ndc=0;
    std::array<float,4> colour{1,1,1,1};
    uint8_t fade_row=0;
    OwnedAssetLease source,colour_row;
    // Exact source-index provenance for cached palette-only re-presentation.
    OwnedQuadPaletteSource palette_source=OwnedQuadPaletteSource::Fixed;
    uint8_t palette_index=0;
    std::array<uint8_t,256> palette_remap{};
};
enum class OwnedImageMode : uint8_t { Opaque, Transparent, Composite, LandviewZoom };
struct OwnedFrameImage {
    OwnedFramePhase phase=OwnedFramePhase::ImagePresents;
    OwnedImageMode mode=OwnedImageMode::Opaque;
    int32_t x=0,y=0;
    uint32_t width=0,height=0; // Destination extent; source dimensions come from its lease.
    OwnedAssetLease source,palette,coverage;
    bool uses_display_palette=false;
    // Map centre XY, native screen centre XY, source pixels per native pixel.
    std::array<float,5> zoom{};
};
enum class OwnedLensKind : uint8_t { None, Copy, Remap, Mist, Overlay };
struct OwnedFrameLens {
    OwnedLensKind kind=OwnedLensKind::None;
    int32_t source_x=0,destination_x=0,destination_y=0;
    uint32_t viewport_width=0,viewport_height=0;
    std::array<float,4> mist_offsets{},clear_rgba{};
    uint8_t mist_lightness=0;
    uint16_t overlay_alpha=0;
    OwnedAssetLease palette,fade,rgb6,density,remap,overlay;
    bool display_clear=false;
    OwnedAssetLease rgb6_base; // Immutable pre-override table for palette-only replay.
};
class OwnedRenderFrame;
using OwnedFrameLease=std::shared_ptr<const OwnedRenderFrame>;
struct OwnedFrameMapFade {
    bool active=false,capture_pending=false;
    float step=0,warp_scale=1;
    uint64_t transition_id=0;
    OwnedAssetLease ghost_table,rgb6,rgb6_base,palette,fade;
};
struct OwnedFrameLimits {
    size_t terrain=65536,sprites=16384,shadows=8192,quads=16384,images=64;
    size_t command_bytes=128u*1024u*1024u; // All three slots, retained capacity.
};
struct OwnedFrameMailboxStats {
    size_t occupied_slots=0,command_capacity_bytes=0;
    bool builder_active=false,pending=false;
};
namespace owned_frame_detail { struct Mailbox; }
class OwnedFrameBuilder;
class OwnedFrameMailbox;
// Fixed producer-only cache for regenerated display-remap rows. Its exclusive
// bank advances only when the target palette identity changes. Partial rows
// survive pressure retries; immutable previously presented frames are untouched.
class OwnedFramePaletteReplay {
public:
    explicit OwnedFramePaletteReplay(OwnedRenderAssets& assets,uint64_t domain=320) noexcept:assets_(assets),domain_(domain){}
    // Scene lifetime reset must not depend on another remap/lens being drawn.
    void ResetEpoch(uint64_t epoch) noexcept;
private:
    OwnedRenderAssets& assets_;
    uint64_t domain_,epoch_=0,generation_=0;
    OwnedAssetKey palette_key_{};
    size_t count_=0;
    std::array<std::array<uint8_t,256>,256> maps_{};
    std::array<OwnedAssetLease,256> rows_{};
    OwnedAssetLease rgb6_;
    OwnedAssetKey rgb6_base_key_{};
    uint64_t rgb6_revision_=0;
    std::array<uint8_t,512u*512u> rgb6_scratch_{}; // Separate fixed producer scratch.
    OwnedFrameStatus Lookup(const OwnedAssetLease& base,const OwnedAssetLease& palette,OwnedAssetLease& result) noexcept;
    OwnedFrameStatus Prepare(const OwnedAssetLease& palette) noexcept;
    OwnedFrameStatus Row(const std::array<uint8_t,256>& map,const OwnedAssetLease& palette,OwnedAssetLease& result) noexcept;
    friend class OwnedFrameBuilder;
};
class OwnedRenderFrame {
public:
    const OwnedFrameInfo& Info() const noexcept { return info_; }
    OwnedFrameStatus Status() const noexcept { return status_; }
    bool Complete() const noexcept { return status_==OwnedFrameStatus::Ready; }
    OwnedPhaseState Phase(OwnedFramePhase phase) const noexcept;
    std::span<const OwnedWorldTerrain> Terrain() const noexcept { return terrain_; }
    std::span<const OwnedWorldSprite> OutlineSprites() const noexcept { return outlines_; }
    std::span<const OwnedWorldSprite> Sprites() const noexcept { return sprites_; }
    std::span<const OwnedWorldShadow> Shadows() const noexcept { return shadows_; }
    std::span<const OwnedFrameQuad> Quads() const noexcept { return quads_; }
    std::span<const OwnedFrameImage> Images() const noexcept { return images_; }
    const OwnedAssetLease& Palette() const noexcept { return palette_; }
    const OwnedAssetLease& Fade() const noexcept { return fade_; }
    const OwnedAssetLease& Lightmap() const noexcept { return lightmap_; }
    const OwnedFrameLens& Lens() const noexcept { return lens_; }
    const OwnedFrameMapFade& MapFade() const noexcept { return map_fade_; }
private:
    OwnedFrameInfo info_;
    OwnedFrameStatus status_=OwnedFrameStatus::Incomplete;
    std::array<OwnedPhaseState,size_t(OwnedFramePhase::Count)> phases_{};
    OwnedAssetLease palette_,fade_,lightmap_;
    OwnedFrameLens lens_;
    OwnedFrameMapFade map_fade_;
    std::vector<OwnedWorldTerrain> terrain_;
    std::vector<OwnedWorldSprite> sprites_,outlines_;
    std::vector<OwnedWorldShadow> shadows_;
    std::vector<OwnedFrameQuad> quads_;
    std::vector<OwnedFrameImage> images_;
    explicit OwnedRenderFrame(const OwnedFrameLimits& limits);
    void Reset(const OwnedFrameInfo& info) noexcept;
    size_t CapacityBytes() const noexcept;
    friend class OwnedFrameBuilder;
    friend class OwnedFrameMailbox;
    friend struct owned_frame_detail::Mailbox;
};
// A move-only producer write window. Seal returns immutable ownership and closes
// the builder. Incomplete fragments are inspectable, but mailbox admission rejects
// them. Explicit FinishPhase means that producer ran (possibly with no commands).
class OwnedFrameBuilder {
public:
    OwnedFrameBuilder() = default;
    ~OwnedFrameBuilder();
    OwnedFrameBuilder(OwnedFrameBuilder&&) noexcept;
    OwnedFrameBuilder& operator=(OwnedFrameBuilder&&) noexcept;
    OwnedFrameBuilder(const OwnedFrameBuilder&)=delete;
    OwnedFrameBuilder& operator=(const OwnedFrameBuilder&)=delete;
    explicit operator bool() const noexcept { return bool(frame_); }
    const OwnedFrameInfo& Info() const;
    bool EmptyDraws() const noexcept;
    OwnedAssetLease Fade() const noexcept { return frame_ ? frame_->Fade() : OwnedAssetLease{}; }
    // Assign final submission order after any cached presentations during capture.
    bool SetPresentationSequence(uint64_t sequence) noexcept;
    bool AddTerrain(OwnedWorldTerrain draw) noexcept;
    // Replay preflights all bounded resources before mutating this builder.
    // WouldBlock leaves it unchanged, allowing the same immutable retry.
    OwnedFrameStatus ReplayDisplayPalette(const OwnedRenderFrame& source,OwnedAssetLease display,OwnedFramePaletteReplay& rows) noexcept;
    // Uses a spare slot in the same bounded mailbox while this normal write
    // window remains active. Output is unchanged when admission blocks.
    OwnedFrameStatus ReplayCachedFrame(const OwnedRenderFrame& source,OwnedAssetLease display,OwnedFramePaletteReplay& rows,
        const OwnedFrameInfo& info,OwnedFrameLease& output) noexcept;
    // Refresh recorded write-side commands in place, without geometry copies or
    // callbacks. Preflight failure leaves commands and phase states unchanged.
    OwnedFrameStatus RefreshDisplayPalette(OwnedAssetLease display,OwnedFramePaletteReplay& rows) noexcept;
    bool SetDisplayClear(uint8_t index,const OwnedAssetLease& display) noexcept;
    bool SetClearRGBA(const std::array<float,4>& colour) noexcept;
    bool SetWorldViewport(int32_t x,int32_t y,uint32_t width,uint32_t height) noexcept;
    bool SetWorldState(const std::array<float,9>& shade_settings,const std::array<float,9>& palette_transform,float time) noexcept;
    bool SetWorldAssets(OwnedAssetLease palette,OwnedAssetLease fade,OwnedAssetLease lightmap) noexcept;
    // Outline draws retain their own producer order before every normal/glow
    // draw, matching the normal instanced sprite phase.
    bool AddSprite(OwnedWorldSprite draw) noexcept;
    bool AddShadow(OwnedWorldShadow draw) noexcept;
    bool AddQuad(OwnedFrameQuad draw) noexcept;
    bool AddImage(OwnedFrameImage draw) noexcept;
    void SetLens(OwnedFrameLens lens) noexcept;
    bool SetMapFade(OwnedFrameMapFade fade) noexcept;
    void FinishPhase(OwnedFramePhase phase) noexcept;
    void RecordPhase(OwnedFramePhase phase) noexcept;
    void Unsupported(OwnedFramePhase phase) noexcept;
    void Fail(OwnedFrameStatus status) noexcept;
    OwnedFrameLease Seal() noexcept;
private:
    std::shared_ptr<owned_frame_detail::Mailbox> owner_;
    std::shared_ptr<OwnedRenderFrame> frame_;
    OwnedFrameBuilder(std::shared_ptr<owned_frame_detail::Mailbox>,std::shared_ptr<OwnedRenderFrame>) noexcept;
    bool owns_window_=true;
    static OwnedFrameStatus PrepareDisplayPalette(const OwnedRenderFrame& source,const OwnedAssetLease& display,
        OwnedFramePaletteReplay& rows,OwnedAssetLease& rgb6,OwnedAssetLease& map_rgb6) noexcept;
    void ApplyDisplayPalette(const OwnedAssetLease& display,OwnedFramePaletteReplay& rows,OwnedAssetLease rgb6,OwnedAssetLease map_rgb6) noexcept;
    void Close() noexcept;
    friend class OwnedFrameMailbox;
};
// Three preallocated packet slots cover front + one pending + one builder.
// Saved/re-presented packets keep their slot occupied. No allocation or packet
// overwrite occurs to bypass a slow reader; Begin returns an empty builder.
// Producer writes a builder exclusively; queue operations may cross threads.
class OwnedFrameMailbox {
public:
    explicit OwnedFrameMailbox(OwnedFrameLimits limits = {});
    OwnedFrameBuilder Begin(const OwnedFrameInfo& info);
    OwnedFrameStatus TryPublish(const OwnedFrameLease& packet) noexcept;
    OwnedFrameLease TakePending() noexcept;
    OwnedFrameMailboxStats Stats() const noexcept;
private:
    std::shared_ptr<owned_frame_detail::Mailbox> state_;
};
}
