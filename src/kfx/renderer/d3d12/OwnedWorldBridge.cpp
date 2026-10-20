// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
// KeeperFX renderer performance changes: 2026-10-19.
#include "OwnedWorldBridge.h"
#include "Dx12Device.h"
#include "RuntimeWorldValidation.h"
#include "kfx/renderer/SpriteRasterBounds.h"
#include <algorithm>
#include <stdexcept>
#include <thread>

namespace kfx::render::dx12 {
namespace {
const OwnedFrameImage* ActiveZoom(const OwnedRenderFrame& p) {
    for(auto it=p.Images().rbegin();it!=p.Images().rend();++it)
        if(it->mode==OwnedImageMode::LandviewZoom)return &*it;
    return nullptr;
}
bool ActiveImage(const OwnedFrameImage& image,const OwnedFrameImage* zoom) {
    return !zoom || image.phase!=OwnedFramePhase::ImagePresents || &image==zoom;
}
bool VisibleSprite(const OwnedWorldSprite& draw,const OwnedFrameInfo& info) {
    const bool cursor=draw.phase==OwnedFramePhase::Cursor;
    return SpriteRasterBounds(draw.x,draw.width,cursor?info.native_width:info.viewport_width).Visible()
        && SpriteRasterBounds(draw.y,draw.height,cursor?info.native_height:info.viewport_height).Visible();
}
bool WorldQuad(OwnedFramePhase p) {
    return p==OwnedFramePhase::WorldSprites || p==OwnedFramePhase::WorldFlatOverlay || p==OwnedFramePhase::Swipe;
}
bool PostQuad(OwnedFramePhase p) {
    return p==OwnedFramePhase::OverheadMap||p==OwnedFramePhase::GameUI||p==OwnedFramePhase::ZoomBoxes
        ||p==OwnedFramePhase::FrontOverlay||p==OwnedFramePhase::Text||p==OwnedFramePhase::Cursor;
}
bool RowQuad(OwnedQuadMode mode) {
    return mode==OwnedQuadMode::Sprite||mode==OwnedQuadMode::Fade||mode==OwnedQuadMode::Clut
        ||mode==OwnedQuadMode::OpaqueIndexed;
}
RuntimePostPhase PostPhase(OwnedFramePhase p) {
    switch(p) {
    case OwnedFramePhase::ImagePresents:return RuntimePostPhase::ImagePresents;
    case OwnedFramePhase::OverheadMap:return RuntimePostPhase::OverheadMap;
    case OwnedFramePhase::GameUI:return RuntimePostPhase::GameUI;
    case OwnedFramePhase::ZoomBoxes:return RuntimePostPhase::ZoomBoxes;
    case OwnedFramePhase::FrontOverlay:return RuntimePostPhase::FrontOverlay;
    case OwnedFramePhase::Text:return RuntimePostPhase::Text;
    case OwnedFramePhase::ImagePresentOverlay:return RuntimePostPhase::ImagePresentOverlay;
    case OwnedFramePhase::Cursor:return RuntimePostPhase::Cursor;
    default:throw std::invalid_argument("Unsupported post-world phase");
    }
}
RuntimePostPhase PostPhase(const OwnedFrameQuad& q) {
    if(!q.parchment_capture)return PostPhase(q.phase);
    switch(q.phase) {
    case OwnedFramePhase::WorldSprites:return RuntimePostPhase::ParchmentWorld;
    case OwnedFramePhase::WorldFlatOverlay:return RuntimePostPhase::ParchmentFlat;
    case OwnedFramePhase::GameUI:return RuntimePostPhase::ParchmentUI;
    case OwnedFramePhase::FrontOverlay:return RuntimePostPhase::ParchmentFront;
    default:throw std::invalid_argument("Unsupported parchment capture phase");
    }
}
bool Covered(const OwnedRenderFrame& p,OwnedFramePhase phase) {
    const auto s=p.Phase(phase); return s==OwnedPhaseState::Empty || s==OwnedPhaseState::Recorded;
}
bool Lineage(const OwnedAssetKey& a,const OwnedAssetKey& b) {
    return a.epoch==b.epoch && a.domain==b.domain && a.logical_id==b.logical_id && a.generation==b.generation;
}
RuntimeAssetKind SpriteKind(const OwnedAssetLease& source) {
    return source.Desc().format==OwnedAssetFormat::RG8?RuntimeAssetKind::SpriteRG8:RuntimeAssetKind::SpriteRGBA8;
}
OwnedAssetFormat Format(RuntimeAssetKind kind) {
    switch(kind) {
    case RuntimeAssetKind::PaletteRGBA8: case RuntimeAssetKind::ColourRowRGBA8: case RuntimeAssetKind::SpriteRGBA8: return OwnedAssetFormat::RGBA8;
    case RuntimeAssetKind::SpriteRG8: return OwnedAssetFormat::RG8;
    case RuntimeAssetKind::LightmapR16: return OwnedAssetFormat::R16LE;
    case RuntimeAssetKind::RemapRG16UInt: return OwnedAssetFormat::RG16SIntLE; // Raw XY bits, no signed conversion.
    default: return OwnedAssetFormat::R8;
    }
}
uint32_t PixelBytes(OwnedAssetFormat f) {
    return f==OwnedAssetFormat::RGBA8 || f==OwnedAssetFormat::RG16SIntLE ? 4 : (f==OwnedAssetFormat::R16LE || f==OwnedAssetFormat::RG8) ? 2 : 1;
}
}
struct OwnedWorldBridge::Impl {
    struct Slot {
        bool assigned=false,selected=false,has_content=false;
        RuntimeAssetKind kind{};
        OwnedAssetKey content{};
        uint64_t generation=1,revision=0;
        RuntimeAssetLease runtime;
    };
    struct Need { OwnedAssetLease owned; RuntimeAssetKind kind; size_t slot=SIZE_MAX; };
    Device& device;
    OwnedWorldBridgeLimits limits;
    std::thread::id thread=std::this_thread::get_id();
    std::vector<Slot> slots;
    std::vector<Need> needs;
    std::vector<size_t> need_lookup,cache_lookup; // Fixed open-address tables, indices plus one.
    OwnedFrameLease pending;
    RuntimeWorldFrame frame;
    bool prepared=false,full_frame=false;
    uint64_t epoch=0;
    size_t victim_cursor=0;
    OwnedWorldBridgeStats counters;
    Impl(Device& d,OwnedWorldBridgeLimits l):device(d),limits(l),slots(l.mapping_slots) {
        needs.reserve(l.mapping_slots);
        size_t capacity=1;while(capacity<size_t(l.mapping_slots)*2)capacity*=2;
        need_lookup.resize(capacity);cache_lookup.resize(capacity);
    }
    static size_t Hash(const OwnedAssetKey& key,RuntimeAssetKind kind) noexcept {
        uint64_t hash=1469598103934665603ull;
        for(auto value:{key.epoch,key.domain,key.logical_id,key.generation,key.content_revision,uint64_t(kind)}) {
            value^=value>>30;value*=0xbf58476d1ce4e5b9ull;value^=value>>27;
            value*=0x94d049bb133111ebull;value^=value>>31;hash=(hash^value)*1099511628211ull;
        }
        return size_t(hash);
    }
    size_t Find(const std::vector<size_t>& table,const OwnedAssetKey& key,RuntimeAssetKind kind,bool cached) const noexcept {
        size_t at=Hash(key,kind)&(table.size()-1);
        for(size_t count=0;count<table.size();++count,at=(at+1)&(table.size()-1)) {
            if(!table[at])return SIZE_MAX;
            const auto index=table[at]-1;
            if(cached ? slots[index].kind==kind&&slots[index].content==key
                      : needs[index].kind==kind&&needs[index].owned.Key()==key)return index;
        }
        return SIZE_MAX;
    }
    static void Insert(std::vector<size_t>& table,const OwnedAssetKey& key,RuntimeAssetKind kind,size_t index) noexcept {
        size_t at=Hash(key,kind)&(table.size()-1);
        while(table[at])at=(at+1)&(table.size()-1);
        table[at]=index+1;
    }
    void CheckThread() const { if(thread!=std::this_thread::get_id()) throw std::logic_error("Owned world bridge requires its render thread"); }
    void Add(const OwnedAssetLease& asset,RuntimeAssetKind kind) {
        if(!asset.Valid()) throw std::invalid_argument("Missing owned world source");
        const auto desc=asset.Desc();
        if(desc.format!=Format(kind) || asset.RowStride()!=uint64_t(desc.width)*PixelBytes(desc.format))
            throw std::invalid_argument("Owned world source is not canonical tight storage");
        if(Find(need_lookup,asset.Key(),kind,false)!=SIZE_MAX)return;
        if(needs.size()==limits.mapping_slots)throw std::length_error("Owned packet exceeds bridge source capacity");
        const auto index=needs.size();needs.push_back({asset,kind,SIZE_MAX});
        Insert(need_lookup,asset.Key(),kind,index);
    }
    void Gather(const OwnedRenderFrame& p) {
        const bool world=!p.Terrain().empty()||!p.Shadows().empty();
        const bool fade=std::any_of(p.Quads().begin(),p.Quads().end(),[&](const auto& d){return ((!d.parchment_capture&&WorldQuad(d.phase))||(full_frame&&(d.parchment_capture||PostQuad(d.phase))))&&d.mode==OwnedQuadMode::Fade;});
        if(world||p.Palette().Valid())Add(p.Palette(),RuntimeAssetKind::PaletteRGBA8);
        if(world||fade||p.Fade().Valid())Add(p.Fade(),RuntimeAssetKind::FadeR8);
        if(world||p.Lightmap().Valid())Add(p.Lightmap(),RuntimeAssetKind::LightmapR16);
        for(const auto& d:p.Terrain()) Add(d.tile,RuntimeAssetKind::TileR8);
        for(const auto& d:p.Shadows()) Add(d.source,SpriteKind(d.source));
        const auto sprites=[&](auto draws){for(const auto& d:draws){ if((d.phase==OwnedFramePhase::Cursor&&!full_frame)||!VisibleSprite(d,p.Info()))continue; Add(d.source,SpriteKind(d.source)); if(d.pass==OwnedSpritePass::Normal) Add(d.colour_row,RuntimeAssetKind::ColourRowRGBA8); }};
        sprites(p.OutlineSprites()); sprites(p.Sprites());
        for(const auto& d:p.Quads()) if((!d.parchment_capture&&WorldQuad(d.phase))||(full_frame&&(d.parchment_capture||PostQuad(d.phase)))) {
            if(d.mode!=OwnedQuadMode::Solid) Add(d.source,RuntimeAssetKind::Indexed8);
            if(RowQuad(d.mode)) Add(d.colour_row,RuntimeAssetKind::ColourRowRGBA8);
        }
        if(full_frame)for(const auto& image:p.Images()) if(ActiveImage(image,ActiveZoom(p))) {
            Add(image.source,RuntimeAssetKind::Indexed8);Add(image.palette,RuntimeAssetKind::PaletteRGBA8);
            if(image.coverage.Valid())Add(image.coverage,RuntimeAssetKind::Indexed8);
        }
        if(full_frame&&p.MapFade().active) {
            const auto& m=p.MapFade();
            Add(m.palette,RuntimeAssetKind::PaletteRGBA8);Add(m.fade,RuntimeAssetKind::FadeR8);
            Add(m.rgb6,RuntimeAssetKind::Indexed8);Add(m.ghost_table,RuntimeAssetKind::Indexed8);
        }
        const auto& l=p.Lens();
        if(l.kind==OwnedLensKind::Remap) Add(l.remap,RuntimeAssetKind::RemapRG16UInt);
        if(l.kind==OwnedLensKind::Mist||l.kind==OwnedLensKind::Overlay) {
            Add(l.palette,RuntimeAssetKind::PaletteRGBA8); Add(l.rgb6,RuntimeAssetKind::Indexed8);
            if(l.kind==OwnedLensKind::Mist) {Add(l.fade,RuntimeAssetKind::FadeR8);Add(l.density,RuntimeAssetKind::Indexed8);}
            else Add(l.overlay,RuntimeAssetKind::Indexed8);
        }
    }
    bool Select() {
        // Pin every exact hit before choosing victims, including later references.
        std::fill(cache_lookup.begin(),cache_lookup.end(),0);
        for(size_t i=0;i<slots.size();++i)if(slots[i].has_content)
            Insert(cache_lookup,slots[i].content,slots[i].kind,i);
        for(auto& n:needs) {
            n.slot=Find(cache_lookup,n.owned.Key(),n.kind,true);
            if(n.slot!=SIZE_MAX)slots[n.slot].selected=true;
        }
        for(auto& n:needs) if(n.slot==SIZE_MAX) {
            size_t chosen=SIZE_MAX;
            for(size_t i=0;i<slots.size();++i) {const auto& s=slots[i];if(!s.selected&&s.assigned&&s.kind==n.kind&&s.has_content&&Lineage(s.content,n.owned.Key())) {chosen=i;break;}}
            // Distinct animation identities occupy cold slots before evicting
            // another cached frame. Only a full cache advances the bounded clock.
            if(chosen==SIZE_MAX) for(size_t i=0;i<slots.size();++i) if(!slots[i].assigned) {chosen=i;break;}
            if(chosen==SIZE_MAX) for(size_t count=0;count<slots.size();++count) {
                const size_t i=(victim_cursor+count)%slots.size();
                if(!slots[i].selected){chosen=i;victim_cursor=(i+1)%slots.size();break;}
            }
            if(chosen==SIZE_MAX) return false;
            auto& s=slots[chosen];
            if(s.has_content&&(s.kind!=n.kind||!Lineage(s.content,n.owned.Key()))) {
                if(s.generation>=UINT64_MAX-1) throw std::overflow_error("Bridge residency generation exhausted");
                ++s.generation;s.revision=0;
            }
            s.runtime.Reset();s.assigned=true;s.selected=true;s.has_content=true;s.kind=n.kind;s.content=n.owned.Key();n.slot=chosen;
        }
        return true;
    }
    RuntimeAssetLease Get(const OwnedAssetLease& a,RuntimeAssetKind k) const {
        const auto index=Find(need_lookup,a.Key(),k,false);
        if(index!=SIZE_MAX)return slots[needs[index].slot].runtime;
        throw std::logic_error("Owned world source was not selected");
    }
    void Lower() {
        RuntimeWorldFrame out;
        const auto& p=*pending;const auto& info=p.Info();
        out.native_width=info.native_width;out.native_height=info.native_height;out.viewport_x=info.viewport_x;out.viewport_y=info.viewport_y;
        out.viewport_width=info.viewport_width;out.viewport_height=info.viewport_height;out.clear_rgba=info.clear_rgba;
        out.shade_settings=info.shade_settings;out.palette_transform=info.palette_transform;out.time=info.time;
        if(p.Palette().Valid())out.palette=Get(p.Palette(),RuntimeAssetKind::PaletteRGBA8);
        if(p.Fade().Valid())out.fade=Get(p.Fade(),RuntimeAssetKind::FadeR8);
        if(p.Lightmap().Valid())out.lightmap=Get(p.Lightmap(),RuntimeAssetKind::LightmapR16);
        out.terrain.reserve(p.Terrain().size());out.shadows.reserve(p.Shadows().size());out.sprites.reserve(p.OutlineSprites().size()+p.Sprites().size());
        for(const auto& d:p.Terrain()) {
            RuntimeWorldTerrain q;q.tile=Get(d.tile,RuntimeAssetKind::TileR8);q.emissive_lava=d.emissive_lava;
            for(size_t i=0;i<3;++i) {const auto& v=d.vertices[i];q.vertices[i]={{v.x,v.y,v.z,v.camera_z},v.u,v.v,v.shade,v.stl_x,v.stl_y};}
            out.terrain.push_back(std::move(q));
        }
        for(const auto& d:p.Shadows()) {
            RuntimeWorldShadow q;q.source=Get(d.source,SpriteKind(d.source));q.draw.z_ndc=d.z_ndc;q.draw.darken=d.darken;q.draw.source_width=d.source_width;q.draw.source_height=d.source_height;
            for(size_t i=0;i<4;++i) {const auto& v=d.vertices[i];q.draw.vertices[i]={v.x,v.y,v.u,v.v};}out.shadows.push_back(std::move(q));
        }
        const auto sprites=[&](auto draws){for(const auto& d:draws) {
            if((d.phase==OwnedFramePhase::Cursor&&!full_frame)||!VisibleSprite(d,info))continue;
            RuntimeWorldSprite q;auto& v=q.draw;v.kind=static_cast<OutlinePassKind>(d.pass);
            v.x=d.x;v.y=d.y;v.width=d.width;v.height=d.height;v.phase_x=d.phase_x;v.phase_y=d.phase_y;v.step_x=d.step_x;v.step_y=d.step_y;
            v.source_width=d.source_width;v.source_height=d.source_height;v.content_height=d.content_height;v.flip=d.flip;v.premultiplied=d.premultiplied;
            v.z_ndc=d.z_ndc;v.colour=d.colour;v.source_alpha=d.source_alpha;v.texture_fallback=d.texture_fallback;
            q.source=Get(d.source,SpriteKind(d.source));
            if(d.pass==OwnedSpritePass::Normal){q.colour_row=Get(d.colour_row,RuntimeAssetKind::ColourRowRGBA8);v.lookup_row=0;}
            if(d.phase==OwnedFramePhase::Cursor)out.cursor_sprites.push_back(std::move(q));else out.sprites.push_back(std::move(q));
        }};
        sprites(p.OutlineSprites());sprites(p.Sprites());
        // Normal UI queues execute by phase, preserving painter order within each.
        for(auto phase:{OwnedFramePhase::WorldSprites,OwnedFramePhase::WorldFlatOverlay,OwnedFramePhase::Swipe})
        for(const auto& d:p.Quads()) if(d.phase==phase&&!d.parchment_capture) {
            RuntimeWorldOverlay q;auto& v=q.draw;v.phase=d.phase==OwnedFramePhase::WorldSprites?WorldOverlayPhase::DepthWorld:d.phase==OwnedFramePhase::WorldFlatOverlay?WorldOverlayPhase::FlatWorld:WorldOverlayPhase::Swipe;
            v.mode=static_cast<WorldOverlayMode>(d.mode);v.depth_test=d.depth_test;v.scissor_enabled=d.scissor_enabled;v.scissor_x=d.scissor_x;v.scissor_y=d.scissor_y;v.scissor_w=d.scissor_w;v.scissor_h=d.scissor_h;
            v.x0=d.x0;v.y0=d.y0;v.x1=d.x1;v.y1=d.y1;v.u0=d.u0;v.v0=d.v0;v.u1=d.u1;v.v1=d.v1;v.z_ndc=d.z_ndc;v.colour=d.colour;v.fade_row=d.fade_row;
            if(d.mode!=OwnedQuadMode::Solid){q.source=Get(d.source,RuntimeAssetKind::Indexed8);v.source=0;}
            if(d.mode==OwnedQuadMode::Sprite||d.mode==OwnedQuadMode::Fade||d.mode==OwnedQuadMode::Clut){q.colour_row=Get(d.colour_row,RuntimeAssetKind::ColourRowRGBA8);v.colour_row=0;}
            out.overlays.push_back(std::move(q));
        }
        const auto& l=p.Lens();if(l.kind!=OwnedLensKind::None) {
            RuntimeWorldLens q;q.kind=l.kind==OwnedLensKind::Copy?LensPassKind::Copy:l.kind==OwnedLensKind::Remap?LensPassKind::Remap:l.kind==OwnedLensKind::Mist?LensPassKind::Mist:LensPassKind::Overlay;
            q.source_x=l.source_x;q.destination_x=l.destination_x;q.destination_y=l.destination_y;q.viewport_width=l.viewport_width;q.viewport_height=l.viewport_height;
            q.mist_offsets=l.mist_offsets;q.clear_rgba=l.clear_rgba;q.mist_lightness=l.mist_lightness;q.overlay_alpha=l.overlay_alpha;
            if(l.kind==OwnedLensKind::Remap) q.remap=Get(l.remap,RuntimeAssetKind::RemapRG16UInt);
            if(l.kind==OwnedLensKind::Mist||l.kind==OwnedLensKind::Overlay) {
                q.palette=Get(l.palette,RuntimeAssetKind::PaletteRGBA8);q.rgb6=Get(l.rgb6,RuntimeAssetKind::Indexed8);
                if(l.kind==OwnedLensKind::Mist){q.fade=Get(l.fade,RuntimeAssetKind::FadeR8);q.density=Get(l.density,RuntimeAssetKind::Indexed8);}else q.overlay=Get(l.overlay,RuntimeAssetKind::Indexed8);
            }out.lens=std::move(q);
        }
        if(full_frame&&p.MapFade().active) {
            const auto& m=p.MapFade();RuntimeWorldMapFade q;
            q.transition_id=m.transition_id;q.capture_pending=m.capture_pending;q.step=m.step;q.warp_scale=m.warp_scale;
            q.palette=Get(m.palette,RuntimeAssetKind::PaletteRGBA8);q.fade=Get(m.fade,RuntimeAssetKind::FadeR8);
            q.rgb6=Get(m.rgb6,RuntimeAssetKind::Indexed8);q.ghost_table=Get(m.ghost_table,RuntimeAssetKind::Indexed8);
            out.map_fade=std::move(q);
        }
        if(full_frame)for(auto phase:{RuntimePostPhase::ImagePresents,RuntimePostPhase::OverheadMap,RuntimePostPhase::GameUI,
            RuntimePostPhase::ParchmentWorld,RuntimePostPhase::ParchmentFlat,RuntimePostPhase::ParchmentUI,RuntimePostPhase::ParchmentFront,
            RuntimePostPhase::ZoomBoxes,RuntimePostPhase::FrontOverlay,RuntimePostPhase::Text,RuntimePostPhase::ImagePresentOverlay,RuntimePostPhase::Cursor}) {
            for(const auto& d:p.Images())if(PostPhase(d.phase)==phase && ActiveImage(d,ActiveZoom(p))) {
                RuntimePostDraw q;q.phase=phase;
                q.zoom=d.zoom;
                q.kind=d.mode==OwnedImageMode::LandviewZoom?RuntimePostKind::ImageZoom:d.mode==OwnedImageMode::Opaque?RuntimePostKind::ImageOpaque:d.mode==OwnedImageMode::Composite?RuntimePostKind::ImageComposite
                    :d.coverage.Valid()?RuntimePostKind::ImageCoverage:RuntimePostKind::ImageTransparent;
                q.draw.phase=WorldOverlayPhase::FlatWorld;q.draw.depth_test=false;q.draw.mode=WorldOverlayMode::Sprite;
                q.draw.x0=float(d.x);q.draw.y0=float(d.y);q.draw.x1=float(int64_t(d.x)+d.width);q.draw.y1=float(int64_t(d.y)+d.height);
                q.draw.u1=q.draw.v1=1;q.draw.source=q.draw.colour_row=0;
                q.source=Get(d.source,RuntimeAssetKind::Indexed8);q.colour_row=Get(d.palette,RuntimeAssetKind::PaletteRGBA8);
                if(d.coverage.Valid())q.coverage=Get(d.coverage,RuntimeAssetKind::Indexed8);
                out.post_composition.push_back(std::move(q));
            }
            for(const auto& d:p.Quads())if((d.parchment_capture||PostQuad(d.phase))&&PostPhase(d)==phase) {
                if(d.x0==d.x1||d.y0==d.y1)continue;
                RuntimePostDraw q;q.phase=phase;auto& v=q.draw;
                const bool opaque=d.mode==OwnedQuadMode::OpaqueIndexed;
                q.kind=opaque?RuntimePostKind::QuadOpaque:RuntimePostKind::Quad;
                v.phase=phase==RuntimePostPhase::ParchmentWorld?WorldOverlayPhase::DepthWorld:WorldOverlayPhase::FlatWorld;
                v.mode=opaque?WorldOverlayMode::Sprite:WorldOverlayMode(d.mode);v.depth_test=d.depth_test;
                v.scissor_enabled=d.scissor_enabled;v.scissor_x=d.scissor_x;v.scissor_y=d.scissor_y;v.scissor_w=d.scissor_w;v.scissor_h=d.scissor_h;
                v.x0=d.x0;v.y0=d.y0;v.x1=d.x1;v.y1=d.y1;v.u0=d.u0;v.v0=d.v0;v.u1=d.u1;v.v1=d.v1;v.z_ndc=d.z_ndc;v.colour=d.colour;v.fade_row=d.fade_row;
                if(d.mode!=OwnedQuadMode::Solid){q.source=Get(d.source,RuntimeAssetKind::Indexed8);v.source=0;}
                if(RowQuad(d.mode)){q.colour_row=Get(d.colour_row,RuntimeAssetKind::ColourRowRGBA8);v.colour_row=0;}
                out.post_composition.push_back(std::move(q));
            }
        }
        ValidateRuntimeWorldFrame(out);frame=std::move(out);prepared=true;
    }
};
OwnedWorldBridge::OwnedWorldBridge(Device& d,OwnedWorldBridgeLimits l) {
    if(!l.mapping_slots||l.mapping_slots>512||!l.first_logical_id||l.first_logical_id>=UINT64_MAX-l.mapping_slots)
        throw std::invalid_argument("Invalid owned world bridge namespace or capacity");
    impl_=std::make_unique<Impl>(d,l);
}
OwnedWorldBridge::~OwnedWorldBridge()=default;
void OwnedWorldBridge::Abandon() noexcept {
    auto& r=*impl_;r.frame={};r.prepared=false;r.pending.reset();r.needs.clear();std::fill(r.need_lookup.begin(),r.need_lookup.end(),0);for(auto& s:r.slots)s.selected=false;
}
void OwnedWorldBridge::TrimUnselected() {
    auto& r=*impl_;r.CheckThread();for(auto& s:r.slots)if(!s.selected)s.runtime.Reset();r.device.CollectRuntimeResources(false);
}
OwnedWorldPrepareStatus OwnedWorldBridge::Prepare(const OwnedFrameLease& p,bool full_frame) {
    auto& r=*impl_;r.CheckThread();
    if(r.pending&&(r.pending!=p||r.full_frame!=full_frame))return OwnedWorldPrepareStatus::PendingPacket;
    if(!p||p->Status()==OwnedFrameStatus::InvalidInput||p->Status()==OwnedFrameStatus::CommandLimit||p->Status()==OwnedFrameStatus::WouldBlock)return OwnedWorldPrepareStatus::InvalidInput;
    for(auto phase:{OwnedFramePhase::Clear,OwnedFramePhase::BeginWorldCapture,OwnedFramePhase::World,OwnedFramePhase::WorldSprites,OwnedFramePhase::WorldFlatOverlay,OwnedFramePhase::Swipe,OwnedFramePhase::ResolveWorldCapture,OwnedFramePhase::LensPaletteUIExclusion})
        if(!Covered(*p,phase))return OwnedWorldPrepareStatus::Unsupported;
    if(full_frame) {
        if(!p->Complete())return OwnedWorldPrepareStatus::Unsupported;
        for(auto phase:{OwnedFramePhase::CaptureMapFadeWorld,OwnedFramePhase::OverheadMap,OwnedFramePhase::ResolveMapFade,OwnedFramePhase::ZoomBoxes})
            if(!Covered(*p,phase))return OwnedWorldPrepareStatus::Unsupported;
        for(auto phase:{OwnedFramePhase::PiPCaptures,OwnedFramePhase::DevTools,OwnedFramePhase::Screenshot,OwnedFramePhase::ScrgbLift})
            if(p->Phase(phase)!=OwnedPhaseState::Empty)return OwnedWorldPrepareStatus::Unsupported;
        for(const auto& q:p->Quads())if(!WorldQuad(q.phase)&&!PostQuad(q.phase))return OwnedWorldPrepareStatus::Unsupported;
        for(const auto& d:p->Images())if(d.phase!=OwnedFramePhase::ImagePresents&&d.phase!=OwnedFramePhase::ImagePresentOverlay)return OwnedWorldPrepareStatus::Unsupported;
        for(const auto& d:p->Sprites())if(d.phase!=OwnedFramePhase::WorldSprites&&d.phase!=OwnedFramePhase::Cursor)return OwnedWorldPrepareStatus::Unsupported;
    }
    const auto epoch=r.device.GetRuntimeResourceDiagnostics().epoch;
    if(epoch!=p->Info().epoch)return OwnedWorldPrepareStatus::InvalidInput;
    if(r.epoch!=epoch){Abandon();for(auto& s:r.slots)s=Impl::Slot{};r.epoch=epoch;}
    if(r.prepared&&std::all_of(r.needs.begin(),r.needs.end(),[&](const Impl::Need& n){return r.slots[n.slot].runtime.Valid();}))
        return OwnedWorldPrepareStatus::Ready;
    if(!r.pending) {
        r.full_frame=full_frame;
        try {r.Gather(*p);if(!r.Select()){Abandon();return OwnedWorldPrepareStatus::WouldBlock;}r.pending=p;}
        catch(const std::invalid_argument&){Abandon();return OwnedWorldPrepareStatus::InvalidInput;}
        catch(const std::length_error&){Abandon();return OwnedWorldPrepareStatus::InvalidInput;}
        catch(...){Abandon();throw;}
    }
    r.prepared=false;r.frame={};
    for(auto& n:r.needs) {
        auto& s=r.slots[n.slot];if(s.runtime.Valid()){++r.counters.cache_hits;continue;}
        if(s.revision>=UINT64_MAX-1)throw std::overflow_error("Bridge residency revision exhausted");
        const RuntimeAssetKey key{epoch,r.limits.first_logical_id+n.slot,s.generation,++s.revision,s.kind};
        const auto d=n.owned.Desc();
        try {s.runtime=r.device.PublishRuntimeAsset(key,{d.width,d.height},n.owned.Bytes());++r.counters.publications;}
        catch(const std::length_error&){++r.counters.publication_blocks;TrimUnselected();return OwnedWorldPrepareStatus::WouldBlock;}
    }
    r.Lower();return OwnedWorldPrepareStatus::Ready;
}
OwnedWorldPrepareStatus OwnedWorldBridge::PrepareWorldFragment(const OwnedFrameLease& p){return Prepare(p,false);}
OwnedWorldPrepareStatus OwnedWorldBridge::PrepareFrame(const OwnedFrameLease& p){return Prepare(p,true);}
const RuntimeWorldFrame* OwnedWorldBridge::PreparedWorldFragment() const noexcept {return impl_->prepared&&!impl_->full_frame?&impl_->frame:nullptr;}
const RuntimeWorldFrame* OwnedWorldBridge::PreparedFrame() const noexcept {return impl_->prepared&&impl_->full_frame?&impl_->frame:nullptr;}
RuntimeSubmitResult OwnedWorldBridge::Submit(bool readback,bool full_frame) {
    auto& r=*impl_;r.CheckThread();if(!r.pending||r.full_frame!=full_frame)throw std::logic_error("No pending owned packet for this submission mode");
    const auto status=Prepare(r.pending,full_frame);if(status!=OwnedWorldPrepareStatus::Ready)return {};
    auto result=r.device.TrySubmitRuntimeWorldFrame(r.frame,readback);
    if(result.status==RuntimeSubmitStatus::WouldBlock&&result.pressure!=RuntimePressureReason::None
        &&result.pressure!=RuntimePressureReason::FrameSlot) {
        // Cache entries not selected by this immutable packet are expendable.
        // Device retains every accepted GPU owner and bounds its own retry.
        TrimUnselected();
    }
    if(result.status==RuntimeSubmitStatus::Submitted)Abandon();
    return result;
}
RuntimeSubmitResult OwnedWorldBridge::TrySubmitPreparedWorldFragment(bool readback){return Submit(readback,false);}
RuntimeSubmitResult OwnedWorldBridge::TrySubmitPreparedFrame(bool readback){return Submit(readback,true);}
OwnedWorldBridgeStats OwnedWorldBridge::Stats() const noexcept {
    const auto& r=*impl_;auto s=r.counters;s.pending=bool(r.pending);s.prepared=r.prepared;
    for(const auto& slot:r.slots){s.occupied_slots+=slot.has_content;s.selected_slots+=slot.selected;}return s;
}
}
