// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#include "pre_inc.h"
#include "kfx/renderer/WorldDrawProducer.h"
#include "kfx/renderer/RendererManager.h"
#include "engine_buckets.h"
#include "engine_render.h"
#include "player_data.h"
#include "local_camera.h"
#include "post_inc.h"

namespace kfx::render {
void DispatchWorldSprite(WorldSpriteDispatch mode, BucketKindJontySprite& sprite, Camera* camera)
{
    switch (mode) {
    case WorldSpriteDispatch::Isometric:
        draw_jonty_mapwho(&sprite);
        break;
    case WorldSpriteDispatch::IsometricOnly: {
        auto* active = get_local_active_camera(get_my_player());
        if (active && (active->view_mode == PVM_IsoWibbleView || active->view_mode == PVM_IsoStraightView))
            draw_jonty_mapwho(&sprite);
        break;
    }
    case WorldSpriteDispatch::Front:
        draw_fastview_mapwho(camera, &sprite);
        break;
    case WorldSpriteDispatch::FrontIsometricOnly:
        draw_iso_only_fastview_mapwho(camera, &sprite);
        break;
    }
}

void DispatchWorldOverlay(WorldOverlayDispatch kind, WorldOverlayDepth depth,
    float z_ndc, BasicQ& entry)
{
    const bool flat = depth == WorldOverlayDepth::Flat;
    if (flat) RendererSetWorldOverlayFlat(z_ndc);
    else RendererSetWorldOverlay(z_ndc);
    switch (kind) {
    case WorldOverlayDispatch::FlagPole:
        draw_engine_room_flagpole(reinterpret_cast<BucketKindRoomFlag*>(&entry));
        break;
    case WorldOverlayDispatch::FlagTop:
        draw_engine_room_flag_top(reinterpret_cast<BucketKindRoomFlag*>(&entry));
        break;
    case WorldOverlayDispatch::Number:
        draw_engine_number(reinterpret_cast<BucketKindFloatingGoldText*>(&entry));
        break;
    case WorldOverlayDispatch::Status: {
        const auto& status = *reinterpret_cast<const BucketKindCreatureStatus*>(&entry);
        draw_status_sprites(status.x, status.y, status.thing);
        break;
    }
    case WorldOverlayDispatch::Selector: {
        const auto& selector = *reinterpret_cast<const BucketKindSlabSelector*>(&entry);
        draw_clipped_line(selector.p.X, selector.p.Y, selector.p.U, selector.p.V, selector.p.S);
        break;
    }
    }
    if (flat) RendererClearWorldOverlayFlat();
    else RendererClearWorldOverlay();
}
}
