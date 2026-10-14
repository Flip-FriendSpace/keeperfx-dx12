// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#include "pre_inc.h"
#include "kfx/renderer/WorldDrawProducer.h"
#include "engine_buckets.h"
#include "post_inc.h"

namespace kfx::render {
void ProduceWorldDraws(WorldDrawView view, std::span<BasicQ* const> source,
    const WorldDrawProjection& projection, int pixel_scale, Camera* camera,
    WorldDrawSink& sink)
{
    const bool front = view == WorldDrawView::Front;
    // ISO deliberately omits bucket zero; front view includes it.
    for (int bucket = (int)source.size() - 1; bucket >= (front ? 0 : 1); --bucket) {
        sink.BeginBucket(bucket);
        for (BasicQ* entry = source[bucket]; entry; entry = entry->next) {
            sink.ObserveEntry(bucket, *entry);
            const float overlay_depth = WorldBucketDepth((float)bucket - 0.5f, projection.bucket_count);
            switch (entry->kind) {
            case QK_PolygonStandard: {
                const auto& polygon = *reinterpret_cast<const BucketKindPolygonStandard*>(entry);
                sink.Triangle(BuildWorldDrawTriangle(projection, bucket, polygon.block,
                    polygon.vertex_first, polygon.vertex_second, polygon.vertex_third, polygon.lightmap));
                break;
            }
            case QK_PolygonNearFP: {
                const auto& polygon = *reinterpret_cast<const BucketKindPolygonNearFP*>(entry);
                sink.Triangle(BuildWorldDrawTriangle(projection, bucket, polygon.block,
                    polygon.vertex_first, polygon.vertex_second, polygon.vertex_third));
                break;
            }
            case QK_TextureQuad:
                if (front) {
                    const auto& quad = *reinterpret_cast<const BucketKindTexturedQuad*>(entry);
                    const auto orientation = quad.orient;
                    const WorldQuadOrientation mapping{{orient_to_mapU1[orientation], orient_to_mapU2[orientation],
                        orient_to_mapU3[orientation], orient_to_mapU4[orientation]},
                        {orient_to_mapV1[orientation], orient_to_mapV2[orientation],
                        orient_to_mapV3[orientation], orient_to_mapV4[orientation]}};
                    const auto triangles = BuildWorldFrontQuad(projection, bucket, pixel_scale, quad, mapping);
                    sink.Triangle(triangles[0]);
                    sink.Triangle(triangles[1]);
                }
                break;
            case QK_JontySprite:
            case QK_JontyISOSprite: {
                const bool iso_only = entry->kind == QK_JontyISOSprite;
                const auto mode = front
                    ? (iso_only ? WorldSpriteDispatch::FrontIsometricOnly : WorldSpriteDispatch::Front)
                    : (iso_only ? WorldSpriteDispatch::IsometricOnly : WorldSpriteDispatch::Isometric);
                sink.Sprite(mode, *reinterpret_cast<BucketKindJontySprite*>(entry), camera);
                break;
            }
            case QK_CreatureShadow:
                if (!front) sink.Shadow(*reinterpret_cast<const BucketKindCreatureShadow*>(entry));
                break;
            case QK_RoomFlagBottomPole:
                sink.Overlay(WorldOverlayDispatch::FlagPole, WorldOverlayDepth::Flat, overlay_depth, *entry);
                break;
            case QK_RoomFlagStatusBox:
                sink.Overlay(WorldOverlayDispatch::FlagTop, WorldOverlayDepth::Flat, overlay_depth, *entry);
                break;
            case QK_FloatingGoldText:
                sink.Overlay(WorldOverlayDispatch::Number, WorldOverlayDepth::Flat, overlay_depth, *entry);
                break;
            case QK_CreatureStatus:
                sink.Overlay(WorldOverlayDispatch::Status, front ? WorldOverlayDepth::Flat : WorldOverlayDepth::Tested,
                    overlay_depth, *entry);
                break;
            case QK_SlabSelector:
                sink.Overlay(WorldOverlayDispatch::Selector, WorldOverlayDepth::Tested,
                    front ? overlay_depth : WorldBucketDepth(((float)bucket + 2.0f) - 0.5f, projection.bucket_count), *entry);
                break;
            default: break;
            }
        }
    }
}
}
