// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#include "OwnedRenderFrame.h"
#include "PaletteIndexTable.h"
#include "TerrainTextureResolution.h"
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace kfx::render {
namespace {
bool ValidInfo(const OwnedFrameInfo& info) noexcept
{
    if (!info.epoch || !info.sequence || !info.native_width || !info.native_height
        || info.native_width>16384 || info.native_height>16384 || info.viewport_x<0 || info.viewport_y<0
        || !info.viewport_width || !info.viewport_height
        || uint64_t(info.viewport_x)+info.viewport_width>info.native_width
        || uint64_t(info.viewport_y)+info.viewport_height>info.native_height || !std::isfinite(info.time)) return false;
    for(float value:info.clear_rgba) if(!std::isfinite(value) || value<0 || value>1) return false;
    for(float value:info.shade_settings) if(!std::isfinite(value)) return false;
    for(float value:info.palette_transform) if(!std::isfinite(value)) return false;
    return true;
}
bool Asset(const OwnedAssetLease& lease, uint64_t epoch, OwnedAssetFormat format,uint32_t w,uint32_t h) noexcept
{ return lease.Valid() && lease.Key().epoch==epoch && lease.Desc()==OwnedAssetDesc{format,w,h}; }
bool PhaseValid(OwnedFramePhase phase) noexcept { return size_t(phase)<size_t(OwnedFramePhase::Count); }
}
OwnedRenderFrame::OwnedRenderFrame(const OwnedFrameLimits& limits)
{
    terrain_.reserve(limits.terrain); sprites_.reserve(limits.sprites); outlines_.reserve(limits.sprites); shadows_.reserve(limits.shadows);
    quads_.reserve(limits.quads); images_.reserve(limits.images);
}
size_t OwnedRenderFrame::CapacityBytes() const noexcept
{
    return sizeof(*this)+terrain_.capacity()*sizeof(OwnedWorldTerrain)+sprites_.capacity()*sizeof(OwnedWorldSprite)
        +outlines_.capacity()*sizeof(OwnedWorldSprite)+shadows_.capacity()*sizeof(OwnedWorldShadow)+quads_.capacity()*sizeof(OwnedFrameQuad)
        +images_.capacity()*sizeof(OwnedFrameImage);
}
void OwnedRenderFrame::Reset(const OwnedFrameInfo& info) noexcept
{
    info_=info; status_=OwnedFrameStatus::Ready; phases_.fill(OwnedPhaseState::Unvisited);
    palette_={}; fade_={}; lightmap_={}; lens_={}; map_fade_={};
    terrain_.clear(); sprites_.clear(); outlines_.clear(); shadows_.clear(); quads_.clear(); images_.clear();
}
OwnedPhaseState OwnedRenderFrame::Phase(OwnedFramePhase phase) const noexcept
{ return PhaseValid(phase)?phases_[size_t(phase)]:OwnedPhaseState::Unsupported; }
namespace owned_frame_detail {
struct Mailbox {
    mutable std::mutex mutex;
    std::array<std::shared_ptr<OwnedRenderFrame>,3> slots;
    OwnedFrameLease pending;
    bool building=false;
    OwnedFrameLimits limits;
    size_t bytes=0;
    explicit Mailbox(OwnedFrameLimits bounds):limits(bounds)
    {
        // Check multiplication/addition before reserving any command storage.
        size_t requested=sizeof(OwnedRenderFrame);
        auto add=[&](size_t count,size_t size) {
            if(count>(SIZE_MAX-requested)/size) throw std::invalid_argument("Frame capacity overflow");
            requested+=count*size;
        };
        add(bounds.terrain,sizeof(OwnedWorldTerrain)); add(bounds.sprites,sizeof(OwnedWorldSprite)); add(bounds.sprites,sizeof(OwnedWorldSprite));
        add(bounds.shadows,sizeof(OwnedWorldShadow)); add(bounds.quads,sizeof(OwnedFrameQuad));
        add(bounds.images,sizeof(OwnedFrameImage));
        if(requested>bounds.command_bytes/3) throw std::invalid_argument("Frame command byte cap exceeded");
        for(auto& slot:slots) {
            slot=std::shared_ptr<OwnedRenderFrame>(new OwnedRenderFrame(bounds));
            if(slot->CapacityBytes()>bounds.command_bytes-bytes) throw std::length_error("Frame reserved capacity exceeds cap");
            bytes+=slot->CapacityBytes();
        }
    }
};
}
OwnedFrameMailbox::OwnedFrameMailbox(OwnedFrameLimits limits):state_(std::make_shared<owned_frame_detail::Mailbox>(limits)) {}
OwnedFrameBuilder OwnedFrameMailbox::Begin(const OwnedFrameInfo& info)
{
    if(!ValidInfo(info)) throw std::invalid_argument("Invalid owned frame identity/viewport/state");
    std::lock_guard lock(state_->mutex);
    if(state_->building) return {};
    std::shared_ptr<OwnedRenderFrame> available;
    for(auto& slot:state_->slots) if(slot.use_count()==1) {
        slot->Reset({}); // Release idle packet asset owners before source acquisition.
        if(!available) available=slot;
    }
    if(!available) return {};
    available->Reset(info); state_->building=true;
    return OwnedFrameBuilder(state_,std::move(available));
}
OwnedFrameStatus OwnedFrameMailbox::TryPublish(const OwnedFrameLease& packet) noexcept
{
    if(!packet) return OwnedFrameStatus::InvalidInput;
    if(!packet->Complete()) return packet->Status();
    std::lock_guard lock(state_->mutex);
    bool belongs=false;
    for(const auto& slot:state_->slots) belongs|=slot.get()==packet.get();
    if(!belongs) return OwnedFrameStatus::InvalidInput;
    if(state_->pending) return OwnedFrameStatus::WouldBlock;
    state_->pending=packet; return OwnedFrameStatus::Ready;
}
OwnedFrameLease OwnedFrameMailbox::TakePending() noexcept
{
    std::lock_guard lock(state_->mutex);
    return std::exchange(state_->pending,{});
}
OwnedFrameMailboxStats OwnedFrameMailbox::Stats() const noexcept
{
    std::lock_guard lock(state_->mutex);
    size_t occupied=0;
    for(const auto& slot:state_->slots) occupied+=slot.use_count()>1;
    return {occupied,state_->bytes,state_->building,bool(state_->pending)};
}
OwnedFrameBuilder::OwnedFrameBuilder(std::shared_ptr<owned_frame_detail::Mailbox> owner,
    std::shared_ptr<OwnedRenderFrame> frame) noexcept:owner_(std::move(owner)),frame_(std::move(frame)) {}
OwnedFrameBuilder::~OwnedFrameBuilder() { Close(); }
OwnedFrameBuilder::OwnedFrameBuilder(OwnedFrameBuilder&& other) noexcept
    :owner_(std::move(other.owner_)),frame_(std::move(other.frame_)),owns_window_(other.owns_window_) {}
OwnedFrameBuilder& OwnedFrameBuilder::operator=(OwnedFrameBuilder&& other) noexcept
{
    if(this!=&other) { Close(); owner_=std::move(other.owner_); frame_=std::move(other.frame_); owns_window_=other.owns_window_; }
    return *this;
}
void OwnedFrameBuilder::Close() noexcept
{
    if(owner_) {
        { std::lock_guard lock(owner_->mutex); if(owns_window_) owner_->building=false; if(frame_) frame_->Reset({}); frame_.reset(); }
        owner_.reset();
    }
}
const OwnedFrameInfo& OwnedFrameBuilder::Info() const
{ if(!frame_) throw std::logic_error("No owned frame builder"); return frame_->info_; }
bool OwnedFrameBuilder::SetPresentationSequence(uint64_t sequence) noexcept
{
    if(!frame_ || !sequence || sequence==UINT64_MAX || sequence<frame_->info_.sequence)return false;
    frame_->info_.sequence=sequence;return true;
}
void OwnedFrameBuilder::Fail(OwnedFrameStatus status) noexcept
{
    if(frame_ && frame_->status_==OwnedFrameStatus::Ready && status!=OwnedFrameStatus::Ready)
        frame_->status_=status;
}
void OwnedFrameBuilder::Unsupported(OwnedFramePhase phase) noexcept
{
    if(!frame_) return;
    if(!PhaseValid(phase)) { Fail(OwnedFrameStatus::InvalidInput); return; }
    frame_->phases_[size_t(phase)]=OwnedPhaseState::Unsupported;
}
void OwnedFrameBuilder::FinishPhase(OwnedFramePhase phase) noexcept
{
    if(!frame_) return;
    if(!PhaseValid(phase)) { Fail(OwnedFrameStatus::InvalidInput); return; }
    auto& state=frame_->phases_[size_t(phase)];
    if(state==OwnedPhaseState::Unvisited) state=OwnedPhaseState::Empty;
}
void OwnedFrameBuilder::RecordPhase(OwnedFramePhase phase) noexcept
{
    if(!frame_) return;
    if(!PhaseValid(phase)) { Fail(OwnedFrameStatus::InvalidInput); return; }
    auto& state=frame_->phases_[size_t(phase)];
    if(state!=OwnedPhaseState::Unsupported) state=OwnedPhaseState::Recorded;
}
bool OwnedFrameBuilder::AddTerrain(OwnedWorldTerrain draw) noexcept
{
    if(!frame_) return false;
    const auto shape=draw.tile.Desc();
    if(!TerrainTextureDimension(shape.width) || shape.width!=shape.height
        || !Asset(draw.tile,frame_->info_.epoch,OwnedAssetFormat::R8,shape.width,shape.height)) {
        Fail(OwnedFrameStatus::InvalidInput);return false;
    }
    for(const auto& v:draw.vertices) {
        const std::array<float,14> fields{v.x,v.y,v.z,v.u,v.v,v.shade,v.stl_x,v.stl_y,v.camera_z,v.atlas_layer,v.wx,v.wy,v.wz,v.tile};
        for(float value:fields) if(!std::isfinite(value)) { Fail(OwnedFrameStatus::InvalidInput); return false; }
    }
    if(frame_->terrain_.size()==owner_->limits.terrain) { Fail(OwnedFrameStatus::CommandLimit); return false; }
    frame_->terrain_.push_back(std::move(draw));
    auto& phase=frame_->phases_[size_t(OwnedFramePhase::World)];
    if(phase!=OwnedPhaseState::Unsupported) phase=OwnedPhaseState::Recorded;
    return true;
}
bool OwnedFrameBuilder::EmptyDraws() const noexcept
{
    return frame_ && frame_->terrain_.empty() && frame_->sprites_.empty() && frame_->outlines_.empty() && frame_->shadows_.empty()
        && frame_->quads_.empty() && frame_->images_.empty() && frame_->lens_.kind==OwnedLensKind::None
        && !frame_->map_fade_.active && !frame_->map_fade_.capture_pending && !frame_->map_fade_.ghost_table.Valid();
}
bool OwnedFrameBuilder::SetClearRGBA(const std::array<float,4>& colour) noexcept
{
    if(!frame_)return false;
    for(float c:colour)if(!std::isfinite(c) || c<0 || c>1){Fail(OwnedFrameStatus::InvalidInput);return false;}
    frame_->info_.clear_rgba=colour;frame_->info_.clear_uses_display_palette=false;return true;
}
bool OwnedFrameBuilder::SetDisplayClear(uint8_t index,const OwnedAssetLease& display) noexcept
{
    if(!frame_ || !Asset(display,frame_->info_.epoch,OwnedAssetFormat::RGBA8,256,1)){Fail(OwnedFrameStatus::InvalidInput);return false;}
    for(size_t c=0;c<4;++c)frame_->info_.clear_rgba[c]=display.Bytes()[size_t(index)*4+c]/255.0f;
    frame_->info_.clear_uses_display_palette=true;frame_->info_.clear_palette_index=index;
    if(frame_->lens_.display_clear)frame_->lens_.clear_rgba=frame_->info_.clear_rgba;
    return true;
}
void OwnedFramePaletteReplay::ResetEpoch(uint64_t epoch) noexcept
{
    if(epoch_==epoch)return;
    epoch_=epoch;generation_=0;palette_key_={};count_=0;rows_={};rgb6_={};rgb6_base_key_={};rgb6_revision_=0;
}
OwnedFrameStatus OwnedFramePaletteReplay::Prepare(const OwnedAssetLease& palette) noexcept
{
    const auto epoch=palette.Key().epoch;
    ResetEpoch(epoch);
    if(palette_key_==palette.Key())return OwnedFrameStatus::Ready;
    if(generation_==UINT64_MAX)return OwnedFrameStatus::InvalidInput;
    if(!assets_.ResetBank(domain_,generation_+1))return OwnedFrameStatus::WouldBlock;
    ++generation_;palette_key_=palette.Key();count_=0;rows_={};rgb6_={};rgb6_base_key_={};rgb6_revision_=0;return OwnedFrameStatus::Ready;
}
OwnedFrameStatus OwnedFramePaletteReplay::Row(const std::array<uint8_t,256>& map,const OwnedAssetLease& palette,OwnedAssetLease& result) noexcept
{
    size_t slot=0;while(slot<count_ && maps_[slot]!=map)++slot;
    if(slot==count_){if(count_==maps_.size())return OwnedFrameStatus::CommandLimit;maps_[slot]=map;++count_;}
    if(rows_[slot].Valid()){result=rows_[slot];return OwnedFrameStatus::Ready;}
    std::array<uint8_t,1024> rgba{};
    for(size_t i=0;i<256;++i)std::memcpy(rgba.data()+i*4,palette.Bytes().data()+size_t(map[i])*4,4);
    auto published=assets_.Publish({epoch_,domain_,slot,generation_,1},{{OwnedAssetFormat::RGBA8,256,1},rgba,1024});
    if(!published)return published.status==OwnedAssetStatus::WouldBlock?OwnedFrameStatus::WouldBlock:OwnedFrameStatus::InvalidInput;
    rows_[slot]=published.lease;result=std::move(published.lease);return OwnedFrameStatus::Ready;
}
OwnedFrameStatus OwnedFramePaletteReplay::Lookup(const OwnedAssetLease& base,const OwnedAssetLease& palette,OwnedAssetLease& result) noexcept
{
    if(!Asset(base,epoch_,OwnedAssetFormat::R8,512,512))return OwnedFrameStatus::InvalidInput;
    if(rgb6_.Valid() && rgb6_base_key_==base.Key()){result=rgb6_;return OwnedFrameStatus::Ready;}
    if(rgb6_revision_==UINT64_MAX)return OwnedFrameStatus::InvalidInput;
    std::copy(base.Bytes().begin(),base.Bytes().end(),rgb6_scratch_.begin());std::array<uint32_t,256> keys{};
    ApplyPaletteIndexOverrides(rgb6_scratch_,palette.Bytes(),keys);
    auto published=assets_.Publish({epoch_,domain_,256,generation_,rgb6_revision_+1},{{OwnedAssetFormat::R8,512,512},rgb6_scratch_,512});
    if(!published)return published.status==OwnedAssetStatus::WouldBlock?OwnedFrameStatus::WouldBlock:OwnedFrameStatus::InvalidInput;
    rgb6_=published.lease;rgb6_base_key_=base.Key();++rgb6_revision_;result=std::move(published.lease);return OwnedFrameStatus::Ready;
}
OwnedFrameStatus OwnedFrameBuilder::PrepareDisplayPalette(const OwnedRenderFrame& source,const OwnedAssetLease& display,
    OwnedFramePaletteReplay& rows,OwnedAssetLease& rgb6,OwnedAssetLease& map_rgb6) noexcept
{
    if(!Asset(display,source.info_.epoch,OwnedAssetFormat::RGBA8,256,1))
        return OwnedFrameStatus::InvalidInput;
    bool remapped=false;
    for(const auto& quad:source.quads_)remapped=remapped || quad.palette_source==OwnedQuadPaletteSource::DisplayRemap;
    const bool lookup=source.lens_.kind==OwnedLensKind::Mist || source.lens_.kind==OwnedLensKind::Overlay;
    if(lookup && !Asset(source.lens_.rgb6_base,source.info_.epoch,OwnedAssetFormat::R8,512,512))return OwnedFrameStatus::InvalidInput;
    const bool map_lookup=source.map_fade_.active;
    if(map_lookup && !Asset(source.map_fade_.rgb6_base,source.info_.epoch,OwnedAssetFormat::R8,512,512))return OwnedFrameStatus::InvalidInput;
    if(remapped || lookup || map_lookup){
        auto status=rows.Prepare(display);if(status!=OwnedFrameStatus::Ready)return status;
        for(const auto& quad:source.quads_)if(quad.palette_source==OwnedQuadPaletteSource::DisplayRemap){
            OwnedAssetLease row;status=rows.Row(quad.palette_remap,display,row);if(status!=OwnedFrameStatus::Ready)return status;
        }
        if(lookup){status=rows.Lookup(source.lens_.rgb6_base,display,rgb6);if(status!=OwnedFrameStatus::Ready)return status;}
        if(map_lookup){status=rows.Lookup(source.map_fade_.rgb6_base,display,map_rgb6);if(status!=OwnedFrameStatus::Ready)return status;}
    }
    return OwnedFrameStatus::Ready;
}
void OwnedFrameBuilder::ApplyDisplayPalette(const OwnedAssetLease& display,OwnedFramePaletteReplay& rows,OwnedAssetLease rgb6,OwnedAssetLease map_rgb6) noexcept
{
    if(frame_->palette_.Valid())frame_->palette_=display;
    if(frame_->info_.clear_uses_display_palette)SetDisplayClear(frame_->info_.clear_palette_index,display);
    if(frame_->lens_.palette.Valid())frame_->lens_.palette=display;
    if(rgb6.Valid())frame_->lens_.rgb6=std::move(rgb6);
    if(map_rgb6.Valid()){frame_->map_fade_.rgb6=std::move(map_rgb6);frame_->map_fade_.palette=display;}
    for(auto& quad:frame_->quads_){
        switch(quad.palette_source){
        case OwnedQuadPaletteSource::Fixed:break;
        case OwnedQuadPaletteSource::Display:quad.colour_row=display;break;
        case OwnedQuadPaletteSource::DisplayIndex:
            for(size_t c=0;c<3;++c)quad.colour[c]=display.Bytes()[size_t(quad.palette_index)*4+c]/255.0f;
            break;
        case OwnedQuadPaletteSource::DisplayRemap:
            // Guaranteed resident by the preflight above; lookup cannot publish.
            for(size_t slot=0;slot<rows.count_;++slot)if(rows.maps_[slot]==quad.palette_remap){quad.colour_row=rows.rows_[slot];break;}
            break;
        }
    }
    for(auto& image:frame_->images_)if(image.uses_display_palette)image.palette=display;
}
OwnedFrameStatus OwnedFrameBuilder::RefreshDisplayPalette(OwnedAssetLease display,OwnedFramePaletteReplay& rows) noexcept
{
    if(!frame_ || frame_->status_!=OwnedFrameStatus::Ready)return OwnedFrameStatus::InvalidInput;
    OwnedAssetLease rgb6,map_rgb6;
    const auto prepared=PrepareDisplayPalette(*frame_,display,rows,rgb6,map_rgb6);
    if(prepared!=OwnedFrameStatus::Ready)return prepared;
    ApplyDisplayPalette(display,rows,std::move(rgb6),std::move(map_rgb6));return OwnedFrameStatus::Ready;
}
OwnedFrameStatus OwnedFrameBuilder::ReplayCachedFrame(const OwnedRenderFrame& source,OwnedAssetLease display,OwnedFramePaletteReplay& rows,
    const OwnedFrameInfo& info,OwnedFrameLease& output) noexcept
{
    if(!frame_ || !owner_ || !owns_window_ || !ValidInfo(info) || info.epoch!=frame_->info_.epoch || source.Info().epoch!=info.epoch
        || !source.Complete() || source.Info().native_width!=info.native_width || source.Info().native_height!=info.native_height
        || !Asset(display,info.epoch,OwnedAssetFormat::RGBA8,256,1))return OwnedFrameStatus::InvalidInput;
    std::shared_ptr<OwnedRenderFrame> available;
    {
        std::lock_guard lock(owner_->mutex);
        for(auto& slot:owner_->slots)if(slot.use_count()==1){slot->Reset({});if(!available)available=slot;}
        if(!available)return OwnedFrameStatus::WouldBlock;
        available->Reset(info);
    }
    OwnedFrameBuilder replay(owner_,std::move(available));
    replay.owns_window_=false; // Never releases the retained normal write window.
    const auto result=replay.ReplayDisplayPalette(source,std::move(display),rows);
    if(result!=OwnedFrameStatus::Ready)return result;
    auto sealed=replay.Seal();
    if(!sealed || !sealed->Complete())return sealed?sealed->Status():OwnedFrameStatus::InvalidInput;
    output=std::move(sealed);return OwnedFrameStatus::Ready;
}
OwnedFrameStatus OwnedFrameBuilder::ReplayDisplayPalette(const OwnedRenderFrame& source,OwnedAssetLease display,OwnedFramePaletteReplay& rows) noexcept
{
    if(!frame_ || frame_->status_!=OwnedFrameStatus::Ready || !source.Complete() || frame_.get()==&source || source.Info().epoch!=frame_->info_.epoch
        || source.Info().native_width!=frame_->info_.native_width || source.Info().native_height!=frame_->info_.native_height
        || !Asset(display,frame_->info_.epoch,OwnedAssetFormat::RGBA8,256,1)
        || !EmptyDraws())return OwnedFrameStatus::InvalidInput;
    const auto& limits=owner_->limits;
    if(source.terrain_.size()>limits.terrain || source.sprites_.size()+source.outlines_.size()>limits.sprites
        || source.shadows_.size()>limits.shadows || source.quads_.size()>limits.quads || source.images_.size()>limits.images)return OwnedFrameStatus::CommandLimit;
    OwnedAssetLease rgb6,map_rgb6;
    const auto prepared=PrepareDisplayPalette(source,display,rows,rgb6,map_rgb6);
    if(prepared!=OwnedFrameStatus::Ready)return prepared;
    // Capacity admission and remap publications are complete. Vector capacity
    // is fixed at mailbox construction; these copies cannot grow or allocate.
    const auto sequence=frame_->info_.sequence;
    frame_->info_=source.info_;frame_->info_.sequence=sequence;frame_->phases_=source.phases_;frame_->status_=source.status_;
    frame_->palette_=source.palette_;frame_->fade_=source.fade_;frame_->lightmap_=source.lightmap_;
    frame_->lens_=source.lens_;frame_->map_fade_=source.map_fade_;
    frame_->map_fade_.capture_pending=false; // Replays retain the already captured transition.
    frame_->terrain_.assign(source.terrain_.begin(),source.terrain_.end());frame_->sprites_.assign(source.sprites_.begin(),source.sprites_.end());
    frame_->outlines_.assign(source.outlines_.begin(),source.outlines_.end());frame_->shadows_.assign(source.shadows_.begin(),source.shadows_.end());
    frame_->quads_.clear();
    for(const auto& quad:source.quads_)if(!quad.parchment_capture)frame_->quads_.push_back(quad);
    frame_->images_.assign(source.images_.begin(),source.images_.end());
    ApplyDisplayPalette(display,rows,std::move(rgb6),std::move(map_rgb6));
    return OwnedFrameStatus::Ready;
}
bool OwnedFrameBuilder::SetWorldViewport(int32_t x,int32_t y,uint32_t width,uint32_t height) noexcept
{
    if(!frame_)return false;
    bool used=!frame_->terrain_.empty() || !frame_->outlines_.empty() || !frame_->shadows_.empty();
    for(const auto& sprite:frame_->sprites_)used=used || sprite.phase==OwnedFramePhase::WorldSprites;
    for(const auto& q:frame_->quads_)used=used || q.phase==OwnedFramePhase::WorldSprites || q.phase==OwnedFramePhase::WorldFlatOverlay;
    const auto& info=frame_->info_;
    if(used || x<0 || y<0 || !width || !height || uint64_t(x)+width>info.native_width || uint64_t(y)+height>info.native_height){
        Fail(OwnedFrameStatus::InvalidInput);return false;
    }
    frame_->info_.viewport_x=x;frame_->info_.viewport_y=y;frame_->info_.viewport_width=width;frame_->info_.viewport_height=height;return true;
}
bool OwnedFrameBuilder::SetWorldState(const std::array<float,9>& shade_settings,const std::array<float,9>& palette_transform,float time) noexcept
{
    if(!frame_) return false;
    for(float value:shade_settings) if(!std::isfinite(value)) { Fail(OwnedFrameStatus::InvalidInput); return false; }
    for(float value:palette_transform) if(!std::isfinite(value)) { Fail(OwnedFrameStatus::InvalidInput); return false; }
    if(!std::isfinite(time)) { Fail(OwnedFrameStatus::InvalidInput); return false; }
    frame_->info_.shade_settings=shade_settings; frame_->info_.palette_transform=palette_transform; frame_->info_.time=time;
    return true;
}
bool OwnedFrameBuilder::SetWorldAssets(OwnedAssetLease palette,OwnedAssetLease fade,OwnedAssetLease lightmap) noexcept
{
    if(!frame_) return false;
    const auto epoch=frame_->info_.epoch;
    if(!Asset(palette,epoch,OwnedAssetFormat::RGBA8,256,1) || !Asset(fade,epoch,OwnedAssetFormat::R8,256,64)
        || !Asset(lightmap,epoch,OwnedAssetFormat::R16LE,511,511)) { Fail(OwnedFrameStatus::InvalidInput); return false; }
    frame_->palette_=std::move(palette); frame_->fade_=std::move(fade); frame_->lightmap_=std::move(lightmap); return true;
}
bool OwnedFrameBuilder::AddSprite(OwnedWorldSprite draw) noexcept
{
    if(!frame_) return false;
    const bool outline=draw.pass==OwnedSpritePass::Silhouette || draw.pass==OwnedSpritePass::Edge;
    const bool normal=draw.pass==OwnedSpritePass::Normal;
    if((draw.phase!=OwnedFramePhase::WorldSprites && draw.phase!=OwnedFramePhase::Cursor)
        || (draw.phase==OwnedFramePhase::Cursor && outline) || unsigned(draw.pass)>unsigned(OwnedSpritePass::GlowDarken) || !draw.source.Valid()
        || draw.source.Key().epoch!=frame_->info_.epoch || draw.source.Desc().format!=OwnedAssetFormat::RGBA8
        || !draw.source_width || !draw.source_height || draw.source_width>1024 || draw.source_height>1024
        || draw.source_width>draw.source.Desc().width || draw.source_height>draw.source.Desc().height
        || !draw.content_height || draw.content_height>draw.source_height || draw.width<=0 || draw.height<=0
        || draw.step_x<=0 || draw.step_y<=0 || !std::isfinite(draw.z_ndc) || !std::isfinite(draw.source_alpha)
        || draw.source_alpha<0 || draw.source_alpha>1 || (!normal && draw.source_alpha!=1)
        || (normal?!Asset(draw.colour_row,frame_->info_.epoch,OwnedAssetFormat::RGBA8,256,1):draw.colour_row.Valid())) {
        Fail(OwnedFrameStatus::InvalidInput); return false;
    }
    for(float value:draw.colour) if(!std::isfinite(value) || value<0 || value>1) { Fail(OwnedFrameStatus::InvalidInput); return false; }
    if(frame_->sprites_.size()+frame_->outlines_.size()==owner_->limits.sprites) { Fail(OwnedFrameStatus::CommandLimit); return false; }
    RecordPhase(draw.phase);
    (outline?frame_->outlines_:frame_->sprites_).push_back(std::move(draw)); return true;
}
bool OwnedFrameBuilder::AddShadow(OwnedWorldShadow draw) noexcept
{
    if(!frame_) return false;
    if(!draw.source.Valid() || draw.source.Key().epoch!=frame_->info_.epoch
        || draw.source.Desc().format!=OwnedAssetFormat::RGBA8 || !draw.source_width || !draw.source_height
        || draw.source_width>draw.source.Desc().width || draw.source_height>draw.source.Desc().height
        || !std::isfinite(draw.z_ndc) || !std::isfinite(draw.darken) || draw.darken<0 || draw.darken>1) {
        Fail(OwnedFrameStatus::InvalidInput); return false;
    }
    for(const auto& v:draw.vertices) for(float value:{v.x,v.y,v.u,v.v})
        if(!std::isfinite(value)) { Fail(OwnedFrameStatus::InvalidInput); return false; }
    RecordPhase(OwnedFramePhase::World);
    if(frame_->shadows_.size()==owner_->limits.shadows) { Fail(OwnedFrameStatus::CommandLimit); return false; }
    frame_->shadows_.push_back(std::move(draw)); return true;
}
bool OwnedFrameBuilder::AddQuad(OwnedFrameQuad draw) noexcept
{
    if(!frame_) return false;
    const bool solid=draw.mode==OwnedQuadMode::Solid;
    const bool needs_row=draw.mode==OwnedQuadMode::Sprite || draw.mode==OwnedQuadMode::Fade || draw.mode==OwnedQuadMode::Clut || draw.mode==OwnedQuadMode::OpaqueIndexed;
    bool valid=PhaseValid(draw.phase) && uint8_t(draw.mode)<=uint8_t(OwnedQuadMode::OpaqueIndexed)
        && std::isfinite(draw.x0) && std::isfinite(draw.x1) && std::isfinite(draw.y0) && std::isfinite(draw.y1)
        && std::isfinite(draw.u0) && std::isfinite(draw.u1) && std::isfinite(draw.v0) && std::isfinite(draw.v1) && std::isfinite(draw.z_ndc)
        && draw.x1>=draw.x0 && draw.y1>=draw.y0 && (!draw.scissor_enabled || (draw.scissor_w>0 && draw.scissor_h>0))
        && (draw.mode!=OwnedQuadMode::Fade || draw.fade_row<64);
    for(const float c:draw.colour)valid=valid && std::isfinite(c) && c>=0 && c<=1;
    valid=valid && uint8_t(draw.palette_source)<=uint8_t(OwnedQuadPaletteSource::DisplayIndex)
        && (draw.palette_source!=OwnedQuadPaletteSource::DisplayIndex || draw.mode==OwnedQuadMode::Solid || draw.mode==OwnedQuadMode::Colored)
        && (draw.palette_source!=OwnedQuadPaletteSource::DisplayRemap || draw.mode==OwnedQuadMode::Clut)
        && (draw.palette_source!=OwnedQuadPaletteSource::Display || needs_row);
    if(!solid)valid=valid && draw.source.Valid() && draw.source.Key().epoch==frame_->info_.epoch && draw.source.Desc().format==OwnedAssetFormat::R8;
    else valid=valid && !draw.source.Valid();
    if(needs_row)valid=valid && draw.colour_row.Valid() && draw.colour_row.Key().epoch==frame_->info_.epoch
        && draw.colour_row.Desc()==OwnedAssetDesc{OwnedAssetFormat::RGBA8,256,1};
    else valid=valid && !draw.colour_row.Valid();
    if(!valid){Fail(OwnedFrameStatus::InvalidInput);return false;}
    if(draw.parchment_capture && draw.phase!=OwnedFramePhase::WorldSprites && draw.phase!=OwnedFramePhase::WorldFlatOverlay
        && draw.phase!=OwnedFramePhase::GameUI && draw.phase!=OwnedFramePhase::FrontOverlay){Fail(OwnedFrameStatus::InvalidInput);return false;}
    RecordPhase(draw.parchment_capture?OwnedFramePhase::ResolveMapFade:draw.phase);
    if(frame_->quads_.size()==owner_->limits.quads) { Fail(OwnedFrameStatus::CommandLimit); return false; }
    frame_->quads_.push_back(std::move(draw)); return true;
}
bool OwnedFrameBuilder::AddImage(OwnedFrameImage draw) noexcept
{
    if(!frame_) return false;
    if(!PhaseValid(draw.phase)) { Fail(OwnedFrameStatus::InvalidInput); return false; }
    if(!draw.source.Valid() || draw.source.Key().epoch!=frame_->info_.epoch || draw.source.Desc().format!=OwnedAssetFormat::R8
        || !Asset(draw.palette,frame_->info_.epoch,OwnedAssetFormat::RGBA8,256,1) || !draw.width || !draw.height
        || uint8_t(draw.mode)>uint8_t(OwnedImageMode::LandviewZoom)
        || (draw.coverage.Valid() && (draw.mode!=OwnedImageMode::Transparent || draw.coverage.Key().epoch!=frame_->info_.epoch || draw.coverage.Desc()!=draw.source.Desc()))) {
        Fail(OwnedFrameStatus::InvalidInput);return false;
    }
    const bool zoom=draw.mode==OwnedImageMode::LandviewZoom;
    for(float v:draw.zoom) if(!std::isfinite(v) || (zoom ? std::abs(v)>16777216.0f : v!=0)) {
        Fail(OwnedFrameStatus::InvalidInput);return false;
    }
    if(zoom && (draw.phase!=OwnedFramePhase::ImagePresents || draw.x || draw.y
        || draw.width!=frame_->info_.native_width || draw.height!=frame_->info_.native_height)) {
        Fail(OwnedFrameStatus::InvalidInput);return false;
    }
    // The normal image pass retains only the latest zoom command.
    if(zoom) for(auto& previous:frame_->images_) if(previous.mode==OwnedImageMode::LandviewZoom) {
        previous=std::move(draw);return true;
    }
    RecordPhase(draw.phase);
    if(frame_->images_.size()==owner_->limits.images) { Fail(OwnedFrameStatus::CommandLimit); return false; }
    frame_->images_.push_back(std::move(draw)); return true;
}
void OwnedFrameBuilder::SetLens(OwnedFrameLens lens) noexcept
{ if(frame_) { frame_->lens_=std::move(lens); RecordPhase(OwnedFramePhase::ResolveWorldCapture); } }
bool OwnedFrameBuilder::SetMapFade(OwnedFrameMapFade fade) noexcept
{
    if(!frame_) return false;
    if(fade.active && (!fade.transition_id || fade.transition_id==UINT64_MAX || !std::isfinite(fade.step) || fade.step<0 || fade.step>32
        || !std::isfinite(fade.warp_scale) || fade.warp_scale<=0 || fade.warp_scale>2.5f
        || !Asset(fade.ghost_table,frame_->info_.epoch,OwnedAssetFormat::R8,256,256)
        || !Asset(fade.rgb6,frame_->info_.epoch,OwnedAssetFormat::R8,512,512)
        || !Asset(fade.rgb6_base,frame_->info_.epoch,OwnedAssetFormat::R8,512,512)
        || !Asset(fade.palette,frame_->info_.epoch,OwnedAssetFormat::RGBA8,256,1)
        || !Asset(fade.fade,frame_->info_.epoch,OwnedAssetFormat::R8,256,64))) {
        Fail(OwnedFrameStatus::InvalidInput);return false;
    }
    if(fade.capture_pending && !fade.active){Fail(OwnedFrameStatus::InvalidInput);return false;}
    // The engine completes redirected parchment drawing before submitting the
    // step. Repeated boundary ticks redraw it even though the immutable GPU
    // capture already exists; match the normal consumer by discarding that work.
    if(fade.active && !fade.capture_pending)
        std::erase_if(frame_->quads_,[](const OwnedFrameQuad& quad){return quad.parchment_capture;});
    frame_->map_fade_=std::move(fade); RecordPhase(OwnedFramePhase::ResolveMapFade); return true;
}
OwnedFrameLease OwnedFrameBuilder::Seal() noexcept
{
    if(!frame_) return {};
    bool has_world=!frame_->terrain_.empty() || !frame_->outlines_.empty() || !frame_->shadows_.empty();
    for(const auto& sprite:frame_->sprites_)has_world=has_world || sprite.phase==OwnedFramePhase::WorldSprites;
    if(has_world && (!frame_->palette_.Valid() || !frame_->fade_.Valid() || !frame_->lightmap_.Valid()))
        Fail(OwnedFrameStatus::Incomplete);
    if(frame_->status_==OwnedFrameStatus::Ready)
        for(auto phase:frame_->phases_) if(phase==OwnedPhaseState::Unvisited || phase==OwnedPhaseState::Unsupported) {
            frame_->status_=OwnedFrameStatus::Incomplete; break;
        }
    OwnedFrameLease sealed=std::move(frame_); Close(); return sealed;
}
}
