// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#pragma once
#include <stdint.h>

struct Camera;
struct M33;
struct Thing;
struct ThingInterpolateResult;
struct Packet;

#ifdef __cplusplus
extern "C" {
#endif
// All capture/lifecycle functions are game-thread only and do not modify Game.
void WorldSceneBegin(const struct Camera* camera, const struct M33* rotation, int front_view, int short_walls);
void WorldSceneCaptureCell(int32_t x, int32_t y);
void WorldSceneCaptureSurfaceMotion(float lava_scroll, float water_scroll);
void WorldSceneCaptureObject(const struct Thing* thing, const struct ThingInterpolateResult* position);
void WorldSceneReset(void);
// Renderer caches observe resets without changing simulation or capture state.
uint64_t WorldSceneEpoch(void);
void WorldSceneThingAllocated(uint32_t slot);
// Read-only diagnostic identity; zero means no established allocation identity.
uint64_t WorldSceneThingIncarnation(uint32_t slot);
void WorldSceneLightAllocated(uint32_t slot);
int WorldSceneCheck(void); // Opt-in local diagnostic; disabled without KFX_SCENE_TEST_DIR.
// Snapshot-only receipt for the next sealed frame; also requires KFX_SCENE_REFERENCE=1.
int WorldSceneRequestReceipt(void);
// Diagnostic-only local packet observation. Copy before dispatch; report only
// after processing completes, with its original scene epoch to reject reset crossings.
int WorldSceneReferenceInputEnabled(void);
void WorldSceneReferencePacketConsumed(const struct Packet* packet,uint64_t original_epoch);
int WorldSceneRequestCapture(void); // Captures the next sealed frame, not stale IDs.
#ifdef __cplusplus
}

#include <memory>
namespace kfx::render { struct WorldScene; }
std::shared_ptr<const kfx::render::WorldScene> WorldSceneSeal();
void WorldSceneValidateConsumed(const kfx::render::WorldScene& scene);
#endif
