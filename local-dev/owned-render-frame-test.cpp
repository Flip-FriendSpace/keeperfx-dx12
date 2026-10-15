// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#include "kfx/renderer/OwnedRenderFrame.h"
#include "kfx/renderer/PaletteIndexTable.h"
#include <cassert>
#include <iostream>
#include <thread>
#include <vector>
using namespace kfx::render;
static OwnedFrameInfo Info(uint64_t sequence) { OwnedFrameInfo i; i.epoch=1;i.sequence=sequence;i.native_width=i.native_height=i.viewport_width=i.viewport_height=64;return i; }
static void Finish(OwnedFrameBuilder& b) { for(unsigned i=0;i<unsigned(OwnedFramePhase::Count);++i)b.FinishPhase(OwnedFramePhase(i)); }
static OwnedAssetLease Asset(OwnedRenderAssets& c,uint64_t id,OwnedAssetFormat f,uint32_t w,uint32_t h,size_t bpp) {
    std::vector<uint8_t> bytes(size_t(w)*h*bpp,255);
    auto result=c.Publish({1,10,id,1,1},{{f,w,h},bytes,size_t(w)*bpp});assert(result);return result.lease;
}
int main()
{
    OwnedRenderAssets assets(1);
    auto tile=Asset(assets,1,OwnedAssetFormat::R8,32,32,1);
    auto palette=Asset(assets,2,OwnedAssetFormat::RGBA8,256,1,4);
    auto fade=Asset(assets,3,OwnedAssetFormat::R8,256,64,1);
    auto lightmap=Asset(assets,4,OwnedAssetFormat::R16LE,511,511,2);
    // Mixed-resolution terrain keeps the same projected world vertices.
    for(uint32_t size:{64u,128u,256u}) {
        OwnedFrameMailbox box({1,0,0,0,0,65536});auto b=box.Begin(Info(1000+size));
        WorldDrawTriangle triangle{};
        auto hd=Asset(assets,9000+size,OwnedAssetFormat::R8,size,size,1);
        assert(b.AddTerrain({triangle,hd}));assert(b.SetWorldAssets(palette,fade,lightmap));Finish(b);auto frame=b.Seal();
        assert(frame->Complete() && frame->Terrain()[0].tile.Desc().width==size);
    }
    { OwnedFrameMailbox box({1,0,0,0,0,65536});auto b=box.Begin(Info(2000));
      auto nonsquare=Asset(assets,10000,OwnedAssetFormat::R8,128,64,1);
      assert(!b.AddTerrain({{},nonsquare}) && b.Seal()->Status()==OwnedFrameStatus::InvalidInput); }
    OwnedFrameLease survives;
    {
        OwnedFrameMailbox box({2,4,2,2,2,65536});
        auto first=box.Begin(Info(1)); assert(first && !box.Begin(Info(2)));
        WorldDrawTriangle triangle{};for(auto& v:triangle)v.camera_z=1;
        assert(first.AddTerrain({triangle,tile}));assert(first.SetWorldAssets(palette,fade,lightmap));Finish(first);
        auto one=first.Seal();assert(one->Complete() && one->Terrain().size()==1 && one->Phase(OwnedFramePhase::World)==OwnedPhaseState::Recorded);
        assert(box.TryPublish(one)==OwnedFrameStatus::Ready);
        auto second=box.Begin(Info(2));Finish(second);auto two=second.Seal();
        assert(box.TryPublish(two)==OwnedFrameStatus::WouldBlock);
        auto front=box.TakePending();assert(front==one && !box.Stats().pending);
        auto third=box.Begin(Info(3));Finish(third);auto three=third.Seal();
        assert(box.Stats().occupied_slots==3 && !box.Begin(Info(4)));
        two.reset();
        {auto cancelled=box.Begin(Info(4));assert(cancelled);assert(cancelled.AddTerrain({triangle,tile}));}
        auto again=box.Begin(Info(5));assert(again);Finish(again);auto five=again.Seal();
        assert(five->Terrain().empty() && one->Terrain()[0].tile.Bytes()[0]==255);
        assert(box.TryPublish(one)==OwnedFrameStatus::Ready); // Explicit re-present of historical packet.
        std::thread consumer([&] { auto p=box.TakePending();assert(p==one && p->Info().sequence==1); });consumer.join();
        survives=one;
        OwnedFrameMailbox foreign({0,0,0,0,0,65536});assert(foreign.TryPublish(one)==OwnedFrameStatus::InvalidInput);
    }
    assert(survives->Terrain()[0].tile.Bytes()[0]==255);
    {
        OwnedFrameMailbox box({1,0,0,0,0,65536});
        auto b=box.Begin(Info(1));b.Unsupported(OwnedFramePhase::Text);Finish(b);auto p=b.Seal();
        assert(!p->Complete() && p->Phase(OwnedFramePhase::Text)==OwnedPhaseState::Unsupported);
        assert(box.TryPublish(p)==OwnedFrameStatus::Incomplete);
        auto unvisited=box.Begin(Info(2));auto partial=unvisited.Seal();assert(partial->Status()==OwnedFrameStatus::Incomplete);
        auto over=box.Begin(Info(3));WorldDrawTriangle t{};assert(over.AddTerrain({t,tile}));assert(!over.AddTerrain({t,tile}));
        auto failed=over.Seal();assert(failed->Status()==OwnedFrameStatus::CommandLimit);
    }
    {
        OwnedFrameMailbox box({1,2,0,2,2,65536});auto b=box.Begin(Info(10));
        assert(b.SetWorldViewport(4,8,32,40));assert(b.SetClearRGBA({0.1f,0.2f,0.3f,1}));
        OwnedWorldSprite cursor;cursor.phase=OwnedFramePhase::Cursor;cursor.source=Asset(assets,5,OwnedAssetFormat::RGBA8,2,2,4);
        cursor.source_width=cursor.source_height=cursor.content_height=2;cursor.width=cursor.height=2;
        cursor.step_x=cursor.step_y=1;cursor.colour_row=palette;
        assert(b.AddSprite(cursor));assert(b.SetWorldViewport(0,0,64,64));
        OwnedFrameImage image;image.mode=OwnedImageMode::Composite;image.source=tile;image.palette=palette;image.width=48;image.height=24;
        assert(b.AddImage(image));Finish(b);auto p=b.Seal();assert(p->Complete());
        assert(p->Images()[0].width==48 && p->Images()[0].source.Desc().width==32 && p->Sprites()[0].phase==OwnedFramePhase::Cursor);
        assert(p->Info().clear_rgba[2]==0.3f);
    }
    {
        OwnedFrameMailbox box({1,0,0,0,0,65536});auto b=box.Begin(Info(11));WorldDrawTriangle t{};
        assert(b.AddTerrain({t,tile}));assert(!b.SetWorldViewport(0,0,32,32));assert(b.Seal()->Status()==OwnedFrameStatus::InvalidInput);
    }
    {
        OwnedFrameMailbox box({0,2,0,0,0,65536});auto b=box.Begin(Info(12));
        OwnedWorldSprite glow;glow.phase=OwnedFramePhase::Cursor;glow.pass=OwnedSpritePass::GlowAdd;
        glow.source=assets.Find({1,10,5,1,1});glow.source_width=glow.source_height=glow.content_height=2;
        glow.width=glow.height=2;glow.step_x=glow.step_y=1;glow.source_alpha=1;
        assert(b.AddSprite(glow));glow.source_alpha=0.5f;assert(!b.AddSprite(glow));
        assert(b.Seal()->Status()==OwnedFrameStatus::InvalidInput);
    }
    {
        OwnedFrameMailbox box({1,2,0,8,2,262144});OwnedFramePaletteReplay replay(assets);
        auto b=box.Begin(Info(20));assert(b.SetWorldAssets(palette,fade,lightmap));assert(b.SetDisplayClear(6,palette));
        WorldDrawTriangle t{};assert(b.AddTerrain({t,tile}));
        OwnedFrameQuad solid;solid.mode=OwnedQuadMode::Solid;solid.x1=solid.y1=8;solid.palette_source=OwnedQuadPaletteSource::DisplayIndex;solid.palette_index=5;assert(b.AddQuad(solid));
        auto coloured=solid;coloured.mode=OwnedQuadMode::Colored;coloured.source=tile;coloured.palette_index=6;assert(b.AddQuad(coloured));
        OwnedFrameQuad mapped;mapped.mode=OwnedQuadMode::Clut;mapped.x1=mapped.y1=16;mapped.source=tile;mapped.colour_row=palette;mapped.palette_source=OwnedQuadPaletteSource::DisplayRemap;
        for(size_t i=0;i<256;++i)mapped.palette_remap[i]=uint8_t(255-i);
        assert(b.AddQuad(mapped));
        auto ordinary=mapped;ordinary.mode=OwnedQuadMode::Sprite;ordinary.palette_source=OwnedQuadPaletteSource::Display;assert(b.AddQuad(ordinary));
        OwnedFrameImage image;image.source=tile;image.palette=palette;image.width=image.height=32;image.uses_display_palette=true;assert(b.AddImage(image));
        image.uses_display_palette=false;assert(b.AddImage(image));
        OwnedWorldSprite cursor;cursor.phase=OwnedFramePhase::Cursor;cursor.source=assets.Find({1,10,5,1,1});cursor.source_width=cursor.source_height=cursor.content_height=2;cursor.width=cursor.height=2;cursor.colour_row=palette;assert(b.AddSprite(cursor));
        std::vector<uint8_t> base(512*512,42),lookup=base;std::array<uint32_t,256> keys{};ApplyPaletteIndexOverrides(lookup,palette.Bytes(),keys);
        auto base_asset=assets.Publish({1,11,1,1,1},{{OwnedAssetFormat::R8,512,512},base,512});assert(base_asset);
        auto lookup_asset=assets.Publish({1,11,2,1,1},{{OwnedAssetFormat::R8,512,512},lookup,512});assert(lookup_asset);
        OwnedFrameLens lens;lens.kind=OwnedLensKind::Mist;lens.palette=palette;lens.fade=fade;lens.rgb6=lookup_asset.lease;lens.rgb6_base=base_asset.lease;lens.density=Asset(assets,8,OwnedAssetFormat::R8,256,256,1);lens.viewport_width=lens.viewport_height=64;lens.display_clear=true;lens.clear_rgba=b.Info().clear_rgba;b.SetLens(lens);
        Finish(b);auto original=b.Seal();assert(original->Complete());
        std::array<uint8_t,1024> new_palette{};for(size_t i=0;i<256;++i)new_palette[i*4+3]=255;new_palette[5*4]=255;new_palette[6*4+2]=255;
        auto latest=assets.Publish({1,10,2,1,2},{{OwnedAssetFormat::RGBA8,256,1},new_palette,1024});assert(latest);
        auto next=box.Begin(Info(21));assert(next.ReplayDisplayPalette(*original,latest.lease,replay)==OwnedFrameStatus::Ready);auto shown=next.Seal();assert(shown->Complete());
        assert(shown->Terrain()[0].tile.Bytes().data()==original->Terrain()[0].tile.Bytes().data());
        assert(shown->Quads()[0].colour[0]==1 && shown->Quads()[0].colour[2]==0 && shown->Quads()[1].colour[0]==0 && shown->Quads()[1].colour[2]==1);
        assert(shown->Quads()[2].colour_row.Bytes()[(255-5)*4]==255 && shown->Quads()[2].palette_remap==mapped.palette_remap);
        assert(shown->Quads()[3].colour_row.Key()==latest.lease.Key() && shown->Images()[0].palette.Key()==latest.lease.Key() && shown->Images()[1].palette.Key()==palette.Key());
        assert(shown->Sprites()[0].colour_row.Key()==palette.Key() && shown->Fade().Key()==fade.Key() && shown->Lightmap().Key()==lightmap.Key());
        assert(shown->Info().palette_transform==original->Info().palette_transform && shown->Info().clear_rgba[2]==1 && shown->Info().clear_rgba[0]==0 && shown->Lens().clear_rgba==shown->Info().clear_rgba);
        assert(shown->Lens().rgb6.Bytes()[262143]==42 && shown->Lens().rgb6.Bytes()[0]==0 && original->Lens().rgb6.Bytes()[262143]==0);
        assert(original->Quads()[0].colour[2]==1 && original->Palette().Key()==palette.Key());
        auto again=box.Begin(Info(22));const auto before=assets.Stats().publications;assert(again.ReplayDisplayPalette(*original,latest.lease,replay)==OwnedFrameStatus::Ready);assert(assets.Stats().publications==before);
        again=OwnedFrameBuilder{};
        OwnedRenderAssets pressure_assets(1,{263168,8,4,16,16});
        std::array<uint8_t,1024> held_bytes{};
        auto held=pressure_assets.Publish({1,1,0,1,1},{{OwnedAssetFormat::R8,1024,1},held_bytes,1024});assert(held);
        OwnedFramePaletteReplay retry_rows(pressure_assets);auto retry=box.Begin(Info(23));
        assert(retry.EmptyDraws());assert(retry.ReplayDisplayPalette(*original,latest.lease,retry_rows)==OwnedFrameStatus::WouldBlock);
        assert(retry.EmptyDraws() && retry.Info().sequence==23 && retry.Info().clear_rgba[2]==0);
        const auto partial=pressure_assets.Stats().publications;held.lease={};pressure_assets.Collect(true);
        assert(retry.ReplayDisplayPalette(*original,latest.lease,retry_rows)==OwnedFrameStatus::Ready);
        assert(pressure_assets.Stats().publications==partial+1 && retry.Seal()->Complete());
        shown.reset();
        auto normal=box.Begin(Info(24));assert(normal && normal.AddQuad(solid) && normal.AddQuad(mapped));
        assert(normal.SetDisplayClear(6,palette));Finish(normal);
        const auto capacity=box.Stats().command_capacity_bytes;
        OwnedFrameLease cached;
        OwnedFramePaletteReplay blocked_rows(pressure_assets,321);
        assert(normal.ReplayCachedFrame(*original,latest.lease,blocked_rows,Info(25),cached)==OwnedFrameStatus::WouldBlock);
        assert(!cached && normal.Info().sequence==24 && box.Stats().builder_active && !box.Begin(Info(25)));
        assert(normal.ReplayCachedFrame(*original,latest.lease,replay,Info(25),cached)==OwnedFrameStatus::Ready);
        assert(cached->Complete() && cached->Info().sequence==25 && cached->Quads().size()==4);
        assert(normal && normal.Info().sequence==24 && box.Stats().builder_active && !box.Begin(Info(26)));
        assert(box.Stats().occupied_slots==3 && box.Stats().command_capacity_bytes==capacity);
        auto prior=cached;
        assert(normal.ReplayCachedFrame(*original,latest.lease,replay,Info(26),cached)==OwnedFrameStatus::WouldBlock && cached==prior);
        cached.reset();
        OwnedFrameLease blocked=prior;
        auto wrong=Info(26);wrong.epoch=2;
        assert(normal.ReplayCachedFrame(*original,latest.lease,replay,wrong,blocked)==OwnedFrameStatus::InvalidInput && blocked==prior);
        assert(box.Stats().builder_active && !box.Begin(Info(27)));
        blocked.reset();prior.reset();
        assert(normal.ReplayCachedFrame(*original,latest.lease,replay,Info(27),cached)==OwnedFrameStatus::Ready);
        assert(cached->Info().sequence==27 && box.Stats().builder_active);
        const auto publications=assets.Stats().publications;
        assert(normal.RefreshDisplayPalette(latest.lease,replay)==OwnedFrameStatus::Ready);
        assert(assets.Stats().publications==publications && normal.Info().sequence==24);
        assert(!normal.SetPresentationSequence(0) && !normal.SetPresentationSequence(UINT64_MAX) && !normal.SetPresentationSequence(23));
        assert(normal.Info().sequence==24 && normal.SetPresentationSequence(24) && normal.SetPresentationSequence(28));
        auto final=normal.Seal();assert(final->Complete() && final->Quads().size()==2 && !box.Stats().builder_active);
        assert(final->Info().sequence==28 && !normal.SetPresentationSequence(29));
        assert(final->Quads()[0].colour[0]==1 && final->Quads()[0].colour[2]==0 && final->Quads()[0].x1==solid.x1);
        assert(final->Quads()[1].colour_row.Key()==cached->Quads()[2].colour_row.Key() && final->Info().clear_rgba[2]==1);
        assert(final->Phase(solid.phase)==OwnedPhaseState::Recorded && original->Quads()[0].colour[2]==1);
        assert(pressure_assets.AdvanceEpoch(2));
        assert(pressure_assets.Stats().payload_bytes==263168);
        retry_rows.ResetEpoch(2);blocked_rows.ResetEpoch(2);pressure_assets.Collect();
        assert(pressure_assets.Stats().payload_bytes==0);
    }
    {
        OwnedFrameMailbox box({0,0,0,0,2,65536});auto b=box.Begin(Info(30));
        OwnedFrameImage zoom;zoom.mode=OwnedImageMode::LandviewZoom;zoom.width=zoom.height=64;
        zoom.source=tile;zoom.palette=palette;zoom.uses_display_palette=true;zoom.zoom={15.5f,16.5f,32,32,0.25f};
        assert(b.AddImage(zoom));zoom.zoom[4]=0;assert(b.AddImage(zoom)); // Latest scalar state replaces, including zero scale.
        Finish(b);auto p=b.Seal();assert(p->Complete() && p->Images().size()==1 && p->Images()[0].zoom[4]==0);
        OwnedFramePaletteReplay replay(assets);auto next=box.Begin(Info(31));
        auto display=assets.Find({1,10,2,1,2});assert(display.Valid());
        assert(next.ReplayDisplayPalette(*p,display,replay)==OwnedFrameStatus::Ready);
        auto shown=next.Seal();assert(shown->Images()[0].zoom==zoom.zoom && shown->Images()[0].source.Key()==tile.Key());
        assert(shown->Images()[0].palette.Key()==display.Key() && p->Images()[0].palette.Key()==palette.Key());
        auto invalid=box.Begin(Info(32));zoom.width=63;assert(!invalid.AddImage(zoom));
        assert(invalid.Seal()->Status()==OwnedFrameStatus::InvalidInput);
    }
    {
        OwnedFrameMailbox box({0,0,0,4,0,65536});auto b=box.Begin(Info(40));
        OwnedFrameMapFade map;map.active=map.capture_pending=true;map.transition_id=7;map.step=9.5f;map.warp_scale=2.5f;
        map.ghost_table=Asset(assets,50,OwnedAssetFormat::R8,256,256,1);
        map.rgb6=map.rgb6_base=Asset(assets,51,OwnedAssetFormat::R8,512,512,1);
        map.palette=palette;map.fade=fade;assert(b.SetMapFade(map));
        OwnedFrameQuad parchment;parchment.parchment_capture=true;parchment.phase=OwnedFramePhase::GameUI;
        parchment.mode=OwnedQuadMode::Solid;parchment.x1=parchment.y1=16;assert(b.AddQuad(parchment));
        OwnedFrameQuad zoom;zoom.phase=OwnedFramePhase::ZoomBoxes;zoom.mode=OwnedQuadMode::OpaqueIndexed;
        zoom.source=tile;zoom.colour_row=palette;zoom.palette_source=OwnedQuadPaletteSource::Display;
        zoom.x1=zoom.y1=32;zoom.u1=zoom.v1=1;assert(b.AddQuad(zoom));Finish(b);
        auto original=b.Seal();assert(original->Complete() && original->Quads().size()==2);
        auto display=assets.Find({1,10,2,1,2});assert(display.Valid());
        OwnedFramePaletteReplay replay(assets,322);auto next=box.Begin(Info(41));
        assert(next.ReplayDisplayPalette(*original,display,replay)==OwnedFrameStatus::Ready);
        auto shown=next.Seal();assert(shown->Complete() && shown->Quads().size()==1);
        assert(!shown->Quads()[0].parchment_capture && shown->Quads()[0].mode==OwnedQuadMode::OpaqueIndexed);
        assert(shown->MapFade().transition_id==7 && !shown->MapFade().capture_pending && shown->MapFade().step==9.5f);
        assert(shown->MapFade().palette.Key()==display.Key() && shown->MapFade().ghost_table.Key()==map.ghost_table.Key());
        assert(shown->MapFade().rgb6_base.Key()==map.rgb6_base.Key() && shown->Quads()[0].colour_row.Key()==display.Key());
        assert(original->MapFade().capture_pending && original->MapFade().palette.Key()==palette.Key() && original->Quads()[0].parchment_capture);
        shown.reset();auto repeated=box.Begin(Info(42));
        assert(repeated.AddQuad(parchment) && repeated.AddQuad(zoom));
        map.capture_pending=false;assert(repeated.SetMapFade(map));Finish(repeated);
        auto repeated_frame=repeated.Seal();assert(repeated_frame->Complete() && repeated_frame->Quads().size()==1);
        assert(!repeated_frame->Quads()[0].parchment_capture && repeated_frame->MapFade().transition_id==7);
        auto invalid=box.Begin(Info(43));map.transition_id=UINT64_MAX;assert(!invalid.SetMapFade(map));
        auto bad=invalid.Seal();assert(bad->Status()==OwnedFrameStatus::InvalidInput);bad.reset();
        invalid=box.Begin(Info(44));map.transition_id=0;assert(!invalid.SetMapFade(map));
        assert(invalid.Seal()->Status()==OwnedFrameStatus::InvalidInput);
    }
    bool refused=false;try {OwnedFrameMailbox impossible({1000,0,0,0,0,1});}catch(const std::invalid_argument&){refused=true;}assert(refused);
    std::cout<<"Owned frame ownership, backpressure, re-present, phase and capacity tests passed\n";
}
