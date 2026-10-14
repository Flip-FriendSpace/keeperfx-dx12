// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#pragma once
#include <stdint.h>
struct SpriteScale;
struct PolyPoint;
#ifdef __cplusplus
extern "C" {
#endif
// Snapshot-only receipt for the next sealed frame; also requires KFX_SCENE_REFERENCE=1.
int WorldSceneRequestReceipt(void);
int WorldSceneRequestCapture(void); // Requires local KFX_SCENE_TEST_DIR opt-in.
// Producer-only, sidecar diagnostic. Count constructed clipping intersections,
// not near-polygon dispatches; no field is added to the binary capture.
void WorldSceneNearPlaneCaptureBegin(uint64_t epoch, uint64_t sequence);
void WorldSceneNearPlaneIntersection(void);
// Diagnostic clear proof, producer thread only; never reads target pixels.
// Screen hook follows a successful fill, lens hook follows its actual memset.
void WorldSceneSoftwareClear(const unsigned char* data, int width, int height, int pitch, unsigned char colour);
void WorldSceneSoftwareLensBegin(const unsigned char* data, int width, int height, unsigned int lens_id);
void WorldSceneSoftwareTargetEnd(void);
void WorldSceneTraceBegin(int backend);
void WorldSceneTraceBucket(int32_t bucket, unsigned int kind);
void WorldSceneTraceObject(uint32_t slot);
// Shadow bucket identity and successful decoder output. Failed decoding leaves
// no mask, so the subsequent included mode-10 submission fails the capture.
void WorldSceneTraceShadowBegin(uint32_t slot);
void WorldSceneTraceShadowMask(const unsigned char* data, int width, int height);
void WorldSceneTraceSprite(const struct SpriteScale* scale, const unsigned char* data,
    int src_w, int src_h, int32_t content_h, unsigned int flags,
    const unsigned char* remap, int32_t sprite_id, int alpha);
// Final software terrain/shadow arguments, before the rasterizer reorders points.
// rasterizer: 0 = draw_gpoly, 1 = trig. No-op outside an active software trace.
void WorldSceneTraceTriangle(unsigned int rasterizer, const struct PolyPoint* a,
    const struct PolyPoint* b, const struct PolyPoint* c, unsigned int mode,
    unsigned int colour, const unsigned char* texture);
void WorldSceneTraceEnd(void);
#ifdef __cplusplus
}
#include "kfx/renderer/SceneAssets.h"
#include "kfx/renderer/SceneGLCompositionTrace.h"
// Producer-thread only. Copy assets while the scene's engine storage is current.
std::shared_ptr<kfx::render::SceneCapture> CaptureWorldSceneAssets(const kfx::render::WorldScene& scene);
void WorldSceneProcessCapture(kfx::render::WorldScene& scene);
void WorldSceneCompleteCapture();
void WorldSceneDiscardDrawTrace();
void WorldSceneCancelCapture();
void WorldSceneRejectPendingCapture(const char* reason);
// Producer-only tags and handoff. The detached session owns all data and is
// subsequently used on the render thread only; no engine pointers cross over.
struct WorldSceneOutlineCapture;
using WorldSceneOutlineSession = std::shared_ptr<WorldSceneOutlineCapture>;
uint32_t WorldSceneOutlineSpriteTag() noexcept;
uint32_t WorldSceneOutlineEntryTag() noexcept;
uint32_t WorldSceneGLShadowTag(uint32_t slot, kfx::render::SceneObjectId& caster) noexcept;
void WorldSceneOutlineFailProducer(const char* reason) noexcept;
WorldSceneOutlineSession WorldSceneTakeOutlineCapture(uint64_t epoch, uint64_t sequence) noexcept;
// Render-thread-only, fail-closed diagnostics. Failures never alter rendering.
void WorldSceneBeginOutlineCapture(const WorldSceneOutlineSession&, uint8_t mode, float alpha,
    const std::array<float, 9>& palette_transform, const std::array<uint8_t, 768>& display_palette) noexcept;
void WorldSceneFailOutlineCapture(const WorldSceneOutlineSession&, const char* reason) noexcept;
void WorldSceneAppendOutlineOccluder(const WorldSceneOutlineSession&, const kfx::render::SceneOutlineOccluder&) noexcept;
void WorldSceneBeginGLWorldCapture(const WorldSceneOutlineSession&, kfx::render::SceneGLWorldTrace state) noexcept;
void WorldSceneAppendGLTerrain(const WorldSceneOutlineSession&, const kfx::render::SceneOutlineOccluder&,
    kfx::render::SceneGLTerrainTriangle, const std::array<uint8_t, 1024>* tile) noexcept;
void WorldSceneAppendGLShadow(const WorldSceneOutlineSession&, kfx::render::SceneGLShadow,
    const kfx::render::SceneGLShadowSource&) noexcept;
uint32_t WorldSceneAppendOutlineSprite(const WorldSceneOutlineSession&, const kfx::render::SceneOutlineSprite&,
    kfx::render::SceneOutlineColourResolution, const std::array<uint8_t, 1024>* effective_row) noexcept;
void WorldSceneAppendOutlineCommand(const WorldSceneOutlineSession&, const kfx::render::SceneOutlineCommand&) noexcept;
void WorldSceneFinishOutlineCapture(const WorldSceneOutlineSession&) noexcept;
void WorldSceneBeginCompositionCapture(const WorldSceneOutlineSession&, uint32_t width, uint32_t height,
    const std::array<float, 4>& clear) noexcept;
void WorldSceneAppendOverlayQuad(const WorldSceneOutlineSession&, kfx::render::SceneGLOverlayQuad,
    const kfx::render::SceneGLOverlaySource*, const std::array<uint8_t, 1024>*, const uint8_t* fade) noexcept;
void WorldSceneCompleteOverlayPhase(const WorldSceneOutlineSession&, kfx::render::SceneGLOverlayPhase) noexcept;
void WorldSceneRecordLensCapture(const WorldSceneOutlineSession&, kfx::render::SceneGLLensTrace) noexcept;
void WorldSceneFinishCompositionCapture(const WorldSceneOutlineSession&) noexcept;

// Bounded render-thread mirror of submitted texture bytes. Unknown/recreated
// storage is never inferred from a CPU request that was not uploaded.
struct WorldSceneGLUploadMirror {
    uint64_t handle = 0;
    uint32_t texture = 0, width = 0, height = 0;
    kfx::render::SceneGLUploadStamp stamp;
    std::vector<uint8_t> bytes;
    void Record(uint64_t resource, uint32_t id, uint32_t w, uint32_t h,
        const void* data, size_t size) noexcept;
    bool Matches(uint64_t resource, uint32_t id, uint32_t w, uint32_t h) const noexcept
    { return handle == resource && texture == id && width == w && height == h
        && stamp.generation && stamp.revision && !bytes.empty(); }
};
#endif
