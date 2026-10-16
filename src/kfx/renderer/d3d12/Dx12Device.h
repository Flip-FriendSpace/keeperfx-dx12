// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#include <span>
// Experimental Windows graphics foundation. No engine or simulation ownership.
#pragma once
#include "OutlinePass.h"
#include "EnhancedWorldPass.h"
#include "LightingRemaster.h"
#include "../SceneGLCompositionTrace.h"
#include "LensPass.h"
#include "WorldOverlayPass.h"
#include "RuntimeAssets.h"
#include "RuntimeWorld.h"
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace kfx::render::dx12 {
struct Vertex {
    float x, y, z; float u, v;
    // -1 selects ordinary RGBA; -2 selects screen-sampled sprite RGBA.
    // Terrain uses a nonnegative palette shade row
    // and an integer texel origin in its owned index/colour-table atlas.
    float shade = -1, tile_x = 0, tile_y = 0;
};
struct Image {
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> rgba;
};
enum class PaletteBlend : uint32_t { Opaque = 0, SourceHigh = 1, DestinationHigh = 2, Remap = 3, Additive = 4 };
struct PaletteQuad {
    int32_t x = 0, y = 0;
    uint32_t width = 0, height = 0;
    uint32_t texture_x = 0, texture_y = 0, texture_width = 0, texture_height = 0;
    PaletteBlend blend = PaletteBlend::Opaque;
    // Optional exact legacy destination-to-source axis maps. Atlas R/G encode
    // source coordinate + 1, little endian; zero marks an undrawn location.
    bool mapped = false;
    uint32_t map_x_row = 0, map_y_row = 0;
    uint32_t lookup_x = 0, lookup_y = 0;
};
struct PaletteTerrainVertex {
    float x = 0, y = 0; // Viewport-local pixel edges.
    float u = 0, v = 0, shade = 0; // Affine texel coordinates and fade row.
};
struct PaletteTriangle {
    std::array<PaletteTerrainVertex, 3> vertices{};
    uint32_t texture_x = 0, texture_y = 0; // Owned, densely packed index tile.
    uint32_t fade_x = 0, fade_y = 0; // Raw 256x64 index fade table.
    uint32_t texture_width = 32, texture_height = 32;
    bool apply_fade = true; // Raw 256x256 ramps also diagnose shadow sampling.
    bool clamp_to_vertices = false; // Shadow ramp probes use the same affine range guard.
};
struct PaletteShadowVertex {
    float x = 0, y = 0, u = 0, v = 0;
};
struct PaletteShadowTriangle {
    std::array<PaletteShadowVertex, 3> vertices{};
    uint32_t mask_x = 0, mask_y = 0, mask_width = 0, mask_height = 0;
    uint32_t fade_x = 0, fade_y = 0;
    uint8_t shade = 16;
};
enum class PalettePrimitiveKind : uint8_t { Sprite, Terrain, Shadow };
struct PalettePrimitive {
    PalettePrimitiveKind kind = PalettePrimitiveKind::Sprite;
    PaletteQuad sprite;
    PaletteTriangle terrain;
    PaletteShadowTriangle shadow;
};
struct Options {
    uint32_t lighting_remaster = 0; // LightingRemasterFlags; zero preserves established rendering.
    bool debug = true;
    bool gpu_validation = false;
    bool warp = false;
    bool vsync = true;
    bool cull_back_faces = false;
    std::wstring shader_directory;
    bool waitable_presentation = false; // Live engine admission; standalone callers opt in explicitly.
    bool presentation_diagnostics = false; // Opt-in endpoint queries; no per-frame output/statistics calls.
    uint32_t waitable_vsync_latency = 1; // Bounded1/2 control; engine default remains1, VSync OFF remains3.
    std::optional<uint64_t> hardware_adapter_luid; // Exact diagnostic selection; unset preserves normal adapter choice.
    bool resize_warning_trace = false; // Opt-in bounded ID1424 stack provenance; never suppresses validation.
};
// Endpoint observations, not scanout timestamps. HRESULTs remain available even
// when a query is unsupported, disjoint, or fails after device removal.
enum class PresentationQueryStatus : uint8_t { Disabled, Valid, Disjoint, Unsupported, Failed };
// Cumulative opt-in observations; bucket boundaries are milliseconds.
struct PresentationGapHistogram {
    uint64_t count = 0;
    std::array<uint64_t,4> buckets{}; // <2, [2,8), [8,20), >=20.
    double maximum_ms = 0;
};
struct PresentationDiagnostics {
    bool enabled = false;
    uint64_t present_ok = 0, present_occluded = 0, present_other = 0;
    int32_t last_present_hresult = 0;
    bool waitable = false, permit_held = false;
    uint64_t admission_polls = 0, admission_grants = 0, admission_not_ready = 0;
    PresentationGapHistogram pending_poll_gaps, grant_poll_gaps, pending_wait_durations;
    PresentationQueryStatus output_status = PresentationQueryStatus::Disabled;
    PresentationQueryStatus frame_statistics_status = PresentationQueryStatus::Disabled;
    PresentationQueryStatus last_count_status = PresentationQueryStatus::Disabled;
    int32_t output_hresult = 0, frame_statistics_hresult = 0, last_count_hresult = 0;
    std::string output_name;
    std::array<int32_t,4> desktop_rect{};
    bool attached_to_desktop = false, refresh_available = false;
    uint32_t refresh_hz = 0, last_present_count = 0;
    uint32_t present_count = 0, present_refresh_count = 0, sync_refresh_count = 0;
    int64_t sync_qpc_time = 0, sync_gpu_time = 0;
};
struct Diagnostics {
    std::string adapter;
    std::string runtime_path;
    uint32_t vendor_id = 0, device_id = 0;
    uint64_t adapter_luid = 0;
    uint32_t shader_model = 0;
    bool enhanced_barriers_supported = false;
    bool rasterizer_ordered_views_query_succeeded = false;
    bool rasterizer_ordered_views_supported = false;
    bool tearing_supported = false;
    bool debug_enabled = false;
    bool gpu_validation_enabled = false;
    uint64_t frames = 0, resizes = 0;
    // Successful frozen operations. The four-byte preparation status check
    // does not count as an image readback.
    uint64_t frozen_preparations = 0, frozen_presentations = 0, frozen_image_readbacks = 0;
    uint64_t allocation_bytes = 0, allocation_count = 0;
    uint64_t validation_errors = 0, validation_warnings = 0;
};

// An immutable, device-owned frozen result. Copies share ownership. In-flight
// presentations retain their own reference; Reset never retires a GPU reader.
// Device destruction invalidates every handle, including surviving copies.
class FrozenGpuFrame {
public:
    FrozenGpuFrame() = default;
    bool Valid() const noexcept;
    uint32_t Width() const noexcept;
    uint32_t Height() const noexcept;
    void Reset() noexcept { state.reset(); }
private:
    struct State;
    std::shared_ptr<State> state;
    friend class Device;
};

// Owns its device, frame resources and one uploaded textured mesh. The test host
// supplies a native HWND; the game can later supply one through its SDL adapter.
// Single render-thread API. Upload/resize are explicit GPU-idle boundaries;
// normal draws use three independently fenced command allocators.
class Device {
public:
    Device();
    ~Device();
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;
    void Initialize(void* native_window, uint32_t width, uint32_t height, const Options& options);
    void Upload(const std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices, const Image& texture);
    void ClearScene(); // Retires GPU readers before releasing mesh/texture storage.
    void Resize(uint32_t width, uint32_t height); // Zero size suspends drawing.
    // Poll before CPU production; repeated calls retain one permit until actual Present.
    // Never waits. An abandoned CPU packet does not discard its unused permit.
    bool TryAcquireRuntimePresentation();
    // Ends diagnostic timing continuity only; does not discard an admission permit.
    void ResetRuntimePresentationPollObservation() noexcept;
    void SetRuntimePresentation(bool vsync, int32_t x, int32_t y, uint32_t width, uint32_t height);
    bool Draw(const std::array<float, 16>& column_major_transform, Image* capture = nullptr);
    // Runtime slice1: actual indexed-world fallback, on this render thread.
    // Publication defensively copies bytes once; caller aliases cannot mutate
    // the lease. Equal keys require equal descriptions/bytes. Budgets throw
    // length_error before publication; malformed/stale keys throw invalid_argument.
    // After eviction an equal key rejects; the producer must advance revision.
    RuntimeAssetLease PublishRuntimeAsset(const RuntimeAssetKey&, const RuntimeAssetDesc&, std::span<const uint8_t>);
    RuntimeAssetLease PublishRuntimeAsset(const RuntimeAssetKey&, const RuntimeAssetDesc&,
        const std::shared_ptr<const std::vector<uint8_t>>& bytes);
    // Epochs/generations/revisions are nonzero, monotonic and never wrap. A new
    // version rejects subsequent use of older leases, but keeps accepted GPU
    // readers alive. Initial epoch is1; SetRuntimeEpoch requires an increase.
    void SetRuntimeEpoch(uint64_t epoch);
    void ConfigureRuntimeResources(const RuntimeResourceBudgets&); // Before first publication/submission.
    // Never consumes the packet and never waits for GPU completion. Slot or
    // resource pressure returns WouldBlock with its remaining constraint. One
    // automatic collection/replan may evict unleased cache entries and completed
    // frame storage; no caller lease or pending GPU reader is released. A
    // diagnostic readback, if requested,
    // belongs to the returned ticket independently of reusable frame surfaces.
    RuntimeSubmitResult TrySubmitRuntimeIndexedFrame(const RuntimeIndexedFrame&, bool diagnostic_readback = false);
    RuntimeSubmitResult TrySubmitRuntimeWorldFrame(const RuntimeWorldFrame&, bool diagnostic_readback = false);
    bool IsRuntimeFrameComplete(const RuntimeFrameTicket&);
    bool TryReadbackRuntimeFrame(const RuntimeFrameTicket&, Image& output); // false=pending; no wait.
    // Poll only: collect stale unleased versions and their page regions after
    // readers retire. true also evicts current unleased cache entries and
    // completed frame storage. Eviction keeps version tombstones.
    void CollectRuntimeResources(bool evict_unused = false);
    // Explicit GPU-idle teardown; invalidates all leases/tickets, starts epoch1
    // and permits Configure again. Cumulative counters remain available.
    void ClearRuntimeResources();
    RuntimeResourceDiagnostics GetRuntimeResourceDiagnostics();
    // Poll completed timing fences only; no GPU wait or diagnostic image copy.
    RuntimeTimingDiagnostics GetRuntimeTimingDiagnostics();
    void EnableRuntimeTimingCompletions(bool enabled);
    size_t TakeRuntimeTimingCompletions(std::span<RuntimeTimingCompletion> destination);
    void ConfigureRuntimeProcessBudget(const RuntimeProcessBudgetConfig&);
    RuntimeProcessBudgetDiagnostics GetRuntimeProcessBudgetDiagnostics();
    RuntimeProcessBudgetScope OverrideRuntimeProcessBudgetForDiagnostics(const RuntimeProcessBudgetOverride&);
    void SetRuntimeFailureInjection(RuntimeFailurePoint); // One-shot diagnostic hook.
    // Requires an empty output and three completed slots; false is poll-only
    // pressure/suspension. A nested active hold is invalid. Held submissions
    // use each existing slot once and omit only swapchain copy/presentation.
    // Publish/epoch/poll/collect remain usable. Other rendering rejects while
    // held; ClearRuntimeResources and destruction release before draining.
    bool TryBeginRuntimeReaderHold(RuntimeReaderHold& output);
    // Synchronous diagnostic compositor, not a runtime upload system. Atlas R
    // holds byte indices, A holds binary coverage; its top-left 256x256 is the
    // ghost table by default (x = low operand, y = high operand). lookup_x/y
    // select an explicit ghost/additive table or 256x1 remap. Rectangles are submitted
    // in caller order. No depth, lighting, filtering or implicit world sorting.
    // The fallback serializes one non-self-overlapping rectangle per draw.
    std::vector<uint8_t> CompositePalette(uint32_t width, uint32_t height,
        const std::vector<uint8_t>& background, const Image& atlas,
        const std::vector<PaletteQuad>& quads, bool ordered_views);
    // Mixed painter stream. Terrain uses hardware coverage and affine sampling;
    // original software scan-conversion differences require separate review.
    std::vector<uint8_t> CompositeIndexed(uint32_t width, uint32_t height,
        const std::vector<uint8_t>& background, const Image& atlas,
        const std::vector<PalettePrimitive>& primitives, bool ordered_views);
    // Frozen-frame preparation, not the runtime asset cache. Synchronously
    // uploads owned inputs, executes the ordered indexed stream, expands the
    // palette on GPU and checks only a four-byte GPU error status. No image
    // readback participates in rendering or presentation. Six-bit channels
    // expand with integer channel*255/63. Target coordinates are viewport-local.
    FrozenGpuFrame PrepareFrozenIndexedFrame(uint32_t width, uint32_t height, uint8_t clear_index,
        const Image& atlas, const std::vector<PalettePrimitive>& primitives,
        const std::array<uint8_t, 768>& display_palette, bool ordered_views);
    // Actual display colours, including values not representable by six-bit
    // VGA channels. Entries must be opaque; index zero remains a literal colour.
    FrozenGpuFrame PrepareFrozenIndexedFrame(uint32_t width, uint32_t height, uint8_t clear_index,
        const Image& atlas, const std::vector<PalettePrimitive>& primitives,
        const std::array<uint8_t, 1024>& display_palette_rgba, bool ordered_views);
    // Complete pre-overlay enhanced world pass. The resident colour-only result
    // has viewport dimensions; native origin is preserved for shader effects.
    // Clear, terrain, shadows and sprite groups execute on one GPU target.
    FrozenGpuFrame PrepareFrozenEnhancedWorldFrame(const EnhancedWorldPass& world,const Image& atlas,
        const std::vector<SpriteColourRow>& resolved_cluts,const std::array<float,9>& palette_transform,
        const std::vector<OutlinePassDraw>& draws,
        EnhancedWorldRasterOrigin raster_origin=EnhancedWorldRasterOrigin::ViewportLocal);
    // Complete pre-UI native frame; retains world depth through recorded
    // overlays and resolves the actual lens directly from resident GPU colour.
    FrozenGpuFrame PrepareFrozenEnhancedCompositionFrame(const EnhancedWorldPass& world,const Image& atlas,
        const std::vector<SpriteColourRow>& cluts,const std::array<float,9>& palette_transform,
        const std::vector<OutlinePassDraw>& draws,const SceneGLCompositionTrace& composition);
    // Explicit diagnostic wrapper; readback is not part of frame preparation.
    Image CompositeEnhancedWorld(const EnhancedWorldPass& world,const Image& atlas,
        const std::vector<SpriteColourRow>& resolved_cluts,const std::array<float,9>& palette_transform,
        const std::vector<OutlinePassDraw>& draws,
        EnhancedWorldRasterOrigin raster_origin=EnhancedWorldRasterOrigin::ViewportLocal);
    // Explicit diagnostic lens composition and readback. Both images are full
    // native top-down RGBA; pixels outside the destination rectangle are kept.
    Image CompositeLens(const LensPass& pass,const Image& native_source,const Image& native_target_background);
    // Original world status/flat/swipe quads over a declared native background.
    // Terrain supplies D24 only; overlays retain it through every phase.
    Image CompositeWorldOverlays(const WorldOverlayPass& pass, const Image& native_background,
        const std::vector<OutlineDepthTriangle>& terrain);
    // Native-sized copy to the swapchain. Size mismatch and foreign/expired
    // handles are errors; a minimized/suspended swapchain returns false.
    bool PresentFrozenFrame(const FrozenGpuFrame& frame);
    // Explicit synchronous diagnostics, independent of the presentation path.
    std::vector<uint8_t> ReadbackFrozenIndices(const FrozenGpuFrame& frame);
    Image ReadbackFrozenRGBA(const FrozenGpuFrame& frame);
    // Separate synchronous RGB outline diagnostic. Terrain writes D24 depth
    // in painter order (ALWAYS); silhouettes/edges use GREATER, visible coverage
    // LEQUAL. Sprites never write depth. Atlas G contains owned binary coverage.
    // VisibleCoverage draws a declared flat test colour, not normal sprite art.
    Image CompositeOutlines(const Image& background, const Image& atlas,
        const std::vector<OutlineDepthTriangle>& terrain, const std::vector<OutlinePassDraw>& draws);
    // Declared-background RGBA sprite composition with original GL pass order.
    // This diagnostic wrapper explicitly reads back a shared GPU render core;
    // that core retains its colour target independently of readback. Terrain
    // contributes depth only here; this is not complete enhanced-world colour.
    Image CompositeEnhancedSprites(const Image& background, const Image& atlas,
        const std::vector<SpriteColourRow>& resolved_cluts, const std::array<float, 9>& palette_transform,
        const std::vector<OutlineDepthTriangle>& terrain, const std::vector<OutlinePassDraw>& draws);
    void WaitIdle();
    // Diagnostic caller must explicitly authorize application-local device removal.
    int32_t DiagnosticRemoveDevice();
    Diagnostics GetDiagnostics();
    PresentationDiagnostics GetPresentationDiagnostics(); // Explicit, opt-in endpoint query; no wait.
    std::vector<std::string> TakeValidationMessages();
    std::string DeviceRemovedDetails() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace kfx::render::dx12
