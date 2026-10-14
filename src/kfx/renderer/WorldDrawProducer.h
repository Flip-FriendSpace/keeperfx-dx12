// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#pragma once
#include "kfx/renderer/WorldDrawMath.h"
#include <span>

struct BasicQ;
struct BucketKindJontySprite;
struct BucketKindCreatureShadow;
struct Camera;

namespace kfx::render {
enum class WorldDrawView { Isometric, Front };
enum class WorldSpriteDispatch { Isometric, IsometricOnly, Front, FrontIsometricOnly };
enum class WorldOverlayDispatch { FlagPole, FlagTop, Number, Status, Selector };
enum class WorldOverlayDepth { Tested, Flat };

// Synchronous game-thread source view: pointers remain borrowed only for the
// duration of each call. An owned-packet sink must resolve/copy before returning.
// No GL resources, context, atlas packing, or capture input is required.
class WorldDrawSink {
public:
    virtual ~WorldDrawSink() = default;
    virtual void BeginBucket(int bucket) = 0;
    virtual void ObserveEntry(int, const BasicQ&) {}
    virtual void Triangle(const WorldDrawTriangle& vertices) = 0;
    virtual void Sprite(WorldSpriteDispatch mode, BucketKindJontySprite& sprite, Camera* camera) = 0;
    virtual void Shadow(const BucketKindCreatureShadow& shadow) = 0;
    virtual void Overlay(WorldOverlayDispatch kind, WorldOverlayDepth depth,
        float z_ndc, BasicQ& entry) = 0;
};
void ProduceWorldDraws(WorldDrawView view, std::span<BasicQ* const> buckets,
    const WorldDrawProjection& projection, int pixel_scale, Camera* camera,
    WorldDrawSink& sink);
// Run normal engine producers once, synchronously. Their renderer submissions
// go to the active backend, so an owned sink can reuse the same sprite scaling,
// concealment and UI decisions without constructing an OpenGL renderer.
void DispatchWorldSprite(WorldSpriteDispatch mode, BucketKindJontySprite& sprite, Camera* camera);
void DispatchWorldOverlay(WorldOverlayDispatch kind, WorldOverlayDepth depth,
    float z_ndc, BasicQ& entry);
}
