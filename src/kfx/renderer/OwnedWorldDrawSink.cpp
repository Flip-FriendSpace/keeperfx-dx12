// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
// KeeperFX renderer performance changes: 2026-10-19.
#include "pre_inc.h"
#include "kfx/renderer/OwnedWorldDrawSink.h"
#include "kfx/renderer/TileSource.h"
#include "kfx/renderer/HdTerrainPack.h"
#include "kfx/renderer/EmissivePresentation.h"
#include "thing_data.h"
#include "thing_list.h"
#include "kfx/renderer/SpriteScale.h"
#include "kfx/renderer/IUIRenderer.h"
#include "kfx/renderer/DrawState.h"
#include "engine_render.h"
#include "engine_buckets.h"
#include "kfx/renderer/WorldShadowSource.h"
#include "bflib_sprite.h"
#include "bflib_sprfnt.h"
#include "kfx/renderer/TextLayout.h"
#include "kfx/renderer/ir/TextCommands.h"
#include "vidmode.h"
#include "player_data.h"
#include "config_players.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include "post_inc.h"

namespace kfx::render {
static_assert(TCls_Object==1 && TCls_Shot==2 && TCls_EffectElem==3);
class OwnedWorldUI final:public IUIRenderer,private TextLayoutSink {
    OwnedAssetLease glyph_source_;
    uint32_t glyph_logical_width_=0, glyph_logical_height_=0;
    const IRTextDrawCmd* text_state_=nullptr;
    bool text_ok_=true;
    OwnedWorldDrawSink& owner;
    bool BeginWesternGlyph(const TbSprite* sprite) override {
        glyph_logical_width_=sprite->SWidth;glyph_logical_height_=sprite->SHeight;
        glyph_source_=owner.UISource(sprite);text_ok_=glyph_source_.Valid() && text_ok_;
        return true; // Preserve normal Western advance even on source failure.
    }
    bool BeginDbcGlyph(const AsianFont* font,uint32_t codepoint) override {
        glyph_source_=owner.DbcSource(font,codepoint);
        glyph_logical_width_=glyph_source_.Valid()?glyph_source_.Desc().width:0;glyph_logical_height_=glyph_source_.Valid()?glyph_source_.Desc().height:0;text_ok_=glyph_source_.Valid() && text_ok_;
        return glyph_source_.Valid();
    }
    void Emit(const TextLayoutGlyph& glyph) override {
        OwnedFrameQuad q;q.phase=owner.zoom_box_capture_?OwnedFramePhase::ZoomBoxes:OwnedFramePhase::GameUI;q.parchment_capture=owner.parchment_capture_;
        q.x0=glyph.x;q.y0=glyph.y;q.colour[3]=glyph.alpha;
        if(glyph.kind==TextLayoutGlyphKind::SolidRect) {
            q.mode=OwnedQuadMode::Solid;q.x1=q.x0+glyph.w;q.y1=q.y0+glyph.h;Colour(q,glyph.colour);
        }else {
            if(!glyph_source_.Valid())return;
            q.source=glyph_source_;const auto shape=q.source.Desc();
            q.x1=q.x0+float(glyph_logical_width_)*float(glyph.units_per_px)/16.0f;q.y1=q.y0+float(glyph_logical_height_)*float(glyph.units_per_px)/16.0f;
            q.u0=(1.0f/32.0f)/shape.width;q.v0=(1.0f/32.0f)/shape.height;q.u1=1-q.u0;q.v1=1-q.v0;
            if(glyph.kind==TextLayoutGlyphKind::ColourSprite){q.mode=OwnedQuadMode::Colored;Colour(q,glyph.colour);}
            else {
                q.mode=text_state_->cmap?OwnedQuadMode::Clut:OwnedQuadMode::Sprite;q.colour_row=owner.ColourRow(text_state_->cmap,true);
                q.palette_source=text_state_->cmap?OwnedQuadPaletteSource::DisplayRemap:OwnedQuadPaletteSource::Display;
                if(text_state_->cmap)std::copy_n(text_state_->cmap,256,q.palette_remap.begin());
            }
        }
        if(text_state_->clip_w>0 && text_state_->clip_h>0) {
            q.scissor_enabled=true;q.scissor_x=text_state_->clip_x;q.scissor_y=text_state_->clip_y;
            q.scissor_w=text_state_->clip_w;q.scissor_h=text_state_->clip_h;
        }
        text_ok_=owner.builder_.AddQuad(std::move(q)) && text_ok_;
    }
    TbResult Reject() { owner.UnsupportedUI(); return Lb_FAIL; }
    OwnedFramePhase Phase() const {
        if(owner.zoom_box_capture_)return OwnedFramePhase::ZoomBoxes;
        if(owner.external_overlay_)return owner.overlay_phase_;
        switch(ComputeCurrentLayer()) {
        case IRUILayer::WorldOverlay:return OwnedFramePhase::WorldSprites;
        case IRUILayer::WorldOverlayFlat:return OwnedFramePhase::WorldFlatOverlay;
        case IRUILayer::Overlay:return OwnedFramePhase::FrontOverlay;
        default:return OwnedFramePhase::GameUI;
        }
    }
    void Colour(OwnedFrameQuad& q,uint8_t index) const {
        q.palette_source=OwnedQuadPaletteSource::DisplayIndex;q.palette_index=index;
        const auto bytes=owner.settings_.ui_palette.Bytes();
        for(size_t c=0;c<3;++c) q.colour[c]=bytes[size_t(index)*4+c]/255.0f;
    }
    bool Push(OwnedFrameQuad q,const IRUIClip& clip) {
        // The normal swipe callback uses GameUI. Reject unimplemented mixed
        // layer replay rather than flattening its original four-pass ordering.
        if(owner.external_overlay_ && ComputeCurrentLayer()!=IRUILayer::GameUI){owner.UnsupportedUI();return false;}
        const float width=q.x1-q.x0,height=q.y1-q.y0;
        if(width<=0 || height<=0) return true;
        const float x0=std::max(q.x0,float(clip.x)),y0=std::max(q.y0,float(clip.y));
        const float x1=std::min(q.x1,float(int64_t(clip.x)+clip.w)),y1=std::min(q.y1,float(int64_t(clip.y)+clip.h));
        if(x1<=x0 || y1<=y0) return true;
        const float du=(q.u1-q.u0)/width,dv=(q.v1-q.v0)/height;
        const float u0=q.u0+(x0-q.x0)*du,u1=q.u0+(x1-q.x0)*du;
        const float v0=q.v0+(y0-q.y0)*dv,v1=q.v0+(y1-q.y0)*dv;
        q.x0=x0;q.y0=y0;q.x1=x1;q.y1=y1;q.u0=u0;q.u1=u1;q.v0=v0;q.v1=v1;
        q.phase=Phase();q.parchment_capture=owner.parchment_capture_;q.depth_test=q.phase==OwnedFramePhase::WorldSprites;q.z_ndc=ComputeCurrentNdcZ();
        return owner.builder_.AddQuad(std::move(q));
    }
    bool Window(int32_t& x,int32_t& y,IRUIClip& clip) {
        const int64_t ax=int64_t(x)+lbDisplay.GraphicsWindowX,ay=int64_t(y)+lbDisplay.GraphicsWindowY;
        if(ax<INT32_MIN || ax>INT32_MAX || ay<INT32_MIN || ay>INT32_MAX) {owner.builder_.Fail(OwnedFrameStatus::InvalidInput);return false;}
        x=int32_t(ax);y=int32_t(ay);
        clip={int32_t(lbDisplay.GraphicsWindowX),int32_t(lbDisplay.GraphicsWindowY),int32_t(lbDisplay.GraphicsWindowWidth),int32_t(lbDisplay.GraphicsWindowHeight)};
        return true;
    }
    TbResult Sprite(int32_t x,int32_t y,const TbSprite* sprite,int32_t w,int32_t h,
        OwnedQuadMode mode,uint8_t colour,const uint8_t* remap,KfxDrawState state,bool natural=false) {
        TbSpriteSourceIdentity placeholder{};
        if(get_ui_sprite_source_identity(sprite,&placeholder) && (!placeholder.width || !placeholder.height))return Lb_SUCCESS;
        auto source=owner.UISource(sprite);if(!source.Valid())return Lb_FAIL;
        if(natural){w=sprite->SWidth;h=sprite->SHeight;}
        if(w<=0 || h<=0)return Lb_SUCCESS;
        IRUIClip clip;if(!Window(x,y,clip))return Lb_FAIL;
        OwnedFrameQuad q;q.mode=mode;q.source=source;
        q.x0=float(x);q.y0=float(y);q.x1=q.x0+float(w);q.y1=q.y0+float(h);
        // Preserve the normal UI nearest-sample inset in source-local texels.
        q.u0=(1.0f/32.0f)/float(source.Desc().width);q.v0=(1.0f/32.0f)/float(source.Desc().height);
        q.u1=1.0f-q.u0;q.v1=1.0f-q.v0;
        if(state.flags&Lb_SPRITE_FLIP_HORIZ)std::swap(q.u0,q.u1);
        if(state.flags&Lb_SPRITE_FLIP_VERTIC)std::swap(q.v0,q.v1);
        q.colour[3]=draw_flags_source_weight(state.flags);
        if(mode==OwnedQuadMode::Colored)Colour(q,colour);
        else if(mode==OwnedQuadMode::Clut) {
            const uintptr_t address=reinterpret_cast<uintptr_t>(remap),fade=reinterpret_cast<uintptr_t>(pixmap.fade_tables);
            if(remap && address>=fade && address-fade<64*256) {
                q.mode=OwnedQuadMode::Fade;q.fade_row=uint8_t((address-fade)/256);q.colour_row=owner.settings_.ui_palette;q.palette_source=OwnedQuadPaletteSource::Display;
            }else {
                q.colour_row=owner.ColourRow(remap,true);q.palette_source=remap?OwnedQuadPaletteSource::DisplayRemap:OwnedQuadPaletteSource::Display;
                if(remap)std::copy_n(remap,256,q.palette_remap.begin());
            }
        }else {q.colour_row=owner.settings_.ui_palette;q.palette_source=OwnedQuadPaletteSource::Display;}
        return Push(std::move(q),clip)?Lb_SUCCESS:Lb_FAIL;
    }
public:
    explicit OwnedWorldUI(OwnedWorldDrawSink& sink):owner(sink) {}
    TbBool Text(int32_t x,int32_t y,int32_t units,const char* text) {
        if(!text || units<=0 || text_state_){owner.builder_.Fail(OwnedFrameStatus::InvalidInput);return false;}
        const auto state=CaptureTextDrawState(x,y,units);text_state_=&state;text_ok_=true;
        try{LayoutText(state,text,*this);}catch(...){text_state_=nullptr;glyph_source_={};throw;}
        text_state_=nullptr;glyph_source_={};return text_ok_;
    }
    SpriteHandle ResolveSprite(const TbSprite*) override { Reject();return kInvalidSpriteHandle; }
    void RegisterSpriteSheet(const TbSpriteSheet*) override {} // Sources resolve synchronously on actual use.
    TbResult SubmitRawSprite(int32_t x,int32_t y,const TbSprite* s,KfxDrawState d) override {return Sprite(x,y,s,0,0,OwnedQuadMode::Sprite,0,nullptr,d,true);}
    TbResult SubmitRawSpriteOneColour(int32_t x,int32_t y,const TbSprite* s,unsigned char c,KfxDrawState d) override {return Sprite(x,y,s,0,0,OwnedQuadMode::Colored,c,nullptr,d,true);}
    TbResult SubmitRawSpriteScaled(int32_t x,int32_t y,const TbSprite* s,int32_t w,int32_t h,KfxDrawState d) override {return Sprite(x,y,s,w,h,OwnedQuadMode::Sprite,0,nullptr,d);}
    TbResult SubmitRawSpriteScaledOneColour(int32_t x,int32_t y,const TbSprite* s,int32_t w,int32_t h,unsigned char c,KfxDrawState d) override {return Sprite(x,y,s,w,h,OwnedQuadMode::Colored,c,nullptr,d);}
    int SubmitRawSpriteScaledRemap(int32_t x,int32_t y,const TbSprite* s,int32_t w,int32_t h,const unsigned char* c,KfxDrawState d) override {return Sprite(x,y,s,w,h,OwnedQuadMode::Clut,0,c,d);}
    void SubmitSolidBox(int32_t x,int32_t y,int32_t w,int32_t h,uint8_t colour,KfxDrawState state) override {
        if(w<=0 || h<=0)return;
        IRUIClip clip;if(!Window(x,y,clip))return;
        auto rect=[&](float px,float py,float width,float height) {
            if(width<=0 || height<=0)return;
            OwnedFrameQuad q;q.mode=OwnedQuadMode::Solid;q.x0=px;q.y0=py;q.x1=px+width;q.y1=py+height;
            Colour(q,colour);q.colour[3]=draw_flags_source_weight(state.flags);Push(std::move(q),clip);
        };
        if(state.flags&Lb_SPRITE_OUTLINE){
            rect(float(x),float(y),float(w),1);rect(float(x),float(int64_t(y)+h-1),float(w),1);
            if(h>2){rect(float(x),float(int64_t(y)+1),1,float(h-2));rect(float(int64_t(x)+w-1),float(int64_t(y)+1),1,float(h-2));}
        }else rect(float(x),float(y),float(w),float(h));
    }
    void SubmitSlabBackground(int32_t,int32_t,int32_t,int32_t) override {Reject();}
    void UpdateSlabTexture(const unsigned char*,int) override {Reject();}
    uint8_t* AcquireMinimapBuffer(int,int,int) override {Reject();throw std::logic_error("Owned minimap producer not implemented");}
    void SubmitMinimap(int,int,int,const int32_t*,const int32_t*) override {Reject();}
    void BeginZoomBoxOverlay(int32_t x,int32_t y,int32_t w,int32_t h) override {
        if(owner.zoom_box_capture_ || owner.parchment_capture_){Reject();return;}
        owner.zoom_box_capture_=true;IUIRenderer::BeginZoomBoxOverlay(x,y,w,h);
    }
    void EndZoomBoxOverlay(int32_t x,int32_t y,int32_t w,int32_t h) override {
        if(!owner.zoom_box_capture_){Reject();return;}
        IUIRenderer::EndZoomBoxOverlay(x,y,w,h);owner.zoom_box_capture_=false;
    }
    void DrawFrontOverlay() override {Reject();}
    void SetUICommandBuffers(UICommandBuffers*) override {Reject();}
    const char* GetName() const override {return "Owned world submission";}
};
OwnedWorldDrawSink::OwnedWorldDrawSink(OwnedFrameBuilder& builder,OwnedRenderAssets& assets,
    OwnedWorldSpriteSourceCache& sources,OwnedWorldDrawSettings settings)
    :builder_(builder),assets_(assets),sources_(sources),settings_(std::move(settings)),ui_(std::make_unique<OwnedWorldUI>(*this))
{
    if(!builder_ || settings_.bucket_count<2 || settings_.outline_mode>2 || !std::isfinite(settings_.outline_alpha)
        || settings_.outline_alpha<0 || settings_.outline_alpha>1 || settings_.terrain_domain==settings_.sprite_domain
        || settings_.terrain_domain==settings_.colour_row_domain || settings_.sprite_domain==settings_.colour_row_domain
        || !settings_.palette.Valid() || settings_.palette.Key().epoch!=builder_.Info().epoch
        || settings_.palette.Desc()!=OwnedAssetDesc{OwnedAssetFormat::RGBA8,256,1}
        || !settings_.ui_palette.Valid() || settings_.ui_palette.Key().epoch!=builder_.Info().epoch
        || settings_.ui_palette.Desc()!=OwnedAssetDesc{OwnedAssetFormat::RGBA8,256,1})
        throw std::invalid_argument("Invalid owned world source settings");
    const uint64_t domains[]={settings_.terrain_domain,settings_.sprite_domain,settings_.colour_row_domain,
        settings_.ui_source_domain,settings_.ui_colour_row_domain};
    if(settings_.dbc_domain_base>UINT64_MAX-(sources_.font_stamps_.size()-1))
        throw std::invalid_argument("Invalid owned font domain range");
    for(size_t i=0;i<std::size(domains);++i){
        if(domains[i]>=settings_.dbc_domain_base && domains[i]-settings_.dbc_domain_base<sources_.font_stamps_.size())
            throw std::invalid_argument("Overlapping owned font source domain");
        for(size_t j=0;j<i;++j)if(domains[i]==domains[j])throw std::invalid_argument("Overlapping owned source domains");
    }
}
OwnedWorldDrawSink::~OwnedWorldDrawSink()=default;
bool OwnedWorldDrawSink::Produce(WorldDrawView view,std::span<BasicQ* const> buckets,
    const WorldDrawProjection& projection,int pixel_scale,Camera* camera)
{
    if(dispatch_active_ || projection.width<=0 || projection.height<=0 || pixel_scale<=0
        || projection.bucket_count!=settings_.bucket_count || buckets.size()>size_t(projection.bucket_count)) {
        builder_.Fail(OwnedFrameStatus::InvalidInput); return false;
    }
    ScopedWorldSubmissionCapture capture(*this);
    if(!capture.Active()) { builder_.Fail(OwnedFrameStatus::Incomplete); return false; }
    dispatch_active_=true;
    try { ProduceWorldDraws(view,buckets,projection,pixel_scale,camera,*this); }
    catch(...) { dispatch_active_=false; builder_.Fail(OwnedFrameStatus::Incomplete); throw; }
    dispatch_active_=false;
    builder_.FinishPhase(OwnedFramePhase::World);
    builder_.FinishPhase(OwnedFramePhase::WorldSprites);
    builder_.FinishPhase(OwnedFramePhase::WorldFlatOverlay);
    return true;
}
void OwnedWorldDrawSink::BeginBucket(int bucket) { BeginSpriteBucket(bucket); }
void OwnedWorldDrawSink::BeginSpriteBucket(int32_t bucket)
{
    bucket=std::clamp(bucket,0,settings_.bucket_count-1);
    sprite_z_=WorldBucketDepth(float(bucket)-0.5f,settings_.bucket_count);
}
void OwnedWorldDrawSink::Triangle(const WorldDrawTriangle& vertices)
{
    const float variation=vertices[0].atlas_layer,tile=vertices[0].tile;
    if(!std::isfinite(variation) || !std::isfinite(tile) || variation<0 || tile<0
        || variation>=TileSourceVariationCount || tile>=TileSourceTileCount
        || std::floor(variation)!=variation || std::floor(tile)!=tile) { builder_.Fail(OwnedFrameStatus::InvalidInput); return; }
    for(const auto& v:vertices) if(v.atlas_layer!=variation || v.tile!=tile) { builder_.Fail(OwnedFrameStatus::InvalidInput); return; }
    TileSourceView source;
    if(!ResolveLogicalTileSource(uint32_t(variation),uint32_t(tile),source)) { builder_.Fail(OwnedFrameStatus::InvalidInput); return; }
    const auto* hd=ResolveHdTerrain(uint32_t(tile),source);
    // HD identity follows the authenticated record. In v2 animated aliases use
    // physical frame IDs; native bank reloads advance the source serial.
    const uint64_t identity=hd?((1ull<<63) | uint64_t(hd->tile)):uint64_t(source.bank_offset);
    const OwnedAssetKey key{builder_.Info().epoch,settings_.terrain_domain,identity,source.mutation_serial,1};
    assets_.ResetBank(key.domain,key.generation);
    auto lease=assets_.Find(key);
    if(!lease.Valid()) {
        const uint32_t dimension=hd?hd->dimension:32;
        const std::span<const uint8_t> pixels=hd?std::span<const uint8_t>(hd->pixels):source.bytes;
        auto result=assets_.Publish(key,{{OwnedAssetFormat::R8,dimension,dimension},pixels,hd?dimension:source.row_stride});
        if(!result) { builder_.Fail(result.status==OwnedAssetStatus::WouldBlock?OwnedFrameStatus::WouldBlock:OwnedFrameStatus::InvalidInput); return; }
        lease=std::move(result.lease);
    }
    // Only the authenticated STANDARD replacement admits the lava response.
    // A different theme or rejected pack retains the exact native fallback.
    builder_.AddTerrain({vertices,std::move(lease),EmissivePresentationEnabled()
        && hd && EmissiveLavaMaterial(uint32_t(tile),uint32_t(variation))});
}
void OwnedWorldDrawSink::Sprite(WorldSpriteDispatch mode,BucketKindJontySprite& sprite,Camera* camera)
{
    if(!dispatch_active_) { builder_.Unsupported(OwnedFramePhase::World); return; }
    source_animation_=sprite.thing?sprite.thing->anim_sprite:0;
    source_class_=sprite.thing?sprite.thing->class_id:0;
    source_model_=sprite.thing?sprite.thing->model:0;
    try { DispatchWorldSprite(mode,sprite,camera); }
    catch(...) { source_animation_=source_class_=source_model_=0;throw; }
    source_animation_=source_class_=source_model_=0;
}
void OwnedWorldDrawSink::Shadow(const BucketKindCreatureShadow& shadow)
{
    WorldShadowSourceView resolved;
    if(ResolveWorldShadowSource(shadow,resolved)!=WorldShadowSourceStatus::Success || !resolved.publication_stamp) {
        builder_.Fail(OwnedFrameStatus::InvalidInput);return;
    }
    auto source=KeeperSource(resolved.bytes.data(),resolved.src_w,resolved.src_h,resolved.draw_idx);
    if(!source.Valid())return;
    OwnedWorldShadow draw;draw.source=std::move(source);draw.source_width=uint32_t(resolved.src_w);draw.source_height=uint32_t(resolved.src_h);
    const auto& g=resolved.geometry;draw.z_ndc=g.z_ndc;draw.darken=g.darken;
    for(size_t i=0;i<4;++i)draw.vertices[i]={g.x[i],g.y[i],g.u[i],g.v[i]};
    builder_.AddShadow(std::move(draw));
}
void OwnedWorldDrawSink::Overlay(WorldOverlayDispatch kind,WorldOverlayDepth depth,float z_ndc,BasicQ& entry)
{
    overlay_phase_=depth==WorldOverlayDepth::Flat?OwnedFramePhase::WorldFlatOverlay:OwnedFramePhase::WorldSprites;
    if(!dispatch_active_) { UnsupportedUI(); return; }
    DispatchWorldOverlay(kind,depth,z_ndc,entry);
}
IUIRenderer& OwnedWorldDrawSink::UI() { return *ui_; }
bool OwnedWorldDrawSink::BeginExternalOverlay(OwnedFramePhase phase) noexcept
{
    if(dispatch_active_ || external_overlay_ || phase!=OwnedFramePhase::Swipe)return false;
    previous_overlay_phase_=overlay_phase_;overlay_phase_=phase;external_overlay_=true;dispatch_active_=true;return true;
}
void OwnedWorldDrawSink::EndExternalOverlay() noexcept
{
    if(!external_overlay_)return;
    overlay_phase_=previous_overlay_phase_;external_overlay_=false;dispatch_active_=false;
}
void OwnedWorldDrawSink::UnsupportedUI() noexcept { builder_.Unsupported(overlay_phase_); }
TbBool OwnedWorldDrawSink::DrawTextResized(int32_t x,int32_t y,int32_t units,const char* text)
{ return static_cast<OwnedWorldUI&>(*ui_).Text(x,y,units,text); }
OwnedAssetLease OwnedWorldDrawSink::ColourRow(const uint8_t* remap,bool ui)
{
    const auto& source_palette=ui?settings_.ui_palette:settings_.palette;
    auto& rows=ui?sources_.ui_:sources_.sprite_;
    const auto domain=ui?settings_.ui_colour_row_domain:settings_.colour_row_domain;
    if(!remap) return source_palette;
    const auto epoch=builder_.Info().epoch;
    if(rows.epoch_!=epoch) { rows.epoch_=epoch; rows.row_generation_=0; rows.palette_key_={}; rows.remaps_=0; }
    if(rows.palette_key_!=source_palette.Key()) {
        if(rows.row_generation_==UINT64_MAX) { builder_.Fail(OwnedFrameStatus::InvalidInput); return {}; }
        ++rows.row_generation_; rows.palette_key_=source_palette.Key(); rows.remaps_=0;
        assets_.ResetBank(domain,rows.row_generation_);
    }
    size_t row=0;
    while(row<rows.remaps_ && std::memcmp(remap,rows.remap_[row].data(),256)!=0) ++row;
    if(row==rows.remaps_) {
        if(row==OwnedWorldSpriteSourceCache::MaxColourRows) { builder_.Fail(OwnedFrameStatus::CommandLimit); return {}; }
        std::memcpy(rows.remap_[row].data(),remap,256); ++rows.remaps_;
    }
    OwnedAssetKey key{epoch,domain,row,rows.row_generation_,1};
    auto lease=assets_.Find(key);
    if(lease.Valid()) return lease;
    std::array<uint8_t,1024> rgba;
    const auto palette=source_palette.Bytes();
    for(size_t i=0;i<256;++i) std::memcpy(rgba.data()+i*4,palette.data()+size_t(remap[i])*4,4);
    auto result=assets_.Publish(key,{{OwnedAssetFormat::RGBA8,256,1},rgba,1024});
    if(!result) builder_.Fail(result.status==OwnedAssetStatus::WouldBlock?OwnedFrameStatus::WouldBlock:OwnedFrameStatus::InvalidInput);
    return std::move(result.lease);
}
OwnedAssetLease OwnedWorldDrawSink::UISource(const TbSprite* sprite)
{
    TbSpriteSourceIdentity identity{};
    if(!get_ui_sprite_source_identity(sprite,&identity) || identity.width>16384 || identity.height>16384) {
        builder_.Fail(OwnedFrameStatus::InvalidInput);return {};
    }
    const OwnedAssetKey key{builder_.Info().epoch,settings_.ui_source_domain,identity.source_id,1,1};
    auto lease=assets_.Find(key);if(lease.Valid())return lease;
    auto result=assets_.PublishUISprite(key,{identity.data,identity.bytes},identity.width,identity.height);
    if(!result)builder_.Fail(result.status==OwnedAssetStatus::WouldBlock?OwnedFrameStatus::WouldBlock:OwnedFrameStatus::InvalidInput);
    return std::move(result.lease);
}
OwnedAssetLease OwnedWorldDrawSink::DbcSource(const AsianFont* font,uint32_t codepoint)
{
    TbDbcGlyphSource source{};
    if(!LbDbcGetGlyphSource(font,codepoint,&source) || source.width<=0 || source.height<=0 || source.scanline_bytes<=0) {
        builder_.Fail(OwnedFrameStatus::InvalidInput);return {};
    }
    const auto epoch=builder_.Info().epoch;
    if(sources_.font_epoch_!=epoch){sources_.font_epoch_=epoch;sources_.font_stamps_.fill(0);}
    size_t slot=0;
    while(slot<sources_.font_stamps_.size() && sources_.font_stamps_[slot] && sources_.font_stamps_[slot]!=source.publication_stamp)++slot;
    if(slot==sources_.font_stamps_.size()){builder_.Fail(OwnedFrameStatus::CommandLimit);return {};}
    sources_.font_stamps_[slot]=source.publication_stamp;
    const OwnedAssetKey key{epoch,settings_.dbc_domain_base+slot,codepoint,source.publication_stamp,1};
    auto lease=assets_.Find(key);if(lease.Valid())return lease;
    auto result=assets_.PublishBitMask(key,{source.data,source.data_bytes},size_t(source.scanline_bytes),uint32_t(source.width),uint32_t(source.height));
    if(!result)builder_.Fail(result.status==OwnedAssetStatus::WouldBlock?OwnedFrameStatus::WouldBlock:OwnedFrameStatus::InvalidInput);
    return std::move(result.lease);
}
OwnedAssetLease OwnedWorldDrawSink::KeeperSource(const uint8_t* data,int width,int height,int32_t sprite_id)
{
    if(!data || width<=0 || height<=0 || width>1024 || height>1024) { builder_.Fail(OwnedFrameStatus::InvalidInput); return {}; }
    int32_t resolved=-1; size_t extent=0; uint64_t stamp=0;
    if(!get_keepersprite_source_identity(sprite_id,data,&resolved,&extent,&stamp)) { builder_.Fail(OwnedFrameStatus::InvalidInput); return {}; }
    const OwnedAssetKey key{builder_.Info().epoch,settings_.sprite_domain,uint64_t(resolved),1,stamp};
    auto source=assets_.Find(key);
    if(!source.Valid()) {
        // Runtime atlas regions accept rectangular images. Keeping the original
        // extent avoids charging transparent square padding to every CPU lease,
        // decode scratch buffer, GPU allocation and upload during crowded battles.
        auto result=assets_.PublishKeeperSprite(key,{data,extent},uint32_t(width),uint32_t(height),uint32_t(width),uint32_t(height),OwnedAssetFormat::RG8);
        if(!result) { builder_.Fail(result.status==OwnedAssetStatus::WouldBlock?OwnedFrameStatus::WouldBlock:OwnedFrameStatus::InvalidInput); return {}; }
        source=std::move(result.lease);
    }
    return source;
}
void OwnedWorldDrawSink::CaptureKeeperSprite(const SpriteScale* scale,const uint8_t* data,int width,int height,
    int32_t content_height,unsigned flags,const uint8_t* remap,int32_t sprite_id,int owner,bool wants_outline,uint8_t alpha)
{
    (void)alpha; // Normal hardware alpha is resolved from draw flags, matching the existing producer.
    if(!scale || !data || width<=0 || height<=0 || width>1024 || height>1024) { builder_.Fail(OwnedFrameStatus::InvalidInput); return; }
    if(content_height<=0 || content_height>height) content_height=height;
    SpriteScaleAxis x{},y{};
    sprite_scale_axis(scale->frame_x,scale->frame_src_w,scale->frame_dst_w,scale->window_w,scale->content_x,width,&x);
    sprite_scale_axis(scale->frame_y,scale->frame_src_h,scale->frame_dst_h,scale->window_h,scale->content_y,content_height,&y);
    if(x.dst_len<=0 || y.dst_len<=0) return;
    auto source=KeeperSource(data,width,height,sprite_id);
    if(!source.Valid()) return;
    const bool additive=(flags&Lb_SPRITE_ALPHA_ADDITIVE)!=0;
    OwnedWorldSprite draw;
    draw.phase=cursor_capture_?OwnedFramePhase::Cursor:OwnedFramePhase::WorldSprites;
    draw.pass=additive?OwnedSpritePass::GlowAdd:OwnedSpritePass::Normal;
    draw.x=x.dst_start; draw.y=y.dst_start; draw.width=x.dst_len; draw.height=y.dst_len;
    draw.phase_x=x.phase; draw.phase_y=y.phase; draw.step_x=x.step; draw.step_y=y.step;
    draw.source_width=uint32_t(width); draw.source_height=uint32_t(height); draw.content_height=uint32_t(content_height);
    draw.flip=(flags&Lb_SPRITE_FLIP_HORIZ)!=0; draw.z_ndc=sprite_z_; draw.source_alpha=additive?1.0f:draw_flags_source_weight(flags);
    draw.source=source;
    if(!cursor_capture_ && EmissivePresentationEnabled())
        draw.colour=EmissiveParameters(ClassifyEmissiveSource(source_animation_,additive,source_class_,source_model_));
    if(!additive) {
        const uint8_t* native_remap=(flags&Lb_SPRITE_REMAP)?remap:nullptr;
        const uint8_t* material_remap=HeartPresentationRemap(EmissivePresentationEnabled(),
            cursor_capture_,additive,source_class_,source_model_,native_remap,pixmap.fade_tables);
        // Bounded diagnostic admission: one record per authenticated source row.
        // Off during normal play and independent of draw cadence or GPU timing.
        if(material_remap!=native_remap) {
            static const bool diagnostic=[] {
                const char* value=std::getenv("KFX_SCENE_REFERENCE");
                return value && std::strcmp(value,"1")==0;
            }();
            static uint64_t reported_rows=0;
            const unsigned row=unsigned((native_remap-pixmap.fade_tables)/256);
            if(diagnostic && !(reported_rows&(uint64_t(1)<<row))) {
                reported_rows|=uint64_t(1)<<row;
                SYNCLOG("Heart presentation fade row %u -> %u (source class %u model %u)",row,
                    unsigned((material_remap-pixmap.fade_tables)/256),source_class_,source_model_);
            }
        }
        draw.colour_row=ColourRow(material_remap);
        if(!draw.colour_row.Valid()) return;
    }
    if(wants_outline && !additive && settings_.outline_mode) {
        auto outline=draw;
        outline.pass=settings_.outline_mode==2?OwnedSpritePass::Edge:OwnedSpritePass::Silhouette;
        outline.colour_row={}; outline.source_alpha=1; outline.z_ndc+=0.002f;
        outline.colour={0.9f,0.9f,0.9f,settings_.outline_alpha};
        if(owner>=0 && owner<PLAYERS_COUNT) {
            const auto colour=get_player_color_idx(PlayerNumber(owner));
            if(colour<9) {
                const size_t index=size_t(player_room_colours[colour])*4;
                for(size_t channel=0;channel<3;++channel) outline.colour[channel]=settings_.palette.Bytes()[index+channel]/255.0f;
            }
        }
        builder_.AddSprite(std::move(outline));
    }
    builder_.AddSprite(draw);
    if(additive && sources_.darken_.Darkens(source,uint32_t(width),uint32_t(height))) {
        draw.pass=OwnedSpritePass::GlowDarken; builder_.AddSprite(std::move(draw));
    }
}
}
