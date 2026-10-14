// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
// Owned, backend-neutral assets for a frozen world scene. No engine pointers.
#pragma once
#include "kfx/renderer/WorldScene.h"
#include "kfx/renderer/SpriteScale.h"
#include "kfx/renderer/SceneRasterTrace.h"
#include "kfx/renderer/SceneOutlineTrace.h"
#include "kfx/renderer/SceneGLWorldTrace.h"
#include "kfx/renderer/SceneGLCompositionTrace.h"
#include <filesystem>
#include <span>
#include <string>

namespace kfx::render {
struct SceneSprite {
    uint32_t id = 0; // Frame-resolved ID, scoped to this capture's epoch.
    uint16_t width = 0, height = 0, frame_width = 0, frame_height = 0;
    int16_t frame_x = 0, frame_y = 0, offset_x = 0, offset_y = 0;
    int16_t shadow_offset = 0, flags = 0;
    // Interleaved palette index and coverage. Opaque index zero is NOT a hole.
    std::vector<uint8_t> pixels;
};
struct SceneSpriteBinding {
    uint16_t animation = 0;
    uint8_t frame = 0, direction = 0; // direction * 256, before the legacy +128 rounding.
    uint32_t sprite = 0; // Index in sprites, not a pointer or engine ID.
    bool flip = false;
    // Legacy placement uses the animation's first-frame origin, which can
    // differ from the selected frame's own cropping rectangle.
    uint16_t origin_width = 0, origin_height = 0;
    int16_t origin_x = 0, origin_y = 0;
};
struct SceneSpritePresentation {
    SceneObjectId id;
    uint16_t lightness = 8192;
    int16_t angle_offset = 0;
    bool rotates = false;
};
struct SceneProjectionReference {
    ScenePosition position;
    int32_t screen_x = 0, screen_y = 0;
    bool visible = false;
};
enum class SceneSpriteBlend : uint8_t { Opaque, Remap, GhostSourceHigh, GhostDestinationHigh, Additive };
inline constexpr uint8_t SceneOrdinarySpriteKind = 2; // Stable QK_JontySprite wire value.
inline constexpr uint8_t SceneSpinningKeySpriteKind = 9; // v6: QK_JontyISOSprite.
// Raw draw-state flags are retained as evidence; this is software precedence.
SceneSpriteBlend ResolveSceneSpriteBlend(uint32_t flags, bool alpha);
struct SceneBucketEntry {
    int32_t bucket = 0;
    uint8_t kind = 0;
};
struct SceneSpriteDraw {
    SceneObjectId object;
    uint32_t entry = 0, sprite = 0;
    SpriteScale scale{};
    int32_t content_height = 0;
    uint32_t flags = 0;
    bool alpha = false, has_remap = false;
    SceneSpriteBlend blend = SceneSpriteBlend::Opaque;
    std::array<uint8_t, 256> remap{};
};
struct SceneDrawTrace {
    bool complete = false;
    uint8_t backend = 0; // 0 = software, 1 = OpenGL producer command walk.
    uint32_t excluded_sprite_calls = 0;
    // Actual painter traversal order, including excluded casters and UI.
    // This is not a complete world command list.
    std::vector<SceneBucketEntry> entries;
    // Actual submission order for allowlisted ordinary objects and v6 spinning keys, including
    // composite sprite layers, effective remaps, flips and source-row clipping.
    std::vector<SceneSpriteDraw> draws;
    // v4 adds final terrain triangles; v5 adds allowlisted creature shadows,
    // interleaved with sprite draws in software submission order. v6 retains
    // the v5 wire layout and adds the spinning-key sprite producer contract.
    SceneRasterTrace raster;
    // v7: optional executed OpenGL outline/RGBA phases; not software triangles.
    SceneOutlineTrace outline;
    SceneGLWorldTrace gl_world; // v10: executed GL terrain, shadows and actual target inputs.
    SceneGLCompositionTrace gl_composition; // v11: world overlays, swipe and actual lens resolution.
};
struct SceneAssets {
    // Original 6-bit RGB and current display palette; preserve original shading.
    std::array<uint8_t, 768> base_palette{}, display_palette{};
    std::array<uint8_t, 64 * 256> fade{};
    std::array<uint8_t, 256 * 256> ghost{};
    // Immutable copy of the texture banks. WorldScene offsets/stride address it.
    // Includes all loaded variations and static frames, including texture packs.
    std::vector<uint8_t> texture_pixels;
    std::vector<SceneSprite> sprites;
    std::vector<SceneSpriteBinding> bindings;
    // v2: resolved on the producer, never looked up through live game state.
    std::vector<SceneSpritePresentation> presentation;
    // Diagnostic output of the original projector, before terrain wibble.
    std::vector<SceneProjectionReference> projection_reference;
    // v3: the actual 65536-byte additive lookup table, not regenerated RGB.
    std::vector<uint8_t> additive;
};
enum class SceneIndexedTargetKind : uint8_t { Unknown = 0, Screen = 1, LensSource = 2 };
// v8: proven software clear and the initialized source target, before world
// drawing. The lens source is pre-composite; camera.lens is not its identity.
// Width is logical pixels, pitch is bytes. No padding pixels are owned here.
struct SceneIndexedTarget {
    SceneIndexedTargetKind kind = SceneIndexedTargetKind::Unknown;
    uint8_t clear_index = 0;
    uint16_t lens_id = 0; // Screen: 0. LensSource: actual nonzero applied lens (1..255).
    uint32_t width = 0, height = 0, pitch = 0;
    int32_t window_x = 0, window_y = 0, window_w = 0, window_h = 0;
    bool operator==(const SceneIndexedTarget&) const = default;
};
struct SceneCapture {
    uint32_t format_version = 2;
    WorldScene scene;
    SceneAssets assets;
    SceneDrawTrace trace; // v7/v9 GL captures wait for the executed render pass.
    SceneIndexedTarget target; // Mandatory software-only v8; canonical zero in GL v9.
};
// Returned handles expose no mutable asset state. GPU consumers never resolve
// these IDs through live Game, block_ptrs or the sprite heap.
using SceneCaptureHandle = std::shared_ptr<const SceneCapture>;

bool DecodeSceneSprite(std::span<const uint8_t> compressed, uint16_t width, uint16_t height,
    std::vector<uint8_t>& pixels);
// Optional static diagnostic reason; populated on rejection, cleared on success.
bool ValidateSceneCapture(const SceneCapture& capture, const char** failure = nullptr);
std::vector<uint8_t> EncodeSceneCapture(const SceneCapture& capture);
SceneCaptureHandle DecodeSceneCapture(std::span<const uint8_t> bytes); // throws on invalid input
SceneCaptureHandle LoadSceneCapture(const std::filesystem::path& path);
void SaveSceneCapture(const std::filesystem::path& path, const SceneCapture& capture);
// Deterministic contact sheet for offline inspection; RGBA, no game/GPU access.
std::vector<uint8_t> SceneAssetContactSheet(const SceneCapture& capture, uint32_t& width, uint32_t& height);
}
