// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
// KeeperFX renderer performance changes: 2026-10-19.
#pragma once
#include <cstdint>
#include <memory>
#include "RuntimeProcessBudget.h"

namespace kfx::render::dx12 {
enum class RuntimeAssetKind : uint8_t {
    Indexed8, PaletteRGBA8, TileR8, SpriteRGBA8, ColourRowRGBA8, FadeR8, LightmapR16, RemapRG16UInt, SpriteRG8
};
inline bool IsRuntimeSprite(RuntimeAssetKind kind) noexcept {
    return kind == RuntimeAssetKind::SpriteRGBA8 || kind == RuntimeAssetKind::SpriteRG8;
}
struct RuntimeAssetKey {
    uint64_t epoch = 0, logical_id = 0, generation = 0, revision = 0;
    RuntimeAssetKind kind = RuntimeAssetKind::Indexed8;
    bool operator==(const RuntimeAssetKey&) const = default;
};
struct RuntimeAssetDesc {
    uint32_t width = 0, height = 0;
    // Tightly packed texels; SpriteRG8 stores palette index and binary coverage.
    bool operator==(const RuntimeAssetDesc&) const = default;
};
class RuntimeAssetLease {
public:
    RuntimeAssetLease() = default;
    bool Valid() const noexcept;
    RuntimeAssetKey Key() const noexcept;
    RuntimeAssetDesc Description() const noexcept;
    void Reset() noexcept { state.reset(); }
private:
    struct State;
    std::shared_ptr<State> state;
    friend class Device;
};
class RuntimeFrameTicket {
public:
    RuntimeFrameTicket() = default;
    bool Valid() const noexcept;
    uint64_t FenceValue() const noexcept;
    bool HasReadback() const noexcept;
    void Reset() noexcept { state.reset(); }
private:
    struct State;
    std::shared_ptr<State> state;
    friend class Device;
};
struct RuntimeIndexedFrame {
    uint32_t native_width = 0, native_height = 0;
    int32_t x = 0, y = 0;
    uint32_t width = 0, height = 0;
    uint8_t clear_index = 0;
    RuntimeAssetLease indices, palette;
};
enum class RuntimeSubmitStatus : uint8_t { Submitted, WouldBlock, Suspended };
// The first reservation constraint that still failed after bounded collection.
// Byte/descriptor reasons name configured caps, not a measured DXGI budget.
// Releasing caller leases/tickets or retiring readers may permit a later retry;
// an undersized cap or fragmented pinned page can require a smaller packet.
// LightmapImages is the hard three-image limit per epoch/id/generation lineage.
enum class RuntimePressureReason : uint8_t {
    None, FrameSlot, DefaultBytes, UploadBytes, ReadbackBytes, Descriptors, DiagnosticTickets, LightmapImages,
    ProcessBudgetLocal, ProcessBudgetNonlocal, ProcessBudgetUnavailable, AllocationDenied, GeometryCpuBytes
};
// Diagnostic-only GPU reader gate. Release is CPU-side, thread-safe, retryable
// on failure, and never touches Device state. All other Device calls retain
// their render-thread contract. Destruction/Reset release without waiting;
// Device separately retains the gate until its queued wait has retired.
class RuntimeReaderHold {
public:
    RuntimeReaderHold() = default;
    ~RuntimeReaderHold();
    RuntimeReaderHold(RuntimeReaderHold&&) noexcept;
    RuntimeReaderHold& operator=(RuntimeReaderHold&&) noexcept;
    RuntimeReaderHold(const RuntimeReaderHold&) = delete;
    RuntimeReaderHold& operator=(const RuntimeReaderHold&) = delete;
    bool Valid() const noexcept;
    bool Released() const noexcept; // An empty handle is released.
    bool Release() noexcept;
    void Reset() noexcept;
private:
    struct State;
    std::shared_ptr<State> state;
    friend class Device;
};
struct RuntimeSubmitResult {
    RuntimeSubmitStatus status = RuntimeSubmitStatus::WouldBlock;
    RuntimeFrameTicket ticket;
    RuntimePressureReason pressure = RuntimePressureReason::None;
    uint64_t submission_id = 0;
    double cpu_submission_ms = 0, cpu_present_ms = 0, present_completed_seconds = 0;
};
struct RuntimeResourceBudgets {
    // CPU bytes count unique private snapshots, including cached lightmap image
    // histories, pending uploads and stale leased versions. GPU caps count
    // D3D12MA allocation sizes (not just
    // logical texels), including accepted/retired readers. Swapchain and other
    // Device APIs are separate existing allocations.
    uint64_t cpu_bytes = 128ull * 1024 * 1024;
    uint64_t default_bytes = 512ull * 1024 * 1024;
    uint64_t upload_bytes = 128ull * 1024 * 1024;
    uint64_t readback_bytes = 128ull * 1024 * 1024;
    uint32_t assets = 4096, descriptors = 131072, diagnostic_tickets = 8;
    // Retained terrain CPU shadows, including old+new growth overlap. Separate
    // from immutable asset snapshots; no reads from write-combined UPLOAD memory.
    uint64_t geometry_cpu_bytes = 128ull * 1024 * 1024;
};
struct RuntimeResourceDiagnostics {
    uint64_t epoch = 1;
    uint64_t publications = 0, publication_reuses = 0, submissions = 0;
    uint64_t would_block = 0, suspended = 0, evictions = 0;
    uint64_t pressure_collections = 0, pressure_recoveries = 0;
    uint64_t presentations = 0, held_submissions = 0;
    uint64_t reader_hold_arms = 0, reader_hold_releases = 0;
    uint32_t reader_hold_slots = 0;
    bool reader_hold_active = false, reader_hold_retained = false;
    uint64_t source_upload_bytes = 0, palette_upload_bytes = 0;
    // World: padded staging span with inter-footprint alignment. Indexed:
    // original per-source API spans. Neither counts persistent pool capacity.
    uint64_t aligned_upload_bytes = 0;
    // Successful allocations/maps include later rollback; growth/reuse counts
    // only committed changed submissions. Growth replaces an existing slot pool;
    // trims count Collect(true), not explicit teardown. Byte totals are resident.
    uint64_t staging_allocations = 0, staging_maps = 0, staging_reuses = 0, staging_growths = 0;
    uint64_t staging_trim_count = 0, staging_capacity_bytes = 0, staging_allocation_bytes = 0;
    uint32_t staging_pools = 0;
    uint64_t resource_allocations = 0, descriptor_heap_allocations = 0;
    uint64_t cpu_bytes = 0, default_bytes = 0, upload_bytes = 0, readback_bytes = 0;
    uint64_t retired_default_bytes = 0, pending_upload_bytes = 0;
    uint64_t cpu_high_water = 0, default_high_water = 0, upload_high_water = 0, readback_high_water = 0;
    uint64_t retired_high_water = 0, pending_high_water = 0;
    uint32_t asset_count = 0, descriptors = 0, descriptor_high_water = 0, diagnostic_tickets = 0;
    uint64_t slot_pressure = 0, fence_waits = 0, global_idle_calls = 0, image_readbacks = 0;
    uint64_t command_bytes = 0; // Submitted root constants; driver storage is not observable.
    // Geometry uploads count dynamic vertices plus changed projected terrain.
    uint64_t world_submissions = 0, world_upload_bytes = 0, geometry_upload_bytes = 0;
    // Submitted terrain comparisons/copies. Compared bytes had a valid prior
    // shadow; reused bytes exclude unchanged interiors included by range fallback.
    uint64_t terrain_compared_bytes = 0, terrain_upload_bytes = 0, terrain_reused_bytes = 0, terrain_dirty_ranges = 0;
    uint64_t terrain_cpu_bytes = 0, terrain_cpu_high_water = 0;
    uint64_t terrain_default_bytes = 0, terrain_capacity_bytes = 0;
    uint64_t world_shader_clears = 0;
    uint64_t world_region_allocations = 0, world_regions_reclaimed = 0;
    uint32_t world_pages = 0, world_regions = 0;
    // Accepted lightmap updates. Logical bytes include unchanged bounding-box
    // interiors; image reuse includes identical revisions with no GPU copies.
    uint64_t lightmap_cold_uploads = 0, lightmap_image_reuses = 0, lightmap_identical_reuses = 0;
    uint64_t lightmap_copy_rectangles = 0, lightmap_upload_bytes = 0, lightmap_bbox_fallbacks = 0;
    uint32_t lightmap_images = 0, lightmap_idle_images = 0;
    uint64_t whole_image_reuses = 0;
    uint32_t whole_images = 0, whole_image_idle = 0;
    bool failed = false, quarantined = false;
};
// Fixed device instrumentation, separate from runtime asset budgets/counters.
// All aggregates are cumulative across ClearRuntimeResources. GPU duration covers
// upload/render commands only, excluding image copies, queue latency and Present.
struct RuntimeTimingCompletion {
    uint64_t submission_id = 0;
    double gpu_upload_render_ms = -1;
    bool world = false, diagnostic = false, held = false;
};
struct RuntimeTimingAggregate {
    uint64_t samples = 0;
    double total_ms = 0, maximum_ms = 0, latest_ms = 0;
};
struct RuntimeTimingBucket {
    RuntimeTimingAggregate cpu_submitted, cpu_would_block, cpu_suspended, cpu_failed;
    RuntimeTimingAggregate cpu_present, gpu_upload_render;
};
struct RuntimeTimingPath {
    // held takes precedence over diagnostic image readback. CPU classification
    // uses the reader gate at call entry; GPU classification uses submission.
    RuntimeTimingBucket normal, diagnostic, held;
};
struct RuntimeTimingDiagnostics {
    uint64_t timestamp_frequency = 0;
    // READBACK allocation sizes are observable; query-heap driver storage is not.
    uint64_t readback_logical_bytes = 0, readback_allocation_bytes = 0;
    uint64_t gpu_completed = 0, gpu_discarded = 0, readback_maps = 0;
    uint64_t completion_overflows = 0;
    uint32_t query_heaps = 0, query_entries = 0, readback_resources = 0, gpu_pending = 0;
    RuntimeTimingPath world, indexed;
};
// Diagnostic failure injection for the real recording/ownership boundaries.
// AfterExecute leaves submitted owners pinned and makes runtime work fail closed.
enum class RuntimeFailurePoint : uint8_t { None, BeforeExecute, AfterExecute };
} // namespace kfx::render::dx12
