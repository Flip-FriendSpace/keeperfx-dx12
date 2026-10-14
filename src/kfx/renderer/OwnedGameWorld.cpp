// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#include "pre_inc.h"
#include "kfx/renderer/OwnedGameWorld.h"
#include "kfx/renderer/OwnedWorldOverlayCapture.h"
#include "kfx/renderer/RendererThread.h"
#include "kfx/renderer/ir/WorldCommands.h"
#include "engine_buckets.h"
#include "engine_render.h"
#include "bflib_render.h"
#include "bflib_vidraw.h"
#include "bflib_video.h"
#include "vidmode.h"
#include "front_simple.h"
#include "game_legacy.h"
#include <algorithm>
#include <stdexcept>
#include "post_inc.h"

namespace kfx::render {
OwnedGameWorld::OwnedGameWorld(OwnedRenderAssets& assets)
    : assets_(assets), globals_(assets,4), lenses_(assets) {}

void OwnedGameWorld::SetDisplayPalette(std::span<const uint8_t> rgb8)
{
    ASSERT_GAME_THREAD();
    if (rgb8.size()!=768) throw std::invalid_argument("Invalid owned display palette");
    for (size_t i=0;i<256;++i) {
        std::copy_n(rgb8.data()+i*3,3,display_palette_.data()+i*4);
        display_palette_[i*4+3]=255;
    }
    have_display_palette_=true;
}

void OwnedGameWorld::SetSpritePalette(std::span<const uint8_t> rgb6)
{
    ASSERT_GAME_THREAD();
    if (rgb6.size()!=768 || std::any_of(rgb6.begin(),rgb6.end(),[](uint8_t value){return value>63;}))
        throw std::invalid_argument("Invalid owned sprite palette");
    std::copy(rgb6.begin(),rgb6.end(),sprite_palette_.begin());
    have_sprite_palette_=true;
}

void OwnedGameWorld::BeginView(int width,int height)
{
    ASSERT_GAME_THREAD();
    if (width<=0 || height<=0) throw std::invalid_argument("Invalid owned world viewport");
    // Same projection globals as the existing GPU BeginWorldPass. No software
    // framebuffer or raster destination is installed.
    vec_window_width=width; vec_window_height=height;
}

bool OwnedGameWorld::PrepareFrameSources(OwnedFrameBuilder& builder)
{
    ASSERT_GAME_THREAD();
    if (!builder) return false;
    lenses_.ResetEpoch(builder.Info().epoch);
    frame_epoch_=frame_sequence_=0;
    frame_palette_={}; frame_fade_={};
    frame_draw_settings_={};
    if (!have_display_palette_ || !have_sprite_palette_) {
        builder.Fail(OwnedFrameStatus::Incomplete); return false;
    }
    OwnedWorldSourceInputs inputs;
    inputs.epoch=builder.Info().epoch;
    inputs.display_palette_rgba=display_palette_;
    inputs.sprite_palette_rgb6=sprite_palette_;
    inputs.base_palette_rgb6={engine_palette,768};
    inputs.fade=pixmap.fade_tables;
    inputs.lightmap=game.lish.subtile_lightness;
    inputs.lightmap_width=uint32_t(std::max(1,int(game.map_subtiles_x)+1));
    inputs.lightmap_height=uint32_t(std::max(1,int(game.map_subtiles_y)+1));
    inputs.settings=g_renderer_settings;
    const auto snapshot=globals_.Update(inputs);
    if (!snapshot) {
        builder.Fail(snapshot.status==OwnedAssetStatus::WouldBlock
            ? OwnedFrameStatus::WouldBlock : OwnedFrameStatus::InvalidInput);
        return false;
    }
    const auto& state=snapshot.state;
    if (!builder.SetWorldAssets(state.palette,state.fade,state.lightmap)) return false;
    // The existing world shader's time uniform is initialized to zero and is
    // never advanced. Preserve that behavior until animation changes are agreed.
    if (!builder.SetWorldState(state.shade_settings,state.palette_transform,0)) return false;
    frame_palette_=state.palette; frame_fade_=state.fade;
    std::copy_n(engine_palette,frame_base_palette_.size(),frame_base_palette_.begin());
    frame_epoch_=builder.Info().epoch; frame_sequence_=builder.Info().sequence;

    OwnedWorldDrawSettings settings;
    settings.palette=state.sprite_palette;
    settings.ui_palette=state.palette;
    settings.bucket_count=BUCKETS_COUNT;
    settings.outline_mode=uint8_t(state.settings.creature_outline_mode);
    settings.outline_alpha=state.settings.creature_outline_alpha;
    frame_draw_settings_=settings;
    return true;
}

bool OwnedGameWorld::Populate(OwnedFrameBuilder& builder,WorldDrawView view,Camera* camera)
{
    ASSERT_GAME_THREAD();
    if (!PrepareFrameSources(builder)) return false;
    render_fade_tables=pixmap.fade_tables;
    render_ghost=pixmap.ghost;
    render_alpha=reinterpret_cast<unsigned char*>(&alpha_sprite_table);
    OwnedWorldDrawSink sink(builder,assets_,sprites_,frame_draw_settings_);
    return sink.Produce(view,buckets,
        {int(builder.Info().viewport_width),int(builder.Info().viewport_height),BUCKETS_COUNT},
        pixel_size,camera);
}

bool OwnedGameWorld::PopulateLens(OwnedFrameBuilder& builder,const IRWorldLensCmd& command)
{
    ASSERT_GAME_THREAD();
    if (!builder) return false;
    if (!command.active) {
        builder.FinishPhase(OwnedFramePhase::ResolveWorldCapture);
        return true;
    }
    if (frame_epoch_!=builder.Info().epoch || frame_sequence_!=builder.Info().sequence) {
        builder.Fail(OwnedFrameStatus::Incomplete);
        return false;
    }
    OwnedWorldLensInputs input;
    input.epoch=builder.Info().epoch;
    input.native_width=builder.Info().native_width;
    input.native_height=builder.Info().native_height;
    input.evaluated=&command;
    input.display_palette=frame_palette_; input.fade=frame_fade_;
    input.base_palette_rgb6=frame_base_palette_;
    input.clear_rgba=builder.Info().clear_rgba;
    auto result=lenses_.Update(input);
    if (!result) {
        builder.Fail(result.status==OwnedAssetStatus::WouldBlock
            ? OwnedFrameStatus::WouldBlock : OwnedFrameStatus::InvalidInput);
        return false;
    }
    result.lens.display_clear=true;
    builder.SetLens(std::move(result.lens));
    return true;
}

std::unique_ptr<OwnedWorldOverlayCapture> OwnedGameWorld::BeginSwipe(OwnedFrameBuilder& builder)
{
    ASSERT_GAME_THREAD();
    if (!builder) return {};
    if (frame_epoch_!=builder.Info().epoch || frame_sequence_!=builder.Info().sequence) {
        builder.Fail(OwnedFrameStatus::Incomplete);
        return {};
    }
    auto capture=std::make_unique<OwnedWorldOverlayCapture>(builder,assets_,sprites_,frame_draw_settings_);
    if (!capture->Active()) return {};
    return capture;
}
}
