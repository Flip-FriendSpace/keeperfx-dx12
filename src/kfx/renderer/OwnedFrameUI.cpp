// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#include "pre_inc.h"
#include "OwnedFrameUI.h"
#include "RendererManager.h"
#include "engine_render.h"
#include "bflib_sprite.h"
#include "SpriteScale.h"
#include "HugeSpriteDecode.h"
#include "front_landview.h"
#include "front_simple.h"
#include "bflib_video.h"
#include "PaletteIndexTable.h"
#include "TileSource.h"
#include <algorithm>
#include <cstring>
#include <cmath>
#include <stdexcept>
#include "post_inc.h"

namespace kfx::render {
OwnedFrameUI::OwnedFrameUI(OwnedRenderAssets& assets,OwnedWorldSpriteSourceCache& sources):assets_(assets),sources_(sources){}
OwnedFrameUI::~OwnedFrameUI()=default;
bool OwnedFrameUI::Bind(OwnedFrameBuilder& builder,OwnedWorldDrawSettings settings)
{
    if(!builder || (builder_ && builder_!=&builder))return false;
    const bool fresh=!builder_;
    if(epoch_!=builder.Info().epoch){epoch_=builder.Info().epoch;slab_={};minimap_={};zoom_={};map_ghost_={};map_rgb6_={};map_base_={};zoom_tiles_={};have_map_base_=false;map_was_active_=false;map_transition_=0;images_={};coverage_={};palettes_={};}
    auto sink=std::make_unique<OwnedWorldDrawSink>(builder,assets_,sources_,settings);
    builder_=&builder;settings_=std::move(settings);sink_=std::move(sink);
    if(fresh){parchment_capture_=zoom_capture_=false;map_submitted_=false;zoom_tile_used_={};image_slot_=0;zoom_submitted_=false;pointer_.reset();image_overlay_.reset();m_world_overlay_active=m_world_overlay_flat_active=m_top_overlay_active=false;}
    else {if(m_world_overlay_active)sink_->UI().SetWorldOverlay(m_world_overlay_z);if(m_world_overlay_flat_active)sink_->UI().SetWorldOverlayFlat(m_world_overlay_flat_z);if(m_top_overlay_active)sink_->UI().SetTopOverlay();}
    sink_->SetParchmentCapture(parchment_capture_);
    return true;
}
void OwnedFrameUI::Unbind() noexcept {Draw();map_was_active_=map_submitted_;parchment_capture_=zoom_capture_=false;sink_.reset();builder_=nullptr;pointer_.reset();image_overlay_.reset();}
size_t OwnedFrameUI::ScratchBytes() const noexcept{return slab_bytes_.capacity()+minimap_bytes_.capacity()+huge_pixels_.capacity()+huge_coverage_.capacity()+map_base_bytes_.capacity()+map_rgb6_bytes_.capacity();}
void OwnedFrameUI::Fail(OwnedFrameStatus status) noexcept {if(builder_)builder_->Fail(status);}
OwnedFramePhase OwnedFrameUI::Phase() const noexcept {
    if(zoom_capture_)return OwnedFramePhase::ZoomBoxes;
    switch(ComputeCurrentLayer()){
    case IRUILayer::WorldOverlay:return OwnedFramePhase::WorldSprites;
    case IRUILayer::WorldOverlayFlat:return OwnedFramePhase::WorldFlatOverlay;
    case IRUILayer::Overlay:return OwnedFramePhase::FrontOverlay;
    default:return OwnedFramePhase::GameUI;}
}
OwnedAssetLease OwnedFrameUI::Publish(Source& current,uint64_t domain,uint64_t id,OwnedAssetView view)
{
    if(!builder_)return {};
    if(current.lease.Valid() && current.lease.Desc()==view.desc){
        bool equal=true;const auto old=current.lease.Bytes();const size_t row=current.lease.RowStride();
        if(view.row_stride<row || view.bytes.size()<row || size_t(view.desc.height-1)>(view.bytes.size()-row)/view.row_stride)equal=false;
        for(uint32_t y=0;equal && y<view.desc.height;++y)equal=std::memcmp(old.data()+y*row,view.bytes.data()+y*view.row_stride,row)==0;
        if(equal)return current.lease;
    }
    uint64_t generation=current.generation?current.generation:1,revision=current.revision+1;
    if(current.lease.Valid() && current.lease.Desc()!=view.desc){if(generation==UINT64_MAX){Fail(OwnedFrameStatus::InvalidInput);return {};}++generation;revision=1;assets_.ResetBank(domain,generation);}
    if(!revision){Fail(OwnedFrameStatus::InvalidInput);return {};}
    auto result=assets_.Publish({epoch_,domain,id,generation,revision},view);
    if(!result){Fail(result.status==OwnedAssetStatus::WouldBlock?OwnedFrameStatus::WouldBlock:OwnedFrameStatus::InvalidInput);return {};}
    current={result.lease,generation,revision};return result.lease;
}
SpriteHandle OwnedFrameUI::ResolveSprite(const TbSprite*){Fail();return kInvalidSpriteHandle;}
TbResult OwnedFrameUI::SubmitRawSprite(int32_t x,int32_t y,const TbSprite* s,KfxDrawState d){return sink_?sink_->UI().SubmitRawSprite(x,y,s,d):Lb_FAIL;}
TbResult OwnedFrameUI::SubmitRawSpriteOneColour(int32_t x,int32_t y,const TbSprite* s,unsigned char c,KfxDrawState d){return sink_?sink_->UI().SubmitRawSpriteOneColour(x,y,s,c,d):Lb_FAIL;}
TbResult OwnedFrameUI::SubmitRawSpriteScaled(int32_t x,int32_t y,const TbSprite* s,int32_t w,int32_t h,KfxDrawState d){return sink_?sink_->UI().SubmitRawSpriteScaled(x,y,s,w,h,d):Lb_FAIL;}
TbResult OwnedFrameUI::SubmitRawSpriteScaledOneColour(int32_t x,int32_t y,const TbSprite* s,int32_t w,int32_t h,unsigned char c,KfxDrawState d){return sink_?sink_->UI().SubmitRawSpriteScaledOneColour(x,y,s,w,h,c,d):Lb_FAIL;}
int OwnedFrameUI::SubmitRawSpriteScaledRemap(int32_t x,int32_t y,const TbSprite* s,int32_t w,int32_t h,const unsigned char* c,KfxDrawState d){return sink_?sink_->UI().SubmitRawSpriteScaledRemap(x,y,s,w,h,c,d):Lb_FAIL;}
void OwnedFrameUI::SubmitSolidBox(int32_t x,int32_t y,int32_t w,int32_t h,uint8_t c,KfxDrawState d){if(sink_)sink_->UI().SubmitSolidBox(x,y,w,h,c,d);}
void OwnedFrameUI::SetWorldOverlay(float z){IUIRenderer::SetWorldOverlay(z);if(sink_)sink_->UI().SetWorldOverlay(z);}
void OwnedFrameUI::ClearWorldOverlay(){IUIRenderer::ClearWorldOverlay();if(sink_)sink_->UI().ClearWorldOverlay();}
void OwnedFrameUI::SetWorldOverlayFlat(float z){IUIRenderer::SetWorldOverlayFlat(z);if(sink_)sink_->UI().SetWorldOverlayFlat(z);}
void OwnedFrameUI::ClearWorldOverlayFlat(){IUIRenderer::ClearWorldOverlayFlat();if(sink_)sink_->UI().ClearWorldOverlayFlat();}
void OwnedFrameUI::SetTopOverlay(){IUIRenderer::SetTopOverlay();if(sink_)sink_->UI().SetTopOverlay();}
void OwnedFrameUI::ClearTopOverlay(){IUIRenderer::ClearTopOverlay();if(sink_)sink_->UI().ClearTopOverlay();}
void OwnedFrameUI::BeginZoomBoxOverlay(int32_t x,int32_t y,int32_t w,int32_t h){
    if(!sink_ || zoom_capture_ || parchment_capture_){Fail();return;}
    zoom_capture_=true;sink_->UI().BeginZoomBoxOverlay(x,y,w,h);
}
void OwnedFrameUI::EndZoomBoxOverlay(int32_t x,int32_t y,int32_t w,int32_t h){
    if(!sink_ || !zoom_capture_){Fail();return;}
    sink_->UI().EndZoomBoxOverlay(x,y,w,h);zoom_capture_=false;
}
void OwnedFrameUI::SetUICommandBuffers(UICommandBuffers* p){if(p)Fail();}
void OwnedFrameUI::SetTextCommandBuffers(TextCommandBuffers* p){if(p)Fail();}
TbBool OwnedFrameUI::DrawTextResized(int32_t x,int32_t y,int32_t units,const char* text){return sink_?sink_->DrawTextResized(x,y,units,text):false;}
void OwnedFrameUI::UpdateSlabTexture(const unsigned char* data,int dim){
    if(!data || dim<=0 || dim>1024){Fail(OwnedFrameStatus::InvalidInput);return;}
    slab_bytes_.resize(size_t(dim)*dim);for(size_t i=0;i<slab_bytes_.size();++i)slab_bytes_[i]=data[i]?data[i]:1;slab_dim_=dim;
}
void OwnedFrameUI::SubmitSlabBackground(int32_t x,int32_t y,int32_t w,int32_t h){
    if(!builder_ || w<=0 || h<=0)return;
    if(!slab_dim_){Fail();return;}
    auto source=Publish(slab_,110,0,{{OwnedAssetFormat::R8,uint32_t(slab_dim_),uint32_t(slab_dim_)},slab_bytes_,size_t(slab_dim_)});if(!source.Valid())return;
    ApplyWindowOffset(ComputeCurrentLayer(),x,y);
    OwnedFrameQuad q;q.parchment_capture=parchment_capture_;q.phase=Phase();q.depth_test=q.phase==OwnedFramePhase::WorldSprites;q.z_ndc=ComputeCurrentNdcZ();q.mode=OwnedQuadMode::Solid;
    q.x0=float(x);q.y0=float(y);q.x1=float(int64_t(x)+w);q.y1=float(int64_t(y)+h);q.colour={0,0,0,1};if(!builder_->AddQuad(q))return;
    q.mode=OwnedQuadMode::Sprite;q.colour={1,1,1,1};q.source=source;q.colour_row=settings_.ui_palette;q.palette_source=OwnedQuadPaletteSource::Display;
    for(int64_t dy=0;dy<h;dy+=slab_dim_)for(int64_t dx=0;dx<w;dx+=slab_dim_){
        const auto tw=std::min<int64_t>(slab_dim_,int64_t(w)-dx),th=std::min<int64_t>(slab_dim_,int64_t(h)-dy);
        q.x0=float(int64_t(x)+dx);q.y0=float(int64_t(y)+dy);q.x1=q.x0+float(tw);q.y1=q.y0+float(th);q.u0=q.v0=0;q.u1=float(tw)/slab_dim_;q.v1=float(th)/slab_dim_;
        if(!builder_->AddQuad(q))return;
    }
}
uint8_t* OwnedFrameUI::AcquireMinimapBuffer(int x,int y,int size){
    if(!builder_ || size<=0 || size>2048){Fail(OwnedFrameStatus::InvalidInput);throw std::invalid_argument("Invalid owned minimap buffer");}
    minimap_bytes_.assign(size_t(size)*size,0);minimap_size_=size;minimap_x_=x;minimap_y_=y;return minimap_bytes_.data();
}
void OwnedFrameUI::SubmitMinimap(int x,int y,int size,const int32_t*,const int32_t*){
    if(!builder_ || size!=minimap_size_ || x!=minimap_x_ || y!=minimap_y_ || size<=0){Fail(OwnedFrameStatus::InvalidInput);return;}
    auto source=Publish(minimap_,111,0,{{OwnedAssetFormat::R8,uint32_t(size),uint32_t(size)},minimap_bytes_,size_t(size)});if(!source.Valid())return;
    ApplyWindowOffset(ComputeCurrentLayer(),x,y);OwnedFrameQuad q;q.parchment_capture=parchment_capture_;q.phase=Phase();q.mode=OwnedQuadMode::Sprite;q.depth_test=q.phase==OwnedFramePhase::WorldSprites;q.z_ndc=ComputeCurrentNdcZ();
    q.x0=float(x);q.y0=float(y);q.x1=q.x0+size;q.y1=q.y0+size;q.u1=q.v1=1;q.source=std::move(source);q.colour_row=settings_.ui_palette;q.palette_source=OwnedQuadPaletteSource::Display;builder_->AddQuad(std::move(q));
}
void OwnedFrameUI::SubmitPointerSprite(const TbSprite* spr,int32_t x,int32_t y,int units){
    pointer_.reset();if(!sink_ || !spr)return;
    TbSpriteSourceIdentity identity{};if(!get_ui_sprite_source_identity(spr,&identity)){Fail(OwnedFrameStatus::InvalidInput);return;}
    if(!identity.width || !identity.height)return;
    if(units<=0){Fail(OwnedFrameStatus::InvalidInput);return;}
    auto source=sink_->UISource(spr);if(!source.Valid())return;
    OwnedFrameQuad q;q.phase=OwnedFramePhase::Cursor;q.mode=OwnedQuadMode::Sprite;q.x0=float(x);q.y0=float(y);q.x1=q.x0+float(identity.width)*units/16.0f;q.y1=q.y0+float(identity.height)*units/16.0f;
    q.u0=(1.0f/32)/identity.width;q.v0=(1.0f/32)/identity.height;q.u1=1-q.u0;q.v1=1-q.v0;q.source=std::move(source);q.colour_row=settings_.ui_palette;q.palette_source=OwnedQuadPaletteSource::Display;pointer_=std::move(q);
}
void OwnedFrameUI::SubmitKeeperHandSprite(short x,short y,unsigned short base,short angle,unsigned char group,int32_t scale,TbDrawFlagsMask flags){
    if(!sink_)return;
    SpriteScale geometry{};int32_t index=-1;const unsigned char* data=nullptr;int w=0,h=0;
    if(!resolve_keepersprite_cursor_geometry(x,y,base,angle,group,scale,&geometry,&index,&data,&w,&h))return;
    sink_->cursor_capture_=true;
    try{sink_->CaptureKeeperSprite(&geometry,data,w,h,h,flags,nullptr,index,-1,false,255);}catch(...){sink_->cursor_capture_=false;throw;}
    sink_->cursor_capture_=false;
}
void OwnedFrameUI::Draw(){if(builder_ && image_overlay_){builder_->AddImage(std::move(*image_overlay_));image_overlay_.reset();}if(builder_ && pointer_){builder_->AddQuad(std::move(*pointer_));pointer_.reset();}}
bool OwnedFrameUI::SetParchmentCapture(bool active) noexcept
{
    if(!builder_ || !sink_ || zoom_capture_ || parchment_capture_==active){Fail();return false;}
    parchment_capture_=active;sink_->SetParchmentCapture(active);return true;
}
bool OwnedFrameUI::SubmitMapFadeStep(int tick,float step,float warp,bool fading_in,const uint8_t* ghost,const OwnedAssetLease& fade)
{
    if(!builder_ || !engine_palette || !std::isfinite(step) || step<0 || step>32 || !std::isfinite(warp) || warp<=0 || warp>2.5f){Fail(OwnedFrameStatus::InvalidInput);return false;}
    const bool capture=((fading_in && tick==0)||(!fading_in && tick==32)) && !map_was_active_;
    if(capture && !map_submitted_) {
        if(!ghost || map_transition_==UINT64_MAX){Fail(OwnedFrameStatus::InvalidInput);return false;}
        if(!Publish(map_ghost_,305,0,{{OwnedAssetFormat::R8,256,256},{ghost,256*256},256}).Valid())return false;
        ++map_transition_;
    }
    if(!map_transition_ || !map_ghost_.lease.Valid()){Fail(OwnedFrameStatus::Incomplete);return false;}
    if(!have_map_base_ || !std::equal(map_base_palette_.begin(),map_base_palette_.end(),engine_palette)) {
        map_base_bytes_.resize(PaletteIndexTableBytes);
        BuildPaletteIndexBase(map_base_bytes_,{engine_palette,768},LbPaletteFindColour);
        std::copy_n(engine_palette,768,map_base_palette_.begin());have_map_base_=true;
    }
    auto base=Publish(map_base_,306,0,{{OwnedAssetFormat::R8,512,512},map_base_bytes_,512});if(!base.Valid())return false;
    map_rgb6_bytes_=map_base_bytes_;std::array<uint32_t,256> keys{};
    ApplyPaletteIndexOverrides(map_rgb6_bytes_,settings_.ui_palette.Bytes(),keys);
    auto rgb6=Publish(map_rgb6_,307,0,{{OwnedAssetFormat::R8,512,512},map_rgb6_bytes_,512});if(!rgb6.Valid())return false;
    OwnedFrameMapFade command;command.active=true;command.capture_pending=capture;command.transition_id=map_transition_;
    command.step=step;command.warp_scale=warp;command.ghost_table=map_ghost_.lease;command.rgb6=rgb6;command.rgb6_base=base;
    command.palette=settings_.ui_palette;command.fade=fade;
    map_submitted_=builder_->SetMapFade(std::move(command));return map_submitted_;
}
bool OwnedFrameUI::SubmitZoomBoxTiles(const uint16_t* ids,int nx,int ny,int x,int y,int w,int h)
{
    if(!builder_ || !ids || nx<=0 || ny<=0 || w<=0 || h<=0 || int64_t(nx)*ny>4096){Fail(OwnedFrameStatus::InvalidInput);return false;}
    for(int dy=0;dy<ny;++dy)for(int dx=0;dx<nx;++dx) {
        const auto id=ids[dy*nx+dx];if(id==UINT16_MAX)continue;
        size_t slot=0;while(slot<zoom_tiles_.size() && (!zoom_tiles_[slot].lease.Valid() || zoom_tile_ids_[slot]!=id))++slot;
        if(slot==zoom_tiles_.size()) {
            for(size_t n=0;n<zoom_tiles_.size();++n){const size_t candidate=(zoom_tile_victim_+n)%zoom_tiles_.size();if(!zoom_tile_used_[candidate]){slot=candidate;zoom_tile_victim_=(candidate+1)%zoom_tiles_.size();break;}}
            if(slot==zoom_tiles_.size()){Fail(OwnedFrameStatus::CommandLimit);return false;}
        }
        TileSourceView view;if(!ResolveLogicalTileSource(id/TileSourceTileCount,id%TileSourceTileCount,view)){Fail(OwnedFrameStatus::InvalidInput);return false;}
        auto source=Publish(zoom_tiles_[slot],308,slot,{{OwnedAssetFormat::R8,32,32},view.bytes,view.row_stride});if(!source.Valid())return false;
        const int64_t px=int64_t(x)+int64_t(dx)*w,py=int64_t(y)+int64_t(dy)*h;
        if(px<INT32_MIN || px>INT32_MAX || py<INT32_MIN || py>INT32_MAX){Fail(OwnedFrameStatus::InvalidInput);return false;}
        zoom_tile_ids_[slot]=id;zoom_tile_used_[slot]=true;
        OwnedFrameQuad quad;quad.phase=OwnedFramePhase::ZoomBoxes;quad.mode=OwnedQuadMode::OpaqueIndexed;
        quad.x0=float(px);quad.y0=float(py);quad.x1=float(px+w);quad.y1=float(py+h);quad.u1=quad.v1=1;
        quad.source=std::move(source);quad.colour_row=settings_.ui_palette;quad.palette_source=OwnedQuadPaletteSource::Display;
        if(!builder_->AddQuad(std::move(quad)))return false;
    }
    return true;
}
void OwnedFrameUI::Clear(){pointer_.reset();}
bool OwnedFrameUI::SubmitLandviewZoom(const unsigned char* pixels,int width,int height,
    float map_x,float map_y,float screen_x,float screen_y,float scale)
{
    if(!builder_ || !pixels || width<=0 || height<=0 || width>16384 || height>16384) {
        Fail(OwnedFrameStatus::InvalidInput);return false;
    }
    OwnedFrameImage image;image.mode=OwnedImageMode::LandviewZoom;
    image.zoom={map_x,map_y,screen_x,screen_y,scale};
    for(float v:image.zoom) if(!std::isfinite(v) || std::abs(v)>16777216.0f) {
        Fail(OwnedFrameStatus::InvalidInput);return false;
    }
    image.width=builder_->Info().native_width;image.height=builder_->Info().native_height;
    image.palette=settings_.ui_palette;image.uses_display_palette=true;
    image.source=Publish(zoom_,304,0,{{OwnedAssetFormat::R8,uint32_t(width),uint32_t(height)},
        {pixels,size_t(width)*size_t(height)},size_t(width)});
    if(!image.source.Valid())return false;
    zoom_submitted_=builder_->AddImage(std::move(image));return zoom_submitted_;
}
bool OwnedFrameUI::SubmitIndexedImage(std::span<const uint8_t> bytes,size_t stride,uint32_t width,uint32_t height,
    int32_t x,int32_t y,OwnedFramePhase phase,OwnedImageMode mode,std::span<const uint8_t> coverage,size_t coverage_stride)
{
    if(!builder_ || !width || !height || width>16384 || height>16384 || stride<width || bytes.size()<width
        || size_t(height-1)>(bytes.size()-width)/stride || uint8_t(mode)>uint8_t(OwnedImageMode::Composite)
        || (!coverage.empty() && (mode!=OwnedImageMode::Transparent || coverage_stride<width || coverage.size()<width
            || size_t(height-1)>(coverage.size()-width)/coverage_stride)) || image_slot_>=images_.size()){
        Fail(OwnedFrameStatus::InvalidInput);return false;
    }
    const size_t slot=image_slot_++;
    OwnedFrameImage image;image.phase=phase;image.mode=mode;image.x=x;image.y=y;image.width=width;image.height=height;image.palette=settings_.ui_palette;image.uses_display_palette=true;
    image.source=Publish(images_[slot],112+slot,0,{{OwnedAssetFormat::R8,width,height},bytes,stride});if(!image.source.Valid())return false;
    if(!coverage.empty()){image.coverage=Publish(coverage_[slot],176+slot,0,{{OwnedAssetFormat::R8,width,height},coverage,coverage_stride});if(!image.coverage.Valid())return false;}
    if(phase==OwnedFramePhase::ImagePresentOverlay){image_overlay_=std::move(image);return true;}
    return builder_->AddImage(std::move(image));
}
bool OwnedFrameUI::PresentImage(const RendererPresentImageDesc& desc,OwnedFramePhase phase)
{
    if(!builder_ || !desc.src || desc.src_w<=0 || desc.src_h<=0 || desc.src_w>16384 || desc.src_h>16384
        || desc.dst_w<=0 || desc.dst_h<=0 || desc.format!=PRESENT_FORMAT_INDEXED8
        || desc.kind<PRESENT_KIND_OPAQUE || desc.kind>PRESENT_KIND_COMPOSITE
        || (desc.palette!=PRESENT_PALETTE_GAME && desc.palette!=PRESENT_PALETTE_EMBEDDED)
        || (desc.palette==PRESENT_PALETTE_EMBEDDED && !desc.embedded_palette)
        || (desc.coverage && desc.kind!=PRESENT_KIND_TRANSPARENT) || image_slot_>=images_.size()){
        Fail(OwnedFrameStatus::InvalidInput);return false;
    }
    const size_t stride=desc.src_pitch>0?size_t(desc.src_pitch):size_t(desc.src_w);
    if(stride<size_t(desc.src_w) || size_t(desc.src_h-1)>(SIZE_MAX-size_t(desc.src_w))/stride){Fail(OwnedFrameStatus::InvalidInput);return false;}
    const size_t span_size=size_t(desc.src_h-1)*stride+desc.src_w,slot=image_slot_++;
    OwnedFrameImage image;image.phase=desc.kind==PRESENT_KIND_TRANSPARENT?OwnedFramePhase::ImagePresentOverlay:phase;
    image.mode=static_cast<OwnedImageMode>(desc.kind);image.x=desc.dst_x;image.y=desc.dst_y;image.width=uint32_t(desc.dst_w);image.height=uint32_t(desc.dst_h);
    image.source=Publish(images_[slot],112+slot,0,{{OwnedAssetFormat::R8,uint32_t(desc.src_w),uint32_t(desc.src_h)},{desc.src,span_size},stride});if(!image.source.Valid())return false;
    image.palette=settings_.ui_palette;image.uses_display_palette=true;
    if(desc.palette==PRESENT_PALETTE_EMBEDDED){
        image.uses_display_palette=false;
        std::array<uint8_t,1024> rgba{};
        for(size_t i=0;i<256;++i){rgba[i*4]=desc.embedded_palette[i*4+2];rgba[i*4+1]=desc.embedded_palette[i*4+1];rgba[i*4+2]=desc.embedded_palette[i*4];rgba[i*4+3]=255;}
        image.palette=Publish(palettes_[slot],240+slot,0,{{OwnedAssetFormat::RGBA8,256,1},rgba,1024});if(!image.palette.Valid())return false;
    }
    if(desc.coverage){image.coverage=Publish(coverage_[slot],176+slot,0,{{OwnedAssetFormat::R8,uint32_t(desc.src_w),uint32_t(desc.src_h)},{desc.coverage,size_t(desc.src_w)*desc.src_h},size_t(desc.src_w)});if(!image.coverage.Valid())return false;}
    if(image.phase==OwnedFramePhase::ImagePresentOverlay){image_overlay_=std::move(image);return true;}
    return builder_->AddImage(std::move(image));
}
void OwnedFrameUI::PresentHugeSprite(const TbHugeSprite* sprite,int32_t length,int32_t x,int32_t y,int32_t units)
{
    if(!builder_)return;
    LandWindowSpriteSource source{};
    const auto& info=builder_->Info();const size_t count=size_t(info.native_width)*info.native_height;
    if(!get_land_window_sprite_source(sprite,&source) || length<=0 || size_t(length)!=source.data_bytes
        || count>32u*1024u*1024u){Fail(OwnedFrameStatus::InvalidInput);return;}
    huge_pixels_.resize(count);huge_coverage_.resize(count);
    if(!DecodeHugeSprite({{source.data,source.data_bytes},{source.line_offsets,source.line_bytes},source.width,source.height},
        huge_pixels_,huge_coverage_,info.native_width,info.native_height,x,y,units)){
        Fail(OwnedFrameStatus::InvalidInput);return;
    }
    RendererPresentImageDesc image{};image.src=huge_pixels_.data();image.coverage=huge_coverage_.data();
    image.src_w=int(info.native_width);image.src_h=int(info.native_height);image.src_pitch=image.src_w;
    image.dst_w=image.src_w;image.dst_h=image.src_h;image.kind=PRESENT_KIND_TRANSPARENT;
    PresentImage(image);
}

}
