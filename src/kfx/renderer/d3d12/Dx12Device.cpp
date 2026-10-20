// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
// KeeperFX renderer performance changes: 2026-10-19.
#include "Dx12Device.h"
#include "kfx/renderer/TerrainTextureResolution.h"
#include "RuntimeWorldValidation.h"
#include "RuntimePageRegions.h"
#include "RuntimeLightmapUpdates.h"
#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <dxguids.h>
#include <wrl/client.h>
#include <D3D12MemAlloc.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <span>
#include <unordered_map>
#include <utility>

namespace kfx::render::dx12 {
using Microsoft::WRL::ComPtr;
namespace {
constexpr UINT frame_count = 3;
constexpr DXGI_FORMAT color_format = DXGI_FORMAT_R8G8B8A8_UNORM;

void Check(HRESULT hr, const char* operation)
{
    if (FAILED(hr)) {
        char text[256];
        std::snprintf(text, sizeof(text), "%s failed (0x%08lx)", operation, static_cast<unsigned long>(hr));
        throw std::runtime_error(text);
    }
}
std::string Utf8(const wchar_t* text)
{
    int count = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (count <= 0) return {};
    std::string result(count, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), count, nullptr, nullptr);
    result.pop_back();
    return result;
}
// Callback context has fixed storage and remains alive through unregistration.
// Successful unregistration drains outstanding callbacks, including other threads.
struct ResizeWarningTrace {
    std::atomic<uint32_t> occurrences{0};
    std::atomic<bool> ready{false};
    void* addresses[32]{};
    USHORT address_count = 0;
    DWORD callback_thread = 0;
    D3D12_MESSAGE_CATEGORY category{};
    D3D12_MESSAGE_SEVERITY severity{};
    static void Callback(D3D12_MESSAGE_CATEGORY category, D3D12_MESSAGE_SEVERITY severity,
        D3D12_MESSAGE_ID id, LPCSTR, void* context) noexcept
    {
        if (unsigned(id) != 1424) return;
        auto& trace = *static_cast<ResizeWarningTrace*>(context);
        uint32_t count = trace.occurrences.load(std::memory_order_relaxed);
        do {
            if (count == UINT32_MAX) return;
        } while (!trace.occurrences.compare_exchange_weak(count, count + 1, std::memory_order_relaxed));
        if (count != 0) return;
        trace.category = category; trace.severity = severity;
        trace.callback_thread = GetCurrentThreadId();
        trace.address_count = CaptureStackBackTrace(0, 32, trace.addresses, nullptr);
        trace.ready.store(true, std::memory_order_release);
    }
};
struct ScopedResizeWarningCallback {
    ComPtr<ID3D12InfoQueue1> queue;
    std::unique_ptr<ResizeWarningTrace> owned_trace = std::make_unique<ResizeWarningTrace>();
    ResizeWarningTrace* trace = owned_trace.get();
    DWORD cookie = 0;
    bool registered = false, quarantined = false;
    HRESULT query_result = E_NOINTERFACE, register_result = E_PENDING, unregister_result = E_PENDING;
    explicit ScopedResizeWarningCallback(ID3D12Device* device)
    {
        query_result = device->QueryInterface(IID_PPV_ARGS(queue.GetAddressOf()));
        if (FAILED(query_result)) return;
        register_result = queue->RegisterMessageCallback(ResizeWarningTrace::Callback,
            D3D12_MESSAGE_CALLBACK_FLAG_NONE, trace, &cookie);
        registered = SUCCEEDED(register_result);
    }
    void Stop() noexcept
    {
        if (!registered) return;
        unregister_result = queue->UnregisterMessageCallback(cookie);
        if (FAILED(unregister_result)) {
            // Detachment is unproven. Retain this one fixed context and queue
            // reference until process exit; the caller fails closed and disables
            // further tracing on this Device. Never free a live callback target.
            quarantined = true;
            (void)owned_trace.release();
            (void)queue.Detach();
        }
        registered = false;
    }
    ~ScopedResizeWarningCallback() { Stop(); }
};
std::string ResizeWarningDescription(const ScopedResizeWarningCallback& scope,
    DWORD caller, uint32_t width, uint32_t height, HRESULT resize_result, uint64_t fence_waits)
{
    const auto& trace = *scope.trace;
    const bool ready = trace.ready.load(std::memory_order_acquire);
    const DWORD callback_thread = ready ? trace.callback_thread : 0;
    const unsigned category = ready ? unsigned(trace.category) : 0;
    const unsigned severity = ready ? unsigned(trace.severity) : 0;
    const USHORT address_count = ready ? trace.address_count : 0;
    char line[512];
    std::snprintf(line, sizeof(line),
        "ResizeBuffers trace: size=%ux%u hr=0x%08lx query=0x%08lx register=0x%08lx unregister=0x%08lx "
        "id=1424 count=%u callerThread=%lu callbackThread=%lu sameThread=%s appFenceWaits=%llu category=%u severity=%u frames=%u callbackQuarantined=%s",
        width, height, static_cast<unsigned long>(resize_result), static_cast<unsigned long>(scope.query_result),
        static_cast<unsigned long>(scope.register_result), static_cast<unsigned long>(scope.unregister_result),
        trace.occurrences.load(std::memory_order_relaxed), static_cast<unsigned long>(caller),
        static_cast<unsigned long>(callback_thread), ready && caller == callback_thread ? "true" : "false",
        static_cast<unsigned long long>(fence_waits), category, severity, unsigned(address_count), scope.quarantined ? "true" : "false");
    std::string result(line);
    if (!ready) return result;
    // Module lookup and formatting occur outside the callback. The first writer
    // publishes immutable fields with ready; failed unregistration can leave
    // later callbacks active, but they touch only the atomic occurrence count.
    // Keep raw addresses and module offsets; no symbol server or DbgHelp state.
    for (USHORT i = 0; i < address_count; ++i) {
        HMODULE module = nullptr;
        const auto address = reinterpret_cast<uintptr_t>(trace.addresses[i]);
        const bool found = GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(trace.addresses[i]), &module) != 0;
        wchar_t path[1024]{};
        const DWORD length = found ? GetModuleFileNameW(module, path, DWORD(std::size(path))) : 0;
        path[std::size(path) - 1] = 0;
        std::snprintf(line, sizeof(line), "\n  #%u address=0x%llx moduleOffset=0x%llx module=", unsigned(i),
            static_cast<unsigned long long>(address), static_cast<unsigned long long>(found ? address - reinterpret_cast<uintptr_t>(module) : 0));
        result += line;
        result += length ? Utf8(path) : "<unresolved>";
        if (length >= std::size(path)) result += " <truncated>";
    }
    return result;
}
std::vector<char> ReadShader(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) throw std::runtime_error("Missing compiled shader: " + path.string());
    auto size = file.tellg();
    if (size <= 0 || size > 16 * 1024 * 1024) throw std::runtime_error("Invalid shader size");
    std::vector<char> bytes(static_cast<size_t>(size));
    file.seekg(0);
    if (!file.read(bytes.data(), size)) throw std::runtime_error("Cannot read shader");
    return bytes;
}
D3D12_CPU_DESCRIPTOR_HANDLE CpuHandle(ID3D12DescriptorHeap* heap)
{
#ifdef __MINGW32__
    D3D12_CPU_DESCRIPTOR_HANDLE handle;
    heap->GetCPUDescriptorHandleForHeapStart(&handle);
    return handle;
#else
    return heap->GetCPUDescriptorHandleForHeapStart();
#endif
}
D3D12_GPU_DESCRIPTOR_HANDLE GpuHandle(ID3D12DescriptorHeap* heap)
{
#ifdef __MINGW32__
    D3D12_GPU_DESCRIPTOR_HANDLE handle;
    heap->GetGPUDescriptorHandleForHeapStart(&handle);
    return handle;
#else
    return heap->GetGPUDescriptorHandleForHeapStart();
#endif
}
D3D12_RESOURCE_DESC BufferDesc(UINT64 bytes)
{
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes; desc.Height = 1; desc.DepthOrArraySize = 1; desc.MipLevels = 1;
    desc.SampleDesc.Count = 1; desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return desc;
}
D3D12_RESOURCE_DESC TextureDesc(UINT width, UINT height, DXGI_FORMAT format)
{
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width; desc.Height = height; desc.DepthOrArraySize = 1; desc.MipLevels = 1;
    desc.Format = format; desc.SampleDesc.Count = 1;
    return desc;
}
void Transition(ID3D12GraphicsCommandList* commands, ID3D12Resource* resource,
    D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
    commands->ResourceBarrier(1, &barrier);
}
// Allocation must outlive its resource. Members are destroyed in reverse order.
struct Resource {
    ComPtr<D3D12MA::Allocation> allocation;
    ComPtr<ID3D12Resource> resource;
    Resource() = default;
    Resource(Resource&&) noexcept = default;
    Resource& operator=(Resource&& other) noexcept
    {
        if (this != &other) {
            Reset();
            allocation = std::move(other.allocation);
            resource = std::move(other.resource);
        }
        return *this;
    }
    Resource(const Resource&) = delete;
    Resource& operator=(const Resource&) = delete;
    void Reset() { resource.Reset(); allocation.Reset(); }
};
// Local replacement owners coexist with the resources counted by RuntimeStats.
// Record their actual peaks before either committing or rolling them back.
struct RuntimeAllocationPeaks {
    RuntimeResourceDiagnostics& stats;
    uint64_t default_bytes, upload_bytes, readback_bytes;
    uint32_t descriptors;
    explicit RuntimeAllocationPeaks(RuntimeResourceDiagnostics& value) noexcept
        : stats(value), default_bytes(value.default_bytes), upload_bytes(value.upload_bytes),
          readback_bytes(value.readback_bytes), descriptors(value.descriptors) {}
    void AddResource(D3D12_HEAP_TYPE heap, uint64_t bytes) noexcept
    {
        switch (heap) {
        case D3D12_HEAP_TYPE_DEFAULT:
            default_bytes += bytes;
            stats.default_high_water = std::max(stats.default_high_water, default_bytes);
            break;
        case D3D12_HEAP_TYPE_UPLOAD:
            upload_bytes += bytes;
            stats.upload_high_water = std::max(stats.upload_high_water, upload_bytes);
            break;
        case D3D12_HEAP_TYPE_READBACK:
            readback_bytes += bytes;
            stats.readback_high_water = std::max(stats.readback_high_water, readback_bytes);
            break;
        default: break;
        }
    }
    void AddDescriptors(uint32_t count) noexcept
    {
        descriptors += count;
        stats.descriptor_high_water = std::max(stats.descriptor_high_water, descriptors);
    }
};
// Borrowed recording inputs. Callers validate, prepare and retain every GPU
// object through their own submission fence; recorders never allocate or wait.
struct WorldSurfaceTarget {
    ID3D12Resource* depth = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE colour{}, write_depth{}, read_depth{};
    D3D12_VIEWPORT viewport{};
    D3D12_RECT scissor{};
};
} // namespace

struct FrozenGpuFrame::State {
    std::weak_ptr<uint8_t> owner;
    uint32_t width = 0, height = 0;
    Resource indices, colour;
};
bool FrozenGpuFrame::Valid() const noexcept
{
    return state && !state->owner.expired() && state->colour.resource;
}
uint32_t FrozenGpuFrame::Width() const noexcept { return Valid() ? state->width : 0; }
uint32_t FrozenGpuFrame::Height() const noexcept { return Valid() ? state->height : 0; }

struct RuntimeWorldPage {
    Resource texture;
    RuntimeAssetKind kind;
    uint32_t width = 0, height = 0;
    RuntimePageRegions regions;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    RuntimeAssetKey lightmap_key{};
    std::shared_ptr<const std::vector<uint8_t>> lightmap_baseline;
    bool has_current_asset = false; // Recomputed by RuntimeStats on the device thread.
    bool LightmapLineage(const RuntimeAssetKey& key) const noexcept
    {
        return kind == RuntimeAssetKind::LightmapR16 && lightmap_key.epoch == key.epoch
            && lightmap_key.logical_id == key.logical_id && lightmap_key.generation == key.generation;
    }
};
struct RuntimeAssetLease::State {
    std::weak_ptr<uint8_t> owner;
    RuntimeAssetKey key;
    RuntimeAssetDesc desc;
    std::shared_ptr<const std::vector<uint8_t>> bytes;
    Resource texture;
    std::shared_ptr<RuntimeWorldPage> world_page;
    RuntimePageRegion world_region;
    bool current = true, resident = false, recently_used = true;
};
bool RuntimeAssetLease::Valid() const noexcept { return state && state->current && !state->owner.expired(); }
RuntimeAssetKey RuntimeAssetLease::Key() const noexcept { return state ? state->key : RuntimeAssetKey{}; }
RuntimeAssetDesc RuntimeAssetLease::Description() const noexcept { return state ? state->desc : RuntimeAssetDesc{}; }
struct RuntimeFrameTicket::State {
    std::weak_ptr<uint8_t> owner;
    uint64_t fence_value = 0;
    uint32_t width = 0, height = 0;
    Resource readback;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    uint64_t copy_bytes = 0;
};
bool RuntimeFrameTicket::Valid() const noexcept { return state && state->fence_value && !state->owner.expired(); }
uint64_t RuntimeFrameTicket::FenceValue() const noexcept { return Valid() ? state->fence_value : 0; }
bool RuntimeFrameTicket::HasReadback() const noexcept { return Valid() && state->readback.resource; }

struct RuntimeReaderHold::State {
    ComPtr<ID3D12Fence> fence;
    SRWLOCK release_lock = SRWLOCK_INIT;
    std::atomic<bool> released{false};
    // These fields are accessed only on the Device render thread.
    uint64_t retirement_fence = 0;
    uint32_t consumed_slots = 0;
    bool release_counted = false;
    bool Release() noexcept
    {
        AcquireSRWLockExclusive(&release_lock);
        bool success = released.load();
        if (!success) {
            success = SUCCEEDED(fence->Signal(1));
            if (success) released.store(true);
        }
        ReleaseSRWLockExclusive(&release_lock);
        return success;
    }
};
RuntimeReaderHold::~RuntimeReaderHold() { Release(); }
RuntimeReaderHold::RuntimeReaderHold(RuntimeReaderHold&& other) noexcept : state(std::move(other.state)) {}
RuntimeReaderHold& RuntimeReaderHold::operator=(RuntimeReaderHold&& other) noexcept
{
    if (this != &other) { Release(); state = std::move(other.state); }
    return *this;
}
bool RuntimeReaderHold::Valid() const noexcept { return bool(state); }
bool RuntimeReaderHold::Released() const noexcept { return !state || state->released.load(); }
bool RuntimeReaderHold::Release() noexcept { return !state || state->Release(); }
void RuntimeReaderHold::Reset() noexcept { Release(); state.reset(); }

struct RuntimeProcessBudgetScope::State {
    RuntimeProcessBudgetOverride provider;
    bool trimmed = false;
};
RuntimeProcessBudgetScope::~RuntimeProcessBudgetScope() = default;
RuntimeProcessBudgetScope::RuntimeProcessBudgetScope(RuntimeProcessBudgetScope&&) noexcept = default;
RuntimeProcessBudgetScope& RuntimeProcessBudgetScope::operator=(RuntimeProcessBudgetScope&&) noexcept = default;
void RuntimeProcessBudgetScope::Reset() noexcept { state.reset(); }
bool RuntimeProcessBudgetScope::Valid() const noexcept { return bool(state); }
struct Device::Impl {
    Options options;
    bool resize_warning_trace_finished = false;
    Diagnostics diagnostics;
    ComPtr<IDXGIFactory6> factory;
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12InfoQueue> info;
    ComPtr<D3D12MA::Allocator> memory;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<IDXGISwapChain3> swapchain;
    PresentationDiagnostics presentation_diagnostics;
    HANDLE presentation_event = nullptr;
    bool presentation_permit = false;
    bool presentation_poll_pending = false;
    std::chrono::steady_clock::time_point presentation_first_timeout{}, presentation_last_poll{};
    static void ObservePresentationGap(PresentationGapHistogram& histogram, double milliseconds) noexcept
    {
        const size_t bucket = milliseconds < 2 ? 0 : milliseconds < 8 ? 1 : milliseconds < 20 ? 2 : 3;
        if (histogram.count != UINT64_MAX) ++histogram.count;
        if (histogram.buckets[bucket] != UINT64_MAX) ++histogram.buckets[bucket];
        histogram.maximum_ms = std::max(histogram.maximum_ms, milliseconds);
    }
    ComPtr<ID3D12DescriptorHeap> rtv_heap, dsv_heap, srv_heap;
    ComPtr<ID3D12RootSignature> root_signature;
    ComPtr<ID3D12PipelineState> pipeline;
    ComPtr<ID3D12RootSignature> palette_root;
    ComPtr<ID3D12PipelineState> palette_serial, palette_ordered;
    ComPtr<ID3D12RootSignature> indexed_present_root;
    ComPtr<ID3D12PipelineState> indexed_present_pipeline;
    ComPtr<ID3D12RootSignature> runtime_indexed_root;
    ComPtr<ID3D12PipelineState> runtime_indexed_pipeline;
    ComPtr<ID3D12RootSignature> runtime_present_root;
    ComPtr<ID3D12PipelineState> runtime_present_pipeline;
    bool runtime_presentation = false;
    std::array<float,4> runtime_present_rect{};
    ComPtr<ID3D12RootSignature> runtime_clear_root;
    ComPtr<ID3D12PipelineState> runtime_clear_pipeline;
    ComPtr<ID3D12RootSignature> outline_root;
    ComPtr<ID3D12PipelineState> outline_depth;
    std::array<ComPtr<ID3D12PipelineState>, 2> outline_hidden, outline_visible, cursor_visible;
    std::array<std::array<ComPtr<ID3D12PipelineState>, 2>, 2> sprite_glow, cursor_glow;
    ComPtr<ID3D12RootSignature> enhanced_world_root;
    ComPtr<ID3D12PipelineState> enhanced_terrain, enhanced_shadow;
    ComPtr<ID3D12RootSignature> lens_root;
    ComPtr<ID3D12PipelineState> lens_pipeline, map_fade_pipeline;
    struct MapFadeCapture {
        uint64_t epoch=0,id=0;
        uint32_t width=0,height=0;
        Resource world,parchment;
    };
    // Registry ownership accounts each allocation exactly once. Frames retain
    // captures through their own fences, including unfenced quarantine.
    std::array<std::shared_ptr<MapFadeCapture>,4> map_fade_captures;
    std::shared_ptr<MapFadeCapture> map_fade_current;
    ComPtr<ID3D12RootSignature> world_overlay_root;
    ComPtr<ID3D12PipelineState> world_overlay_depth, world_overlay_flat;
    struct Frame {
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> runtime_commands;
        ComPtr<ID3D12Resource> backbuffer;
        UINT64 fence_value = 0;
        std::shared_ptr<FrozenGpuFrame::State> frozen_reader;
        Resource runtime_colour;
        uint32_t runtime_width = 0, runtime_height = 0;
        ComPtr<ID3D12DescriptorHeap> runtime_heap;
        uint32_t runtime_descriptors = 0;
        std::array<std::shared_ptr<RuntimeAssetLease::State>, 2> runtime_assets;
        Resource runtime_staging;
        uint8_t* runtime_staging_map = nullptr;
        uint64_t runtime_staging_capacity = 0;
        std::shared_ptr<RuntimeFrameTicket::State> runtime_ticket;
        bool runtime_quarantined = false;
        Resource timing_readback;
        UINT timing_query = 0;
        UINT64 timing_fence = 0;
        bool timing_pending = false;
        RuntimeTimingCompletion timing_completion{};
        RuntimeTimingBucket* timing_bucket = nullptr;
        Resource world_colour, world_depth, world_geometry, world_lens_colour;
        Resource world_terrain;
        std::unique_ptr<uint8_t[]> world_terrain_shadow;
        uint64_t world_terrain_capacity = 0, world_terrain_valid = 0, world_terrain_epoch = 0;
        std::array<float, 4> world_clear_rgba{}, world_lens_clear_rgba{};
        uint8_t* world_geometry_map = nullptr;
        uint64_t world_geometry_capacity = 0;
        uint32_t world_width = 0, world_height = 0, world_descriptors = 0;
        uint32_t world_lens_width = 0, world_lens_height = 0;
        ComPtr<ID3D12DescriptorHeap> world_heap, world_rtv, world_dsv, world_lens_rtv;
        std::vector<std::shared_ptr<RuntimeAssetLease::State>> world_assets;
        std::shared_ptr<MapFadeCapture> map_fade_capture;
    };
    std::array<Frame, frame_count> frames;
    ComPtr<ID3D12GraphicsCommandList> commands;
    ComPtr<ID3D12Fence> fence;
    HANDLE fence_event = nullptr;
    UINT64 next_fence = 1;
    UINT64 last_signalled_fence = 0;
    bool runtime_unfenced = false;
    UINT width = 0, height = 0, rtv_stride = 0, swap_flags = 0;
    bool suspended = false, initialized = false;
    Resource depth, texture, vertices, indices;
    D3D12_VERTEX_BUFFER_VIEW vertex_view = {};
    D3D12_INDEX_BUFFER_VIEW index_view = {};
    UINT index_count = 0;
    std::vector<std::string> messages;
    std::shared_ptr<uint8_t> frozen_owner = std::make_shared<uint8_t>(0);
    std::vector<std::weak_ptr<FrozenGpuFrame::State>> frozen_frames;
    std::shared_ptr<uint8_t> runtime_owner = std::make_shared<uint8_t>(0);
    std::vector<std::shared_ptr<RuntimeAssetLease::State>> runtime_assets;
    // Exactly one weak entry per fresh private snapshot. Page histories share
    // storage without retaining State or creating an ownership cycle.
    std::vector<std::weak_ptr<const std::vector<uint8_t>>> runtime_snapshots;
    std::vector<RuntimeAssetKey> runtime_versions;
    std::vector<std::shared_ptr<RuntimeWorldPage>> runtime_world_pages;
    std::vector<std::weak_ptr<RuntimeFrameTicket::State>> runtime_tickets;
    RuntimeResourceBudgets runtime_budgets;
    RuntimeResourceDiagnostics runtime_stats;
    ComPtr<ID3D12QueryHeap> runtime_timing_queries;
    std::array<RuntimeTimingCompletion, 256> timing_completions{};
    size_t timing_completion_head = 0, timing_completion_count = 0;
    bool timing_completions_enabled = false;
    uint64_t next_timing_submission = 0;
    double runtime_present_completed_seconds = 0;
    RuntimeTimingDiagnostics runtime_timing;
    RuntimeTimingBucket* runtime_cpu_timing = nullptr;
    double runtime_present_ms = 0;
    RuntimeFailurePoint runtime_failure = RuntimeFailurePoint::None;
    bool runtime_config_locked = false;
    std::shared_ptr<RuntimeReaderHold::State> runtime_reader_hold;

    struct ProcessNotification {
        ComPtr<IDXGIAdapter3> adapter;
        HANDLE event = nullptr;
        DWORD cookie = 0;
        bool registered = false;
        ~ProcessNotification() {
            if (registered) adapter->UnregisterVideoMemoryBudgetChangeNotification(cookie);
            if (event) CloseHandle(event);
        }
    } process_notification;
    RuntimeProcessBudgetDiagnostics process_budget;
    std::weak_ptr<RuntimeProcessBudgetScope::State> process_override;
    std::array<std::chrono::steady_clock::time_point, 2> process_sample_times{};
    std::chrono::steady_clock::time_point process_telemetry_time{};
    std::array<uint64_t, 2> process_anchor_usage{}, process_anchor_blocks{};
    std::array<bool, 2> process_anchor_valid{};
    std::array<RuntimeProcessBudgetSample, 2> process_attempt_samples{};
    // Registry identities only: attempt shared owners still fully unwind before trim.
    std::vector<RuntimeWorldPage*> process_preserve_pages;
    UINT process_frame_index = 0;
    bool process_world_bootstrapped = false, process_indexed_bootstrapped = false;
    struct ProcessDenial { RuntimePressureReason reason; };
    using RuntimeClock = std::chrono::steady_clock;
    static double RuntimeElapsed(RuntimeClock::time_point start) noexcept
    {
        return std::chrono::duration<double, std::milli>(RuntimeClock::now() - start).count();
    }
    static void AddRuntimeTiming(RuntimeTimingAggregate& aggregate, double milliseconds) noexcept
    {
        ++aggregate.samples; aggregate.latest_ms = milliseconds;
        aggregate.total_ms += milliseconds;
        aggregate.maximum_ms = std::max(aggregate.maximum_ms, milliseconds);
    }
    RuntimeTimingBucket& TimingBucket(bool world, bool diagnostic, bool held) noexcept
    {
        auto& path = world ? runtime_timing.world : runtime_timing.indexed;
        return held ? path.held : diagnostic ? path.diagnostic : path.normal;
    }
    template<class Submit> RuntimeSubmitResult RuntimeTimedSubmit(bool world, bool diagnostic, Submit&& submit)
    {
        const auto start = RuntimeClock::now();
        if (initialized) { ++process_frame_index; ProcessTelemetry(); }
        auto& bucket = TimingBucket(world, diagnostic, runtime_reader_hold && !runtime_reader_hold->released.load());
        runtime_cpu_timing = &bucket; runtime_present_ms = 0; runtime_present_completed_seconds = 0;
        try {
            auto result = RuntimeTrySubmit(std::forward<Submit>(submit));
            auto& aggregate = result.status == RuntimeSubmitStatus::Submitted ? bucket.cpu_submitted
                : result.status == RuntimeSubmitStatus::WouldBlock ? bucket.cpu_would_block : bucket.cpu_suspended;
            AddRuntimeTiming(aggregate, std::max(0.0, RuntimeElapsed(start) - runtime_present_ms));
            runtime_cpu_timing = nullptr;
            result.cpu_present_ms = runtime_present_ms;
            result.present_completed_seconds = runtime_present_completed_seconds;
            result.cpu_submission_ms = aggregate.latest_ms;
            return result;
        } catch (...) {
            AddRuntimeTiming(bucket.cpu_failed, std::max(0.0, RuntimeElapsed(start) - runtime_present_ms));
            runtime_cpu_timing = nullptr;
            throw;
        }
    }
    void RuntimePresent(const char* operation)
    {
        const auto start = RuntimeClock::now();
        const HRESULT result = swapchain->Present(options.vsync ? 1 : 0,
            !options.vsync && diagnostics.tearing_supported ? DXGI_PRESENT_ALLOW_TEARING : 0);
        presentation_permit = false; // Every actual Present ends this admission, including occlusion.
        presentation_poll_pending = false;
        if (options.presentation_diagnostics) {
            auto& stats = presentation_diagnostics;
            stats.last_present_hresult = static_cast<int32_t>(result);
            auto& count = result == S_OK ? stats.present_ok
                : result == DXGI_STATUS_OCCLUDED ? stats.present_occluded : stats.present_other;
            if (count != UINT64_MAX) ++count;
        }
        runtime_present_completed_seconds = timing_completions_enabled ? std::chrono::duration<double>(RuntimeClock::now().time_since_epoch()).count() : 0;
        const double milliseconds = RuntimeElapsed(start);
        runtime_present_ms += milliseconds;
        AddRuntimeTiming(runtime_cpu_timing->cpu_present, milliseconds);
        Check(result, operation);
    }
    void BeginRuntimeTiming(Frame& frame)
    {
        frame.runtime_commands->EndQuery(runtime_timing_queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, frame.timing_query);
    }
    void EndRuntimeTiming(Frame& frame)
    {
        auto* commands = frame.runtime_commands.Get();
        commands->EndQuery(runtime_timing_queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, frame.timing_query + 1);
        commands->ResolveQueryData(runtime_timing_queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, frame.timing_query, 2,
            frame.timing_readback.resource.Get(), 0);
    }
    void ArmRuntimeTiming(Frame& frame, bool world, bool diagnostic, bool held) noexcept
    {
        // Publication precedes Execute. The previous frame fence must never
        // authorize reading this pair if Execute or Signal later fails.
        frame.timing_completion = {++next_timing_submission, -1, world, diagnostic, held};
        frame.timing_pending = true; frame.timing_fence = 0;
        frame.timing_bucket = &TimingBucket(world, diagnostic, held);
        ++runtime_timing.gpu_pending;
    }
    void PublishTimingCompletion(const RuntimeTimingCompletion& value) noexcept
    {
        if (!timing_completions_enabled) return;
        if (timing_completion_count == timing_completions.size()) {
            ++runtime_timing.completion_overflows;
            timing_completion_head = (timing_completion_head + 1) % timing_completions.size();
            --timing_completion_count;
        }
        timing_completions[(timing_completion_head + timing_completion_count++) % timing_completions.size()] = value;
    }
    void DiscardRuntimeTiming(Frame& frame) noexcept
    {
        if (frame.timing_pending) {
            PublishTimingCompletion(frame.timing_completion);
            ++runtime_timing.gpu_discarded; --runtime_timing.gpu_pending;
        }
        frame.timing_pending = false; frame.timing_fence = 0; frame.timing_bucket = nullptr;
    }
    void CollectRuntimeTiming(UINT64 completed)
    {
        for (auto& frame : frames) {
            if (!frame.timing_pending || frame.runtime_quarantined || !frame.timing_fence
                || frame.timing_fence > completed) continue;
            // Map after completion to invalidate CPU caches for this GPU write.
            // Keeping READBACK mapped across submissions would not do that.
            const D3D12_RANGE read_range{0, 2 * sizeof(UINT64)};
            void* mapped = nullptr;
            Check(frame.timing_readback.resource->Map(0, &read_range, &mapped), "Map completed runtime timestamps");
            ++runtime_timing.readback_maps;
            UINT64 ticks[2]{}; std::memcpy(ticks, mapped, sizeof(ticks));
            const D3D12_RANGE written_range{0, 0};
            frame.timing_readback.resource->Unmap(0, &written_range);
            const double milliseconds = double(ticks[1] - ticks[0]) * 1000.0 / double(runtime_timing.timestamp_frequency);
            if (ticks[1] >= ticks[0] && std::isfinite(milliseconds)) {
                AddRuntimeTiming(frame.timing_bucket->gpu_upload_render, milliseconds);
                frame.timing_completion.gpu_upload_render_ms = milliseconds;
                ++runtime_timing.gpu_completed;
            } else ++runtime_timing.gpu_discarded;
            PublishTimingCompletion(frame.timing_completion);
            --runtime_timing.gpu_pending;
            frame.timing_pending = false; frame.timing_fence = 0; frame.timing_bucket = nullptr;
        }
    }
    void RuntimeCollect(bool evict_unused, bool process_trim = false);
    void RuntimeMakeRoom(uint64_t bytes);
    size_t runtime_eviction_cursor = 0;
    void RuntimeStats();
    void RuntimeClearOwners();
    RuntimeSubmitResult TrySubmitRuntimeWorldFrame(const RuntimeWorldFrame&, bool diagnostic_readback);
    RuntimeSubmitResult TrySubmitRuntimeIndexedFrame(const RuntimeIndexedFrame&, bool diagnostic_readback);
    void ProcessAllocatorStats()
    {
        D3D12MA::Budget budgets[2]{};
        memory->GetBudget(&budgets[0], &budgets[1]);
        for (unsigned i = 0; i < 2; ++i) {
            auto& out = process_budget.segments[i]; const auto& budget = budgets[i];
            out.allocator_block_bytes = budget.Stats.BlockBytes;
            out.allocator_allocation_bytes = budget.Stats.AllocationBytes;
            out.allocator_block_count = budget.Stats.BlockCount;
            out.allocator_allocation_count = budget.Stats.AllocationCount;
            out.allocator_estimated_usage_bytes = budget.UsageBytes;
            out.allocator_estimated_budget_bytes = budget.BudgetBytes;
            out.allocator_block_high_water = std::max(out.allocator_block_high_water, budget.Stats.BlockBytes);
        }
    }
    void RefreshProcessSegment(unsigned segment)
    {
        auto& out = process_budget.segments[segment];
        out.applicable = !process_budget.uma || segment == 0;
        if (!out.applicable) return;
        RuntimeProcessBudgetSample sample{};
        const auto diagnostic = process_override.lock();
        out.injected = bool(diagnostic);
        if (diagnostic) sample = diagnostic->trimmed && diagnostic->provider.replace_after_trim
            ? diagnostic->provider.after_trim[segment] : diagnostic->provider.samples[segment];
        else {
            DXGI_QUERY_VIDEO_MEMORY_INFO value{};
            const HRESULT hr = process_notification.adapter ? process_notification.adapter->QueryVideoMemoryInfo(0,
                segment ? DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL : DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &value) : E_NOINTERFACE;
            sample.hresult = static_cast<int32_t>(hr);
            if (SUCCEEDED(hr)) {
                sample.budget_bytes = value.Budget; sample.usage_bytes = value.CurrentUsage;
                sample.available_for_reservation_bytes = value.AvailableForReservation;
                sample.reservation_bytes = value.CurrentReservation;
            }
        }
        out.sample = sample; out.available = sample.hresult >= 0;
        ++out.sample_serial; ++out.refreshes;
        if (!out.available) ++out.failures;
        process_sample_times[segment] = RuntimeClock::now(); out.age_ms = 0;
    }
    void ProcessTelemetry(bool force = false)
    {
        if (!memory) return;
        const auto now = RuntimeClock::now();
        bool notified = false;
        if (process_notification.registered && WaitForSingleObject(process_notification.event, 0) == WAIT_OBJECT_0) {
            ++process_budget.notifications; notified = true;
        }
        const bool injected = !process_override.expired();
        const bool provider_changed = process_budget.override_active != injected;
        process_budget.override_active = injected;
        if (force || notified || provider_changed || now - process_telemetry_time >= std::chrono::milliseconds(250)) {
            memory->SetCurrentFrameIndex(process_frame_index);
            RefreshProcessSegment(0); RefreshProcessSegment(1); ProcessAllocatorStats();
            process_telemetry_time = now; ++process_budget.telemetry_refreshes;
        }
    }
    void InitializeProcessBudget()
    {
        process_budget.uma = memory->IsUMA();
        process_budget.segments[1].applicable = !process_budget.uma;
        HRESULT hr = adapter.As(&process_notification.adapter);
        if (SUCCEEDED(hr)) {
            process_notification.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (!process_notification.event) hr = HRESULT_FROM_WIN32(GetLastError());
            else hr = process_notification.adapter->RegisterVideoMemoryBudgetChangeNotificationEvent(
                process_notification.event, &process_notification.cookie);
        }
        process_notification.registered = SUCCEEDED(hr);
        process_budget.notification_registered = process_notification.registered;
        process_budget.notification_hresult = static_cast<int32_t>(hr);
        ProcessTelemetry(true);
    }
    static bool ProcessPressure(RuntimePressureReason reason) noexcept
    {
        return reason == RuntimePressureReason::ProcessBudgetLocal || reason == RuntimePressureReason::ProcessBudgetNonlocal
            || reason == RuntimePressureReason::ProcessBudgetUnavailable || reason == RuntimePressureReason::AllocationDenied;
    }
    void BeginProcessAttempt()
    {
        process_anchor_valid = {}; process_preserve_pages.clear();
    }
    void PrewarmRuntimePipelines(bool world)
    {
        bool& done = world ? process_world_bootstrapped : process_indexed_bootstrapped;
        if (done && (!runtime_presentation || runtime_present_pipeline)) return;
        const auto start = RuntimeClock::now();
        if (world) { CreateEnhancedWorldPipelines(); CreateOutlinePipelines(); CreateRuntimeClearPipeline(); CreateWorldOverlayPipelines(); CreateLensPipeline(); }
        else CreateRuntimeIndexedPipeline();
        if (runtime_presentation) CreateRuntimePresentPipeline();
        process_budget.bootstrap_ms += RuntimeElapsed(start);
        if (world) ++process_budget.world_bootstraps; else ++process_budget.indexed_bootstraps;
        done = true;
    }
    void CheckRuntimeAllocation(HRESULT hr, const char* operation)
    {
        if (hr == E_OUTOFMEMORY) {
            Check(device->GetDeviceRemovedReason(), "Runtime allocation device health");
            process_budget.last_allocation_hresult = static_cast<int32_t>(hr);
            process_budget.last_segment = UINT32_MAX; process_budget.last_requested_bytes = 0;
            ++process_budget.allocation_denials;
            throw ProcessDenial{RuntimePressureReason::AllocationDenied};
        }
        Check(hr, operation);
    }
    Resource AllocateRuntime(const D3D12_RESOURCE_DESC& desc, D3D12_HEAP_TYPE heap,
        D3D12_RESOURCE_STATES state, const wchar_t* name, const D3D12_CLEAR_VALUE* clear = nullptr,
        D3D12MA::ALLOCATION_FLAGS flags = D3D12MA::ALLOCATION_FLAG_NONE)
    {
        const auto kind = heap == D3D12_HEAP_TYPE_DEFAULT ? RuntimeProcessMemoryHeap::Default
            : heap == D3D12_HEAP_TYPE_UPLOAD ? RuntimeProcessMemoryHeap::Upload : RuntimeProcessMemoryHeap::Readback;
        const unsigned segment = RuntimeProcessSegment(process_budget.uma, kind);
        const uint64_t size = AllocationSize(desc);
        process_budget.last_segment = segment; process_budget.last_requested_bytes = size;
        D3D12MA::ALLOCATION_DESC allocation{}; allocation.HeapType = heap;
        Resource result;
        const auto create = [&]() {
            return memory->CreateResource(&allocation, &desc, state, clear, result.allocation.GetAddressOf(),
                IID_PPV_ARGS(result.resource.GetAddressOf()));
        };
        HRESULT hr = E_OUTOFMEMORY;
        if (!(flags & D3D12MA::ALLOCATION_FLAG_COMMITTED)) {
            // Only previously placed-eligible runtime resources probe existing blocks.
            allocation.Flags = D3D12MA::ALLOCATION_FLAG_NEVER_ALLOCATE;
            ++process_budget.existing_block_probes; hr = create();
            if (SUCCEEDED(hr)) ++process_budget.existing_block_hits;
            else {
                result.Reset();
                // A valid resource larger than the allocator pool cannot be placed.
                // NEVER_ALLOCATE then reports E_INVALIDARG; real creation below
                // still validates the resource and retains fatal error semantics.
                if (hr == E_INVALIDARG) ++process_budget.existing_block_unsupported;
                else if (hr != E_OUTOFMEMORY) Check(hr, "Probe existing runtime backing");
                ++process_budget.existing_block_misses;
            }
        }
        if (FAILED(hr)) {
            // Bootstrap completed before any call here. Refresh the actual segment;
            // allocator estimates remain separately labelled and are not this policy.
            if (!process_anchor_valid[segment]) {
                memory->SetCurrentFrameIndex(process_frame_index);
                RefreshProcessSegment(segment); ++process_budget.growth_refreshes; ProcessAllocatorStats();
                process_attempt_samples[segment] = process_budget.segments[segment].sample;
                process_anchor_usage[segment] = process_attempt_samples[segment].usage_bytes;
                process_anchor_blocks[segment] = process_budget.segments[segment].allocator_block_bytes;
                process_anchor_valid[segment] = true;
            } else ProcessAllocatorStats();
            auto sample = process_attempt_samples[segment];
            // One fresh baseline per segment/attempt. Do not rebase while local
            // candidates live: OS usage may lag their committed backing growth.
            const auto blocks = process_budget.segments[segment].allocator_block_bytes;
            const uint64_t added = blocks > process_anchor_blocks[segment] ? blocks - process_anchor_blocks[segment] : 0;
            sample.usage_bytes = added > UINT64_MAX - process_anchor_usage[segment]
                ? UINT64_MAX : process_anchor_usage[segment] + added;
            const auto decision = EvaluateRuntimeProcessBudget(sample, process_budget.config.reserve_bytes[segment], size,
                process_budget.config.unavailable);
            if (decision != RuntimeProcessBudgetDecision::Allow) {
                if (decision == RuntimeProcessBudgetDecision::Unavailable) {
                    ++process_budget.unavailable_denials; throw ProcessDenial{RuntimePressureReason::ProcessBudgetUnavailable};
                }
                ++process_budget.budget_denials;
                throw ProcessDenial{segment ? RuntimePressureReason::ProcessBudgetNonlocal : RuntimePressureReason::ProcessBudgetLocal};
            }
            // Bounded committed growth keeps new backing equal to a checked range;
            // WITHIN_BUDGET is only an additional allocator safeguard, never DXGI proof.
            allocation.Flags = D3D12MA::ALLOCATION_FLAGS(flags | D3D12MA::ALLOCATION_FLAG_COMMITTED | D3D12MA::ALLOCATION_FLAG_WITHIN_BUDGET);
            hr = create(); process_budget.last_allocation_hresult = static_cast<int32_t>(hr);
            if (hr == E_OUTOFMEMORY) {
                result.Reset(); Check(device->GetDeviceRemovedReason(), "Runtime allocation device health");
                ++process_budget.allocation_denials; throw ProcessDenial{RuntimePressureReason::AllocationDenied};
            }
            Check(hr, "Allocate bounded runtime backing");
            ++process_budget.committed_growths; process_budget.committed_growth_bytes += result.allocation->GetSize();
            if (sample.hresult < 0) ++process_budget.compatibility_growths;
        }
        process_budget.last_allocation_hresult = static_cast<int32_t>(hr);
        ProcessAllocatorStats();
        Check(result.resource->SetName(name), "Name runtime GPU resource"); result.allocation->SetName(name);
        return result;
    }
    RuntimePressureReason RuntimePressure(uint64_t defaults, uint64_t uploads, uint64_t readbacks,
        uint64_t descriptors, bool diagnostic_readback) const noexcept
    {
        const auto exceeds = [](uint64_t owned, uint64_t needed, uint64_t cap) {
            return needed > cap || owned > cap - needed;
        };
        if (exceeds(runtime_stats.default_bytes, defaults, runtime_budgets.default_bytes)) return RuntimePressureReason::DefaultBytes;
        if (exceeds(runtime_stats.upload_bytes, uploads, runtime_budgets.upload_bytes)) return RuntimePressureReason::UploadBytes;
        if (exceeds(runtime_stats.readback_bytes, readbacks, runtime_budgets.readback_bytes)) return RuntimePressureReason::ReadbackBytes;
        if (exceeds(runtime_stats.descriptors, descriptors, runtime_budgets.descriptors)) return RuntimePressureReason::Descriptors;
        if (diagnostic_readback && runtime_stats.diagnostic_tickets >= runtime_budgets.diagnostic_tickets)
            return RuntimePressureReason::DiagnosticTickets;
        return RuntimePressureReason::None;
    }
    template<class Submit> RuntimeSubmitResult RuntimeTrySubmit(Submit&& submit)
    {
        bool process_retry = false;
        for (unsigned attempt = 0;; ++attempt) {
            BeginProcessAttempt();
            RuntimeSubmitResult result;
            // Only this typed preparation refusal is retryable. All attempt
            // owners unwind before collection; postExecute failures never use it.
            try { result = submit(); }
            catch (const ProcessDenial& denied) { result = {RuntimeSubmitStatus::WouldBlock, {}, denied.reason}; }
            if (result.status != RuntimeSubmitStatus::WouldBlock) {
                if (attempt && result.status == RuntimeSubmitStatus::Submitted) {
                    ++runtime_stats.pressure_recoveries;
                    if (process_retry) ++process_budget.pressure_recoveries;
                }
                process_preserve_pages.clear();
            return result;
            }
            if (attempt || result.pressure == RuntimePressureReason::FrameSlot) {
                ++runtime_stats.would_block; process_preserve_pages.clear(); return result;
            }
            ++runtime_stats.pressure_collections;
            process_retry = ProcessPressure(result.pressure);
            RuntimeCollect(true, process_retry);
            process_preserve_pages.clear();
            if (process_retry) {
                ++process_budget.pressure_trims;
                if (const auto diagnostic = process_override.lock()) diagnostic->trimmed = true;
                ProcessTelemetry(true);
            }
            // Scalar-cap trimming keeps its qualified behavior. Process pressure
            // preserves all three slot working sets so rotation can still advance.
        }
    }
    void RequireHealthy() const
    {
        if (runtime_stats.failed || runtime_stats.quarantined)
            throw std::logic_error("Runtime submission failed; explicitly drain and clear before other rendering");
        if (runtime_reader_hold && !runtime_reader_hold->released.load())
            throw std::logic_error("Other rendering is unavailable while diagnostic GPU readers are held");
    }
    void ReleaseRuntimeReaderHold()
    {
        if (runtime_reader_hold && !runtime_reader_hold->Release()) {
            runtime_stats.failed = true;
            throw std::runtime_error("Could not release diagnostic GPU reader gate");
        }
    }
    void CreateRuntimeIndexedPipeline();
    void CreateRuntimeClearPipeline();
    void CreateRuntimePresentPipeline();
    void RecordRuntimePresent(ID3D12GraphicsCommandList*, Frame&, ID3D12Resource*, ID3D12DescriptorHeap*, uint32_t, uint32_t, uint32_t);
    uint64_t AllocationSize(const D3D12_RESOURCE_DESC& desc) const
    {
        D3D12_RESOURCE_ALLOCATION_INFO allocation{};
#ifdef __MINGW32__
        device->GetResourceAllocationInfo(&allocation, 0, 1, &desc);
#else
        allocation = device->GetResourceAllocationInfo(0, 1, &desc);
#endif
        if (!allocation.SizeInBytes || allocation.SizeInBytes == UINT64_MAX)
            throw std::invalid_argument("Invalid runtime GPU allocation description");
        return allocation.SizeInBytes;
    }
    // Preserve the API's original single-source span for indexed telemetry,
    // while reserving complete padded rows at disjoint, aligned pool offsets.
    uint64_t RuntimeUploadFootprint(const D3D12_RESOURCE_DESC& desc, uint64_t row_bytes,
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint, uint64_t& cursor) const
    {
        UINT rows = 0; UINT64 row_size = 0, length = 0;
        device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &rows, &row_size, &length);
        if (!length || length == UINT64_MAX || rows != desc.Height || footprint.Footprint.Depth != 1
            || row_size != row_bytes || footprint.Footprint.RowPitch < row_bytes)
            throw std::invalid_argument("Invalid runtime upload footprint");
        if (cursor > UINT64_MAX - 511) throw std::overflow_error("Runtime upload alignment overflow");
        const uint64_t offset = (cursor + 511) & ~uint64_t(511);
        const uint64_t pitch = footprint.Footprint.RowPitch;
        if (rows && pitch > (UINT64_MAX - offset) / rows)
            throw std::overflow_error("Runtime upload footprint overflow");
        footprint.Offset = offset; cursor = offset + pitch * rows;
        return length;
    }
    struct RuntimeStagingPlan {
        uint64_t capacity = 0, allocation = 0;
    };
    RuntimeStagingPlan PlanRuntimeStaging(const Frame& frame, uint64_t required, uint64_t other_uploads) const
    {
        if (!required || required <= frame.runtime_staging_capacity)
            return {frame.runtime_staging_capacity, 0};
        if (required > UINT64_MAX - 511) throw std::overflow_error("Runtime staging capacity overflow");
        const uint64_t minimum = (required + 511) & ~uint64_t(511);
        uint64_t capacity = std::max<uint64_t>(65536, frame.runtime_staging_capacity);
        while (capacity < minimum) {
            if (capacity > UINT64_MAX / 2) { capacity = minimum; break; }
            capacity *= 2;
        }
        uint64_t allocation = AllocationSize(BufferDesc(capacity));
        const auto fits = [&](uint64_t needed) {
            const auto cap = runtime_budgets.upload_bytes;
            return runtime_stats.upload_bytes <= cap && other_uploads <= cap - runtime_stats.upload_bytes
                && needed <= cap - runtime_stats.upload_bytes - other_uploads;
        };
        // Over-rounding must not reject a fitting request. The old pool and any
        // geometry candidate remain fully charged until commit or collection.
        if (capacity != minimum && !fits(allocation)) {
            capacity = minimum; allocation = AllocationSize(BufferDesc(capacity));
        }
        return {capacity, allocation};
    }
    struct RuntimeStagingAttempt {
        Resource candidate;
        ID3D12Resource* resource = nullptr;
        uint8_t* mapped = nullptr;
    };
    RuntimeStagingAttempt PrepareRuntimeStaging(const Frame& frame, const RuntimeStagingPlan& plan,
        uint64_t required, UINT slot, RuntimeAllocationPeaks& peaks)
    {
        RuntimeStagingAttempt result;
        if (!required) return result;
        if (plan.allocation) {
            wchar_t name[128]{};
            std::swprintf(name, std::size(name), L"Runtime asset staging slot%u capacity%llu", slot,
                static_cast<unsigned long long>(plan.capacity));
            result.candidate = AllocateRuntime(BufferDesc(plan.capacity), D3D12_HEAP_TYPE_UPLOAD,
                D3D12_RESOURCE_STATE_GENERIC_READ, name);
            ++runtime_stats.resource_allocations; ++runtime_stats.staging_allocations;
            peaks.AddResource(D3D12_HEAP_TYPE_UPLOAD, result.candidate.allocation->GetSize());
            if (result.candidate.allocation->GetSize() > plan.allocation)
                throw std::runtime_error("Runtime staging allocation exceeded reservation");
            void* mapped = nullptr; D3D12_RANGE none{0, 0};
            Check(result.candidate.resource->Map(0, &none, &mapped), "Map runtime persistent staging");
            ++runtime_stats.staging_maps;
            result.resource = result.candidate.resource.Get(); result.mapped = static_cast<uint8_t*>(mapped);
        } else {
            result.resource = frame.runtime_staging.resource.Get(); result.mapped = frame.runtime_staging_map;
        }
        if (!result.resource || !result.mapped || required > plan.capacity)
            throw std::logic_error("Runtime staging pool is unavailable or undersized");
        return result;
    }
    void CommitRuntimeStaging(Frame& frame, RuntimeStagingAttempt& staging,
        const RuntimeStagingPlan& plan, uint64_t required) noexcept
    {
        if (!required) return;
        if (plan.allocation) {
            if (frame.runtime_staging.resource) ++runtime_stats.staging_growths;
            frame.runtime_staging = std::move(staging.candidate);
            frame.runtime_staging_map = staging.mapped; frame.runtime_staging_capacity = plan.capacity;
        } else ++runtime_stats.staging_reuses;
    }
    void ReleaseRuntimeStaging(Frame& frame, bool trim) noexcept
    {
        if (trim && frame.runtime_staging.resource) ++runtime_stats.staging_trim_count;
        frame.runtime_staging.Reset(); frame.runtime_staging_map = nullptr; frame.runtime_staging_capacity = 0;
    }
    uint64_t CompletedRuntimeFence()
    {
        const uint64_t completed = fence->GetCompletedValue();
        if (completed == UINT64_MAX) {
            runtime_stats.failed = true;
            throw std::runtime_error("Runtime GPU fence returned the device-removal sentinel");
        }
        return completed;
    }

    Resource RenderIndexedSurface(uint32_t width, uint32_t height, const std::vector<uint8_t>* background,
        uint8_t clear_index, const Image& atlas, const std::vector<PalettePrimitive>& primitives, bool ordered_views);
    void RecordSurfaceBegin(const WorldSurfaceTarget&, const float* clear_rgba);
    void RecordEnhancedWorld(const WorldSurfaceTarget&, ID3D12DescriptorHeap*, D3D12_GPU_DESCRIPTOR_HANDLE,
        const std::array<float,16>&, const D3D12_VERTEX_BUFFER_VIEW&, const D3D12_VERTEX_BUFFER_VIEW&,
        bool finish_terrain = true, bool terrain_ready = false);
    void RecordOutlineSprites(const WorldSurfaceTarget&, ID3D12DescriptorHeap*, D3D12_GPU_DESCRIPTOR_HANDLE,
        const std::array<float,12>&, const D3D12_VERTEX_BUFFER_VIEW&, UINT depth_vertices,
        bool depth_ready, std::span<const OutlinePassDraw>, bool flat = false);
    uint32_t RecordWorldOverlays(const WorldSurfaceTarget&, ID3D12DescriptorHeap*,
        D3D12_GPU_DESCRIPTOR_HANDLE sources, D3D12_GPU_DESCRIPTOR_HANDLE tables, UINT stride, UINT null_source,
        const D3D12_VERTEX_BUFFER_VIEW&, uint32_t native_width, uint32_t native_height, std::span<const WorldOverlayQuad>, const RuntimePostDraw* post = nullptr);
    Resource RenderOutlineSurface(const Image& background, const Image& atlas,
        const std::vector<SpriteColourRow>& cluts, const std::array<float, 9>& palette_transform,
        const std::vector<OutlineDepthTriangle>& terrain, const std::vector<OutlinePassDraw>& draws, bool enhanced,
        const EnhancedWorldPass* world = nullptr,
        EnhancedWorldRasterOrigin raster_origin=EnhancedWorldRasterOrigin::ViewportLocal,
        const WorldOverlayPass* overlays = nullptr, bool retain_native = false);
    void RecordLens(D3D12_CPU_DESCRIPTOR_HANDLE, ID3D12DescriptorHeap*,
        D3D12_GPU_DESCRIPTOR_HANDLE, const LensConstants&, bool draw = true);
    Resource RenderLensSurface(const LensPass&, const Image* source_image, ID3D12Resource* resident_source,
        const Image* background, const std::array<float,4>* clear, bool draw = true);
    std::vector<uint8_t> ReadTexture(ID3D12Resource* resource, uint32_t width, uint32_t height,
        DXGI_FORMAT format, D3D12_RESOURCE_STATES state);
    std::shared_ptr<FrozenGpuFrame::State> GetFrozen(const FrozenGpuFrame& frame) const
    {
        RequireHealthy();
        if (!initialized || !frame.Valid() || frame.state->owner.lock() != frozen_owner)
            throw std::invalid_argument("Frozen frame is invalid or belongs to another device");
        return frame.state;
    }

    ~Impl()
    {
        RuntimeClearOwners();
        frozen_owner.reset();
        for (auto& frame : frames) frame.frozen_reader.reset();
        for (auto& weak : frozen_frames) if (auto frame = weak.lock()) {
            frame->colour.Reset(); frame->indices.Reset();
        }
        if (presentation_event) CloseHandle(presentation_event);
        if (fence_event) CloseHandle(fence_event);
    }
    void Wait(UINT64 value)
    {
        auto completed = fence->GetCompletedValue();
        if (completed == UINT64_MAX) {
            Check(device->GetDeviceRemovedReason(), "GPU fence/device removed");
            throw std::runtime_error("GPU fence returned the device-removal sentinel");
        }
        if (completed >= value) return;
        ++runtime_stats.fence_waits;
        Check(fence->SetEventOnCompletion(value, fence_event), "SetEventOnCompletion");
        DWORD result = WaitForSingleObject(fence_event, 30000);
        if (result != WAIT_OBJECT_0) throw std::runtime_error("GPU fence wait failed or exceeded 30 seconds");
        Check(device->GetDeviceRemovedReason(), "GPU completion");
    }
    void Execute(UINT count, ID3D12CommandList* const* lists)
    {
        if (next_fence == UINT64_MAX) throw std::overflow_error("GPU fence sequence exhausted before execution");
        runtime_unfenced = true;
        queue->ExecuteCommandLists(count, lists);
    }
    UINT64 Signal()
    {
        if (next_fence == UINT64_MAX) throw std::overflow_error("GPU fence sequence exhausted");
        const UINT64 value = next_fence++;
        Check(queue->Signal(fence.Get(), value), "Signal");
        last_signalled_fence = value; runtime_unfenced = false;
        return value;
    }
    void Idle()
    {
        ++runtime_stats.global_idle_calls;
        if (queue && fence && fence_event) {
            if (next_fence < UINT64_MAX) Wait(Signal());
            else {
                if (runtime_unfenced) throw std::overflow_error("Cannot drain an unfenced exhausted GPU queue");
                Wait(last_signalled_fence);
            }
        }
        for (auto& frame : frames) frame.frozen_reader.reset();
    }
    Resource Allocate(const D3D12_RESOURCE_DESC& desc, D3D12_HEAP_TYPE heap,
        D3D12_RESOURCE_STATES state, const wchar_t* name, const D3D12_CLEAR_VALUE* clear = nullptr,
        D3D12MA::ALLOCATION_FLAGS flags = D3D12MA::ALLOCATION_FLAG_NONE)
    {
        D3D12MA::ALLOCATION_DESC allocation = {};
        allocation.HeapType = heap; allocation.Flags = flags;
        Resource result;
        Check(memory->CreateResource(&allocation, &desc, state, clear, result.allocation.GetAddressOf(),
            IID_PPV_ARGS(result.resource.GetAddressOf())), "Allocate GPU resource");
        Check(result.resource->SetName(name), "Set resource name");
        result.allocation->SetName(name);
        return result;
    }
    void CollectMessages()
    {
        if (!info) return;
        if (info->GetNumMessagesDiscardedByMessageCountLimit() != 0)
            throw std::runtime_error("D3D12 validation message queue overflowed");
        UINT64 count = info->GetNumStoredMessagesAllowedByRetrievalFilter();
        for (UINT64 i = 0; i < count; ++i) {
            SIZE_T size = 0;
            Check(info->GetMessage(i, nullptr, &size), "Get validation message size");
            std::vector<uint8_t> storage(size);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            Check(info->GetMessage(i, message, &size), "Get validation message");
            if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) ++diagnostics.validation_errors;
            if (message->Severity == D3D12_MESSAGE_SEVERITY_WARNING) ++diagnostics.validation_warnings;
            if (message->Severity <= D3D12_MESSAGE_SEVERITY_WARNING)
                messages.emplace_back("D3D12[id=" + std::to_string(unsigned(message->ID))
                    + ",category=" + std::to_string(unsigned(message->Category))
                    + ",severity=" + std::to_string(unsigned(message->Severity)) + "]: " + message->pDescription);
        }
        info->ClearStoredMessages();
    }
    void CreateTargets()
    {
        auto handle = CpuHandle(rtv_heap.Get());
        for (UINT i = 0; i < frame_count; ++i) {
            Check(swapchain->GetBuffer(i, IID_PPV_ARGS(frames[i].backbuffer.GetAddressOf())), "Get swap buffer");
            device->CreateRenderTargetView(frames[i].backbuffer.Get(), nullptr, handle);
            handle.ptr += rtv_stride;
        }
        auto desc = TextureDesc(width, height, DXGI_FORMAT_D32_FLOAT);
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        D3D12_CLEAR_VALUE clear = {};
        clear.Format = desc.Format; clear.DepthStencil.Depth = 1.0f;
        depth = Allocate(desc, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_DEPTH_WRITE, L"Depth", &clear);
        device->CreateDepthStencilView(depth.resource.Get(), nullptr, CpuHandle(dsv_heap.Get()));
    }
    void CreatePipeline()
    {
        D3D12_DESCRIPTOR_RANGE range = {};
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = 1;
        range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        D3D12_ROOT_PARAMETER parameters[2] = {};
        parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[0].Constants = {0, 0, 16};
        parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
        parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[1].DescriptorTable = {1, &range};
        parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_STATIC_SAMPLER_DESC sampler = {};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC root = {};
        root.NumParameters = 2; root.pParameters = parameters;
        root.NumStaticSamplers = 1; root.pStaticSamplers = &sampler;
        root.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        ComPtr<ID3DBlob> signature, errors;
        HRESULT hr = D3D12SerializeRootSignature(&root, D3D_ROOT_SIGNATURE_VERSION_1,
            signature.GetAddressOf(), errors.GetAddressOf());
        if (FAILED(hr) && errors) messages.emplace_back(static_cast<const char*>(errors->GetBufferPointer()));
        Check(hr, "Serialize root signature");
        Check(device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(),
            IID_PPV_ARGS(root_signature.GetAddressOf())), "Create root signature");
        auto vs = ReadShader(std::filesystem::path(options.shader_directory) / "textured.vs.dxil");
        auto ps = ReadShader(std::filesystem::path(options.shader_directory) / "textured.ps.dxil");
        D3D12_INPUT_ELEMENT_DESC layout[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 1, DXGI_FORMAT_R32_FLOAT, 0, 20, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 2, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}
        };
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
        desc.pRootSignature = root_signature.Get();
        desc.VS = {vs.data(), vs.size()}; desc.PS = {ps.data(), ps.size()};
        desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        desc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
        desc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_ZERO;
        desc.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
        desc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
        desc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
        desc.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
        desc.BlendState.RenderTarget[0].LogicOp = D3D12_LOGIC_OP_NOOP;
        desc.SampleMask = UINT_MAX;
        desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        desc.RasterizerState.CullMode = options.cull_back_faces ? D3D12_CULL_MODE_BACK : D3D12_CULL_MODE_NONE;
        // Terrain quads use outward CCW winding in the Z-up world contract.
        desc.RasterizerState.FrontCounterClockwise = options.cull_back_faces;
        desc.RasterizerState.DepthClipEnable = TRUE;
        desc.DepthStencilState.DepthEnable = TRUE;
        desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
        desc.InputLayout = {layout, 4};
        desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        desc.NumRenderTargets = 1; desc.RTVFormats[0] = color_format;
        desc.DSVFormat = DXGI_FORMAT_D32_FLOAT; desc.SampleDesc.Count = 1;
        Check(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(pipeline.GetAddressOf())), "Create graphics pipeline");
    }
    void CreateIndexedPresentPipeline()
    {
        if (indexed_present_pipeline) return;
        D3D12_DESCRIPTOR_RANGE ranges[2] = {};
        ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[0].NumDescriptors = 2;
        ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[1].NumDescriptors = 2;
        ranges[1].OffsetInDescriptorsFromTableStart = 2;
        D3D12_ROOT_PARAMETER parameter = {};
        parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameter.DescriptorTable = {2, ranges};
        D3D12_ROOT_SIGNATURE_DESC root = {};
        root.NumParameters = 1; root.pParameters = &parameter;
        ComPtr<ID3DBlob> signature, errors;
        Check(D3D12SerializeRootSignature(&root, D3D_ROOT_SIGNATURE_VERSION_1,
            signature.GetAddressOf(), errors.GetAddressOf()), "Serialize indexed expansion root signature");
        Check(device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(),
            IID_PPV_ARGS(indexed_present_root.ReleaseAndGetAddressOf())), "Create indexed expansion root signature");
        const auto shader = ReadShader(std::filesystem::path(options.shader_directory) / "indexed-present.cs.dxil");
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};
        desc.pRootSignature = indexed_present_root.Get(); desc.CS = {shader.data(), shader.size()};
        Check(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(indexed_present_pipeline.GetAddressOf())),
            "Create indexed expansion pipeline");
    }
    void CreateOutlinePipelines()
    {
        if (outline_depth) return;
        D3D12_DESCRIPTOR_RANGE range = {};
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; range.NumDescriptors = 2;
        D3D12_ROOT_PARAMETER parameters[2] = {};
        parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[0].DescriptorTable = {1, &range};
        parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[1].Constants = {0, 0, 12};
        parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC root = {};
        root.NumParameters = 2; root.pParameters = parameters;
        root.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        ComPtr<ID3DBlob> signature, errors;
        Check(D3D12SerializeRootSignature(&root, D3D_ROOT_SIGNATURE_VERSION_1,
            signature.GetAddressOf(), errors.GetAddressOf()), "Serialize outline root signature");
        Check(device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(),
            IID_PPV_ARGS(outline_root.GetAddressOf())), "Create outline root signature");
        const auto directory = std::filesystem::path(options.shader_directory);
        const auto vs = ReadShader(directory / "outline.vs.dxil");
        const auto ps = ReadShader(directory / "outline.ps.dxil");
        D3D12_INPUT_ELEMENT_DESC layout[] = {
            {"POSITION",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,0,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
            {"TEXCOORD",0,DXGI_FORMAT_R32G32_FLOAT,0,16,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
            {"TEXCOORD",1,DXGI_FORMAT_R32G32B32A32_SINT,0,24,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
            {"TEXCOORD",2,DXGI_FORMAT_R32G32B32A32_UINT,0,40,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
            {"TEXCOORD",3,DXGI_FORMAT_R32G32B32A32_UINT,0,56,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
            {"COLOR",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,72,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
            {"TEXCOORD",4,DXGI_FORMAT_R32G32_UINT,0,88,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
            {"TEXCOORD",5,DXGI_FORMAT_R32_FLOAT,0,96,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0}
        };
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
        desc.pRootSignature = outline_root.Get(); desc.VS = {vs.data(), vs.size()};
        desc.SampleMask = UINT_MAX;
        desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        desc.RasterizerState.DepthClipEnable = TRUE;
        desc.DepthStencilState.DepthEnable = TRUE;
        desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        desc.InputLayout = {layout, 8}; desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        desc.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT; desc.SampleDesc.Count = 1;
        ComPtr<ID3D12PipelineState> depth_pipeline;
        Check(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(depth_pipeline.GetAddressOf())), "Create outline terrain-depth pipeline");
        desc.PS = {ps.data(), ps.size()}; desc.NumRenderTargets = 1; desc.RTVFormats[0] = color_format;
        desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        auto& blend = desc.BlendState.RenderTarget[0];
        blend.BlendEnable = TRUE; blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        blend.DestBlend = blend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        blend.LogicOp = D3D12_LOGIC_OP_NOOP;
        for (size_t i = 0; i < 2; ++i) {
            blend.SrcBlend = blend.SrcBlendAlpha = i ? D3D12_BLEND_ONE : D3D12_BLEND_SRC_ALPHA;
            desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER;
            Check(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(outline_hidden[i].ReleaseAndGetAddressOf())), "Create hidden outline pipeline");
            desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
            Check(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(outline_visible[i].ReleaseAndGetAddressOf())), "Create visible coverage pipeline");
        }
        for (size_t premultiplied = 0; premultiplied < 2; ++premultiplied) {
            blend.SrcBlend = blend.SrcBlendAlpha = D3D12_BLEND_ONE;
            blend.DestBlend = blend.DestBlendAlpha = premultiplied ? D3D12_BLEND_INV_SRC_ALPHA : D3D12_BLEND_ONE;
            for (size_t darken = 0; darken < 2; ++darken) {
                blend.BlendOp = blend.BlendOpAlpha = darken ? D3D12_BLEND_OP_REV_SUBTRACT : D3D12_BLEND_OP_ADD;
                Check(device->CreateGraphicsPipelineState(&desc,
                    IID_PPV_ARGS(sprite_glow[premultiplied][darken].ReleaseAndGetAddressOf())), "Create sprite glow pipeline");
            }
        }
        desc.DepthStencilState.DepthEnable=FALSE;
        blend.DestBlend=blend.DestBlendAlpha=D3D12_BLEND_INV_SRC_ALPHA;
        blend.BlendOp=blend.BlendOpAlpha=D3D12_BLEND_OP_ADD;
        for(size_t i=0;i<2;++i) {
            blend.SrcBlend=blend.SrcBlendAlpha=i?D3D12_BLEND_ONE:D3D12_BLEND_SRC_ALPHA;
            Check(device->CreateGraphicsPipelineState(&desc,IID_PPV_ARGS(cursor_visible[i].ReleaseAndGetAddressOf())),"Create flat cursor pipeline");
        }
        for(size_t premultiplied=0;premultiplied<2;++premultiplied) {
            blend.SrcBlend=blend.SrcBlendAlpha=D3D12_BLEND_ONE;
            blend.DestBlend=blend.DestBlendAlpha=premultiplied?D3D12_BLEND_INV_SRC_ALPHA:D3D12_BLEND_ONE;
            for(size_t darken=0;darken<2;++darken) {
                blend.BlendOp=blend.BlendOpAlpha=darken?D3D12_BLEND_OP_REV_SUBTRACT:D3D12_BLEND_OP_ADD;
                Check(device->CreateGraphicsPipelineState(&desc,IID_PPV_ARGS(cursor_glow[premultiplied][darken].ReleaseAndGetAddressOf())),"Create flat cursor glow pipeline");
            }
        }
        // Publish readiness only after every required pipeline exists.
        outline_depth = std::move(depth_pipeline);
    }
    void CreateEnhancedWorldPipelines()
    {
        if(enhanced_terrain)return;
        D3D12_DESCRIPTOR_RANGE range{};range.RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV;range.NumDescriptors=5;
        D3D12_ROOT_PARAMETER parameters[2]{};
        parameters[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;parameters[0].DescriptorTable={1,&range};
        parameters[0].ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;
        parameters[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;parameters[1].Constants={0,0,16};
        parameters[1].ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;
        D3D12_STATIC_SAMPLER_DESC sampler{};sampler.Filter=D3D12_FILTER_MIN_MAG_MIP_POINT;
        sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.ComparisonFunc=D3D12_COMPARISON_FUNC_ALWAYS;sampler.MaxAnisotropy=1;
        sampler.MaxLOD=D3D12_FLOAT32_MAX;sampler.ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC root{};root.NumParameters=2;root.pParameters=parameters;
        root.NumStaticSamplers=1;root.pStaticSamplers=&sampler;root.Flags=D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        ComPtr<ID3DBlob> signature,errors;
        Check(D3D12SerializeRootSignature(&root,D3D_ROOT_SIGNATURE_VERSION_1,signature.GetAddressOf(),errors.GetAddressOf()),"Serialize enhanced world root");
        Check(device->CreateRootSignature(0,signature->GetBufferPointer(),signature->GetBufferSize(),
            IID_PPV_ARGS(enhanced_world_root.ReleaseAndGetAddressOf())),"Create enhanced world root");
        const auto directory=std::filesystem::path(options.shader_directory);
        const auto vs=ReadShader(directory/"enhanced-world-terrain.vs.dxil"),ps=ReadShader(directory/"enhanced-world-terrain.ps.dxil");
        const auto shadow_vs=ReadShader(directory/"enhanced-world-shadow.vs.dxil"),shadow_ps=ReadShader(directory/"enhanced-world-shadow.ps.dxil");
        D3D12_INPUT_ELEMENT_DESC layout[]={
            {"POSITION",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,0,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
            {"TEXCOORD",0,DXGI_FORMAT_R32G32_FLOAT,0,16,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
            {"TEXCOORD",1,DXGI_FORMAT_R32_FLOAT,0,24,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
            {"TEXCOORD",2,DXGI_FORMAT_R32G32_FLOAT,0,28,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
            {"TEXCOORD",3,DXGI_FORMAT_R32_UINT,0,36,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0}};
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};desc.pRootSignature=enhanced_world_root.Get();
        desc.VS={vs.data(),vs.size()};desc.PS={ps.data(),ps.size()};desc.SampleMask=UINT_MAX;
        desc.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;desc.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;
        desc.RasterizerState.DepthClipEnable=TRUE;desc.DepthStencilState.DepthEnable=TRUE;
        desc.DepthStencilState.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ALL;desc.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_ALWAYS;
        desc.InputLayout={layout,5};desc.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        desc.DSVFormat=DXGI_FORMAT_D24_UNORM_S8_UINT;desc.NumRenderTargets=1;desc.RTVFormats[0]=color_format;desc.SampleDesc.Count=1;
        auto& blend=desc.BlendState.RenderTarget[0];blend.RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
        blend.SrcBlend=blend.SrcBlendAlpha=D3D12_BLEND_ONE;blend.DestBlend=blend.DestBlendAlpha=D3D12_BLEND_ZERO;
        blend.BlendOp=blend.BlendOpAlpha=D3D12_BLEND_OP_ADD;blend.LogicOp=D3D12_LOGIC_OP_NOOP;
        ComPtr<ID3D12PipelineState> terrain;
        Check(device->CreateGraphicsPipelineState(&desc,IID_PPV_ARGS(terrain.GetAddressOf())),"Create enhanced terrain pipeline");
        D3D12_INPUT_ELEMENT_DESC shadow_layout[]={
            {"POSITION",0,DXGI_FORMAT_R32G32_FLOAT,0,0,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
            {"TEXCOORD",0,DXGI_FORMAT_R32G32_FLOAT,0,8,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
            {"TEXCOORD",1,DXGI_FORMAT_R32G32_FLOAT,0,16,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
            {"TEXCOORD",2,DXGI_FORMAT_R32G32B32A32_UINT,0,24,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0}};
        desc.InputLayout={shadow_layout,4};desc.VS={shadow_vs.data(),shadow_vs.size()};desc.PS={shadow_ps.data(),shadow_ps.size()};
        desc.DepthStencilState.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ZERO;desc.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_LESS_EQUAL;
        blend.BlendEnable=TRUE;blend.SrcBlend=blend.SrcBlendAlpha=D3D12_BLEND_ZERO;
        blend.DestBlend=D3D12_BLEND_SRC_COLOR;blend.DestBlendAlpha=D3D12_BLEND_SRC_ALPHA;
        blend.BlendOp=blend.BlendOpAlpha=D3D12_BLEND_OP_ADD;blend.LogicOp=D3D12_LOGIC_OP_NOOP;
        Check(device->CreateGraphicsPipelineState(&desc,IID_PPV_ARGS(enhanced_shadow.ReleaseAndGetAddressOf())),"Create enhanced shadow pipeline");
        enhanced_terrain=std::move(terrain);
    }
    void CreateWorldOverlayPipelines()
    {
        if (world_overlay_depth && world_overlay_flat) return;
        D3D12_DESCRIPTOR_RANGE ranges[2]{};
        ranges[0].RangeType = ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[0].NumDescriptors = 1;
        ranges[1].BaseShaderRegister = 1; ranges[1].NumDescriptors = 2;
        D3D12_ROOT_PARAMETER parameters[3]{};
        for (UINT i = 0; i < 2; ++i) {
            parameters[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            parameters[i].DescriptorTable = {1, &ranges[i]};
            parameters[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        }
        parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[2].Constants = {0, 0, 16}; parameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_STATIC_SAMPLER_DESC sampler{};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.MaxAnisotropy = 1;
        sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        sampler.MaxLOD = D3D12_FLOAT32_MAX; sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC root{};
        root.NumParameters = 3; root.pParameters = parameters;
        root.NumStaticSamplers = 1; root.pStaticSamplers = &sampler;
        root.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        ComPtr<ID3DBlob> signature, errors;
        Check(D3D12SerializeRootSignature(&root, D3D_ROOT_SIGNATURE_VERSION_1,
            signature.GetAddressOf(), errors.GetAddressOf()), "Serialize world overlay root");
        Check(device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(),
            IID_PPV_ARGS(world_overlay_root.ReleaseAndGetAddressOf())), "Create world overlay root");
        const auto directory = std::filesystem::path(options.shader_directory);
        const auto vs = ReadShader(directory / "world-overlay.vs.dxil"), ps = ReadShader(directory / "world-overlay.ps.dxil");
        const D3D12_INPUT_ELEMENT_DESC layout[] = {
            {"POSITION",0,DXGI_FORMAT_R32G32_FLOAT,0,0,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
            {"TEXCOORD",0,DXGI_FORMAT_R32G32_FLOAT,0,8,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
            {"COLOR",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,16,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
            {"TEXCOORD",1,DXGI_FORMAT_R32_FLOAT,0,32,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0}};
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = world_overlay_root.Get(); desc.InputLayout = {layout, 4};
        desc.VS = {vs.data(), vs.size()}; desc.PS = {ps.data(), ps.size()}; desc.SampleMask = UINT_MAX;
        desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        desc.RasterizerState.DepthClipEnable = TRUE;
        desc.DepthStencilState.DepthEnable = TRUE; desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        auto& blend = desc.BlendState.RenderTarget[0]; blend.BlendEnable = TRUE;
        blend.SrcBlend = blend.SrcBlendAlpha = D3D12_BLEND_SRC_ALPHA;
        blend.DestBlend = blend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD; blend.LogicOp = D3D12_LOGIC_OP_NOOP;
        blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        desc.NumRenderTargets = 1; desc.RTVFormats[0] = color_format;
        desc.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT; desc.SampleDesc.Count = 1;
        Check(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(world_overlay_depth.ReleaseAndGetAddressOf())),
            "Create depth-tested world overlay pipeline");
        desc.DepthStencilState.DepthEnable = FALSE;
        Check(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(world_overlay_flat.ReleaseAndGetAddressOf())),
            "Create flat world overlay pipeline");
    }
    void CreateLensPipeline()
    {
        if(lens_pipeline&&map_fade_pipeline)return;
        D3D12_DESCRIPTOR_RANGE range{};range.RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors=7;
        D3D12_ROOT_PARAMETER parameters[2]{};
        parameters[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[0].DescriptorTable={1,&range};parameters[0].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
        parameters[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[1].Constants={0,0,20};parameters[1].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC root{};root.NumParameters=2;root.pParameters=parameters;
        ComPtr<ID3DBlob> signature,errors;
        Check(D3D12SerializeRootSignature(&root,D3D_ROOT_SIGNATURE_VERSION_1,
            signature.GetAddressOf(),errors.GetAddressOf()),"Serialize lens root");
        Check(device->CreateRootSignature(0,signature->GetBufferPointer(),signature->GetBufferSize(),
            IID_PPV_ARGS(lens_root.ReleaseAndGetAddressOf())),"Create lens root");
        const auto directory=std::filesystem::path(options.shader_directory);
        const auto vs=ReadShader(directory/"lens.vs.dxil"),ps=ReadShader(directory/"lens.ps.dxil");
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};desc.pRootSignature=lens_root.Get();
        desc.VS={vs.data(),vs.size()};desc.PS={ps.data(),ps.size()};desc.SampleMask=UINT_MAX;
        desc.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;desc.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;
        desc.RasterizerState.DepthClipEnable=TRUE;
        desc.DepthStencilState.DepthEnable=FALSE;desc.DepthStencilState.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ZERO;
        desc.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_ALWAYS;
        auto& blend=desc.BlendState.RenderTarget[0];blend.RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
        blend.SrcBlend=blend.SrcBlendAlpha=D3D12_BLEND_ONE;blend.DestBlend=blend.DestBlendAlpha=D3D12_BLEND_ZERO;
        blend.BlendOp=blend.BlendOpAlpha=D3D12_BLEND_OP_ADD;blend.LogicOp=D3D12_LOGIC_OP_NOOP;
        desc.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        desc.NumRenderTargets=1;desc.RTVFormats[0]=color_format;desc.SampleDesc.Count=1;
        Check(device->CreateGraphicsPipelineState(&desc,IID_PPV_ARGS(lens_pipeline.ReleaseAndGetAddressOf())),
            "Create lens pipeline");
        const auto map_ps=ReadShader(directory/"mapfade.ps.dxil");
        desc.PS={map_ps.data(),map_ps.size()};
        Check(device->CreateGraphicsPipelineState(&desc,IID_PPV_ARGS(map_fade_pipeline.ReleaseAndGetAddressOf())),
            "Create map-fade pipeline");
    }
    void CreatePalettePipelines()
    {
        if (palette_serial) return;
        D3D12_DESCRIPTOR_RANGE ranges[2] = {};
        ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        for (UINT i = 0; i < 2; ++i) {
            ranges[i].NumDescriptors = 1;
            ranges[i].OffsetInDescriptorsFromTableStart = i;
        }
        D3D12_ROOT_PARAMETER parameter = {};
        parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameter.DescriptorTable = {2, ranges};
        parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC root = {};
        root.NumParameters = 1; root.pParameters = &parameter;
        root.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        ComPtr<ID3DBlob> signature, errors;
        Check(D3D12SerializeRootSignature(&root, D3D_ROOT_SIGNATURE_VERSION_1,
            signature.GetAddressOf(), errors.GetAddressOf()), "Serialize palette root signature");
        Check(device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(),
            IID_PPV_ARGS(palette_root.GetAddressOf())), "Create palette root signature");
        const auto directory = std::filesystem::path(options.shader_directory);
        const auto vs = ReadShader(directory / "palette.vs.dxil");
        D3D12_INPUT_ELEMENT_DESC layout[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_SINT, 0, 8, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 1, DXGI_FORMAT_R32G32_UINT, 0, 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 2, DXGI_FORMAT_R32G32B32A32_UINT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 3, DXGI_FORMAT_R32_UINT, 0, 40, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 4, DXGI_FORMAT_R32G32B32_UINT, 0, 44, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 5, DXGI_FORMAT_R32G32_UINT, 0, 56, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 6, DXGI_FORMAT_R32G32B32_FLOAT, 0, 64, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 7, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 76, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}
        };
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
        desc.pRootSignature = palette_root.Get(); desc.VS = {vs.data(), vs.size()};
        desc.SampleMask = UINT_MAX;
        desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        desc.RasterizerState.DepthClipEnable = TRUE;
        desc.InputLayout = {layout, 9};
        desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        desc.SampleDesc.Count = 1;
        // No render target, depth, MSAA or blending: only the indexed UAV.
        auto ps = ReadShader(directory / "palette.serial.ps.dxil");
        desc.PS = {ps.data(), ps.size()};
        Check(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(palette_serial.GetAddressOf())), "Create serial palette pipeline");
        if (diagnostics.rasterizer_ordered_views_supported) {
            ps = ReadShader(directory / "palette.ordered.ps.dxil");
            desc.PS = {ps.data(), ps.size()};
            Check(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(palette_ordered.GetAddressOf())), "Create ordered palette pipeline");
        }
    }
};

void Device::Impl::RuntimeStats()
{
    auto& s = runtime_stats;
    s.reader_hold_retained = bool(runtime_reader_hold);
    s.reader_hold_active = runtime_reader_hold && !runtime_reader_hold->released.load();
    s.reader_hold_slots = runtime_reader_hold ? runtime_reader_hold->consumed_slots : 0;
    if (runtime_reader_hold && !s.reader_hold_active && !runtime_reader_hold->release_counted) {
        ++s.reader_hold_releases; runtime_reader_hold->release_counted = true;
    }
    s.cpu_bytes = s.default_bytes = s.upload_bytes = s.readback_bytes = 0;
    s.retired_default_bytes = s.pending_upload_bytes = 0;
    s.descriptors = s.diagnostic_tickets = s.staging_pools = 0;
    s.staging_capacity_bytes = s.staging_allocation_bytes = 0;
    s.asset_count = static_cast<uint32_t>(runtime_assets.size());
    auto bytes = [](const Resource& resource) { return resource.allocation ? resource.allocation->GetSize() : 0ull; };
    // Lock/sum does not allocate: RuntimeStats also runs after ownership commit.
    // The ledger includes idle image histories even after their State retires.
    for (const auto& weak : runtime_snapshots) if (const auto snapshot = weak.lock()) s.cpu_bytes += snapshot->size();
    // Mark page liveness once. Searching every asset for every page makes
    // diagnostics quadratic and turns a larger warm cache into a CPU bottleneck.
    for (const auto& page : runtime_world_pages) page->has_current_asset = false;
    for (const auto& asset : runtime_assets) {
        if (asset->current && asset->world_page) asset->world_page->has_current_asset = true;
        s.default_bytes += bytes(asset->texture);
        if (!asset->current) s.retired_default_bytes += bytes(asset->texture);
        if (!asset->resident && !asset->world_page) s.pending_upload_bytes += asset->bytes->size();
    }
    s.world_pages = UINT(runtime_world_pages.size()); s.world_regions = 0;
    s.lightmap_images = s.lightmap_idle_images = 0;
    s.whole_images = s.whole_image_idle = 0;
    s.terrain_cpu_bytes = s.terrain_default_bytes = s.terrain_capacity_bytes = 0;
    for (const auto& page : runtime_world_pages) {
        s.default_bytes += bytes(page->texture);
        s.world_regions += UINT(page->regions.AllocationCount());
        if (page->kind == RuntimeAssetKind::LightmapR16) {
            ++s.lightmap_images;
            if (!page->regions.AllocationCount()) ++s.lightmap_idle_images;
        }
        if (page->kind == RuntimeAssetKind::Indexed8 || page->kind == RuntimeAssetKind::FadeR8
            || page->kind == RuntimeAssetKind::PaletteRGBA8 || page->kind == RuntimeAssetKind::RemapRG16UInt) {
            ++s.whole_images;
            if (!page->regions.AllocationCount()) ++s.whole_image_idle;
        }
        if (!page->has_current_asset) s.retired_default_bytes += bytes(page->texture);
    }
    for(const auto& capture:map_fade_captures)if(capture)
        s.default_bytes+=bytes(capture->world)+bytes(capture->parchment);
    for (const auto& frame : frames) {
        s.default_bytes += bytes(frame.runtime_colour);
        s.default_bytes += bytes(frame.world_colour) + bytes(frame.world_depth) + bytes(frame.world_lens_colour);
        s.upload_bytes += bytes(frame.world_geometry);
        const auto terrain_allocation = bytes(frame.world_terrain);
        s.default_bytes += terrain_allocation; s.terrain_default_bytes += terrain_allocation;
        s.terrain_capacity_bytes += frame.world_terrain_capacity;
        if (frame.world_terrain_shadow) s.terrain_cpu_bytes += frame.world_terrain_capacity;
        s.descriptors += frame.world_descriptors;
        if (frame.world_rtv) ++s.descriptors;
        if (frame.world_lens_rtv) ++s.descriptors;
        if (frame.world_dsv) s.descriptors += 2;
        const auto staging_bytes = bytes(frame.runtime_staging);
        s.upload_bytes += staging_bytes; s.staging_allocation_bytes += staging_bytes;
        s.staging_capacity_bytes += frame.runtime_staging_capacity;
        if (frame.runtime_staging.resource) ++s.staging_pools;
        if (frame.runtime_heap) s.descriptors += frame.runtime_descriptors;
    }
    for (const auto& weak : runtime_tickets) if (auto ticket = weak.lock()) {
        if (ticket->readback.resource) { s.readback_bytes += bytes(ticket->readback); ++s.diagnostic_tickets; }
    }
    s.terrain_cpu_high_water = std::max(s.terrain_cpu_high_water, s.terrain_cpu_bytes);
    s.cpu_high_water = std::max(s.cpu_high_water, s.cpu_bytes);
    s.default_high_water = std::max(s.default_high_water, s.default_bytes);
    s.upload_high_water = std::max(s.upload_high_water, s.upload_bytes);
    s.readback_high_water = std::max(s.readback_high_water, s.readback_bytes);
    s.retired_high_water = std::max(s.retired_high_water, s.retired_default_bytes);
    s.pending_high_water = std::max(s.pending_high_water, s.pending_upload_bytes);
    s.descriptor_high_water = std::max(s.descriptor_high_water, s.descriptors);
}
void Device::Impl::RuntimeCollect(bool evict_unused, bool process_trim)
{
    const auto completed = CompletedRuntimeFence();
    CollectRuntimeTiming(completed);
    if (runtime_reader_hold && runtime_reader_hold->released.load()
        && runtime_reader_hold->retirement_fence && runtime_reader_hold->retirement_fence <= completed) {
        if (!runtime_reader_hold->release_counted) ++runtime_stats.reader_hold_releases;
        runtime_reader_hold.reset();
    }
    for (auto& frame : frames) if (!frame.runtime_quarantined && frame.fence_value <= completed) {
        frame.runtime_assets = {}; frame.world_assets.clear(); frame.runtime_ticket.reset(); frame.map_fade_capture.reset();
        if (evict_unused && !process_trim) {
            ReleaseRuntimeStaging(frame, true);
            frame.runtime_colour.Reset(); frame.runtime_heap.Reset();
            frame.runtime_width = frame.runtime_height = 0;
            frame.world_colour.Reset(); frame.world_depth.Reset(); frame.world_geometry.Reset(); frame.world_lens_colour.Reset();
            frame.world_geometry_map = nullptr; frame.world_geometry_capacity = 0;
            frame.world_terrain.Reset(); frame.world_terrain_shadow.reset();
            frame.world_terrain_capacity = frame.world_terrain_valid = frame.world_terrain_epoch = 0;
            frame.world_width = frame.world_height = frame.world_descriptors = 0;
            frame.world_lens_width = frame.world_lens_height = 0;
            frame.world_heap.Reset(); frame.world_rtv.Reset(); frame.world_dsv.Reset(); frame.world_lens_rtv.Reset();
        }
    }
    if(map_fade_current&&map_fade_current->epoch!=runtime_stats.epoch)map_fade_current.reset();
    for(auto& capture:map_fade_captures)if(capture&&capture.use_count()==1)capture.reset();
    runtime_tickets.erase(std::remove_if(runtime_tickets.begin(), runtime_tickets.end(),
        [](const auto& weak) { return weak.expired(); }), runtime_tickets.end());
    for (size_t i = 0; i < runtime_assets.size();) {
        const auto& asset = runtime_assets[i];
        // Current versions are cache entries until pressure eviction. A stale
        // version is reclaimable only after both caller and GPU owners vanish.
        if (asset.use_count() != 1 || (asset->current && !evict_unused)) { ++i; continue; }
        if (asset->world_page) {
            // Release may allocate free-list metadata. Complete it before the
            // nonthrowing erase, never inside a partially compacted remove_if.
            if (!asset->world_page->regions.Release(asset->world_region))
                throw std::logic_error("Runtime page lost an owned region");
            asset->world_page.reset(); asset->world_region = {};
            ++runtime_stats.world_regions_reclaimed;
        }
        runtime_assets.erase(runtime_assets.begin() + i); ++runtime_stats.evictions;
    }
    runtime_world_pages.erase(std::remove_if(runtime_world_pages.begin(), runtime_world_pages.end(),
        [this, evict_unused, process_trim](const auto& page) {
            if (page.use_count() != 1 || (process_trim && std::find(process_preserve_pages.begin(),
                process_preserve_pages.end(), page.get()) != process_preserve_pages.end())) return false;
            if (page->kind == RuntimeAssetKind::LightmapR16) {
                const bool current_lineage = page->lightmap_key.epoch == runtime_stats.epoch
                    && std::any_of(runtime_versions.begin(), runtime_versions.end(),
                        [&](const auto& key) { return key.kind == RuntimeAssetKind::LightmapR16 && page->LightmapLineage(key); });
                return evict_unused || !current_lineage;
            }
            // Keep empty atlas and whole-image storage for replacement revisions.
            // Full allocations remain charged; pressure may reclaim idle storage.
            return evict_unused;
        }), runtime_world_pages.end());
    runtime_snapshots.erase(std::remove_if(runtime_snapshots.begin(), runtime_snapshots.end(),
        [](const auto& weak) { return weak.expired(); }), runtime_snapshots.end());
    RuntimeStats();
}
void Device::Impl::RuntimeMakeRoom(uint64_t bytes)
{
    uint64_t used = runtime_stats.cpu_bytes;
    const auto fits = [&]() {
        return bytes <= runtime_budgets.cpu_bytes && used <= runtime_budgets.cpu_bytes - bytes
            && runtime_assets.size() < runtime_budgets.assets;
    };
    // RuntimeCollect(false) already retired completed GPU readers. Only evict
    // cache-only versions; keep frame buffers, upload pools and atlas pages warm.
    const size_t probes = runtime_assets.size() * 2;
    for (size_t n = 0; !fits() && n < probes && !runtime_assets.empty(); ++n) {
        runtime_eviction_cursor %= runtime_assets.size();
        const auto& asset = runtime_assets[runtime_eviction_cursor];
        if (asset.use_count() != 1) { ++runtime_eviction_cursor; continue; }
        if (asset->recently_used) { asset->recently_used = false; ++runtime_eviction_cursor; continue; }
        if (asset->world_page) {
            if (!asset->world_page->regions.Release(asset->world_region))
                throw std::logic_error("Runtime page lost an owned region");
            asset->world_page.reset(); asset->world_region = {};
            ++runtime_stats.world_regions_reclaimed;
        }
        if (asset->bytes.use_count() == 1) used -= asset->bytes->size();
        runtime_assets.erase(runtime_assets.begin() + runtime_eviction_cursor);
        ++runtime_stats.evictions;
    }
    // Idle lightmap histories also own CPU snapshots. Drop only unshared
    // histories if still needed; the reusable GPU page remains allocated.
    for (auto& page : runtime_world_pages) {
        if (fits()) break;
        if (!page->regions.AllocationCount() && page->lightmap_baseline
            && page->lightmap_baseline.use_count() == 1) {
            used -= page->lightmap_baseline->size(); page->lightmap_baseline.reset();
        }
    }
    RuntimeStats();
}
void Device::Impl::RuntimeClearOwners()
{
    process_preserve_pages.clear();
    if (runtime_reader_hold && runtime_reader_hold->released.load() && !runtime_reader_hold->release_counted)
        ++runtime_stats.reader_hold_releases;
    runtime_reader_hold.reset();
    runtime_owner.reset();
    map_fade_current.reset(); map_fade_captures={};
    for (auto& frame : frames) {
        DiscardRuntimeTiming(frame);
        frame.runtime_assets = {}; frame.runtime_ticket.reset(); frame.map_fade_capture.reset();
        ReleaseRuntimeStaging(frame, false);
        frame.runtime_colour.Reset(); frame.runtime_heap.Reset();
        frame.runtime_width = frame.runtime_height = 0; frame.runtime_quarantined = false;
        frame.world_assets.clear(); frame.world_colour.Reset(); frame.world_depth.Reset(); frame.world_geometry.Reset(); frame.world_lens_colour.Reset();
        frame.world_geometry_map = nullptr; frame.world_geometry_capacity = 0;
        frame.world_terrain.Reset(); frame.world_terrain_shadow.reset();
        frame.world_terrain_capacity = frame.world_terrain_valid = frame.world_terrain_epoch = 0;
        frame.world_width = frame.world_height = frame.world_descriptors = 0;
        frame.world_lens_width = frame.world_lens_height = 0;
        frame.world_heap.Reset(); frame.world_rtv.Reset(); frame.world_dsv.Reset(); frame.world_lens_rtv.Reset();
    }
    for (auto& asset : runtime_assets) {
        asset->current = false; asset->texture.Reset(); asset->world_page.reset(); asset->bytes.reset();
    }
    for (auto& weak : runtime_tickets) if (auto ticket = weak.lock()) ticket->readback.Reset();
    runtime_assets.clear(); runtime_versions.clear(); runtime_tickets.clear();
    runtime_world_pages.clear(); runtime_snapshots.clear();
}
void Device::Impl::CreateRuntimeIndexedPipeline()
{
    if (runtime_indexed_pipeline) return;
    D3D12_DESCRIPTOR_RANGE ranges[2] = {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; ranges[0].NumDescriptors = 2;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; ranges[1].NumDescriptors = 1;
    ranges[1].OffsetInDescriptorsFromTableStart = 2;
    D3D12_ROOT_PARAMETER parameters[2] = {};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[0].DescriptorTable = {2, ranges};
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[1].Constants = {0, 0, 8};
    D3D12_ROOT_SIGNATURE_DESC root = {}; root.NumParameters = 2; root.pParameters = parameters;
    ComPtr<ID3DBlob> signature, errors;
    Check(D3D12SerializeRootSignature(&root, D3D_ROOT_SIGNATURE_VERSION_1,
        signature.GetAddressOf(), errors.GetAddressOf()), "Serialize runtime indexed root");
    Check(device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(),
        IID_PPV_ARGS(runtime_indexed_root.ReleaseAndGetAddressOf())), "Create runtime indexed root");
    const auto shader = ReadShader(std::filesystem::path(options.shader_directory) / "runtime-indexed-present.cs.dxil");
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};
    desc.pRootSignature = runtime_indexed_root.Get(); desc.CS = {shader.data(), shader.size()};
    Check(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(runtime_indexed_pipeline.GetAddressOf())),
        "Create runtime indexed pipeline");
}

void Device::Impl::CreateRuntimeClearPipeline()
{
    if (runtime_clear_pipeline) return;
    D3D12_ROOT_PARAMETER parameter{};
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameter.Constants = {0, 0, 4};
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC root{}; root.NumParameters = 1; root.pParameters = &parameter;
    ComPtr<ID3DBlob> signature, errors;
    Check(D3D12SerializeRootSignature(&root, D3D_ROOT_SIGNATURE_VERSION_1,
        signature.GetAddressOf(), errors.GetAddressOf()), "Serialize runtime clear root");
    Check(device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(),
        IID_PPV_ARGS(runtime_clear_root.ReleaseAndGetAddressOf())), "Create runtime clear root");
    const auto directory = std::filesystem::path(options.shader_directory);
    const auto vs = ReadShader(directory / "runtime-clear.vs.dxil"), ps = ReadShader(directory / "runtime-clear.ps.dxil");
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = runtime_clear_root.Get();
    desc.VS = {vs.data(), vs.size()}; desc.PS = {ps.data(), ps.size()}; desc.SampleMask = UINT_MAX;
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.DepthStencilState.DepthEnable = FALSE; desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    auto& blend = desc.BlendState.RenderTarget[0]; blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    blend.SrcBlend = blend.SrcBlendAlpha = D3D12_BLEND_ONE;
    blend.DestBlend = blend.DestBlendAlpha = D3D12_BLEND_ZERO;
    blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD; blend.LogicOp = D3D12_LOGIC_OP_NOOP;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1; desc.RTVFormats[0] = color_format; desc.SampleDesc.Count = 1;
    Check(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(runtime_clear_pipeline.ReleaseAndGetAddressOf())),
        "Create runtime clear pipeline");
}

void Device::Impl::CreateRuntimePresentPipeline()
{
    if (runtime_present_pipeline) return;
    D3D12_DESCRIPTOR_RANGE range{}; range.RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV; range.NumDescriptors=1;
    D3D12_ROOT_PARAMETER parameters[2]{};
    parameters[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[0].DescriptorTable={1,&range}; parameters[0].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[1].Constants={0,0,8}; parameters[1].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC root{}; root.NumParameters=2; root.pParameters=parameters;
    ComPtr<ID3DBlob> signature, errors;
    Check(D3D12SerializeRootSignature(&root, D3D_ROOT_SIGNATURE_VERSION_1,
        signature.GetAddressOf(), errors.GetAddressOf()), "Serialize runtime present root");
    Check(device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(),
        IID_PPV_ARGS(runtime_present_root.ReleaseAndGetAddressOf())), "Create runtime present root");
    const auto directory = std::filesystem::path(options.shader_directory);
    const auto vs = ReadShader(directory / "runtime-clear.vs.dxil"), ps = ReadShader(directory / "runtime-present.ps.dxil");
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = runtime_present_root.Get();
    desc.VS = {vs.data(), vs.size()}; desc.PS = {ps.data(), ps.size()}; desc.SampleMask = UINT_MAX;
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.DepthStencilState.DepthEnable = FALSE; desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    auto& blend = desc.BlendState.RenderTarget[0]; blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    blend.SrcBlend = blend.SrcBlendAlpha = D3D12_BLEND_ONE;
    blend.DestBlend = blend.DestBlendAlpha = D3D12_BLEND_ZERO;
    blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD; blend.LogicOp = D3D12_LOGIC_OP_NOOP;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1; desc.RTVFormats[0] = color_format; desc.SampleDesc.Count = 1;
    Check(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(runtime_present_pipeline.ReleaseAndGetAddressOf())),
        "Create runtime present pipeline");
}

void Device::ResetRuntimePresentationPollObservation() noexcept
{
    impl->presentation_poll_pending = false;
}

bool Device::TryAcquireRuntimePresentation()
{
    auto& r = *impl;
    if (!r.initialized) throw std::logic_error("Presentation admission before initialization");
    if (r.runtime_stats.failed || r.runtime_stats.quarantined)
        throw std::logic_error("Presentation admission after runtime submission failure");
    if (r.suspended) { r.presentation_poll_pending = false; return false; }
    // Held diagnostic readers do not present; their three independent GPU slots
    // must remain testable without consuming display readiness.
    if (!r.options.waitable_presentation || !r.options.vsync
        || (r.runtime_reader_hold && !r.runtime_reader_hold->released.load())) {
        r.presentation_poll_pending = false;
        return true;
    }
    if (r.presentation_permit) return true;
    const auto increment = [](uint64_t& value) { if (value != UINT64_MAX) ++value; };
    if (r.options.presentation_diagnostics) increment(r.presentation_diagnostics.admission_polls);
    const auto poll_time = r.options.presentation_diagnostics ? Impl::RuntimeClock::now() : Impl::RuntimeClock::time_point{};
    const DWORD result = WaitForSingleObjectEx(r.presentation_event, 0, FALSE);
    if (r.options.presentation_diagnostics && (result == WAIT_OBJECT_0 || result == WAIT_TIMEOUT)) {
        auto& stats = r.presentation_diagnostics;
        if (r.presentation_poll_pending) {
            const double gap = std::chrono::duration<double, std::milli>(poll_time - r.presentation_last_poll).count();
            Impl::ObservePresentationGap(result == WAIT_TIMEOUT ? stats.pending_poll_gaps : stats.grant_poll_gaps, gap);
            if (result == WAIT_OBJECT_0)
                Impl::ObservePresentationGap(stats.pending_wait_durations,
                    std::chrono::duration<double, std::milli>(poll_time - r.presentation_first_timeout).count());
        } else if (result == WAIT_TIMEOUT) {
            r.presentation_first_timeout = poll_time;
        }
        r.presentation_last_poll = poll_time;
        r.presentation_poll_pending = result == WAIT_TIMEOUT;
    }
    if (result == WAIT_OBJECT_0) {
        r.presentation_permit = true;
        if (r.options.presentation_diagnostics) increment(r.presentation_diagnostics.admission_grants);
        return true;
    }
    if (result == WAIT_TIMEOUT) {
        if (r.options.presentation_diagnostics) increment(r.presentation_diagnostics.admission_not_ready);
        if (r.fence->GetCompletedValue() == UINT64_MAX) {
            Check(r.device->GetDeviceRemovedReason(), "Presentation admission device removed");
            throw std::runtime_error("Presentation admission observed device-removal fence sentinel");
        }
        return false;
    }
    const DWORD error = GetLastError();
    r.presentation_poll_pending = false;
    Check(r.device->GetDeviceRemovedReason(), "Presentation admission device health");
    if (result == WAIT_FAILED) Check(HRESULT_FROM_WIN32(error), "Presentation admission event");
    throw std::runtime_error("Unexpected presentation admission event result");
}
void Device::SetRuntimePresentation(bool vsync, int32_t x, int32_t y, uint32_t width, uint32_t height)
{
    auto& r=*impl;
    if (x<0 || y<0 || !width || !height || uint64_t(x)+width>r.width || uint64_t(y)+height>r.height)
        throw std::invalid_argument("Runtime presentation rectangle outside backbuffer");
    if (r.options.waitable_presentation && r.options.vsync != vsync)
        Check(r.swapchain->SetMaximumFrameLatency(vsync ? r.options.waitable_vsync_latency : frame_count), "Set presentation latency after VSync change");
    if (r.options.vsync != vsync || !vsync) r.presentation_poll_pending = false;
    r.options.vsync=vsync; r.runtime_presentation=true;
    r.runtime_present_rect={float(x),float(y),float(width),float(height)};
}
void Device::Impl::RecordRuntimePresent(ID3D12GraphicsCommandList* list, Frame& frame,
    ID3D12Resource* source, ID3D12DescriptorHeap* heap, uint32_t slot, uint32_t source_width, uint32_t source_height)
{
    if (!runtime_presentation) {
        Transition(list,frame.backbuffer.Get(),D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyResource(frame.backbuffer.Get(),source);
        Transition(list,frame.backbuffer.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_PRESENT);
        return;
    }
    if (runtime_present_rect[0]+runtime_present_rect[2]>width || runtime_present_rect[1]+runtime_present_rect[3]>height)
        throw std::invalid_argument("Refresh runtime presentation rectangle after resize");
    const auto stride=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto cpu=CpuHandle(heap); cpu.ptr+=UINT64(slot)*stride;
    auto gpu=GpuHandle(heap); gpu.ptr+=UINT64(slot)*stride;
    D3D12_SHADER_RESOURCE_VIEW_DESC view{}; view.Format=color_format;
    view.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D; view.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;view.Texture2D.MipLevels=1;
    device->CreateShaderResourceView(source,&view,cpu);
    Transition(list,source,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Transition(list,frame.backbuffer.Get(),D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_RENDER_TARGET);
    auto target=CpuHandle(rtv_heap.Get());target.ptr+=UINT64(&frame-frames.data())*rtv_stride;
    list->OMSetRenderTargets(1,&target,FALSE,nullptr);
    D3D12_VIEWPORT viewport{0,0,float(width),float(height),0,1};D3D12_RECT scissor{0,0,LONG(width),LONG(height)};
    list->RSSetViewports(1,&viewport);list->RSSetScissorRects(1,&scissor);
    list->SetDescriptorHeaps(1,&heap);list->SetGraphicsRootSignature(runtime_present_root.Get());list->SetPipelineState(runtime_present_pipeline.Get());
    list->SetGraphicsRootDescriptorTable(0,gpu);
    list->SetGraphicsRoot32BitConstants(1,4,runtime_present_rect.data(),0);
    const uint32_t sizes[4]={source_width,source_height,0,0};list->SetGraphicsRoot32BitConstants(1,4,sizes,4);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);list->DrawInstanced(3,1,0,0);
    Transition(list,frame.backbuffer.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PRESENT);
    Transition(list,source,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_SOURCE);
}
RuntimeAssetLease Device::PublishRuntimeAsset(const RuntimeAssetKey& key, const RuntimeAssetDesc& desc,
    const std::shared_ptr<const std::vector<uint8_t>>& input)
{
    if (!input) throw std::invalid_argument("Null runtime asset bytes");
    return PublishRuntimeAsset(key, desc, std::span<const uint8_t>(*input));
}
RuntimeAssetLease Device::PublishRuntimeAsset(const RuntimeAssetKey& key, const RuntimeAssetDesc& desc,
    std::span<const uint8_t> input)
{
    auto& r = *impl;
    if (!r.initialized || r.runtime_stats.failed) throw std::logic_error("Runtime device is unavailable");
    const auto valid_id = [](uint64_t id) { return id && id < UINT64_MAX; };
    if (!valid_id(key.epoch) || !valid_id(key.logical_id) || !valid_id(key.generation) || !valid_id(key.revision)
        || key.epoch != r.runtime_stats.epoch || !desc.width || !desc.height
        || desc.width > 16384 || desc.height > 16384)
        throw std::invalid_argument("Invalid runtime asset identity or dimensions");
    uint64_t length = uint64_t(desc.width) * desc.height;
    if (key.kind == RuntimeAssetKind::PaletteRGBA8 || key.kind == RuntimeAssetKind::ColourRowRGBA8) {
        if (desc.width != 256 || desc.height != 1) throw std::invalid_argument("Runtime palette must be256x1");
        length *= 4;
    } else if (key.kind == RuntimeAssetKind::TileR8) {
        if (!TerrainTextureDimension(desc.width) || desc.width != desc.height)
            throw std::invalid_argument("Runtime tile must be square with dimension32,64,128 or256");
    } else if (IsRuntimeSprite(key.kind)) {
        if (desc.width > 1024 || desc.height > 1024) throw std::invalid_argument("Runtime sprite exceeds1024x1024");
        length *= key.kind == RuntimeAssetKind::SpriteRG8 ? 2 : 4;
    } else if (key.kind == RuntimeAssetKind::FadeR8) {
        if (desc.width != 256 || desc.height != 64) throw std::invalid_argument("Runtime fade must be256x64");
    } else if (key.kind == RuntimeAssetKind::LightmapR16) {
        if (desc.width != 511 || desc.height != 511) throw std::invalid_argument("Runtime lightmap must be511x511");
        length *= 2;
    } else if (key.kind == RuntimeAssetKind::RemapRG16UInt) {
        if (length > 16ull * 1024 * 1024) throw std::invalid_argument("Runtime lens remap exceeds64MiB");
        length *= 4;
    } else if (key.kind != RuntimeAssetKind::Indexed8 || length > 16ull * 1024 * 1024)
        throw std::invalid_argument("Invalid runtime indexed source");
    if (input.size() != length) throw std::invalid_argument("Runtime asset byte length mismatch");
    if (key.kind == RuntimeAssetKind::PaletteRGBA8)
        for (size_t i = 3; i < input.size(); i += 4)
            if (input[i] != 255) throw std::invalid_argument("Runtime palette must be opaque");
    if (IsRuntimeSprite(key.kind))
        for (size_t i = 1; i < input.size(); i += key.kind == RuntimeAssetKind::SpriteRG8 ? 2 : 4)
            if (input[i] != 0 && input[i] != 255)
                throw std::invalid_argument("Runtime sprite coverage must be binary");
    auto version = std::find_if(r.runtime_versions.begin(), r.runtime_versions.end(), [&](const auto& current) {
        return current.kind == key.kind && current.logical_id == key.logical_id;
    });
    if (version != r.runtime_versions.end() && (key.generation < version->generation
        || (key.generation == version->generation && key.revision < version->revision)))
        throw std::invalid_argument("Stale runtime asset version");
    for (const auto& asset : r.runtime_assets) if (asset->key == key) {
        if (!asset->current || !(asset->desc == desc) || !std::equal(asset->bytes->begin(), asset->bytes->end(), input.begin(), input.end()))
            throw std::invalid_argument("Conflicting runtime asset publication");
        asset->recently_used = true;
        ++r.runtime_stats.publication_reuses;
        RuntimeAssetLease lease; lease.state = asset; return lease;
    }
    if (version != r.runtime_versions.end() && *version == key)
        throw std::invalid_argument("Evicted runtime asset keys require a new revision");
    // Collect only completed readers. Cache eviction is pressure driven, and
    // cannot release a caller lease or any accepted frame's version.
    r.RuntimeCollect(false);
    if (r.runtime_stats.cpu_bytes + length > r.runtime_budgets.cpu_bytes
        || r.runtime_assets.size() >= r.runtime_budgets.assets) r.RuntimeMakeRoom(length);
    if (r.runtime_stats.cpu_bytes + length > r.runtime_budgets.cpu_bytes
        || r.runtime_assets.size() >= r.runtime_budgets.assets
        || (version == r.runtime_versions.end() && r.runtime_versions.size() >= r.runtime_budgets.assets))
        throw std::length_error("Runtime publication budget exhausted");
    auto asset = std::make_shared<RuntimeAssetLease::State>();
    asset->owner = r.runtime_owner; asset->key = key; asset->desc = desc;
    asset->bytes = std::make_shared<const std::vector<uint8_t>>(input.begin(), input.end());
    // Count the real private-copy overlap even if a later reservation fails.
    r.runtime_stats.cpu_high_water = std::max(r.runtime_stats.cpu_high_water, r.runtime_stats.cpu_bytes + length);
    // Reserve every registry before changing admission of the old version.
    r.runtime_snapshots.reserve(r.runtime_snapshots.size() + 1);
    r.runtime_assets.reserve(r.runtime_assets.size() + 1);
    const size_t version_index = static_cast<size_t>(version - r.runtime_versions.begin());
    r.runtime_versions.reserve(r.runtime_versions.size() + (version == r.runtime_versions.end() ? 1 : 0));
    r.runtime_snapshots.emplace_back(asset->bytes);
    if (version_index == r.runtime_versions.size()) r.runtime_versions.push_back(key);
    else r.runtime_versions[version_index] = key;
    for (auto& previous : r.runtime_assets)
        if (previous->key.logical_id == key.logical_id && previous->key.kind == key.kind) previous->current = false;
    r.runtime_assets.push_back(asset); ++r.runtime_stats.publications; r.runtime_config_locked = true;
    r.RuntimeStats(); RuntimeAssetLease lease; lease.state = std::move(asset); return lease;
}
void Device::SetRuntimeEpoch(uint64_t epoch)
{
    auto& r = *impl;
    if (!r.initialized || r.runtime_stats.failed) throw std::logic_error("Runtime device is unavailable");
    if (!epoch || epoch == UINT64_MAX || epoch <= r.runtime_stats.epoch)
        throw std::invalid_argument("Runtime epoch must increase without wrapping");
    for (auto& asset : r.runtime_assets) asset->current = false;
    r.runtime_versions.clear(); r.runtime_stats.epoch = epoch; r.RuntimeCollect(false);
}
void Device::ConfigureRuntimeResources(const RuntimeResourceBudgets& budgets)
{
    auto& r = *impl; const RuntimeResourceBudgets maximum;
    if (!r.initialized || r.runtime_config_locked || r.runtime_stats.failed)
        throw std::logic_error("Configure runtime budgets before publication or submission");
    if (!budgets.cpu_bytes || budgets.cpu_bytes > maximum.cpu_bytes || !budgets.default_bytes
        || budgets.default_bytes > maximum.default_bytes || !budgets.upload_bytes || budgets.upload_bytes > maximum.upload_bytes
        || !budgets.readback_bytes || budgets.readback_bytes > maximum.readback_bytes || !budgets.assets
        || budgets.assets > maximum.assets || budgets.descriptors < 3 || budgets.descriptors > maximum.descriptors
        || !budgets.diagnostic_tickets || budgets.diagnostic_tickets > maximum.diagnostic_tickets
        || !budgets.geometry_cpu_bytes || budgets.geometry_cpu_bytes > maximum.geometry_cpu_bytes)
        throw std::invalid_argument("Invalid runtime resource budget");
    r.runtime_budgets = budgets;
}
void Device::CollectRuntimeResources(bool evict_unused)
{
    if (!impl->initialized) throw std::logic_error("Device has not been initialized");
    impl->RuntimeCollect(evict_unused);
}
RuntimeResourceDiagnostics Device::GetRuntimeResourceDiagnostics()
{
    impl->RuntimeStats(); return impl->runtime_stats;
}
void Device::EnableRuntimeTimingCompletions(bool enabled)
{
    impl->timing_completions_enabled = enabled;
    impl->timing_completion_head = impl->timing_completion_count = 0;
}

size_t Device::TakeRuntimeTimingCompletions(std::span<RuntimeTimingCompletion> destination)
{
    auto& r = *impl;
    r.CollectRuntimeTiming(r.CompletedRuntimeFence());
    const size_t count = std::min(destination.size(), r.timing_completion_count);
    for (size_t i = 0; i < count; ++i) {
        destination[i] = r.timing_completions[r.timing_completion_head];
        r.timing_completion_head = (r.timing_completion_head + 1) % r.timing_completions.size();
    }
    r.timing_completion_count -= count;
    return count;
}
RuntimeTimingDiagnostics Device::GetRuntimeTimingDiagnostics()
{
    auto& r = *impl;
    if (!r.initialized) throw std::logic_error("Device has not been initialized");
    r.CollectRuntimeTiming(r.CompletedRuntimeFence());
    return r.runtime_timing;
}
void Device::ConfigureRuntimeProcessBudget(const RuntimeProcessBudgetConfig& config)
{
    auto& r = *impl;
    if (!r.initialized || r.runtime_stats.failed) throw std::logic_error("Runtime device is unavailable");
    if (static_cast<unsigned>(config.unavailable) > 1) throw std::invalid_argument("Invalid process budget policy");
    r.process_budget.config = config;
}
RuntimeProcessBudgetDiagnostics Device::GetRuntimeProcessBudgetDiagnostics()
{
    auto& r = *impl;
    if (!r.initialized) throw std::logic_error("Device has not been initialized");
    if (r.process_budget.override_active != !r.process_override.expired()) r.ProcessTelemetry(true);
    r.ProcessAllocatorStats();
    for (unsigned i = 0; i < 2; ++i) if (r.process_budget.segments[i].sample_serial)
        r.process_budget.segments[i].age_ms = Impl::RuntimeElapsed(r.process_sample_times[i]);
    return r.process_budget;
}
RuntimeProcessBudgetScope Device::OverrideRuntimeProcessBudgetForDiagnostics(const RuntimeProcessBudgetOverride& provider)
{
    auto& r = *impl;
    if (!r.initialized || r.runtime_stats.failed) throw std::logic_error("Runtime device is unavailable");
    if (!r.process_override.expired()) throw std::logic_error("Process budget diagnostic scopes cannot nest");
    RuntimeProcessBudgetScope result;
    result.state = std::make_shared<RuntimeProcessBudgetScope::State>(); result.state->provider = provider;
    r.process_override = result.state; r.ProcessTelemetry(true);
    return result;
}
void Device::SetRuntimeFailureInjection(RuntimeFailurePoint point)
{
    if (!impl->initialized || impl->runtime_stats.failed || static_cast<unsigned>(point) > 2)
        throw std::invalid_argument("Invalid runtime failure injection");
    impl->runtime_failure = point;
}
void Device::ClearRuntimeResources()
{
    auto& r = *impl;
    if (!r.initialized) throw std::logic_error("Device has not been initialized");
    r.ReleaseRuntimeReaderHold();
    r.Idle(); // Explicit teardown only; failure leaves every submitted owner pinned.
    r.CollectRuntimeTiming(r.CompletedRuntimeFence());
    r.RuntimeClearOwners(); r.runtime_owner = std::make_shared<uint8_t>(0);
    r.runtime_stats.epoch = 1; r.runtime_stats.failed = r.runtime_stats.quarantined = false;
    r.runtime_failure = RuntimeFailurePoint::None; r.runtime_config_locked = false;
    r.RuntimeStats(); r.CollectMessages();
}

bool Device::TryBeginRuntimeReaderHold(RuntimeReaderHold& output)
{
    auto& r = *impl;
    if (!r.initialized || r.runtime_stats.failed) throw std::logic_error("Runtime device is unavailable");
    if (output.Valid()) throw std::invalid_argument("Reader hold output must be empty");
    if (r.runtime_reader_hold && !r.runtime_reader_hold->released.load())
        throw std::logic_error("A diagnostic reader hold is already active");
    r.RuntimeCollect(false);
    if (r.suspended || r.runtime_reader_hold) return false;
    const auto completed = r.CompletedRuntimeFence();
    for (const auto& frame : r.frames)
        if (frame.runtime_quarantined || frame.fence_value > completed) return false;
    // Reserve an arm signal, three submitted frames and a possible drain.
    if (r.next_fence > UINT64_MAX - (frame_count + 2))
        throw std::overflow_error("Insufficient fence headroom for diagnostic reader hold");
    auto gate = std::make_shared<RuntimeReaderHold::State>();
    Check(r.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(gate->fence.GetAddressOf())),
        "Create diagnostic reader gate");
    // The queued wait itself owns a GPU reference, even with zero frames.
    // Publish its owner before Wait; a failed covering signal is not rollback.
    r.runtime_reader_hold = gate;
    r.runtime_unfenced = true;
    try {
        Check(r.queue->Wait(gate->fence.Get(), 1), "Queue diagnostic reader gate");
        ++r.runtime_stats.reader_hold_arms;
        gate->retirement_fence = r.Signal();
    } catch (...) {
        r.runtime_stats.failed = r.runtime_stats.quarantined = true;
        gate->Release(); r.RuntimeStats(); throw;
    }
    output.state = std::move(gate);
    r.RuntimeStats(); return true;
}

RuntimeSubmitResult Device::TrySubmitRuntimeWorldFrame(const RuntimeWorldFrame& packet, bool diagnostic_readback)
{
    return impl->RuntimeTimedSubmit(true, diagnostic_readback, [&] { return impl->TrySubmitRuntimeWorldFrame(packet, diagnostic_readback); });
}
RuntimeSubmitResult Device::Impl::TrySubmitRuntimeWorldFrame(const RuntimeWorldFrame& packet, bool diagnostic_readback)
{
    auto& r = *this;
    if (!r.initialized || r.runtime_stats.failed) throw std::logic_error("Runtime device is unavailable");
    ValidateRuntimeWorldFrame(packet);
    using Asset = RuntimeAssetLease::State;
    struct Binding {
        std::shared_ptr<Asset> asset;
        std::shared_ptr<RuntimeWorldPage> page;
        uint32_t x = 0, y = 0;
        RuntimePageRegion region;
        bool new_residency = false;
    };
    std::vector<Binding> bindings;
    struct Upload {
        const Binding* binding = nullptr;
        RuntimeLightmapRect rectangle;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    };
    std::vector<Upload> uploads;
    std::unordered_map<Asset*, size_t> indices;
    const auto admit = [&](const RuntimeAssetLease& lease, RuntimeAssetKind kind) {
        if (!lease.Valid() || lease.state->owner.lock() != r.runtime_owner || lease.Key().kind != kind
            || lease.Key().epoch != r.runtime_stats.epoch)
            throw std::invalid_argument("Invalid, foreign or stale runtime world lease");
        lease.state->recently_used = true;
        if (indices.emplace(lease.state.get(), bindings.size()).second)
            bindings.push_back({lease.state, lease.state->world_page, lease.state->world_region.x,
                lease.state->world_region.y, lease.state->world_region});
    };
    const auto admit_sprite = [&](const RuntimeAssetLease& lease) {
        if (!IsRuntimeSprite(lease.Key().kind)) throw std::invalid_argument("Invalid runtime sprite format");
        admit(lease, lease.Key().kind);
    };
    const bool needs_world = !packet.terrain.empty() || !packet.shadows.empty();
    const bool needs_fade = needs_world
        || std::any_of(packet.overlays.begin(), packet.overlays.end(), [](const auto& q) { return q.draw.mode == WorldOverlayMode::Fade; })
        || std::any_of(packet.post_composition.begin(), packet.post_composition.end(), [](const auto& q) { return q.draw.mode == WorldOverlayMode::Fade; });
    if (needs_world || packet.palette.state) admit(packet.palette, RuntimeAssetKind::PaletteRGBA8);
    if (needs_fade || packet.fade.state) admit(packet.fade, RuntimeAssetKind::FadeR8);
    if (needs_world || packet.lightmap.state) admit(packet.lightmap, RuntimeAssetKind::LightmapR16);
    for (const auto& item : packet.terrain) if (item.tile.state) admit(item.tile, RuntimeAssetKind::TileR8);
    for (const auto& item : packet.shadows) admit_sprite(item.source);
    for (const auto* list : {&packet.sprites,&packet.cursor_sprites}) for (const auto& item : *list) {
        admit_sprite(item.source);
        if (item.draw.kind == OutlinePassKind::NormalSprite) admit(item.colour_row, RuntimeAssetKind::ColourRowRGBA8);
        else if (item.colour_row.state) throw std::invalid_argument("Unexpected runtime sprite colour row");
    }
    for (const auto& item : packet.overlays) {
        if (item.draw.mode != WorldOverlayMode::Solid) admit(item.source, RuntimeAssetKind::Indexed8);
        else if (item.source.state) throw std::invalid_argument("Unused runtime solid overlay source lease");
        if (item.draw.mode != WorldOverlayMode::Solid && item.draw.mode != WorldOverlayMode::Colored)
            admit(item.colour_row, RuntimeAssetKind::ColourRowRGBA8);
        else if (item.colour_row.state) throw std::invalid_argument("Unused runtime overlay colour row lease");
    }
    for(const auto& item:packet.post_composition) {
        if(item.draw.mode!=WorldOverlayMode::Solid)admit(item.source,RuntimeAssetKind::Indexed8);
        else if(item.source.state)throw std::invalid_argument("Unused post-world solid source");
        if(item.draw.mode!=WorldOverlayMode::Solid&&item.draw.mode!=WorldOverlayMode::Colored)
            admit(item.colour_row,(item.kind==RuntimePostKind::Quad||item.kind==RuntimePostKind::QuadOpaque)?RuntimeAssetKind::ColourRowRGBA8:RuntimeAssetKind::PaletteRGBA8);
        else if(item.colour_row.state)throw std::invalid_argument("Unused post-world colour row");
        if(item.kind==RuntimePostKind::ImageCoverage)admit(item.coverage,RuntimeAssetKind::Indexed8);
        else if(item.coverage.state)throw std::invalid_argument("Unused post-world coverage");
    }
    LensConstants map_fade_constants{};
    if(packet.map_fade) {
        const auto& m=*packet.map_fade;
        admit(m.palette,RuntimeAssetKind::PaletteRGBA8);admit(m.fade,RuntimeAssetKind::FadeR8);
        admit(m.rgb6,RuntimeAssetKind::Indexed8);admit(m.ghost_table,RuntimeAssetKind::Indexed8);
        if(m.rgb6.Description()!=RuntimeAssetDesc{512,512}||m.ghost_table.Description()!=RuntimeAssetDesc{256,256})
            throw std::invalid_argument("Invalid map-fade auxiliary dimensions");
        map_fade_constants={{packet.native_width,packet.native_height,0,0},{packet.native_width,packet.native_height,0,0},
            {},{},{m.step,m.warp_scale,0,0}};
    }
    LensConstants lens_constants{};
    if (packet.lens) {
        const auto& lens = *packet.lens;
        const bool mist = lens.kind == LensPassKind::Mist, overlay = lens.kind == LensPassKind::Overlay;
        uint64_t auxiliary_bytes = 0;
        const auto auxiliary = [&](const RuntimeAssetLease& lease, bool required, RuntimeAssetKind kind,
            uint32_t width = 0, uint32_t height = 0) {
            if (!required) {
                if (lease.state) throw std::invalid_argument("Unused runtime lens auxiliary lease");
                return;
            }
            admit(lease, kind);
            const auto desc = lease.Description();
            if ((width && desc.width != width) || (height && desc.height != height))
                throw std::invalid_argument("Invalid runtime lens auxiliary dimensions");
            auxiliary_bytes += lease.state->bytes->size();
        };
        auxiliary(lens.palette, mist || overlay, RuntimeAssetKind::PaletteRGBA8);
        auxiliary(lens.fade, mist, RuntimeAssetKind::FadeR8);
        auxiliary(lens.rgb6, mist || overlay, RuntimeAssetKind::Indexed8, 512, 512);
        auxiliary(lens.density, mist, RuntimeAssetKind::Indexed8, 256, 256);
        auxiliary(lens.remap, lens.kind == LensPassKind::Remap, RuntimeAssetKind::RemapRG16UInt);
        auxiliary(lens.overlay, overlay, RuntimeAssetKind::Indexed8);
        if (auxiliary_bytes > 64ull * 1024 * 1024) throw std::invalid_argument("Runtime lens auxiliary bytes exceed64MiB");
        const auto remap = lens.remap.Description(), mask = lens.overlay.Description();
        lens_constants = {{packet.native_width,packet.native_height,uint32_t(lens.destination_x),uint32_t(lens.destination_y)},
            {lens.viewport_width,lens.viewport_height,uint32_t(lens.kind),lens.mist_lightness},
            {remap.width,remap.height,mask.width,mask.height}, {lens.source_x,0,int32_t(lens.overlay_alpha),0}, lens.mist_offsets};
    }
    if (r.suspended) { ++r.runtime_stats.suspended; return {RuntimeSubmitStatus::Suspended, {}}; }
    if (!r.runtime_presentation && (packet.native_width != r.width || packet.native_height != r.height))
        throw std::invalid_argument("Runtime world presentation requires matching native dimensions");
    r.RuntimeCollect(false);
    const auto hold = r.runtime_reader_hold && !r.runtime_reader_hold->released.load() ? r.runtime_reader_hold : nullptr;
    const auto pressure = []() { return RuntimeSubmitResult{RuntimeSubmitStatus::WouldBlock, {}, RuntimePressureReason::FrameSlot}; };
    if (hold && hold->consumed_slots == frame_count) { ++r.runtime_stats.slot_pressure; return pressure(); }
    const UINT current = hold ? hold->consumed_slots : r.swapchain->GetCurrentBackBufferIndex();
    auto& frame = r.frames[current];
    if (frame.runtime_quarantined || frame.fence_value > r.CompletedRuntimeFence()) {
        ++r.runtime_stats.slot_pressure; return pressure();
    }
    // The slot can complete between the collection snapshot and the readiness
    // check above. Consume that newly completed timing before reusing its pair.
    if (frame.timing_pending) r.CollectRuntimeTiming(r.CompletedRuntimeFence());
    if (frame.timing_pending) throw std::logic_error("Runtime timing slot was not retired");
    if (r.next_fence == UINT64_MAX) throw std::overflow_error("Runtime fence sequence exhausted");

    std::shared_ptr<MapFadeCapture> map_capture;
    bool new_map_capture=false;size_t map_capture_slot=0;
    if(packet.map_fade) {
        const auto& m=*packet.map_fade;
        if(r.map_fade_current&&r.map_fade_current->id==m.transition_id)map_capture=r.map_fade_current;
        else {
            if(!m.capture_pending||(r.map_fade_current&&m.transition_id<=r.map_fade_current->id))
                throw std::invalid_argument("Map-fade transition has no current immutable capture");
            while(map_capture_slot<r.map_fade_captures.size()&&r.map_fade_captures[map_capture_slot])++map_capture_slot;
            if(map_capture_slot==r.map_fade_captures.size())return pressure();
            new_map_capture=true;
        }
        if(map_capture&&(map_capture->width!=packet.native_width||map_capture->height!=packet.native_height))
            throw std::invalid_argument("Map-fade capture native dimensions changed");
    }
    // Reservations are local copies. An abandoned command list changes neither
    // page occupancy nor the submitted resource state. Live regions never move.
    struct PagePlan {
        std::shared_ptr<RuntimeWorldPage> page;
        RuntimePageRegions regions;
        bool planned = false, created = false, dirty = false, reserved = false;
    };
    std::vector<PagePlan> pages;
    for (const auto& page : r.runtime_world_pages) pages.push_back({page, {}});
    uint64_t staging_bytes = 0, source_bytes = 0, default_needed = 0;
    uint32_t created_pages = 0;
    uint64_t whole_image_reuses = 0;
    uint64_t lightmap_cold_uploads = 0, lightmap_image_reuses = 0, lightmap_identical_reuses = 0;
    uint64_t lightmap_copy_rectangles = 0, lightmap_upload_bytes = 0, lightmap_bbox_fallbacks = 0;
    const auto format = [](RuntimeAssetKind kind) {
        if (kind == RuntimeAssetKind::TileR8 || kind == RuntimeAssetKind::FadeR8 || kind == RuntimeAssetKind::Indexed8) return DXGI_FORMAT_R8_UNORM;
        if (kind == RuntimeAssetKind::SpriteRG8) return DXGI_FORMAT_R8G8_UNORM;
        if (kind == RuntimeAssetKind::LightmapR16) return DXGI_FORMAT_R16_UINT;
        if (kind == RuntimeAssetKind::RemapRG16UInt) return DXGI_FORMAT_R16G16_UINT;
        return color_format;
    };
    const auto append_upload = [&](const Binding& binding, RuntimeLightmapRect rectangle) {
        const auto& asset = *binding.asset;
        if (!rectangle.width || !rectangle.height || rectangle.x >= asset.desc.width || rectangle.y >= asset.desc.height
            || rectangle.width > asset.desc.width - rectangle.x || rectangle.height > asset.desc.height - rectangle.y)
            throw std::logic_error("Runtime upload rectangle exceeds its private source");
        const uint64_t texel_bytes = asset.bytes->size() / (uint64_t(asset.desc.width) * asset.desc.height);
        Upload upload{&binding, rectangle, {}};
        r.RuntimeUploadFootprint(TextureDesc(rectangle.width, rectangle.height, binding.page->format),
            rectangle.width * texel_bytes, upload.footprint, staging_bytes);
        uploads.push_back(upload);
        source_bytes += uint64_t(rectangle.width) * rectangle.height * texel_bytes;
    };
    for (auto& binding : bindings) {
        if (binding.page) continue;
        const auto& asset = *binding.asset;
        const auto kind = asset.key.kind;
        const bool lightmap = kind == RuntimeAssetKind::LightmapR16;
        const bool pooled = kind == RuntimeAssetKind::TileR8 || IsRuntimeSprite(kind)
            || kind == RuntimeAssetKind::ColourRowRGBA8;
        PagePlan* selected = nullptr;
        RuntimePageRegion region;
        if (lightmap) {
            uint32_t images = 0;
            for (auto& plan : pages) {
                if (!plan.page->LightmapLineage(asset.key)) continue;
                ++images;
                // Only last-lease/reader retirement releases the full region.
                // Temporary PagePlan references do not affect this ownership fact.
                if (selected || (plan.planned ? plan.regions : plan.page->regions).AllocationCount()) continue;
                if (!plan.planned) { plan.regions = plan.page->regions; plan.planned = true; }
                if (!plan.regions.TryAllocate(asset.desc.width, asset.desc.height, region))
                    throw std::logic_error("Idle lightmap image lost its full region");
                selected = &plan;
            }
            if (!selected && images >= 3)
                return {RuntimeSubmitStatus::WouldBlock, {}, RuntimePressureReason::LightmapImages};
        } else if (pooled) for (auto& plan : pages) {
            if (plan.page->kind != kind) continue;
            // Unchanged frames never copy occupancy metadata. A touched page
            // gets one transactional plan, shared by this attempt's bindings.
            if (!plan.planned) { plan.regions = plan.page->regions; plan.planned = true; }
            if (plan.regions.TryAllocate(asset.desc.width, asset.desc.height, region)) { selected = &plan; break; }
        }
        if (!lightmap && !pooled) for (auto& plan : pages) {
            if (plan.page->kind != kind || plan.page->format != format(kind)
                || plan.page->width != asset.desc.width || plan.page->height != asset.desc.height
                || (plan.planned ? plan.regions : plan.page->regions).AllocationCount()) continue;
            if (!plan.planned) { plan.regions = plan.page->regions; plan.planned = true; }
            if (!plan.regions.TryAllocate(asset.desc.width, asset.desc.height, region))
                throw std::logic_error("Idle whole image lost its full region");
            selected = &plan; ++whole_image_reuses; break;
        }
        if (!selected) {
            auto page = std::make_shared<RuntimeWorldPage>(); page->kind = kind; page->format = format(kind);
            page->width = asset.desc.width; page->height = asset.desc.height;
            if (kind == RuntimeAssetKind::TileR8) { page->width = 4096; page->height = 256; }
            if (IsRuntimeSprite(kind)) { page->width = 1024; page->height = 1024; }
            if (kind == RuntimeAssetKind::ColourRowRGBA8) { page->width = 256; page->height = 256; }
            if (lightmap) page->lightmap_key = asset.key;
            default_needed += r.AllocationSize(TextureDesc(page->width, page->height, page->format));
            pages.push_back({page, RuntimePageRegions(page->width, page->height), true, true, false, false});
            selected = &pages.back(); ++created_pages;
            if (!selected->regions.TryAllocate(asset.desc.width, asset.desc.height, region))
                throw std::logic_error("Runtime asset did not fit its fresh page");
        }
        if (!selected->created && std::find(r.process_preserve_pages.begin(), r.process_preserve_pages.end(),
            selected->page.get()) == r.process_preserve_pages.end())
            r.process_preserve_pages.push_back(selected->page.get());
        selected->reserved = true;
        binding.page = selected->page; binding.region = region;
        binding.x = region.x; binding.y = region.y; binding.new_residency = true;
        if (lightmap) {
            const auto& baseline = selected->page->lightmap_baseline;
            const auto update = PlanRuntimeLightmapUpdates(baseline ? std::span<const uint8_t>(*baseline) : std::span<const uint8_t>{}, *asset.bytes);
            for (uint32_t i = 0; i < update.count; ++i) append_upload(binding, update.rectangles[i]);
            selected->dirty = update.count != 0;
            if (selected->created) ++lightmap_cold_uploads;
            else ++lightmap_image_reuses;
            if (!update.count) ++lightmap_identical_reuses;
            lightmap_copy_rectangles += update.count; lightmap_upload_bytes += update.upload_bytes;
            if (update.bounding_box_fallback) ++lightmap_bbox_fallbacks;
        } else {
            selected->dirty = true;
            append_upload(binding, {0, 0, asset.desc.width, asset.desc.height});
        }
    }
    const auto resolve = [&](const RuntimeAssetLease& lease) -> const Binding* {
        return lease.state ? &bindings.at(indices.at(lease.state.get())) : nullptr;
    };
    struct Run {
        size_t first = 0, count = 0;
        RuntimeWorldPage* source = nullptr;
        RuntimeWorldPage* row = nullptr;
        uint32_t descriptor = 0;
    };
    std::vector<Run> terrain_runs, shadow_runs, sprite_runs, overlay_runs, cursor_runs;
    uint32_t descriptors = 0;
    const auto append = [&](std::vector<Run>& runs, size_t first, const Binding* source, const Binding* row, uint32_t count) {
        auto* a = source ? source->page.get() : nullptr; auto* b = row ? row->page.get() : nullptr;
        if (!runs.empty() && runs.back().source == a && runs.back().row == b) { ++runs.back().count; return; }
        runs.push_back({first, 1, a, b, descriptors}); descriptors += count;
    };
    for (size_t i = 0; i < packet.terrain.size(); ++i) append(terrain_runs, i, resolve(packet.terrain[i].tile), nullptr, 5);
    if (terrain_runs.empty()) { terrain_runs.push_back({0, 0, nullptr, nullptr, descriptors}); descriptors += 5; }
    for (size_t i = 0; i < packet.shadows.size(); ++i) append(shadow_runs, i, resolve(packet.shadows[i].source), nullptr, 5);
    for (size_t i = 0; i < packet.sprites.size(); ++i)
        append(sprite_runs, i, resolve(packet.sprites[i].source), resolve(packet.sprites[i].colour_row), 2);

    for (size_t i = 0; i < packet.overlays.size(); ++i)
        append(overlay_runs, i, resolve(packet.overlays[i].source), resolve(packet.overlays[i].colour_row), 3);
    const uint32_t post_descriptor=descriptors;
    descriptors+=uint32_t(packet.post_composition.size())*3;
    for(size_t i=0;i<packet.cursor_sprites.size();++i)
        append(cursor_runs,i,resolve(packet.cursor_sprites[i].source),resolve(packet.cursor_sprites[i].colour_row),2);
    const uint32_t lens_descriptor = descriptors;
    if (packet.lens) descriptors += 7;
    const uint32_t map_fade_descriptor=descriptors;
    if(packet.map_fade)descriptors+=7;
    const uint32_t present_descriptor = descriptors;
    if (r.runtime_presentation) ++descriptors;
    auto colour_desc = TextureDesc(packet.native_width, packet.native_height, color_format);
    colour_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    auto depth_desc = TextureDesc(packet.native_width, packet.native_height, DXGI_FORMAT_D24_UNORM_S8_UINT);
    depth_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    const bool new_surface = !frame.world_colour.resource || frame.world_width != packet.native_width || frame.world_height != packet.native_height;
    const bool shader_clear = !new_surface && frame.world_clear_rgba != packet.clear_rgba;
    if (new_surface) default_needed += r.AllocationSize(colour_desc) + r.AllocationSize(depth_desc);
    if(new_map_capture)default_needed+=2*r.AllocationSize(TextureDesc(packet.native_width,packet.native_height,color_format));
    const bool new_lens_surface = packet.lens && (!frame.world_lens_colour.resource
        || frame.world_lens_width != packet.native_width || frame.world_lens_height != packet.native_height);
    const bool lens_shader_clear = packet.lens && !new_lens_surface && frame.world_lens_clear_rgba != packet.lens->clear_rgba;
    if (new_lens_surface) default_needed += r.AllocationSize(colour_desc);
    const uint64_t terrain_bytes = packet.terrain.size() * 3ull * 40;
    const uint64_t shadow_bytes = packet.shadows.size() * 6ull * 40;
    const uint64_t sprite_bytes = packet.sprites.size() * 6ull * 100;
    const uint64_t overlay_bytes = packet.overlays.size() * 6ull * sizeof(WorldOverlayVertex);
    const uint64_t post_bytes=packet.post_composition.size()*6ull*sizeof(WorldOverlayVertex);
    const uint64_t cursor_bytes=packet.cursor_sprites.size()*6ull*100;
    const uint64_t geometry_bytes = terrain_bytes + shadow_bytes + sprite_bytes + overlay_bytes + post_bytes + cursor_bytes;
    const uint64_t dynamic_bytes = geometry_bytes - terrain_bytes;
    const bool new_geometry = dynamic_bytes && (!frame.world_geometry.resource || frame.world_geometry_capacity < dynamic_bytes);
    uint64_t geometry_capacity = std::max<uint64_t>(65536, frame.world_geometry_capacity);
    while (geometry_capacity < dynamic_bytes) geometry_capacity *= 2;
    const bool new_heap = !frame.world_heap || frame.world_descriptors < descriptors;
    uint32_t heap_capacity = std::max<uint32_t>(64, frame.world_descriptors);
    while (heap_capacity < descriptors) heap_capacity *= 2;
    const uint64_t geometry_allocation = new_geometry ? r.AllocationSize(BufferDesc(geometry_capacity)) : 0;
    // Retain projected terrain independently of the changing dynamic prefix.
    // Atlas addresses are resolved first, so relocation is part of exact bytes.
    struct TerrainVertex { OutlineDepthVertex position; float u, v, shade, sx, sy; uint32_t tile; };
    static_assert(sizeof(TerrainVertex) == 40);
    const auto terrain_record = [&](size_t index) noexcept {
        const auto& triangle = packet.terrain[index];
        // Every source was admitted before planning. No throwing lookup or
        // allocation is permitted when regenerating committed CPU shadow bytes.
        const auto* binding = triangle.tile.state ? &bindings[indices.find(triangle.tile.state.get())->second] : nullptr;
        const uint32_t tile = binding ? TerrainTextureReference(binding->x,binding->y,triangle.tile.Description().width)
            | (triangle.emissive_lava?0x10000000u:0u) : MissingEnhancedTile;
        std::array<TerrainVertex, 3> vertices{};
        for (size_t i = 0; i < vertices.size(); ++i) {
            const auto& v = triangle.vertices[i];
            vertices[i] = {v.position, v.u, v.v, v.shade, v.stl_x, v.stl_y, tile};
        }
        return vertices;
    };
    const bool new_terrain = terrain_bytes && (!frame.world_terrain.resource || frame.world_terrain_capacity < terrain_bytes);
    uint64_t terrain_capacity = frame.world_terrain_capacity, terrain_allocation = 0;
    if (new_terrain) {
        terrain_capacity = std::max<uint64_t>(65536, terrain_capacity);
        while (terrain_capacity < terrain_bytes) terrain_capacity *= 2;
        terrain_allocation = r.AllocationSize(BufferDesc(terrain_capacity));
        const auto fits = [&](uint64_t capacity, uint64_t allocation) {
            return r.runtime_stats.terrain_cpu_bytes <= r.runtime_budgets.geometry_cpu_bytes
                && capacity <= r.runtime_budgets.geometry_cpu_bytes - r.runtime_stats.terrain_cpu_bytes
                && r.runtime_stats.default_bytes <= r.runtime_budgets.default_bytes
                && default_needed <= r.runtime_budgets.default_bytes - r.runtime_stats.default_bytes
                && allocation <= r.runtime_budgets.default_bytes - r.runtime_stats.default_bytes - default_needed;
        };
        if (terrain_capacity != terrain_bytes && !fits(terrain_capacity, terrain_allocation)) {
            terrain_capacity = terrain_bytes;
            terrain_allocation = r.AllocationSize(BufferDesc(terrain_capacity));
        }
        if (r.runtime_stats.terrain_cpu_bytes > r.runtime_budgets.geometry_cpu_bytes
            || terrain_capacity > r.runtime_budgets.geometry_cpu_bytes - r.runtime_stats.terrain_cpu_bytes)
            return {RuntimeSubmitStatus::WouldBlock, {}, RuntimePressureReason::GeometryCpuBytes};
        default_needed += terrain_allocation;
    }
    struct TerrainRange { uint64_t offset = 0, bytes = 0, staging = 0; };
    std::array<TerrainRange, 256> terrain_ranges{};
    size_t terrain_range_count = 0;
    bool terrain_range_fallback = false;
    uint64_t terrain_compared = 0, terrain_uploaded = 0;
    const bool baseline = !new_terrain && frame.world_terrain_shadow && frame.world_terrain_epoch == r.runtime_stats.epoch;
    for (size_t i = 0; i < packet.terrain.size(); ++i) {
        const uint64_t offset = i * 120ull;
        const auto vertices = terrain_record(i);
        if (baseline && offset + sizeof(vertices) <= frame.world_terrain_valid) {
            terrain_compared += sizeof(vertices);
            if (std::memcmp(frame.world_terrain_shadow.get() + offset, vertices.data(), sizeof(vertices)) == 0) continue;
        }
        if (terrain_range_fallback) terrain_ranges[0].bytes = offset + 120 - terrain_ranges[0].offset;
        else if (terrain_range_count && terrain_ranges[terrain_range_count - 1].offset + terrain_ranges[terrain_range_count - 1].bytes == offset)
            terrain_ranges[terrain_range_count - 1].bytes += 120;
        else if (terrain_range_count < terrain_ranges.size()) terrain_ranges[terrain_range_count++] = {offset, 120, 0};
        else {
            terrain_range_fallback = true; terrain_range_count = 1;
            terrain_ranges[0].bytes = offset + 120 - terrain_ranges[0].offset;
        }
    }
    const uint64_t asset_staging_bytes = staging_bytes;
    for (size_t i = 0; i < terrain_range_count; ++i) {
        auto& range = terrain_ranges[i];
        range.staging = staging_bytes; staging_bytes += range.bytes; terrain_uploaded += range.bytes;
    }
    const auto staging_plan = r.PlanRuntimeStaging(frame, staging_bytes, geometry_allocation);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT readback_footprint{};
    uint64_t readback_bytes = 0, readback_allocation = 0;
    if (diagnostic_readback) {
        r.device->GetCopyableFootprints(&colour_desc, 0, 1, 0, &readback_footprint, nullptr, nullptr, &readback_bytes);
        readback_allocation = r.AllocationSize(BufferDesc(readback_bytes));
    }
    // Count replacement peaks, entire pages (including holes), frame surfaces,
    // persistent geometry and descriptor heaps while the old owners still live.
    const uint32_t descriptor_needed = (new_heap ? heap_capacity : 0) + (!frame.world_rtv ? 1 : 0) + (!frame.world_dsv ? 2 : 0)
        + (packet.lens && !frame.world_lens_rtv ? 1 : 0);
    const auto resource_pressure = r.RuntimePressure(default_needed, staging_plan.allocation + geometry_allocation,
        readback_allocation, descriptor_needed, diagnostic_readback);
    if (resource_pressure != RuntimePressureReason::None)
        return {RuntimeSubmitStatus::WouldBlock, {}, resource_pressure};

    // Allocation and recording remain transactional until all commands close.
    RuntimeAllocationPeaks peaks(r.runtime_stats);
    const auto allocate = [&](const D3D12_RESOURCE_DESC& desc, D3D12_HEAP_TYPE heap,
        D3D12_RESOURCE_STATES state, const wchar_t* name, const D3D12_CLEAR_VALUE* clear = nullptr,
        D3D12MA::ALLOCATION_FLAGS flags = D3D12MA::ALLOCATION_FLAG_NONE) {
        const auto reserved = r.AllocationSize(desc);
        auto resource = r.AllocateRuntime(desc, heap, state, name, clear, flags); ++r.runtime_stats.resource_allocations;
        peaks.AddResource(heap, resource.allocation->GetSize());
        if (resource.allocation->GetSize() > reserved) throw std::runtime_error("Runtime world allocation exceeded reservation");
        return resource;
    };
    r.PrewarmRuntimePipelines(true);
    for (auto& plan : pages) if (plan.created) {
        const auto& page = *plan.page;
        plan.page->texture = allocate(TextureDesc(page.width, page.height, page.format), D3D12_HEAP_TYPE_DEFAULT,
            D3D12_RESOURCE_STATE_COPY_DEST, L"Runtime immutable world page", nullptr, D3D12MA::ALLOCATION_FLAG_COMMITTED);
    }
    if(new_map_capture) {
        map_capture=std::make_shared<MapFadeCapture>();
        map_capture->epoch=r.runtime_stats.epoch;map_capture->id=packet.map_fade->transition_id;
        map_capture->width=packet.native_width;map_capture->height=packet.native_height;
        const auto desc=TextureDesc(packet.native_width,packet.native_height,color_format);
        map_capture->world=allocate(desc,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COPY_DEST,L"Runtime map-fade world capture");
        map_capture->parchment=allocate(desc,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COPY_DEST,L"Runtime map-fade parchment capture");
    }
    Resource colour, depth, geometry, lens_colour, terrain;
    std::unique_ptr<uint8_t[]> terrain_shadow;
    uint8_t* geometry_map = frame.world_geometry_map;
    if (new_terrain) {
        terrain_shadow.reset(new uint8_t[size_t(terrain_capacity)]);
        r.runtime_stats.terrain_cpu_high_water = std::max(r.runtime_stats.terrain_cpu_high_water,
            r.runtime_stats.terrain_cpu_bytes + terrain_capacity);
        terrain = allocate(BufferDesc(terrain_capacity), D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON,
            L"Runtime retained projected terrain");
    }
    if (new_surface) {
        D3D12_CLEAR_VALUE clear{}; clear.Format = color_format;
        std::copy(packet.clear_rgba.begin(), packet.clear_rgba.end(), clear.Color);
        colour = allocate(colour_desc, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_RENDER_TARGET, L"Runtime world colour", &clear);
        clear = {}; clear.Format = depth_desc.Format; clear.DepthStencil.Depth = 1;
        depth = allocate(depth_desc, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_DEPTH_WRITE, L"Runtime world D24", &clear);
    }
    if (new_lens_surface) {
        D3D12_CLEAR_VALUE clear{}; clear.Format = color_format;
        std::copy(packet.lens->clear_rgba.begin(), packet.lens->clear_rgba.end(), clear.Color);
        lens_colour = allocate(colour_desc, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_RENDER_TARGET,
            L"Runtime lens output", &clear);
    }
    if (new_geometry) {
        geometry = allocate(BufferDesc(geometry_capacity), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, L"Runtime world geometry");
        void* mapped = nullptr; D3D12_RANGE none{0, 0};
        Check(geometry.resource->Map(0, &none, &mapped), "Map runtime world geometry"); geometry_map = static_cast<uint8_t*>(mapped);
    }
    auto staging = r.PrepareRuntimeStaging(frame, staging_plan, staging_bytes, current, peaks);
    // The slot's prior fence is complete. Writes into a reused pool need no
    // rollback: every later accepted upload rewrites each referenced footprint.
    for (const auto& upload : uploads) {
        const auto& asset = *upload.binding->asset;
        const auto& rect = upload.rectangle;
        const size_t source_stride = asset.bytes->size() / asset.desc.height;
        const size_t texel_bytes = source_stride / asset.desc.width;
        for (uint32_t y = 0; y < rect.height; ++y)
            std::memcpy(staging.mapped + upload.footprint.Offset + size_t(y) * upload.footprint.Footprint.RowPitch,
                asset.bytes->data() + size_t(rect.y + y) * source_stride + size_t(rect.x) * texel_bytes,
                size_t(rect.width) * texel_bytes);
    }
    for (size_t i = 0; i < terrain_range_count; ++i) {
        const auto& range = terrain_ranges[i];
        for (uint64_t offset = 0; offset < range.bytes; offset += 120) {
            const auto vertices = terrain_record(size_t((range.offset + offset) / 120));
            std::memcpy(staging.mapped + range.staging + offset, vertices.data(), sizeof(vertices));
        }
    }
    auto ticket = std::make_shared<RuntimeFrameTicket::State>();
    ticket->owner = r.runtime_owner; ticket->width = packet.native_width; ticket->height = packet.native_height;
    ticket->footprint = readback_footprint; ticket->copy_bytes = readback_bytes;
    if (diagnostic_readback) {
        ticket->readback = allocate(BufferDesc(readback_bytes), D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, L"Runtime world selected readback");
        r.runtime_tickets.reserve(r.runtime_tickets.size() + 1);
    }
    ComPtr<ID3D12DescriptorHeap> heap, rtv, dsv, lens_rtv;
    const auto make_heap = [&](D3D12_DESCRIPTOR_HEAP_TYPE type, UINT count, bool visible, ComPtr<ID3D12DescriptorHeap>& output) {
        D3D12_DESCRIPTOR_HEAP_DESC desc{}; desc.Type = type; desc.NumDescriptors = count;
        desc.Flags = visible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        r.CheckRuntimeAllocation(r.device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(output.GetAddressOf())), "Create runtime world descriptors");
        ++r.runtime_stats.descriptor_heap_allocations;
        peaks.AddDescriptors(count);
    };
    if (new_heap) make_heap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, heap_capacity, true, heap);
    if (!frame.world_rtv) make_heap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, false, rtv);
    if (!frame.world_dsv) make_heap(D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 2, false, dsv);
    if (packet.lens && !frame.world_lens_rtv) make_heap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, false, lens_rtv);
    auto* descriptor_heap = new_heap ? heap.Get() : frame.world_heap.Get();
    auto* colour_resource = new_surface ? colour.resource.Get() : frame.world_colour.resource.Get();
    auto* depth_resource = new_surface ? depth.resource.Get() : frame.world_depth.resource.Get();
    auto colour_handle = CpuHandle(rtv ? rtv.Get() : frame.world_rtv.Get());
    auto* lens_resource = new_lens_surface ? lens_colour.resource.Get() : frame.world_lens_colour.resource.Get();
    D3D12_CPU_DESCRIPTOR_HANDLE lens_handle{};
    if (packet.lens) {
        lens_handle = CpuHandle(lens_rtv ? lens_rtv.Get() : frame.world_lens_rtv.Get());
        r.device->CreateRenderTargetView(lens_resource, nullptr, lens_handle);
    }
    auto* present_resource = packet.lens ? lens_resource : colour_resource;
    auto write_depth = CpuHandle(dsv ? dsv.Get() : frame.world_dsv.Get()), read_depth = write_depth;
    read_depth.ptr += r.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    r.device->CreateRenderTargetView(colour_resource, nullptr, colour_handle);
    D3D12_DEPTH_STENCIL_VIEW_DESC depth_view{}; depth_view.Format = depth_desc.Format; depth_view.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    r.device->CreateDepthStencilView(depth_resource, &depth_view, write_depth);
    depth_view.Flags = D3D12_DSV_FLAG_READ_ONLY_DEPTH | D3D12_DSV_FLAG_READ_ONLY_STENCIL;
    r.device->CreateDepthStencilView(depth_resource, &depth_view, read_depth);
    const UINT stride = r.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const auto srv = [&](uint32_t slot, RuntimeWorldPage* page, DXGI_FORMAT null_format) {
        auto handle = CpuHandle(descriptor_heap); handle.ptr += UINT64(slot) * stride;
        D3D12_SHADER_RESOURCE_VIEW_DESC desc{}; desc.Format = page ? page->format : null_format;
        desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        desc.Texture2D.MipLevels = 1;
        r.device->CreateShaderResourceView(page ? page->texture.resource.Get() : nullptr, &desc, handle);
    };
    const auto page_for = [&](const RuntimeAssetLease& lease) { const auto* binding = resolve(lease); return binding ? binding->page.get() : nullptr; };
    const auto world_table = [&](const Run& run, bool shadow) {
        srv(run.descriptor, shadow ? nullptr : run.source, DXGI_FORMAT_R8_UNORM);
        srv(run.descriptor + 1, page_for(packet.palette), color_format);
        srv(run.descriptor + 2, page_for(packet.lightmap), DXGI_FORMAT_R16_UINT);
        srv(run.descriptor + 3, page_for(packet.fade), DXGI_FORMAT_R8_UNORM);
        srv(run.descriptor + 4, shadow ? run.source : nullptr, color_format);
    };
    for (const auto& run : terrain_runs) world_table(run, false);
    for (const auto& run : shadow_runs) world_table(run, true);
    for (const auto& run : sprite_runs) { srv(run.descriptor, run.source, color_format); srv(run.descriptor + 1, run.row, color_format); }

    for (const auto& run : overlay_runs) {
        srv(run.descriptor, run.source, DXGI_FORMAT_R8_UNORM);
        srv(run.descriptor + 1, run.row, color_format);
        srv(run.descriptor + 2, page_for(packet.fade), DXGI_FORMAT_R8_UNORM);
    }
    for(size_t i=0;i<packet.post_composition.size();++i) {
        const auto& item=packet.post_composition[i];const auto* source=resolve(item.source);const auto* row=resolve(item.colour_row);
        const auto* auxiliary=resolve(item.kind==RuntimePostKind::ImageCoverage?item.coverage:packet.fade);
        srv(post_descriptor+uint32_t(i)*3,source?source->page.get():nullptr,DXGI_FORMAT_R8_UNORM);
        srv(post_descriptor+uint32_t(i)*3+1,row?row->page.get():nullptr,color_format);
        srv(post_descriptor+uint32_t(i)*3+2,auxiliary?auxiliary->page.get():nullptr,DXGI_FORMAT_R8_UNORM);
    }
    for(const auto& run:cursor_runs){srv(run.descriptor,run.source,color_format);srv(run.descriptor+1,run.row,color_format);}
    if (packet.lens) {
        const auto& lens = *packet.lens;
        auto handle = CpuHandle(descriptor_heap); handle.ptr += UINT64(lens_descriptor) * stride;
        D3D12_SHADER_RESOURCE_VIEW_DESC desc{}; desc.Format = color_format;
        desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        desc.Texture2D.MipLevels = 1;
        r.device->CreateShaderResourceView(colour_resource, &desc, handle);
        const auto auxiliary_srv = [&](uint32_t slot, const RuntimeAssetLease& lease, DXGI_FORMAT format) {
            const auto* binding = resolve(lease);
            srv(lens_descriptor + slot, binding ? binding->page.get() : nullptr, format);
        };
        auxiliary_srv(1, lens.palette, color_format);
        auxiliary_srv(2, lens.fade, DXGI_FORMAT_R8_UNORM);
        auxiliary_srv(3, lens.rgb6, DXGI_FORMAT_R8_UNORM);
        auxiliary_srv(4, lens.density, DXGI_FORMAT_R8_UNORM);
        auxiliary_srv(5, lens.remap, DXGI_FORMAT_R16G16_UINT);
        auxiliary_srv(6, lens.overlay, DXGI_FORMAT_R8_UNORM);
    }
    if(packet.map_fade) {
        const auto& m=*packet.map_fade;
        const auto capture_srv=[&](uint32_t slot,ID3D12Resource* resource) {
            auto handle=CpuHandle(descriptor_heap);handle.ptr+=UINT64(map_fade_descriptor+slot)*stride;
            D3D12_SHADER_RESOURCE_VIEW_DESC desc{};desc.Format=color_format;
            desc.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;desc.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            desc.Texture2D.MipLevels=1;r.device->CreateShaderResourceView(resource,&desc,handle);
        };
        capture_srv(0,map_capture->world.resource.Get());
        srv(map_fade_descriptor+1,page_for(m.palette),color_format);
        srv(map_fade_descriptor+2,page_for(m.fade),DXGI_FORMAT_R8_UNORM);
        srv(map_fade_descriptor+3,page_for(m.rgb6),DXGI_FORMAT_R8_UNORM);
        srv(map_fade_descriptor+4,page_for(m.ghost_table),DXGI_FORMAT_R8_UNORM);
        capture_srv(5,map_capture->parchment.resource.Get());
        srv(map_fade_descriptor+6,nullptr,DXGI_FORMAT_R8_UNORM);
    }
    // Dynamic geometry retains the exact existing layouts and triangulation.
    struct ShadowVertex { float x, y, u, v, z, darken; uint32_t mask_x, mask_y, width, height; };
    struct SpriteVertex {
        float x, y, z, w, local_x, local_y;
        int32_t phase_x, phase_y, step_x, step_y;
        uint32_t mask_x, mask_y, source_w, source_h, content_h, flip, kind, premultiplied;
        std::array<float, 4> colour;
        uint32_t lookup_row, texture_fallback;
        float source_alpha;
    };
    static_assert(sizeof(TerrainVertex) == 40 && sizeof(ShadowVertex) == 40 && sizeof(SpriteVertex) == 100);
    uint8_t* output = geometry_map;
    constexpr int corners[]{0, 1, 2, 0, 2, 3};
    for (const auto& shadow : packet.shadows) {
        const auto& draw = shadow.draw; const auto* binding = resolve(shadow.source);
        for (int corner : corners) {
            const auto& v = draw.vertices[corner];
            const ShadowVertex vertex{v.x, v.y, v.u, v.v, draw.z_ndc, draw.darken,
                draw.mask_x + binding->x, draw.mask_y + binding->y, draw.source_width, draw.source_height};
            std::memcpy(output, &vertex, sizeof(vertex)); output += sizeof(vertex);
        }
    }
    std::vector<OutlinePassDraw> draws; draws.reserve(packet.sprites.size());
    for (const auto& sprite : packet.sprites) {
        const auto& draw = sprite.draw; const auto* binding = resolve(sprite.source); const auto* row = resolve(sprite.colour_row);
        draws.push_back(draw);
        const auto vertex = [&](float x, float y) {
            return SpriteVertex{2.0f * (draw.x + x) / packet.viewport_width - 1.0f,
                1.0f - 2.0f * (draw.y + y) / packet.viewport_height, draw.z_ndc, 1, x, y,
                draw.phase_x, draw.phase_y, draw.step_x, draw.step_y, draw.mask_x + binding->x, draw.mask_y + binding->y,
                draw.source_width, draw.source_height, draw.content_height, uint32_t(draw.flip), uint32_t(draw.kind),
                uint32_t(draw.premultiplied), draw.colour, row ? row->y : NoSpriteColourRow,
                uint32_t(draw.texture_fallback), draw.source_alpha};
        };
        auto clip_x=SpriteRasterBounds(draw.x,draw.width,packet.viewport_width);
        auto clip_y=SpriteRasterBounds(draw.y,draw.height,packet.viewport_height);
        if(!clip_x.Visible()||!clip_y.Visible())clip_x=clip_y={};
        const SpriteVertex quad[]{vertex(float(clip_x.first),float(clip_y.first)),vertex(float(clip_x.end),float(clip_y.first)),
            vertex(float(clip_x.end),float(clip_y.end)),vertex(float(clip_x.first),float(clip_y.end))};
        const std::array<int, 6> order = draw.premultiplied ? std::array<int, 6>{0, 1, 3, 3, 1, 2} : std::array<int, 6>{0, 1, 2, 0, 2, 3};
        for (int corner : order) { std::memcpy(output, &quad[corner], sizeof(SpriteVertex)); output += sizeof(SpriteVertex); }
    }
    std::vector<WorldOverlayQuad> overlays; overlays.reserve(packet.overlays.size());
    for (const auto& item : packet.overlays) {
        auto draw = item.draw;
        const auto* row = resolve(item.colour_row);
        draw.colour_row = row ? row->y : NoWorldOverlayResource;
        overlays.push_back(draw);
        const WorldOverlayVertex quad[]{
            {draw.x0,draw.y0,draw.u0,draw.v0,draw.colour,draw.z_ndc},
            {draw.x1,draw.y0,draw.u1,draw.v0,draw.colour,draw.z_ndc},
            {draw.x1,draw.y1,draw.u1,draw.v1,draw.colour,draw.z_ndc},
            {draw.x0,draw.y1,draw.u0,draw.v1,draw.colour,draw.z_ndc}};
        for (int corner : corners) {
            std::memcpy(output, &quad[corner], sizeof(WorldOverlayVertex)); output += sizeof(WorldOverlayVertex);
        }
    }
    std::vector<WorldOverlayQuad> post_draws;post_draws.reserve(packet.post_composition.size());
    for(const auto& item:packet.post_composition) {
        auto draw=item.draw;const auto* row=resolve(item.colour_row);draw.colour_row=row?row->y:NoWorldOverlayResource;
        if(item.kind!=RuntimePostKind::Quad)draw.mode=WorldOverlayMode(item.kind==RuntimePostKind::ImageZoom?8:item.kind==RuntimePostKind::ImageTransparent?6:item.kind==RuntimePostKind::ImageCoverage?7:5);
        post_draws.push_back(draw);
        const WorldOverlayVertex quad[]{
            {draw.x0,draw.y0,draw.u0,draw.v0,draw.colour,draw.z_ndc},{draw.x1,draw.y0,draw.u1,draw.v0,draw.colour,draw.z_ndc},
            {draw.x1,draw.y1,draw.u1,draw.v1,draw.colour,draw.z_ndc},{draw.x0,draw.y1,draw.u0,draw.v1,draw.colour,draw.z_ndc}};
        for(int corner:corners){std::memcpy(output,&quad[corner],sizeof(WorldOverlayVertex));output+=sizeof(WorldOverlayVertex);}
    }
    std::vector<OutlinePassDraw> cursor_draws;cursor_draws.reserve(packet.cursor_sprites.size());
    for(const auto& sprite:packet.cursor_sprites) {
        const auto& draw=sprite.draw;const auto* binding=resolve(sprite.source);const auto* row=resolve(sprite.colour_row);cursor_draws.push_back(draw);
        const auto vertex=[&](float x,float y) {
            return SpriteVertex{2.0f*(draw.x+x)/packet.native_width-1.0f,1.0f-2.0f*(draw.y+y)/packet.native_height,draw.z_ndc,1,x,y,
                draw.phase_x,draw.phase_y,draw.step_x,draw.step_y,draw.mask_x+binding->x,draw.mask_y+binding->y,draw.source_width,draw.source_height,
                draw.content_height,uint32_t(draw.flip),uint32_t(draw.kind),uint32_t(draw.premultiplied),draw.colour,row?row->y:NoSpriteColourRow,uint32_t(draw.texture_fallback),draw.source_alpha};
        };
        auto clip_x=SpriteRasterBounds(draw.x,draw.width,packet.native_width);
        auto clip_y=SpriteRasterBounds(draw.y,draw.height,packet.native_height);
        if(!clip_x.Visible()||!clip_y.Visible())clip_x=clip_y={};
        const SpriteVertex quad[]{vertex(float(clip_x.first),float(clip_y.first)),vertex(float(clip_x.end),float(clip_y.first)),
            vertex(float(clip_x.end),float(clip_y.end)),vertex(float(clip_x.first),float(clip_y.end))};
        const std::array<int,6> order=draw.premultiplied?std::array<int,6>{0,1,3,3,1,2}:std::array<int,6>{0,1,2,0,2,3};
        for(int corner:order){std::memcpy(output,&quad[corner],sizeof(SpriteVertex));output+=sizeof(SpriteVertex);}
    }
    std::vector<std::shared_ptr<Asset>> owners; owners.reserve(bindings.size());
    for (const auto& binding : bindings) owners.push_back(binding.asset);
    r.runtime_world_pages.reserve(r.runtime_world_pages.size() + created_pages);
    auto* commands = frame.runtime_commands.Get();
    Check(frame.allocator->Reset(), "Reset runtime world allocator");
    Check(commands->Reset(frame.allocator.Get(), nullptr), "Reset runtime world commands");
    r.BeginRuntimeTiming(frame);
    auto* terrain_resource = new_terrain ? terrain.resource.Get() : frame.world_terrain.resource.Get();
    if (terrain_range_count) {
        // New buffers start in COMMON, and reused buffers decay to COMMON after
        // ExecuteCommandLists. This slot's prior fence is complete. Unchanged
        // buffers promote implicitly on vertex reads.
        Transition(commands, terrain_resource, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        for (size_t i = 0; i < terrain_range_count; ++i) {
            const auto& range = terrain_ranges[i];
            commands->CopyBufferRegion(terrain_resource, range.offset, staging.resource, range.staging, range.bytes);
        }
        Transition(commands, terrain_resource, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
    }
    const auto readable = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    for (const auto& plan : pages) if (plan.dirty && !plan.created)
        Transition(commands, plan.page->texture.resource.Get(), readable, D3D12_RESOURCE_STATE_COPY_DEST);
    for (const auto& upload : uploads) {
        const auto& binding = *upload.binding;
        D3D12_TEXTURE_COPY_LOCATION from{}, to{};
        from.pResource = staging.resource; from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; from.PlacedFootprint = upload.footprint;
        to.pResource = binding.page->texture.resource.Get(); to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        commands->CopyTextureRegion(&to, binding.x + upload.rectangle.x, binding.y + upload.rectangle.y, 0, &from, nullptr);
    }
    for (const auto& plan : pages) if (plan.dirty)
        Transition(commands, plan.page->texture.resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST, readable);
    if (!new_surface) {
        Transition(commands, colour_resource, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        Transition(commands, depth_resource, D3D12_RESOURCE_STATE_DEPTH_READ, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    }
    const WorldSurfaceTarget target{depth_resource, colour_handle, write_depth, read_depth,
        {float(packet.viewport_x), float(packet.viewport_y), float(packet.viewport_width), float(packet.viewport_height), 0, 1},
        {packet.viewport_x, packet.viewport_y, LONG(packet.viewport_x + packet.viewport_width), LONG(packet.viewport_y + packet.viewport_height)}};
    std::array<float, 16> constants{};
    for (size_t i = 0; i < 9; ++i) constants[i] = packet.shade_settings[i];
    constants[9] = packet.time; constants[10] = float(packet.fade_addressing);
    constants[11] = float(r.options.lighting_remaster);
    constants[12] = float(packet.viewport_width); constants[13] = float(packet.viewport_height); constants[15] = float(packet.native_height);
    std::array<float, 12> matrix{};
    for (size_t row = 0; row < 3; ++row) for (size_t col = 0; col < 3; ++col) matrix[row * 4 + col] = packet.palette_transform[row * 3 + col];
    uint32_t recorded_overlays = 0;
    auto* dynamic_resource = new_geometry ? geometry.resource.Get() : frame.world_geometry.resource.Get();
    const auto address = dynamic_resource ? dynamic_resource->GetGPUVirtualAddress() : 0;
    const auto terrain_address = terrain_resource ? terrain_resource->GetGPUVirtualAddress() : 0;
    const auto table = [&](uint32_t slot) { auto handle = GpuHandle(descriptor_heap); handle.ptr += UINT64(slot) * stride; return handle; };
    {
        // The common record core targets this frame's persistent command list.
        // Restore the diagnostic list even when recording raises an exception.
        struct Restore { ComPtr<ID3D12GraphicsCommandList>& target; ComPtr<ID3D12GraphicsCommandList> previous;
            ~Restore() { target = std::move(previous); } } restore{r.commands, r.commands};
        r.commands = frame.runtime_commands;
        const auto shader_clear_target = [&](D3D12_CPU_DESCRIPTOR_HANDLE handle, const std::array<float,4>& rgba) {
            const D3D12_VIEWPORT viewport{0,0,float(packet.native_width),float(packet.native_height),0,1};
            const D3D12_RECT scissor{0,0,LONG(packet.native_width),LONG(packet.native_height)};
            commands->RSSetViewports(1, &viewport); commands->RSSetScissorRects(1, &scissor);
            commands->OMSetRenderTargets(1, &handle, FALSE, nullptr);
            commands->SetGraphicsRootSignature(r.runtime_clear_root.Get());
            commands->SetPipelineState(r.runtime_clear_pipeline.Get());
            commands->SetGraphicsRoot32BitConstants(0, 4, rgba.data(), 0);
            commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            commands->DrawInstanced(3, 1, 0, 0);
        };
        if (shader_clear) shader_clear_target(colour_handle, packet.clear_rgba);
        r.RecordSurfaceBegin(target, shader_clear ? nullptr : packet.clear_rgba.data());
        for (size_t i = 0; i < terrain_runs.size(); ++i) {
            const auto& run = terrain_runs[i];
            const D3D12_VERTEX_BUFFER_VIEW view{terrain_address + run.first * 120, UINT(run.count * 120), 40};
            r.RecordEnhancedWorld(target, descriptor_heap, table(run.descriptor), constants, view, {}, i + 1 == terrain_runs.size(), false);
        }
        for (const auto& run : shadow_runs) {
            const D3D12_VERTEX_BUFFER_VIEW view{address + run.first * 240, UINT(run.count * 240), 40};
            r.RecordEnhancedWorld(target, descriptor_heap, table(run.descriptor), constants, {}, view, true, true);
        }
        for (const auto& run : sprite_runs) {
            const D3D12_VERTEX_BUFFER_VIEW view{address + shadow_bytes + run.first * 600, UINT(run.count * 600), 100};
            r.RecordOutlineSprites(target, descriptor_heap, table(run.descriptor), matrix, view, 0, true,
                std::span<const OutlinePassDraw>(draws.data() + run.first, run.count));
        }
        for (const auto& run : overlay_runs) {
            const D3D12_VERTEX_BUFFER_VIEW view{address + shadow_bytes + sprite_bytes
                + run.first * 6 * sizeof(WorldOverlayVertex), UINT(run.count * 6 * sizeof(WorldOverlayVertex)), sizeof(WorldOverlayVertex)};
            recorded_overlays += r.RecordWorldOverlays(target, descriptor_heap, table(run.descriptor), table(run.descriptor + 1),
                stride, 0, view, packet.native_width, packet.native_height,
                std::span<const WorldOverlayQuad>(overlays.data() + run.first, run.count));
        }
        if (packet.lens) {
            Transition(commands, colour_resource, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            if (!new_lens_surface)
                Transition(commands, lens_resource, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
            if (lens_shader_clear) shader_clear_target(lens_handle, packet.lens->clear_rgba);
            else commands->ClearRenderTargetView(lens_handle, packet.lens->clear_rgba.data(), 0, nullptr);
            r.RecordLens(lens_handle, descriptor_heap, table(lens_descriptor), lens_constants);

            Transition(commands, colour_resource, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
        }
        auto post_target=target;post_target.colour=packet.lens?lens_handle:colour_handle;
        post_target.viewport={0,0,float(packet.native_width),float(packet.native_height),0,1};
        post_target.scissor={0,0,LONG(packet.native_width),LONG(packet.native_height)};
        const auto capture_image=[&](Resource& destination) {
            Transition(commands,present_resource,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE);
            commands->CopyResource(destination.resource.Get(),present_resource);
            Transition(commands,destination.resource.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            Transition(commands,present_resource,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);
        };
        if(new_map_capture) {
            capture_image(map_capture->world);
            shader_clear_target(post_target.colour,{0,0,0,1});
            Transition(commands,depth_resource,D3D12_RESOURCE_STATE_DEPTH_READ,D3D12_RESOURCE_STATE_DEPTH_WRITE);
            commands->ClearDepthStencilView(write_depth,D3D12_CLEAR_FLAG_DEPTH|D3D12_CLEAR_FLAG_STENCIL,1,0,0,nullptr);
            Transition(commands,depth_resource,D3D12_RESOURCE_STATE_DEPTH_WRITE,D3D12_RESOURCE_STATE_DEPTH_READ);
        }
        bool map_fade_resolved=false;
        const auto resolve_map_fade=[&] {
            if(!packet.map_fade||map_fade_resolved)return;
            if(new_map_capture)capture_image(map_capture->parchment);
            commands->SetGraphicsRootSignature(r.lens_root.Get());commands->SetPipelineState(r.map_fade_pipeline.Get());
            ID3D12DescriptorHeap* heaps[]{descriptor_heap};commands->SetDescriptorHeaps(1,heaps);
            commands->SetGraphicsRootDescriptorTable(0,table(map_fade_descriptor));
            commands->SetGraphicsRoot32BitConstants(1,20,&map_fade_constants,0);
            commands->OMSetRenderTargets(1,&post_target.colour,FALSE,nullptr);
            commands->RSSetViewports(1,&post_target.viewport);commands->RSSetScissorRects(1,&post_target.scissor);
            commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);commands->DrawInstanced(3,1,0,0);
            map_fade_resolved=true;
        };
        const auto record_cursor=[&] {
            commands->RSSetViewports(1,&post_target.viewport);commands->RSSetScissorRects(1,&post_target.scissor);
            for(const auto& run:cursor_runs) {
                const D3D12_VERTEX_BUFFER_VIEW view{address+shadow_bytes+sprite_bytes+overlay_bytes+post_bytes+run.first*600,UINT(run.count*600),100};
                r.RecordOutlineSprites(post_target,descriptor_heap,table(run.descriptor),matrix,view,0,true,
                    std::span<const OutlinePassDraw>(cursor_draws.data()+run.first,run.count),true);
            }
        };
        const bool clear_images=std::any_of(packet.post_composition.begin(),packet.post_composition.end(),[](const auto& item){return item.kind==RuntimePostKind::ImageOpaque||item.kind==RuntimePostKind::ImageZoom;});
        bool cleared_images=false,cursor_recorded=false;
        for(size_t i=0;i<post_draws.size();++i) {
            const auto& item=packet.post_composition[i];
            if(item.phase>=RuntimePostPhase::ZoomBoxes)resolve_map_fade();
            if(item.phase>=RuntimePostPhase::ParchmentWorld&&item.phase<=RuntimePostPhase::ParchmentFront&&!new_map_capture)continue;
            if(clear_images&&!cleared_images&&item.phase==RuntimePostPhase::ImagePresents) {
                shader_clear_target(post_target.colour,{0,0,0,1});cleared_images=true;
            }
            if(item.phase==RuntimePostPhase::Cursor&&!cursor_recorded){record_cursor();cursor_recorded=true;}
            const D3D12_VERTEX_BUFFER_VIEW view{address+shadow_bytes+sprite_bytes+overlay_bytes+i*6*sizeof(WorldOverlayVertex),6*sizeof(WorldOverlayVertex),sizeof(WorldOverlayVertex)};
            const auto descriptor=post_descriptor+uint32_t(i)*3;
            recorded_overlays+=r.RecordWorldOverlays(post_target,descriptor_heap,table(descriptor),table(descriptor+1),stride,0,view,
                packet.native_width,packet.native_height,std::span<const WorldOverlayQuad>(&post_draws[i],1),&item);
        }
        resolve_map_fade();
        if(!cursor_recorded)record_cursor();
        Transition(commands,present_resource,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE);
    }

    r.EndRuntimeTiming(frame);
    if (diagnostic_readback) {
        D3D12_TEXTURE_COPY_LOCATION from{}, to{};
        from.pResource = present_resource; from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        to.pResource = ticket->readback.resource.Get(); to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; to.PlacedFootprint = readback_footprint;
        commands->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    }
    if (!hold) r.RecordRuntimePresent(commands, frame, present_resource, descriptor_heap, present_descriptor, packet.native_width, packet.native_height);
    Check(commands->Close(), "Close runtime world commands");
    if (r.runtime_failure == RuntimeFailurePoint::BeforeExecute) {
        r.runtime_failure = RuntimeFailurePoint::None; throw std::runtime_error("Injected runtime failure before ExecuteCommandLists");
    }
    // No throwing allocation after publishing the frame owners. On an unfenced
    // execution failure, quarantine retains every page, staging range and target.
    r.ArmRuntimeTiming(frame, true, diagnostic_readback, bool(hold));
    frame.world_assets = std::move(owners); frame.runtime_ticket = ticket;
    frame.map_fade_capture=map_capture;
    if(new_map_capture)r.map_fade_captures[map_capture_slot]=map_capture;
    r.CommitRuntimeStaging(frame, staging, staging_plan, staging_bytes);
    frame.frozen_reader.reset();
    if (new_surface) {
        frame.world_colour = std::move(colour); frame.world_depth = std::move(depth);
        frame.world_clear_rgba = packet.clear_rgba;
    }
    frame.world_width = packet.native_width; frame.world_height = packet.native_height;
    if (packet.lens) {
        if (new_lens_surface) {
            frame.world_lens_colour = std::move(lens_colour);
            frame.world_lens_clear_rgba = packet.lens->clear_rgba;
        }
        frame.world_lens_width = packet.native_width; frame.world_lens_height = packet.native_height;
        if (lens_rtv) frame.world_lens_rtv = std::move(lens_rtv);
    }
    if (new_geometry) { frame.world_geometry = std::move(geometry); frame.world_geometry_map = geometry_map; frame.world_geometry_capacity = geometry_capacity; }
    if (new_terrain) {
        frame.world_terrain = std::move(terrain); frame.world_terrain_shadow = std::move(terrain_shadow);
        frame.world_terrain_capacity = terrain_capacity;
    }
    for (size_t i = 0; i < terrain_range_count; ++i) {
        const auto& range = terrain_ranges[i];
        for (uint64_t offset = 0; offset < range.bytes; offset += 120) {
            const auto vertices = terrain_record(size_t((range.offset + offset) / 120));
            std::memcpy(frame.world_terrain_shadow.get() + range.offset + offset, vertices.data(), sizeof(vertices));
        }
    }
    frame.world_terrain_valid = terrain_bytes; frame.world_terrain_epoch = r.runtime_stats.epoch;
    if (new_heap) { frame.world_heap = std::move(heap); frame.world_descriptors = heap_capacity; }
    if (rtv) frame.world_rtv = std::move(rtv);
    if (dsv) frame.world_dsv = std::move(dsv);
    if (diagnostic_readback) r.runtime_tickets.emplace_back(ticket);
    for (auto& plan : pages) if (plan.reserved) {
        plan.page->regions.Swap(plan.regions);
        if (plan.created) r.runtime_world_pages.push_back(plan.page);
    }
    for (const auto& binding : bindings) if (binding.new_residency) {
        binding.asset->world_page = binding.page; binding.asset->world_region = binding.region;
        if (binding.asset->key.kind == RuntimeAssetKind::LightmapR16) {
            // Identical revisions still acquire ownership and update history.
            // Shared immutable storage makes this commit nonallocating.
            binding.page->lightmap_key = binding.asset->key;
            binding.page->lightmap_baseline = binding.asset->bytes;
        }
        ++r.runtime_stats.world_region_allocations;
    }
    r.runtime_stats.whole_image_reuses += whole_image_reuses;
    r.runtime_stats.lightmap_cold_uploads += lightmap_cold_uploads;
    r.runtime_stats.lightmap_image_reuses += lightmap_image_reuses;
    r.runtime_stats.lightmap_identical_reuses += lightmap_identical_reuses;
    r.runtime_stats.lightmap_copy_rectangles += lightmap_copy_rectangles;
    r.runtime_stats.lightmap_upload_bytes += lightmap_upload_bytes;
    r.runtime_stats.lightmap_bbox_fallbacks += lightmap_bbox_fallbacks;
    r.runtime_stats.world_upload_bytes += source_bytes; r.runtime_stats.aligned_upload_bytes += asset_staging_bytes;
    r.runtime_stats.geometry_upload_bytes += dynamic_bytes + terrain_uploaded;
    r.runtime_stats.terrain_compared_bytes += terrain_compared;
    r.runtime_stats.terrain_upload_bytes += terrain_uploaded;
    r.runtime_stats.terrain_reused_bytes += terrain_bytes - terrain_uploaded;
    r.runtime_stats.terrain_dirty_ranges += terrain_range_count;
    frame.runtime_quarantined = true; r.runtime_stats.quarantined = true; r.runtime_config_locked = true;
    RuntimeSubmitResult result{RuntimeSubmitStatus::Submitted, {}}; result.ticket.state = ticket;
    result.submission_id = frame.timing_completion.submission_id;
    r.RuntimeStats();
    ID3D12CommandList* lists[]{commands}; r.Execute(1, lists);
    if (hold) { ++hold->consumed_slots; ++r.runtime_stats.held_submissions; }
    try {
        if (r.runtime_failure == RuntimeFailurePoint::AfterExecute) {
            r.runtime_failure = RuntimeFailurePoint::None; throw std::runtime_error("Injected runtime failure after ExecuteCommandLists");
        }
        frame.fence_value = r.Signal(); ticket->fence_value = frame.fence_value;
        frame.timing_fence = frame.fence_value;
        if (!hold) {
            r.RuntimePresent("Present runtime world frame");
            ++r.runtime_stats.presentations; ++r.diagnostics.frames;
        }
        r.CollectMessages();
    } catch (...) { r.runtime_stats.failed = true; throw; }
    frame.runtime_quarantined = false; r.runtime_stats.quarantined = false;
    if(new_map_capture)r.map_fade_current=map_capture;
    ++r.runtime_stats.submissions; ++r.runtime_stats.world_submissions;
    r.runtime_stats.command_bytes += sizeof(constants) * (terrain_runs.size() + shadow_runs.size()) + sizeof(matrix) * sprite_runs.size()
        + sizeof(WorldOverlayConstants) * recorded_overlays;
    if (shader_clear) { ++r.runtime_stats.world_shader_clears; r.runtime_stats.command_bytes += sizeof(packet.clear_rgba); }
    if (packet.lens) r.runtime_stats.command_bytes += sizeof(lens_constants);
    if (lens_shader_clear) { ++r.runtime_stats.world_shader_clears; r.runtime_stats.command_bytes += sizeof(packet.lens->clear_rgba); }
    return result;
}

RuntimeSubmitResult Device::TrySubmitRuntimeIndexedFrame(const RuntimeIndexedFrame& packet, bool diagnostic_readback)
{
    return impl->RuntimeTimedSubmit(false, diagnostic_readback, [&] { return impl->TrySubmitRuntimeIndexedFrame(packet, diagnostic_readback); });
}
RuntimeSubmitResult Device::Impl::TrySubmitRuntimeIndexedFrame(const RuntimeIndexedFrame& packet, bool diagnostic_readback)
{
    auto& r = *this;
    if (!r.initialized || r.runtime_stats.failed) throw std::logic_error("Runtime device is unavailable");
    const auto validate = [&](const RuntimeAssetLease& lease, RuntimeAssetKind kind) {
        if (!lease.Valid() || lease.state->owner.lock() != r.runtime_owner || lease.state->key.kind != kind
            || lease.state->key.epoch != r.runtime_stats.epoch)
            throw std::invalid_argument("Invalid, foreign or stale runtime asset lease");
        return lease.state;
    };
    std::array<std::shared_ptr<RuntimeAssetLease::State>, 2> assets = {
        validate(packet.indices, RuntimeAssetKind::Indexed8), validate(packet.palette, RuntimeAssetKind::PaletteRGBA8)};
    if (!packet.native_width || !packet.native_height || packet.native_width > 16384 || packet.native_height > 16384
        || uint64_t(packet.native_width) * packet.native_height > 16ull * 1024 * 1024
        || packet.x < 0 || packet.y < 0 || !packet.width || !packet.height
        || uint64_t(packet.x) + packet.width > packet.native_width || uint64_t(packet.y) + packet.height > packet.native_height
        || assets[0]->desc.width != packet.width || assets[0]->desc.height != packet.height)
        throw std::invalid_argument("Invalid runtime indexed frame rectangle");
    if (r.suspended) { ++r.runtime_stats.suspended; return {RuntimeSubmitStatus::Suspended, {}}; }
    if (!r.runtime_presentation && (packet.native_width != r.width || packet.native_height != r.height))
        throw std::invalid_argument("Runtime presentation requires matching native dimensions");
    r.RuntimeCollect(false);
    const auto hold = r.runtime_reader_hold && !r.runtime_reader_hold->released.load()
        ? r.runtime_reader_hold : nullptr;
    const auto pressure = []() { return RuntimeSubmitResult{RuntimeSubmitStatus::WouldBlock, {}, RuntimePressureReason::FrameSlot}; };
    if (hold && hold->consumed_slots == frame_count) { ++r.runtime_stats.slot_pressure; return pressure(); }
    const UINT current = hold ? hold->consumed_slots : r.swapchain->GetCurrentBackBufferIndex();
    auto& frame = r.frames[current];
    auto* commands = frame.runtime_commands.Get();
    if (frame.runtime_quarantined || frame.fence_value > r.CompletedRuntimeFence()) {
        ++r.runtime_stats.slot_pressure; return pressure();
    }
    // The slot can complete between the collection snapshot and the readiness
    // check above. Consume that newly completed timing before reusing its pair.
    if (frame.timing_pending) r.CollectRuntimeTiming(r.CompletedRuntimeFence());
    if (frame.timing_pending) throw std::logic_error("Runtime timing slot was not retired");
    if (r.next_fence == UINT64_MAX) throw std::overflow_error("Runtime fence sequence exhausted");
    auto colour_desc = TextureDesc(packet.native_width, packet.native_height, color_format);
    colour_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    const bool new_colour = !frame.runtime_colour.resource || frame.runtime_width != packet.native_width
        || frame.runtime_height != packet.native_height;
    const uint32_t descriptor_count = r.runtime_presentation ? 4 : 3;
    const bool new_heap = !frame.runtime_heap || frame.runtime_descriptors < descriptor_count;
    std::array<D3D12_RESOURCE_DESC, 2> descriptions;
    std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT, 2> footprints{};
    std::array<uint64_t, 2> upload_lengths{}, allocation_lengths{};
    uint64_t default_needed = new_colour ? r.AllocationSize(colour_desc) : 0;
    uint64_t staging_bytes = 0;
    for (size_t i = 0; i < assets.size(); ++i) {
        descriptions[i] = TextureDesc(assets[i]->desc.width, assets[i]->desc.height,
            i ? DXGI_FORMAT_R8G8B8A8_UINT : DXGI_FORMAT_R8_UINT);
        if (assets[i]->resident) continue;
        allocation_lengths[i] = r.AllocationSize(descriptions[i]); default_needed += allocation_lengths[i];
        upload_lengths[i] = r.RuntimeUploadFootprint(descriptions[i], uint64_t(assets[i]->desc.width) * (i ? 4 : 1),
            footprints[i], staging_bytes);
    }
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT readback_footprint{};
    uint64_t readback_length = 0, readback_allocation = 0;
    if (diagnostic_readback) {
        r.device->GetCopyableFootprints(&colour_desc, 0, 1, 0, &readback_footprint, nullptr, nullptr, &readback_length);
        readback_allocation = r.AllocationSize(BufferDesc(readback_length));
    }
    // Count replacement peaks. The caller retries once after collecting unused
    // resources, including completed cached outputs, when this plan cannot fit.
    const auto staging_plan = r.PlanRuntimeStaging(frame, staging_bytes, 0);
    const auto resource_pressure = r.RuntimePressure(default_needed, staging_plan.allocation, readback_allocation,
        new_heap ? descriptor_count : 0, diagnostic_readback);
    if (resource_pressure != RuntimePressureReason::None)
        return {RuntimeSubmitStatus::WouldBlock, {}, resource_pressure};
    r.PrewarmRuntimePipelines(false);
    auto ticket = std::make_shared<RuntimeFrameTicket::State>();
    ticket->owner = r.runtime_owner; ticket->width = packet.native_width; ticket->height = packet.native_height;
    ticket->footprint = readback_footprint; ticket->copy_bytes = readback_length;
    // All allocations/recording below precede submission. Local owners roll
    // back safely on any failure; no admission/current version changes occur.
    RuntimeAllocationPeaks peaks(r.runtime_stats);
    const auto allocate = [&](const D3D12_RESOURCE_DESC& desc, D3D12_HEAP_TYPE heap,
        D3D12_RESOURCE_STATES state, const wchar_t* name, uint64_t reserved,
        D3D12MA::ALLOCATION_FLAGS flags = D3D12MA::ALLOCATION_FLAG_NONE) {
        auto resource = r.AllocateRuntime(desc, heap, state, name, nullptr, flags); ++r.runtime_stats.resource_allocations;
        peaks.AddResource(heap, resource.allocation->GetSize());
        if (resource.allocation->GetSize() > reserved) throw std::runtime_error("Runtime allocation exceeded its reservation");
        return resource;
    };
    Resource colour;
    if (new_colour) colour = allocate(colour_desc, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        L"Runtime indexed colour", r.AllocationSize(colour_desc));
    std::array<Resource, 2> textures;
    auto staging = r.PrepareRuntimeStaging(frame, staging_plan, staging_bytes, current, peaks);
    std::array<ID3D12Resource*, 2> gpu_assets{};
    for (size_t i = 0; i < assets.size(); ++i) {
        if (assets[i]->resident) { gpu_assets[i] = assets[i]->texture.resource.Get(); continue; }
        const auto& key = assets[i]->key;
        wchar_t asset_name[256]{};
        std::swprintf(asset_name, std::size(asset_name),
            L"Runtime %ls e%llu id%llu g%llu r%llu allocation%llu slot%u",
            i ? L"palette" : L"indices", static_cast<unsigned long long>(key.epoch),
            static_cast<unsigned long long>(key.logical_id), static_cast<unsigned long long>(key.generation),
            static_cast<unsigned long long>(key.revision),
            static_cast<unsigned long long>(r.runtime_stats.resource_allocations + 1), current);
        // Bounded committed backing avoids intermittent placed-texture validation
        // under replacement. Barriers are unchanged; the cause remains unproven.
        // Budgets charge the actual allocation size.
        textures[i] = allocate(descriptions[i], D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST,
            asset_name, allocation_lengths[i], D3D12MA::ALLOCATION_FLAG_COMMITTED);
        gpu_assets[i] = textures[i].resource.Get();
        const size_t row_bytes = size_t(assets[i]->desc.width) * (i ? 4 : 1);
        for (uint32_t y = 0; y < assets[i]->desc.height; ++y)
            std::memcpy(staging.mapped + footprints[i].Offset + size_t(y) * footprints[i].Footprint.RowPitch,
                assets[i]->bytes->data() + size_t(y) * row_bytes, row_bytes);
    }
    if (diagnostic_readback) {
        ticket->readback = allocate(BufferDesc(readback_length), D3D12_HEAP_TYPE_READBACK,
            D3D12_RESOURCE_STATE_COPY_DEST, L"Runtime selected frame readback", readback_allocation);
        r.runtime_tickets.reserve(r.runtime_tickets.size() + 1);
    }
    ComPtr<ID3D12DescriptorHeap> heap;
    if (new_heap) {
        D3D12_DESCRIPTOR_HEAP_DESC desc{};
        desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; desc.NumDescriptors = descriptor_count;
        desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        r.CheckRuntimeAllocation(r.device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(heap.GetAddressOf())), "Create runtime frame descriptors");
        ++r.runtime_stats.descriptor_heap_allocations;
        peaks.AddDescriptors(desc.NumDescriptors);
        wchar_t heap_name[128]{};
        std::swprintf(heap_name, std::size(heap_name), L"Runtime descriptors slot%u allocation%llu", current,
            static_cast<unsigned long long>(r.runtime_stats.descriptor_heap_allocations));
        Check(heap->SetName(heap_name), "Name runtime frame descriptors");
    }
    auto* descriptor_heap = new_heap ? heap.Get() : frame.runtime_heap.Get();
    auto handle = CpuHandle(descriptor_heap);
    const UINT stride = r.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    for (size_t i = 0; i < assets.size(); ++i) {
        D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
        desc.Format = descriptions[i].Format; desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; desc.Texture2D.MipLevels = 1;
        r.device->CreateShaderResourceView(gpu_assets[i], &desc, handle); handle.ptr += stride;
    }
    auto* output = new_colour ? colour.resource.Get() : frame.runtime_colour.resource.Get();
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{}; uav.Format = color_format; uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    r.device->CreateUnorderedAccessView(output, nullptr, &uav, handle);
    Check(frame.allocator->Reset(), "Reset runtime frame allocator");
    Check(commands->Reset(frame.allocator.Get(), r.runtime_indexed_pipeline.Get()), "Reset runtime indexed commands");
    r.BeginRuntimeTiming(frame);
    for (size_t i = 0; i < assets.size(); ++i) if (!assets[i]->resident) {
        D3D12_TEXTURE_COPY_LOCATION from{}, to{};
        from.pResource = staging.resource; from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        from.PlacedFootprint = footprints[i];
        to.pResource = gpu_assets[i]; to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        commands->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        Transition(commands, gpu_assets[i], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    if (!new_colour) Transition(commands, output, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ID3D12DescriptorHeap* heaps[] = {descriptor_heap}; commands->SetDescriptorHeaps(1, heaps);
    commands->SetComputeRootSignature(r.runtime_indexed_root.Get());
    commands->SetComputeRootDescriptorTable(0, GpuHandle(descriptor_heap));
    const uint32_t constants[] = {packet.native_width, packet.native_height, uint32_t(packet.x), uint32_t(packet.y),
        packet.width, packet.height, packet.clear_index, 0};
    commands->SetComputeRoot32BitConstants(1, 8, constants, 0);
    commands->Dispatch((packet.native_width + 7) / 8, (packet.native_height + 7) / 8, 1);
    Transition(commands, output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    r.EndRuntimeTiming(frame);
    if (diagnostic_readback) {
        D3D12_TEXTURE_COPY_LOCATION from{}, to{};
        from.pResource = output; from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        to.pResource = ticket->readback.resource.Get(); to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        to.PlacedFootprint = readback_footprint;
        commands->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    }
    if (!hold) r.RecordRuntimePresent(commands, frame, output, descriptor_heap, 3, packet.native_width, packet.native_height);
    Check(commands->Close(), "Close runtime indexed commands");
    if (r.runtime_failure == RuntimeFailurePoint::BeforeExecute) {
        r.runtime_failure = RuntimeFailurePoint::None;
        throw std::runtime_error("Injected runtime failure before ExecuteCommandLists");
    }
    // The selected existing frame owns everything before the first possible
    // GPU reader. No throwing allocation or ownership publication follows it.
    r.ArmRuntimeTiming(frame, false, diagnostic_readback, bool(hold));
    frame.runtime_assets = assets; frame.runtime_ticket = ticket;
    r.CommitRuntimeStaging(frame, staging, staging_plan, staging_bytes);
    frame.frozen_reader.reset();
    if (new_colour) frame.runtime_colour = std::move(colour);
    frame.runtime_width = packet.native_width; frame.runtime_height = packet.native_height;
    if (new_heap) { frame.runtime_heap = std::move(heap); frame.runtime_descriptors = descriptor_count; }
    if (diagnostic_readback) r.runtime_tickets.emplace_back(ticket);
    for (size_t i = 0; i < assets.size(); ++i) if (!assets[i]->resident) {
        assets[i]->texture = std::move(textures[i]); assets[i]->resident = true;
        if (i) r.runtime_stats.palette_upload_bytes += assets[i]->bytes->size();
        else r.runtime_stats.source_upload_bytes += assets[i]->bytes->size();
        r.runtime_stats.aligned_upload_bytes += upload_lengths[i];
    }
    frame.runtime_quarantined = true; r.runtime_stats.quarantined = true; r.runtime_config_locked = true;
    RuntimeSubmitResult result{RuntimeSubmitStatus::Submitted, {}}; result.ticket.state = ticket;
    result.submission_id = frame.timing_completion.submission_id;
    r.RuntimeStats();
    ID3D12CommandList* lists[] = {commands}; r.Execute(1, lists);
    if (hold) { ++hold->consumed_slots; ++r.runtime_stats.held_submissions; }
    try {
        if (r.runtime_failure == RuntimeFailurePoint::AfterExecute) {
            r.runtime_failure = RuntimeFailurePoint::None;
            throw std::runtime_error("Injected runtime failure after ExecuteCommandLists");
        }
        frame.fence_value = r.Signal(); ticket->fence_value = frame.fence_value;
        frame.timing_fence = frame.fence_value;
        if (!hold) {
            r.RuntimePresent("Present runtime indexed frame");
            ++r.runtime_stats.presentations; ++r.diagnostics.frames;
        }
        r.CollectMessages();
    } catch (...) { r.runtime_stats.failed = true; throw; }
    frame.runtime_quarantined = false; r.runtime_stats.quarantined = false;
    ++r.runtime_stats.submissions;
    r.runtime_stats.command_bytes += sizeof(constants); // Root-constant payload, not driver command-buffer storage.
    return result;
}
bool Device::IsRuntimeFrameComplete(const RuntimeFrameTicket& ticket)
{
    auto& r = *impl;
    if (!r.initialized || r.runtime_stats.failed || !ticket.Valid() || ticket.state->owner.lock() != r.runtime_owner)
        throw std::invalid_argument("Invalid or foreign runtime frame ticket");
    return r.CompletedRuntimeFence() >= ticket.state->fence_value;
}
bool Device::TryReadbackRuntimeFrame(const RuntimeFrameTicket& ticket, Image& output)
{
    if (!ticket.HasReadback()) throw std::invalid_argument("Runtime readback was not requested before submission");
    if (!IsRuntimeFrameComplete(ticket)) return false;
    const auto& state = *ticket.state;
    Image result{state.width, state.height, std::vector<uint8_t>(size_t(state.width) * state.height * 4)};
    void* mapped = nullptr; D3D12_RANGE read{0, SIZE_T(state.copy_bytes)};
    Check(state.readback.resource->Map(0, &read, &mapped), "Map runtime selected frame readback");
    for (uint32_t y = 0; y < state.height; ++y)
        std::memcpy(result.rgba.data() + size_t(y) * state.width * 4,
            static_cast<const uint8_t*>(mapped) + state.footprint.Offset + size_t(y) * state.footprint.Footprint.RowPitch,
            size_t(state.width) * 4);
    D3D12_RANGE none{0, 0}; state.readback.resource->Unmap(0, &none);
    output = std::move(result); ++impl->runtime_stats.image_readbacks; return true;
}

Device::Device() : impl(std::make_unique<Impl>()) {}
Device::~Device()
{
    try { impl->ReleaseRuntimeReaderHold(); impl->Idle(); }
    catch (...) {
        if (impl->device && SUCCEEDED(impl->device->GetDeviceRemovedReason())) {
            // A timeout is not proof of device removal. Deliberately retain
            // this bounded failed device and all submitted owners until process
            // teardown rather than free memory that the live GPU may still read.
            impl->runtime_owner.reset(); impl->frozen_owner.reset();
            std::fputs("DX12 shutdown could not confirm GPU completion; retaining failed device resources\n", stderr);
            (void)impl.release(); return;
        }
    }
}

void Device::Initialize(void* native_window, uint32_t width, uint32_t height, const Options& options)
{
    if (impl->device || !native_window || !width || !height || width > 16384 || height > 16384)
        throw std::invalid_argument("Invalid/repeated DX12 initialization");
    auto& r = *impl;
    if (!ValidLightingRemaster(options.lighting_remaster))
        throw std::invalid_argument("Invalid lighting remaster controls");
    r.options = options;
    if (options.resize_warning_trace && !options.debug)
        throw std::invalid_argument("Resize warning trace requires validation");
    if (options.warp && options.hardware_adapter_luid)
        throw std::invalid_argument("WARP and exact hardware adapter selection are mutually exclusive");
    if (options.waitable_vsync_latency < 1 || options.waitable_vsync_latency > 2)
        throw std::invalid_argument("Waitable VSync latency must be1 or2");
    if (options.gpu_validation && !options.debug) throw std::invalid_argument("GPU validation requires debug layer");
    if (options.debug) {
        ComPtr<ID3D12Debug> debug;
        Check(D3D12GetDebugInterface(IID_PPV_ARGS(debug.GetAddressOf())), "Enable debug layer (Windows Graphics Tools must be installed)");
        debug->EnableDebugLayer();
        if (options.gpu_validation) {
            ComPtr<ID3D12Debug1> debug1;
            Check(debug.As(&debug1), "GPU validation interface");
            debug1->SetEnableGPUBasedValidation(TRUE);
        }
        r.diagnostics.debug_enabled = true;
        r.diagnostics.gpu_validation_enabled = options.gpu_validation;
        ComPtr<ID3D12DeviceRemovedExtendedDataSettings> dred;
        Check(D3D12GetDebugInterface(IID_PPV_ARGS(dred.GetAddressOf())), "DRED settings");
        dred->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        dred->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
    }
    Check(CreateDXGIFactory2(0, IID_PPV_ARGS(r.factory.GetAddressOf())), "Create DXGI factory");
    if (options.hardware_adapter_luid) {
        const uint64_t value=*options.hardware_adapter_luid;
        const LUID requested{DWORD(value),LONG(uint32_t(value>>32))};
        Check(r.factory->EnumAdapterByLuid(requested,IID_PPV_ARGS(r.adapter.GetAddressOf())), "Select requested hardware adapter LUID");
        DXGI_ADAPTER_DESC1 selected{};
        Check(r.adapter->GetDesc1(&selected), "Read requested hardware adapter");
        if(selected.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
            throw std::invalid_argument("Requested hardware adapter LUID identifies a software adapter");
    } else if (options.warp) {
        Check(r.factory->EnumWarpAdapter(IID_PPV_ARGS(r.adapter.GetAddressOf())), "Select WARP adapter");
    } else {
        SIZE_T best_memory = 0;
        for (UINT i = 0;; ++i) {
            ComPtr<IDXGIAdapter1> candidate;
            HRESULT hr = r.factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                IID_PPV_ARGS(candidate.GetAddressOf()));
            if (hr == DXGI_ERROR_NOT_FOUND) break;
            Check(hr, "Enumerate GPU adapters");
            DXGI_ADAPTER_DESC1 desc = {};
            Check(candidate->GetDesc1(&desc), "Read GPU description");
            if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
            if (FAILED(D3D12CreateDevice(candidate.Get(), D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), nullptr))) continue;
            if (!r.adapter || desc.DedicatedVideoMemory > best_memory) {
                r.adapter = candidate; best_memory = desc.DedicatedVideoMemory;
            }
        }
        if (!r.adapter) throw std::runtime_error("No DirectX 12 hardware adapter; use the existing game renderer");
    }
    DXGI_ADAPTER_DESC1 adapter_desc = {};
    Check(r.adapter->GetDesc1(&adapter_desc), "Read selected adapter");
    r.diagnostics.adapter = Utf8(adapter_desc.Description);
    r.diagnostics.vendor_id = adapter_desc.VendorId;
    r.diagnostics.device_id = adapter_desc.DeviceId;
    r.diagnostics.adapter_luid = (uint64_t(uint32_t(adapter_desc.AdapterLuid.HighPart))<<32) | adapter_desc.AdapterLuid.LowPart;
    Check(D3D12CreateDevice(r.adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(r.device.GetAddressOf())), "Create D3D12 device");
    if (options.debug) {
        Check(r.device.As(&r.info), "D3D12 validation queue");
        Check(r.info->SetMessageCountLimit(16384), "Validation queue limit");
    }
    wchar_t runtime_path[32768] = {};
    HMODULE core = GetModuleHandleW(L"D3D12Core.dll");
    if (!core || !GetModuleFileNameW(core, runtime_path, 32768))
        throw std::runtime_error("Cannot identify the loaded DirectX runtime");
    r.diagnostics.runtime_path = Utf8(runtime_path);
    for (UINT model = 0x69; model >= 0x60; --model) {
        D3D12_FEATURE_DATA_SHADER_MODEL sm = {static_cast<D3D_SHADER_MODEL>(model)};
        if (SUCCEEDED(r.device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm)))) {
            r.diagnostics.shader_model = sm.HighestShaderModel; break;
        }
    }
    if (r.diagnostics.shader_model < 0x60) throw std::runtime_error("Shader Model 6.0 is required");
    // Palette operators read the destination index, so ordinary alpha blending
    // cannot substitute for them. Query ordered pixel-shader UAV access before
    // choosing that implementation; unsupported devices need an ordered fallback.
    D3D12_FEATURE_DATA_D3D12_OPTIONS base_capabilities = {};
    r.diagnostics.rasterizer_ordered_views_query_succeeded = SUCCEEDED(
        r.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &base_capabilities, sizeof(base_capabilities)));
    if (r.diagnostics.rasterizer_ordered_views_query_succeeded)
        r.diagnostics.rasterizer_ordered_views_supported = base_capabilities.ROVsSupported != FALSE;
    D3D12_FEATURE_DATA_D3D12_OPTIONS12 capabilities = {};
    if (SUCCEEDED(r.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS12, &capabilities, sizeof(capabilities))))
        r.diagnostics.enhanced_barriers_supported = capabilities.EnhancedBarriersSupported != FALSE;
    BOOL tearing = FALSE;
    if (SUCCEEDED(r.factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing))))
        r.diagnostics.tearing_supported = tearing != FALSE;
    // The first path deliberately uses legacy barriers on every device. Enhanced
    // barriers can be enabled later behind this measured capability.
    r.swap_flags = r.diagnostics.tearing_supported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    if (options.waitable_presentation) r.swap_flags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    D3D12MA::ALLOCATOR_DESC memory_desc = {};
    memory_desc.pDevice = r.device.Get(); memory_desc.pAdapter = r.adapter.Get();
    Check(D3D12MA::CreateAllocator(&memory_desc, r.memory.GetAddressOf()), "Create memory allocator");
    D3D12_COMMAND_QUEUE_DESC queue_desc = {};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    Check(r.device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(r.queue.GetAddressOf())), "Create graphics queue");
    UINT64 timestamp_frequency = 0;
    Check(r.queue->GetTimestampFrequency(&timestamp_frequency), "Get runtime timestamp frequency");
    if (!timestamp_frequency) throw std::runtime_error("Runtime timestamp frequency is zero");
    r.runtime_timing.timestamp_frequency = timestamp_frequency;
    D3D12_QUERY_HEAP_DESC timing_queries{};
    timing_queries.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP; timing_queries.Count = frame_count * 2;
    Check(r.device->CreateQueryHeap(&timing_queries, IID_PPV_ARGS(r.runtime_timing_queries.GetAddressOf())), "Create runtime timing queries");
    Check(r.runtime_timing_queries->SetName(L"Runtime timing queries"), "Name runtime timing queries");
    r.runtime_timing.query_heaps = 1; r.runtime_timing.query_entries = timing_queries.Count;
    Check(r.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(r.fence.GetAddressOf())), "Create fence");
    Check(r.fence->SetName(L"KeeperFX submission fence"), "Name submission fence");
    r.fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!r.fence_event) throw std::runtime_error("Cannot create fence event");
    DXGI_SWAP_CHAIN_DESC1 swap_desc = {};
    swap_desc.Width = width; swap_desc.Height = height; swap_desc.Format = color_format;
    swap_desc.SampleDesc.Count = 1; swap_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swap_desc.BufferCount = frame_count; swap_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swap_desc.Scaling = DXGI_SCALING_STRETCH; swap_desc.Flags = r.swap_flags;
    ComPtr<IDXGISwapChain1> swap;
    auto window = static_cast<HWND>(native_window);
    Check(r.factory->CreateSwapChainForHwnd(r.queue.Get(), window, &swap_desc, nullptr, nullptr, swap.GetAddressOf()), "Create swapchain");
    Check(swap.As(&r.swapchain), "Swapchain interface");
    if (options.waitable_presentation) {
        Check(r.swapchain->SetMaximumFrameLatency(options.vsync ? options.waitable_vsync_latency : frame_count), "Set presentation latency");
        r.presentation_event = r.swapchain->GetFrameLatencyWaitableObject();
        if (!r.presentation_event) throw std::runtime_error("Cannot obtain presentation admission event");
    }
    Check(r.factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER), "Window association");
    D3D12_DESCRIPTOR_HEAP_DESC heap = {};
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; heap.NumDescriptors = frame_count;
    Check(r.device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(r.rtv_heap.GetAddressOf())), "Create RTV heap");
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV; heap.NumDescriptors = 1;
    Check(r.device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(r.dsv_heap.GetAddressOf())), "Create depth heap");
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    Check(r.device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(r.srv_heap.GetAddressOf())), "Create texture heap");
    r.rtv_stride = r.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    for (UINT slot = 0; slot < frame_count; ++slot) {
        auto& frame = r.frames[slot];
        Check(r.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(frame.allocator.GetAddressOf())), "Create frame allocator");
        // Keep each runtime recording with its already-fenced allocator. The
        // synchronous diagnostic paths retain their separate shared list.
        Check(r.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, frame.allocator.Get(), nullptr,
            IID_PPV_ARGS(frame.runtime_commands.GetAddressOf())), "Create runtime frame command list");
        Check(frame.runtime_commands->Close(), "Close initial runtime frame command list");
        wchar_t name[64]{};
        std::swprintf(name, std::size(name), L"Runtime command list slot%u", slot);
        Check(frame.runtime_commands->SetName(name), "Name runtime frame command list");
        std::swprintf(name, std::size(name), L"Runtime timing readback slot%u", slot);
        frame.timing_readback = r.Allocate(BufferDesc(2 * sizeof(UINT64)), D3D12_HEAP_TYPE_READBACK,
            D3D12_RESOURCE_STATE_COPY_DEST, name);
        frame.timing_query = slot * 2;
        ++r.runtime_timing.readback_resources;
        r.runtime_timing.readback_logical_bytes += 2 * sizeof(UINT64);
        r.runtime_timing.readback_allocation_bytes += frame.timing_readback.allocation->GetSize();
    }
    Check(r.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, r.frames[0].allocator.Get(), nullptr,
        IID_PPV_ARGS(r.commands.GetAddressOf())), "Create command list");
    Check(r.commands->Close(), "Close initial command list");
    r.width = width; r.height = height;
    r.CreateTargets(); r.CreatePipeline(); r.CollectMessages();
    r.initialized = true; r.InitializeProcessBudget();
}

void Device::Upload(const std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices, const Image& image)
{
    impl->RequireHealthy();
    auto& r = *impl;
    if (!r.initialized || vertices.empty() || indices.empty() || indices.size() % 3 != 0 ||
        vertices.size() > UINT_MAX / sizeof(Vertex) || indices.size() > UINT_MAX / sizeof(uint32_t) ||
        !image.width || !image.height || image.width > 16384 || image.height > 16384 ||
        image.rgba.size() != uint64_t(image.width) * image.height * 4)
        throw std::invalid_argument("Invalid mesh or RGBA texture upload");
    for (auto index : indices) if (index >= vertices.size()) throw std::invalid_argument("Mesh index out of bounds");
    r.Idle(); // Retire every reader before replacing the single scene/SRV.
    auto& frame = r.frames[0];
    Check(frame.allocator->Reset(), "Reset upload allocator");
    Check(r.commands->Reset(frame.allocator.Get(), nullptr), "Reset upload commands");
    std::vector<Resource> staging;
    auto upload_buffer = [&](const void* data, size_t bytes, D3D12_RESOURCE_STATES final_state, const wchar_t* name) {
        // Start default-heap buffers in COMMON, including on WARP runtimes
        // that ignore a requested creation state. Transition before the copy.
        Resource destination = r.Allocate(BufferDesc(bytes), D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON, name);
        Resource upload = r.Allocate(BufferDesc(bytes), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, L"Staging buffer");
        void* mapped = nullptr;
        D3D12_RANGE read = {0, 0};
        Check(upload.resource->Map(0, &read, &mapped), "Map upload buffer");
        std::memcpy(mapped, data, bytes);
        upload.resource->Unmap(0, nullptr);
        Transition(r.commands.Get(), destination.resource.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        r.commands->CopyBufferRegion(destination.resource.Get(), 0, upload.resource.Get(), 0, bytes);
        Transition(r.commands.Get(), destination.resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST, final_state);
        staging.push_back(std::move(upload));
        return destination;
    };
    // Release resource before its allocation when replacing an existing upload.
    r.vertices.Reset(); r.indices.Reset(); r.texture.Reset(); r.index_count = 0;
    r.vertices = upload_buffer(vertices.data(), vertices.size() * sizeof(Vertex), D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, L"Mesh vertices");
    r.indices = upload_buffer(indices.data(), indices.size() * sizeof(uint32_t), D3D12_RESOURCE_STATE_INDEX_BUFFER, L"Mesh indices");
    auto desc = TextureDesc(image.width, image.height, color_format);
    r.texture = r.Allocate(desc, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST, L"Scene texture");
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
    UINT64 bytes = 0;
    r.device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
    Resource upload = r.Allocate(BufferDesc(bytes), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, L"Texture staging");
    void* mapped = nullptr;
    D3D12_RANGE read = {0, 0};
    Check(upload.resource->Map(0, &read, &mapped), "Map texture staging");
    for (UINT y = 0; y < image.height; ++y)
        std::memcpy(static_cast<uint8_t*>(mapped) + footprint.Offset + size_t(y) * footprint.Footprint.RowPitch,
            image.rgba.data() + size_t(y) * image.width * 4, size_t(image.width) * 4);
    upload.resource->Unmap(0, nullptr);
    D3D12_TEXTURE_COPY_LOCATION source = {}, destination = {};
    source.pResource = upload.resource.Get(); source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; source.PlacedFootprint = footprint;
    destination.pResource = r.texture.resource.Get(); destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    r.commands->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    Transition(r.commands.Get(), r.texture.resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Check(r.commands->Close(), "Close upload commands");
    ID3D12CommandList* lists[] = {r.commands.Get()};
    r.Execute(1, lists);
    frame.fence_value = r.Signal();
    r.Wait(frame.fence_value); // Staging resources stay alive until the copy finishes.
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Format = color_format; srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; srv.Texture2D.MipLevels = 1;
    r.device->CreateShaderResourceView(r.texture.resource.Get(), &srv, CpuHandle(r.srv_heap.Get()));
    r.vertex_view = {r.vertices.resource->GetGPUVirtualAddress(), UINT(vertices.size() * sizeof(Vertex)), sizeof(Vertex)};
    r.index_view = {r.indices.resource->GetGPUVirtualAddress(), UINT(indices.size() * sizeof(uint32_t)), DXGI_FORMAT_R32_UINT};
    r.index_count = static_cast<UINT>(indices.size());
    r.CollectMessages();
}

void Device::Resize(uint32_t width, uint32_t height)
{
    impl->RequireHealthy();
    auto& r = *impl;
    if (!r.initialized || width > 16384 || height > 16384) throw std::invalid_argument("Invalid resize");
    if (!width || !height || r.suspended || width != r.width || height != r.height)
        r.presentation_poll_pending = false;
    r.suspended = !width || !height;
    if (r.suspended || (width == r.width && height == r.height)) return;
    r.Idle();
    for (auto& frame : r.frames) { frame.backbuffer.Reset(); frame.fence_value = 0; }
    r.depth.Reset();
    HRESULT resize_result;
    if (r.options.resize_warning_trace && !r.resize_warning_trace_finished) {
        const DWORD caller = GetCurrentThreadId();
        ScopedResizeWarningCallback scope(r.device.Get());
        resize_result = r.swapchain->ResizeBuffers(frame_count, width, height, color_format, r.swap_flags);
        scope.Stop(); // Also runs for a failed ResizeBuffers, before Check can throw.
        if (scope.trace->occurrences.load(std::memory_order_relaxed) || FAILED(scope.query_result)
            || FAILED(scope.register_result) || FAILED(scope.unregister_result)) {
            r.resize_warning_trace_finished = true; // At most one bounded trace/status per Device.
            r.messages.push_back(ResizeWarningDescription(scope, caller, width, height, resize_result, r.runtime_stats.fence_waits));
        }
        if (scope.quarantined) Check(scope.unregister_result, "Unregister resize warning callback (context retained)");
    } else {
        resize_result = r.swapchain->ResizeBuffers(frame_count, width, height, color_format, r.swap_flags);
    }
    Check(resize_result, "Resize swapchain");
    r.width = width; r.height = height;
    r.CreateTargets(); ++r.diagnostics.resizes; r.CollectMessages();
}

void Device::ClearScene()
{
    impl->RequireHealthy();
    auto& r = *impl;
    if (!r.initialized) throw std::logic_error("Device has not been initialized");
    r.Idle();
    r.vertices.Reset(); r.indices.Reset(); r.texture.Reset();
    r.index_count = 0; r.vertex_view = {}; r.index_view = {};
    // Replace the descriptor with a null SRV so no retired resource identity
    // remains reachable if this heap is reused by a later rendering pass.
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Format = color_format; srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; srv.Texture2D.MipLevels = 1;
    r.device->CreateShaderResourceView(nullptr, &srv, CpuHandle(r.srv_heap.Get()));
    r.CollectMessages();
}

bool Device::Draw(const std::array<float, 16>& transform, Image* capture)
{
    impl->RequireHealthy();
    auto& r = *impl;
    if (!r.initialized || !r.index_count) throw std::logic_error("Upload a scene before drawing");
    if (r.suspended) return false;
    UINT current = r.swapchain->GetCurrentBackBufferIndex();
    auto& frame = r.frames[current];
    r.Wait(frame.fence_value);
    frame.frozen_reader.reset();
    Check(frame.allocator->Reset(), "Reset frame allocator");
    Check(r.commands->Reset(frame.allocator.Get(), r.pipeline.Get()), "Reset frame commands");
    Transition(r.commands.Get(), frame.backbuffer.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    auto rtv = CpuHandle(r.rtv_heap.Get()); rtv.ptr += size_t(current) * r.rtv_stride;
    auto dsv = CpuHandle(r.dsv_heap.Get());
    constexpr float background[] = {0.025f, 0.035f, 0.055f, 1.0f};
    r.commands->ClearRenderTargetView(rtv, background, 0, nullptr);
    r.commands->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    r.commands->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
    D3D12_VIEWPORT viewport = {0, 0, float(r.width), float(r.height), 0, 1};
    D3D12_RECT scissor = {0, 0, LONG(r.width), LONG(r.height)};
    r.commands->RSSetViewports(1, &viewport); r.commands->RSSetScissorRects(1, &scissor);
    r.commands->SetGraphicsRootSignature(r.root_signature.Get());
    r.commands->SetGraphicsRoot32BitConstants(0, 16, transform.data(), 0);
    ID3D12DescriptorHeap* heaps[] = {r.srv_heap.Get()};
    r.commands->SetDescriptorHeaps(1, heaps);
    r.commands->SetGraphicsRootDescriptorTable(1, GpuHandle(r.srv_heap.Get()));
    r.commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    r.commands->IASetVertexBuffers(0, 1, &r.vertex_view); r.commands->IASetIndexBuffer(&r.index_view);
    r.commands->DrawIndexedInstanced(r.index_count, 1, 0, 0, 0);
    Resource readback;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT64 readback_bytes = 0;
    if (capture) {
        auto desc = TextureDesc(r.width, r.height, color_format);
        r.device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &readback_bytes);
        readback = r.Allocate(BufferDesc(readback_bytes), D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, L"Screenshot readback");
        Transition(r.commands.Get(), frame.backbuffer.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION source = {}, destination = {};
        source.pResource = frame.backbuffer.Get(); source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        destination.pResource = readback.resource.Get(); destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        destination.PlacedFootprint = footprint;
        r.commands->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        Transition(r.commands.Get(), frame.backbuffer.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
    } else {
        Transition(r.commands.Get(), frame.backbuffer.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    }
    Check(r.commands->Close(), "Close frame commands");
    ID3D12CommandList* lists[] = {r.commands.Get()};
    r.Execute(1, lists);
    frame.fence_value = r.Signal();
    HRESULT hr = r.swapchain->Present(r.options.vsync ? 1 : 0,
        !r.options.vsync && r.diagnostics.tearing_supported ? DXGI_PRESENT_ALLOW_TEARING : 0);
    Check(hr, "Present");
    ++r.diagnostics.frames;
    if (capture) {
        r.Wait(frame.fence_value);
        void* mapped = nullptr;
        D3D12_RANGE read = {0, static_cast<SIZE_T>(readback_bytes)};
        Check(readback.resource->Map(0, &read, &mapped), "Map screenshot");
        capture->width = r.width; capture->height = r.height;
        capture->rgba.resize(size_t(r.width) * r.height * 4);
        for (UINT y = 0; y < r.height; ++y)
            std::memcpy(capture->rgba.data() + size_t(y) * r.width * 4,
                static_cast<const uint8_t*>(mapped) + footprint.Offset + size_t(y) * footprint.Footprint.RowPitch, size_t(r.width) * 4);
        D3D12_RANGE written = {0, 0};
        readback.resource->Unmap(0, &written);
    }
    r.CollectMessages();
    return hr != DXGI_STATUS_OCCLUDED;
}

std::vector<uint8_t> Device::CompositePalette(uint32_t width, uint32_t height,
    const std::vector<uint8_t>& background, const Image& atlas,
    const std::vector<PaletteQuad>& quads, bool ordered_views)
{
    if (quads.empty() || quads.size() > 4096) throw std::invalid_argument("Invalid palette rectangle count");
    std::vector<PalettePrimitive> primitives;
    primitives.reserve(quads.size());
    for (const auto& quad : quads) {
        PalettePrimitive primitive;
        primitive.sprite = quad;
        primitives.push_back(primitive);
    }
    return CompositeIndexed(width, height, background, atlas, primitives, ordered_views);
}

std::vector<uint8_t> Device::CompositeIndexed(uint32_t width, uint32_t height,
    const std::vector<uint8_t>& background, const Image& atlas,
    const std::vector<PalettePrimitive>& primitives, bool ordered_views)
{
    auto surface = impl->RenderIndexedSurface(width, height, &background, 0, atlas, primitives, ordered_views);
    const auto bytes = impl->ReadTexture(surface.resource.Get(), width, height,
        DXGI_FORMAT_R32_UINT, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    std::vector<uint8_t> result(size_t(width) * height);
    for (size_t i = 0; i < result.size(); ++i) {
        uint32_t index;
        std::memcpy(&index, bytes.data() + i * 4, sizeof(index));
        if (index > 255) throw std::runtime_error("Palette compositor encountered an invalid index or shadow sample outside its initialized mask");
        result[i] = uint8_t(index);
    }
    return result;
}

Resource Device::Impl::RenderIndexedSurface(uint32_t width, uint32_t height,
    const std::vector<uint8_t>* background, uint8_t clear_index, const Image& atlas,
    const std::vector<PalettePrimitive>& primitives, bool ordered_views)
{
    RequireHealthy();
    auto& r = *this;
    if (!r.initialized || !width || !height || width > 4096 || height > 4096 ||
        (background && background->size() != uint64_t(width) * height) || atlas.width < 256 || atlas.height < 256 ||
        atlas.width > 16384 || atlas.height > 16384 || atlas.rgba.size() > 64 * 1024 * 1024 ||
        atlas.rgba.size() != uint64_t(atlas.width) * atlas.height * 4 || primitives.empty() || primitives.size() > 327680)
        throw std::invalid_argument("Invalid palette fixture dimensions or data");
    if (ordered_views && !r.diagnostics.rasterizer_ordered_views_supported)
        throw std::invalid_argument("Rasterizer ordered views are not supported");
    struct PaletteVertex {
        float x, y;
        int32_t origin_x, origin_y;
        uint32_t width, height, texture_x, texture_y, texture_width, texture_height, mode;
        uint32_t mapped, map_x_row, map_y_row, lookup_x, lookup_y;
        float u, v, shade;
        float min_u = 0, min_v = 0, max_u = 0, max_v = 0;
    };
    static_assert(sizeof(PaletteVertex) == 92);
    std::vector<PaletteVertex> vertices;
    std::vector<std::pair<UINT, UINT>> draws;
    size_t maximum_vertices = 0;
    for (const auto& primitive : primitives)
        maximum_vertices += primitive.kind == PalettePrimitiveKind::Sprite ? 6 : 3;
    if (maximum_vertices * sizeof(PaletteVertex) > 128 * 1024 * 1024)
        throw std::invalid_argument("Indexed geometry exceeds frozen preparation budget");
    vertices.reserve(maximum_vertices); draws.reserve(primitives.size());
    for (const auto& primitive : primitives) {
        const UINT first_vertex = UINT(vertices.size());
        if (primitive.kind == PalettePrimitiveKind::Shadow) {
            const auto& shadow = primitive.shadow;
            if (!shadow.mask_width || !shadow.mask_height || shadow.mask_width > 256 || shadow.mask_height > 256
                || uint64_t(shadow.mask_x) + shadow.mask_width > atlas.width
                || uint64_t(shadow.mask_y) + shadow.mask_height > atlas.height
                || uint64_t(shadow.fade_x) + 256 > atlas.width || uint64_t(shadow.fade_y) + 64 > atlas.height
                || shadow.shade < 16 || shadow.shade > 31)
                throw std::invalid_argument("Invalid palette shadow source or shade");
            for (const auto& point : shadow.vertices) {
                if (!std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.u) || !std::isfinite(point.v)
                    || std::abs(point.x) > 65536 || std::abs(point.y) > 65536
                    || std::abs(point.u) > 32768 || std::abs(point.v) > 32768)
                    throw std::invalid_argument("Invalid palette shadow vertex");
            }
            for (uint32_t y = 0; y < shadow.mask_height; ++y) for (uint32_t x = 0; x < shadow.mask_width; ++x) {
                const uint8_t mask = atlas.rgba[(size_t(shadow.mask_y + y) * atlas.width + shadow.mask_x + x) * 4];
                if (mask != 0 && mask != 255) throw std::invalid_argument("Shadow mask is not binary");
            }
            const auto& a = shadow.vertices[0]; const auto& b = shadow.vertices[1]; const auto& c = shadow.vertices[2];
            const double area = (double(b.x) - a.x) * (double(c.y) - a.y) - (double(b.y) - a.y) * (double(c.x) - a.x);
            // Original Trig rejects the reverse screen-space winding. Its
            // additional thin-triangle quantization remains oracle evidence.
            if (area <= 0) continue;
            const float min_u = std::min({a.u, b.u, c.u}), max_u = std::max({a.u, b.u, c.u});
            const float min_v = std::min({a.v, b.v, c.v}), max_v = std::max({a.v, b.v, c.v});
            for (const auto& point : shadow.vertices)
                vertices.push_back({2.0f * point.x / width - 1.0f, 1.0f - 2.0f * point.y / height,
                    0, 0, 0, 0, shadow.mask_x, shadow.mask_y, shadow.mask_width, shadow.mask_height, 6,
                    0, 0, 0, shadow.fade_x, shadow.fade_y, point.u, point.v, float(shadow.shade),
                    min_u, min_v, max_u, max_v});
            draws.emplace_back(first_vertex, 3);
            continue;
        }
        if (primitive.kind == PalettePrimitiveKind::Terrain) {
            const auto& triangle = primitive.terrain;
            if ((triangle.texture_width != 32 && triangle.texture_width != 256)
                || (triangle.texture_height != 32 && triangle.texture_height != 256)
                || uint64_t(triangle.texture_x) + triangle.texture_width > atlas.width
                || uint64_t(triangle.texture_y) + triangle.texture_height > atlas.height
                || (triangle.apply_fade && (uint64_t(triangle.fade_x) + 256 > atlas.width
                    || uint64_t(triangle.fade_y) + 64 > atlas.height)))
                throw std::invalid_argument("Palette terrain source outside atlas");
            float min_u = triangle.vertices[0].u, min_v = triangle.vertices[0].v;
            float max_u = min_u, max_v = min_v;
            for (const auto& point : triangle.vertices) {
                if (!std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.u) ||
                    !std::isfinite(point.v) || !std::isfinite(point.shade) || std::abs(point.x) > 65536 ||
                    std::abs(point.y) > 65536 || std::abs(point.u) > 32768 || std::abs(point.v) > 32768 ||
                    point.shade < 0 || point.shade > 64)
                    throw std::invalid_argument("Invalid palette terrain vertex");
                min_u = std::min(min_u, point.u); max_u = std::max(max_u, point.u);
                min_v = std::min(min_v, point.v); max_v = std::max(max_v, point.v);
            }
            for (const auto& point : triangle.vertices) {
                vertices.push_back({2.0f * point.x / width - 1.0f, 1.0f - 2.0f * point.y / height,
                    0, 0, 0, 0, triangle.texture_x, triangle.texture_y, triangle.texture_width, triangle.texture_height,
                    triangle.apply_fade ? 5u : 7u,
                    triangle.clamp_to_vertices ? 1u : 0u, 0, 0, triangle.fade_x, triangle.fade_y, point.u, point.v, point.shade,
                    min_u, min_v, max_u, max_v});
            }
            draws.emplace_back(first_vertex, 3);
            continue;
        }
        if (primitive.kind != PalettePrimitiveKind::Sprite) throw std::invalid_argument("Invalid palette primitive kind");
        const auto& q = primitive.sprite;
        if (!q.width || !q.height || q.width > 16384 || q.height > 16384 ||
            q.x < -16384 || q.x > 16384 || q.y < -16384 || q.y > 16384 ||
            !q.texture_width || !q.texture_height || uint64_t(q.texture_x) + q.texture_width > atlas.width ||
            uint64_t(q.texture_y) + q.texture_height > atlas.height || uint32_t(q.blend) > 4)
            throw std::invalid_argument("Invalid palette rectangle");
        if (q.blend != PaletteBlend::Opaque && (uint64_t(q.lookup_x) + 256 > atlas.width ||
            uint64_t(q.lookup_y) + (q.blend == PaletteBlend::Remap ? 1 : 256) > atlas.height))
            throw std::invalid_argument("Palette lookup outside atlas");
        if (q.mapped) {
            if (q.width > atlas.width || q.height > atlas.width || q.map_x_row >= atlas.height || q.map_y_row >= atlas.height)
                throw std::invalid_argument("Palette axis map outside atlas");
            auto check_axis = [&](uint32_t row, uint32_t length, uint32_t extent) {
                for (uint32_t i = 0; i < length; ++i) {
                    const size_t offset = (size_t(row) * atlas.width + i) * 4;
                    const uint32_t value = atlas.rgba[offset] | (uint32_t(atlas.rgba[offset + 1]) << 8);
                    if (value > extent) throw std::invalid_argument("Palette axis map outside sprite");
                }
            };
            check_axis(q.map_x_row, q.width, q.texture_width);
            check_axis(q.map_y_row, q.height, q.texture_height);
        }
        const float left = 2.0f * q.x / width - 1.0f, right = 2.0f * (q.x + int32_t(q.width)) / width - 1.0f;
        const float top = 1.0f - 2.0f * q.y / height, bottom = 1.0f - 2.0f * (q.y + int32_t(q.height)) / height;
        auto vertex = [&](float x, float y) {
            return PaletteVertex{x,y,q.x,q.y,q.width,q.height,q.texture_x,q.texture_y,
                q.texture_width,q.texture_height,uint32_t(q.blend),q.mapped ? 1u : 0u,
                q.map_x_row,q.map_y_row,q.lookup_x,q.lookup_y,0,0,0};
        };
        const PaletteVertex rect[] = {vertex(left,top),vertex(right,top),vertex(right,bottom),vertex(left,bottom)};
        for (auto index : {0,1,2,0,2,3}) vertices.push_back(rect[index]);
        draws.emplace_back(first_vertex, 6);
    }
    r.Idle();
    r.CreatePalettePipelines();
    auto& frame = r.frames[0];
    Check(frame.allocator->Reset(), "Reset palette allocator");
    Check(r.commands->Reset(frame.allocator.Get(), ordered_views ? r.palette_ordered.Get() : r.palette_serial.Get()), "Reset palette commands");
    auto desc = TextureDesc(width, height, DXGI_FORMAT_R32_UINT);
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    auto destination = r.Allocate(desc, D3D12_HEAP_TYPE_DEFAULT, background
        ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"Palette indices");
    auto source_desc = TextureDesc(atlas.width, atlas.height, color_format);
    auto source = r.Allocate(source_desc, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST, L"Palette source atlas");
    std::vector<Resource> staging;
    auto upload_texture = [&](ID3D12Resource* target, const D3D12_RESOURCE_DESC& texture_desc,
        const void* data, size_t row_bytes, D3D12_RESOURCE_STATES final_state) {
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
        UINT64 bytes = 0;
        r.device->GetCopyableFootprints(&texture_desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
        auto upload = r.Allocate(BufferDesc(bytes), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, L"Palette staging");
        void* mapped = nullptr; D3D12_RANGE no_read = {0,0};
        Check(upload.resource->Map(0, &no_read, &mapped), "Map palette staging");
        for (UINT y = 0; y < texture_desc.Height; ++y)
            std::memcpy(static_cast<uint8_t*>(mapped) + footprint.Offset + size_t(y) * footprint.Footprint.RowPitch,
                static_cast<const uint8_t*>(data) + size_t(y) * row_bytes, row_bytes);
        upload.resource->Unmap(0, nullptr);
        D3D12_TEXTURE_COPY_LOCATION src = {}, dst = {};
        src.pResource = upload.resource.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint = footprint;
        dst.pResource = target; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        r.commands->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        Transition(r.commands.Get(), target, D3D12_RESOURCE_STATE_COPY_DEST, final_state);
        staging.push_back(std::move(upload));
    };
    if (background) {
        const std::vector<uint32_t> initial(background->begin(), background->end());
        upload_texture(destination.resource.Get(), desc, initial.data(), size_t(width) * 4, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    upload_texture(source.resource.Get(), source_desc, atlas.rgba.data(), size_t(atlas.width) * 4, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    auto vertex_buffer = r.Allocate(BufferDesc(std::max<size_t>(1, vertices.size()) * sizeof(PaletteVertex)), D3D12_HEAP_TYPE_UPLOAD,
        D3D12_RESOURCE_STATE_GENERIC_READ, L"Palette rectangles");
    void* mapped = nullptr; D3D12_RANGE no_read = {0,0};
    Check(vertex_buffer.resource->Map(0, &no_read, &mapped), "Map palette rectangles");
    if (!vertices.empty()) std::memcpy(mapped, vertices.data(), vertices.size() * sizeof(PaletteVertex));
    vertex_buffer.resource->Unmap(0, nullptr);
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {};
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_desc.NumDescriptors = 2; heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ComPtr<ID3D12DescriptorHeap> heap;
    Check(r.device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(heap.GetAddressOf())), "Create palette descriptor heap");
    auto handle = CpuHandle(heap.Get());
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Format = color_format; srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; srv.Texture2D.MipLevels = 1;
    r.device->CreateShaderResourceView(source.resource.Get(), &srv, handle);
    handle.ptr += r.device->GetDescriptorHandleIncrementSize(heap_desc.Type);
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
    uav.Format = DXGI_FORMAT_R32_UINT; uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    r.device->CreateUnorderedAccessView(destination.resource.Get(), nullptr, &uav, handle);
    ComPtr<ID3D12DescriptorHeap> clear_heap;
    if (!background) {
        auto clear_desc = heap_desc; clear_desc.NumDescriptors = 1; clear_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        Check(r.device->CreateDescriptorHeap(&clear_desc, IID_PPV_ARGS(clear_heap.GetAddressOf())), "Create frozen clear descriptor");
        r.device->CreateUnorderedAccessView(destination.resource.Get(), nullptr, &uav, CpuHandle(clear_heap.Get()));
    }
    r.commands->OMSetRenderTargets(0, nullptr, FALSE, nullptr);
    D3D12_VIEWPORT viewport = {0,0,float(width),float(height),0,1};
    D3D12_RECT scissor = {0,0,LONG(width),LONG(height)};
    r.commands->RSSetViewports(1, &viewport); r.commands->RSSetScissorRects(1, &scissor);
    ID3D12DescriptorHeap* heaps[] = {heap.Get()};
    r.commands->SetDescriptorHeaps(1, heaps);
    if (!background) {
        auto gpu_uav = GpuHandle(heap.Get());
        gpu_uav.ptr += r.device->GetDescriptorHandleIncrementSize(heap_desc.Type);
        const UINT clear[4] = {clear_index, clear_index, clear_index, clear_index};
        r.commands->ClearUnorderedAccessViewUint(gpu_uav, CpuHandle(clear_heap.Get()), destination.resource.Get(), clear, 0, nullptr);
        D3D12_RESOURCE_BARRIER barrier = {}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barrier.UAV.pResource = destination.resource.Get(); r.commands->ResourceBarrier(1, &barrier);
    }
    r.commands->SetGraphicsRootSignature(r.palette_root.Get());
    r.commands->SetGraphicsRootDescriptorTable(0, GpuHandle(heap.Get()));
    D3D12_VERTEX_BUFFER_VIEW view = {vertex_buffer.resource->GetGPUVirtualAddress(), UINT(vertices.size() * sizeof(PaletteVertex)), sizeof(PaletteVertex)};
    r.commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    if (!vertices.empty()) r.commands->IASetVertexBuffers(0, 1, &view);
    if (ordered_views && !vertices.empty()) {
        r.commands->DrawInstanced(UINT(vertices.size()), 1, 0, 0);
    } else {
        // Top-left rasterization visits the shared diagonal exactly once.
        // Each draw is one triangle or one proven non-overlapping rectangle;
        // barriers order dependent destination accesses across draws.
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barrier.UAV.pResource = destination.resource.Get();
        for (UINT i = 0; i < draws.size(); ++i) {
            if (i) r.commands->ResourceBarrier(1, &barrier);
            r.commands->DrawInstanced(draws[i].second, 1, draws[i].first, 0);
        }
    }
    Transition(r.commands.Get(), destination.resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Check(r.commands->Close(), "Close palette commands");
    // Upload storage and descriptors remain owned until this preparation ends.
    ID3D12CommandList* lists[] = {r.commands.Get()};
    r.Execute(1, lists);
    frame.fence_value = r.Signal(); r.Wait(frame.fence_value);
    r.CollectMessages();
    return destination;
}

std::vector<uint8_t> Device::Impl::ReadTexture(ID3D12Resource* resource, uint32_t width,
    uint32_t height, DXGI_FORMAT format, D3D12_RESOURCE_STATES state)
{
    RequireHealthy();
    auto& r = *this;
    r.Idle();
    auto& frame = r.frames[0];
    Check(frame.allocator->Reset(), "Reset texture readback allocator");
    Check(r.commands->Reset(frame.allocator.Get(), nullptr), "Reset texture readback commands");
    const auto desc = TextureDesc(width, height, format);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {}; UINT64 bytes = 0;
    r.device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
    auto readback = r.Allocate(BufferDesc(bytes), D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, L"Palette readback");
    D3D12_TEXTURE_COPY_LOCATION src = {}, dst = {};
    src.pResource = resource; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.pResource = readback.resource.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = footprint;
    if (state != D3D12_RESOURCE_STATE_COPY_SOURCE)
        Transition(r.commands.Get(), resource, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    r.commands->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    if (state != D3D12_RESOURCE_STATE_COPY_SOURCE)
        Transition(r.commands.Get(), resource, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
    Check(r.commands->Close(), "Close texture readback commands");
    ID3D12CommandList* lists[] = {r.commands.Get()};
    r.Execute(1, lists);
    frame.fence_value = r.Signal(); r.Wait(frame.fence_value);
    void* mapped = nullptr;
    D3D12_RANGE read = {0, SIZE_T(bytes)};
    Check(readback.resource->Map(0, &read, &mapped), "Map palette readback");
    std::vector<uint8_t> result(size_t(width) * height * 4);
    for (UINT y = 0; y < height; ++y) {
        std::memcpy(result.data() + size_t(y) * width * 4,
            static_cast<const uint8_t*>(mapped) + footprint.Offset + size_t(y) * footprint.Footprint.RowPitch, size_t(width) * 4);
    }
    D3D12_RANGE no_read = {0, 0};
    readback.resource->Unmap(0, &no_read);
    r.CollectMessages();
    return result;
}

FrozenGpuFrame Device::PrepareFrozenIndexedFrame(uint32_t width, uint32_t height, uint8_t clear_index,
    const Image& atlas, const std::vector<PalettePrimitive>& primitives,
    const std::array<uint8_t, 768>& display_palette, bool ordered_views)
{
    impl->RequireHealthy();
    if (!impl->initialized) throw std::logic_error("Device has not been initialized");
    for (uint8_t channel : display_palette)
        if (channel > 63) throw std::invalid_argument("Frozen frame palette is not six-bit RGB");
    std::array<uint8_t, 1024> rgba{};
    for (size_t i = 0; i < 256; ++i) {
        for (size_t channel = 0; channel < 3; ++channel)
            rgba[i * 4 + channel] = uint8_t(uint32_t(display_palette[i * 3 + channel]) * 255 / 63);
        rgba[i * 4 + 3] = 255;
    }
    return PrepareFrozenIndexedFrame(width, height, clear_index, atlas, primitives, rgba, ordered_views);
}

FrozenGpuFrame Device::PrepareFrozenIndexedFrame(uint32_t width, uint32_t height, uint8_t clear_index,
    const Image& atlas, const std::vector<PalettePrimitive>& primitives,
    const std::array<uint8_t, 1024>& display_palette_rgba, bool ordered_views)
{
    impl->RequireHealthy();
    auto& r = *impl;
    if (!r.initialized) throw std::logic_error("Device has not been initialized");
    for (size_t i = 3; i < display_palette_rgba.size(); i += 4)
        if (display_palette_rgba[i] != 255) throw std::invalid_argument("Frozen frame display palette is not opaque");
    r.Idle();
    r.frozen_frames.erase(std::remove_if(r.frozen_frames.begin(), r.frozen_frames.end(),
        [](const auto& weak) { return weak.expired(); }), r.frozen_frames.end());
    uint64_t resident_bytes = uint64_t(width) * height * 8;
    for (const auto& weak : r.frozen_frames) if (auto frame = weak.lock())
        resident_bytes += uint64_t(frame->width) * frame->height * (frame->indices.resource ? 8 : 4);
    if (r.frozen_frames.size() >= 8 || resident_bytes > 512 * 1024 * 1024)
        throw std::invalid_argument("Frozen frame resident budget exceeded");
    r.CreateIndexedPresentPipeline();
    auto result = std::make_shared<FrozenGpuFrame::State>();
    result->width = width; result->height = height; result->owner = r.frozen_owner;
    result->indices = r.RenderIndexedSurface(width, height, nullptr, clear_index, atlas, primitives, ordered_views);
    auto& frame = r.frames[0];
    Check(frame.allocator->Reset(), "Reset frozen palette expansion allocator");
    Check(r.commands->Reset(frame.allocator.Get(), r.indexed_present_pipeline.Get()), "Reset frozen palette expansion commands");
    auto colour_desc = TextureDesc(width, height, color_format);
    colour_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    result->colour = r.Allocate(colour_desc, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        L"Frozen palette-expanded colour");
    auto palette = r.Allocate(BufferDesc(256 * sizeof(uint32_t)), D3D12_HEAP_TYPE_UPLOAD,
        D3D12_RESOURCE_STATE_GENERIC_READ, L"Frozen display palette");
    auto status_desc = BufferDesc(sizeof(uint32_t)); status_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    // Match other default-heap buffers: WARP creates these in COMMON even
    // when a different initial state is requested. Transition before copying.
    auto status = r.Allocate(status_desc, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON, L"Frozen index status");
    auto status_upload = r.Allocate(BufferDesc(sizeof(uint32_t)), D3D12_HEAP_TYPE_UPLOAD,
        D3D12_RESOURCE_STATE_GENERIC_READ, L"Frozen index status clear");
    auto status_readback = r.Allocate(BufferDesc(sizeof(uint32_t)), D3D12_HEAP_TYPE_READBACK,
        D3D12_RESOURCE_STATE_COPY_DEST, L"Frozen index status readback");
    void* mapped = nullptr; D3D12_RANGE no_read = {0, 0};
    Check(palette.resource->Map(0, &no_read, &mapped), "Map frozen palette");
    auto* entries = static_cast<uint32_t*>(mapped);
    for (size_t i = 0; i < 256; ++i)
        entries[i] = uint32_t(display_palette_rgba[i * 4]) | uint32_t(display_palette_rgba[i * 4 + 1]) << 8
            | uint32_t(display_palette_rgba[i * 4 + 2]) << 16;
    palette.resource->Unmap(0, nullptr);
    Check(status_upload.resource->Map(0, &no_read, &mapped), "Map frozen index status clear");
    std::memset(mapped, 0, sizeof(uint32_t)); status_upload.resource->Unmap(0, nullptr);
    Transition(r.commands.Get(), status.resource.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    r.commands->CopyBufferRegion(status.resource.Get(), 0, status_upload.resource.Get(), 0, sizeof(uint32_t));
    Transition(r.commands.Get(), status.resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {};
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; heap_desc.NumDescriptors = 4;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ComPtr<ID3D12DescriptorHeap> heap;
    Check(r.device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(heap.GetAddressOf())), "Create frozen expansion descriptors");
    auto handle = CpuHandle(heap.Get()); const UINT stride = r.device->GetDescriptorHandleIncrementSize(heap_desc.Type);
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Format = DXGI_FORMAT_R32_UINT; srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; srv.Texture2D.MipLevels = 1;
    r.device->CreateShaderResourceView(result->indices.resource.Get(), &srv, handle); handle.ptr += stride;
    srv = {}; srv.Format = DXGI_FORMAT_R32_UINT; srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; srv.Buffer.NumElements = 256;
    r.device->CreateShaderResourceView(palette.resource.Get(), &srv, handle); handle.ptr += stride;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
    uav.Format = color_format; uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    r.device->CreateUnorderedAccessView(result->colour.resource.Get(), nullptr, &uav, handle); handle.ptr += stride;
    uav = {}; uav.Format = DXGI_FORMAT_R32_TYPELESS; uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uav.Buffer.NumElements = 1; uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    r.device->CreateUnorderedAccessView(status.resource.Get(), nullptr, &uav, handle);
    ID3D12DescriptorHeap* heaps[] = {heap.Get()}; r.commands->SetDescriptorHeaps(1, heaps);
    r.commands->SetComputeRootSignature(r.indexed_present_root.Get());
    r.commands->SetComputeRootDescriptorTable(0, GpuHandle(heap.Get()));
    r.commands->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
    Transition(r.commands.Get(), result->colour.resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Transition(r.commands.Get(), status.resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    r.commands->CopyBufferRegion(status_readback.resource.Get(), 0, status.resource.Get(), 0, sizeof(uint32_t));
    Check(r.commands->Close(), "Close frozen palette expansion commands");
    ID3D12CommandList* lists[] = {r.commands.Get()}; r.Execute(1, lists);
    frame.fence_value = r.Signal(); r.Wait(frame.fence_value);
    D3D12_RANGE read = {0, sizeof(uint32_t)};
    Check(status_readback.resource->Map(0, &read, &mapped), "Map frozen index status");
    uint32_t failed = 0; std::memcpy(&failed, mapped, sizeof(failed)); status_readback.resource->Unmap(0, &no_read);
    r.CollectMessages();
    if (failed) throw std::runtime_error("Palette compositor encountered an invalid index or shadow sample outside its initialized mask");
    r.frozen_frames.emplace_back(result);
    ++r.diagnostics.frozen_preparations;
    FrozenGpuFrame output; output.state = std::move(result); return output;
}

bool Device::PresentFrozenFrame(const FrozenGpuFrame& frozen)
{
    auto& r = *impl;
    const auto owned = r.GetFrozen(frozen);
    if (r.suspended) return false;
    if (r.width != owned->width || r.height != owned->height)
        throw std::invalid_argument("Frozen presentation requires matching native target dimensions");
    const UINT current = r.swapchain->GetCurrentBackBufferIndex();
    auto& frame = r.frames[current]; r.Wait(frame.fence_value); frame.frozen_reader.reset();
    Check(frame.allocator->Reset(), "Reset frozen presentation allocator");
    Check(r.commands->Reset(frame.allocator.Get(), nullptr), "Reset frozen presentation commands");
    Transition(r.commands.Get(), frame.backbuffer.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
    r.commands->CopyResource(frame.backbuffer.Get(), owned->colour.resource.Get());
    Transition(r.commands.Get(), frame.backbuffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
    Check(r.commands->Close(), "Close frozen presentation commands");
    frame.frozen_reader = owned;
    ID3D12CommandList* lists[] = {r.commands.Get()}; r.Execute(1, lists);
    frame.fence_value = r.Signal();
    const HRESULT hr = r.swapchain->Present(r.options.vsync ? 1 : 0,
        !r.options.vsync && r.diagnostics.tearing_supported ? DXGI_PRESENT_ALLOW_TEARING : 0);
    Check(hr, "Present frozen frame"); ++r.diagnostics.frames; ++r.diagnostics.frozen_presentations; r.CollectMessages();
    return hr != DXGI_STATUS_OCCLUDED;
}

std::vector<uint8_t> Device::ReadbackFrozenIndices(const FrozenGpuFrame& frame)
{
    const auto owned = impl->GetFrozen(frame);
    if(!owned->indices.resource)throw std::invalid_argument("Frozen enhanced frame has no index surface");
    const auto bytes = impl->ReadTexture(owned->indices.resource.Get(), owned->width, owned->height,
        DXGI_FORMAT_R32_UINT, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    std::vector<uint8_t> result(size_t(owned->width) * owned->height);
    for (size_t i = 0; i < result.size(); ++i) {
        uint32_t index = 0; std::memcpy(&index, bytes.data() + i * 4, sizeof(index));
        if (index > 255) throw std::runtime_error("Frozen index surface contains an invalid index");
        result[i] = uint8_t(index);
    }
    ++impl->diagnostics.frozen_image_readbacks;
    return result;
}

Image Device::ReadbackFrozenRGBA(const FrozenGpuFrame& frame)
{
    const auto owned = impl->GetFrozen(frame);
    Image result{owned->width, owned->height, impl->ReadTexture(owned->colour.resource.Get(),
        owned->width, owned->height, color_format, D3D12_RESOURCE_STATE_COPY_SOURCE)};
    ++impl->diagnostics.frozen_image_readbacks;
    return result;
}

Image Device::CompositeOutlines(const Image& background, const Image& atlas,
    const std::vector<OutlineDepthTriangle>& terrain, const std::vector<OutlinePassDraw>& draws)
{
    const std::array<float, 9> identity{1,0,0,0,1,0,0,0,1};
    auto surface = impl->RenderOutlineSurface(background, atlas, {}, identity, terrain, draws, false);
    return {background.width, background.height, impl->ReadTexture(surface.resource.Get(), background.width,
        background.height, color_format, D3D12_RESOURCE_STATE_COPY_SOURCE)};
}

Image Device::CompositeEnhancedSprites(const Image& background, const Image& atlas,
    const std::vector<SpriteColourRow>& cluts, const std::array<float, 9>& palette_transform,
    const std::vector<OutlineDepthTriangle>& terrain, const std::vector<OutlinePassDraw>& draws)
{
    auto surface = impl->RenderOutlineSurface(background, atlas, cluts, palette_transform, terrain, draws, true);
    return {background.width, background.height, impl->ReadTexture(surface.resource.Get(), background.width,
        background.height, color_format, D3D12_RESOURCE_STATE_COPY_SOURCE)};
}

FrozenGpuFrame Device::PrepareFrozenEnhancedWorldFrame(const EnhancedWorldPass& world,const Image& atlas,
    const std::vector<SpriteColourRow>& cluts,const std::array<float,9>& palette_transform,
    const std::vector<OutlinePassDraw>& draws,EnhancedWorldRasterOrigin raster_origin)
{
    impl->RequireHealthy();
    auto& r=*impl;
    if(!r.initialized)throw std::logic_error("Device has not been initialized");
    if(raster_origin!=EnhancedWorldRasterOrigin::ViewportLocal&&raster_origin!=EnhancedWorldRasterOrigin::Native)
        throw std::invalid_argument("Invalid enhanced world raster origin");
    ValidateEnhancedWorldPass(world,atlas.width,atlas.height,atlas.rgba.size(),cluts.size());
    r.Idle();
    r.frozen_frames.erase(std::remove_if(r.frozen_frames.begin(),r.frozen_frames.end(),
        [](const auto& weak){return weak.expired();}),r.frozen_frames.end());
    uint64_t resident_bytes=uint64_t(world.viewport_width)*world.viewport_height*4;
    for(const auto& weak:r.frozen_frames)if(auto frame=weak.lock())
        resident_bytes+=uint64_t(frame->width)*frame->height*(frame->indices.resource?8:4);
    if(r.frozen_frames.size()>=8||resident_bytes>512*1024*1024)
        throw std::invalid_argument("Frozen frame resident budget exceeded");
    auto result=std::make_shared<FrozenGpuFrame::State>();
    result->owner=r.frozen_owner;result->width=world.viewport_width;result->height=world.viewport_height;
    const Image target{world.viewport_width,world.viewport_height,{}};
    result->colour=r.RenderOutlineSurface(target,atlas,cluts,palette_transform,{},draws,true,&world,raster_origin);
    r.frozen_frames.push_back(result);++r.diagnostics.frozen_preparations;
    FrozenGpuFrame output;output.state=std::move(result);return output;
}

Image Device::CompositeEnhancedWorld(const EnhancedWorldPass& world,const Image& atlas,
    const std::vector<SpriteColourRow>& cluts,const std::array<float,9>& palette_transform,
    const std::vector<OutlinePassDraw>& draws,EnhancedWorldRasterOrigin raster_origin)
{
    auto frame=PrepareFrozenEnhancedWorldFrame(world,atlas,cluts,palette_transform,draws,raster_origin);
    return ReadbackFrozenRGBA(frame);
}

FrozenGpuFrame Device::PrepareFrozenEnhancedCompositionFrame(const EnhancedWorldPass& world,const Image& atlas,
    const std::vector<SpriteColourRow>& cluts,const std::array<float,9>& palette_transform,
    const std::vector<OutlinePassDraw>& draws,const SceneGLCompositionTrace& composition)
{
    impl->RequireHealthy();
    auto& r=*impl;
    if(!r.initialized)throw std::logic_error("Device has not been initialized");
    const auto require=[](bool good,const char* message){if(!good)throw std::invalid_argument(message);};
    ValidateEnhancedWorldPass(world,atlas.width,atlas.height,atlas.rgba.size(),cluts.size());
    ValidateWorldOverlayPass(composition.overlays);
    require(composition.present&&composition.complete&&composition.completed_phases==15
        &&composition.overlays.native_width==world.native_width&&composition.overlays.native_height==world.native_height,
        "Incomplete enhanced composition or mismatched native extent");
    std::array<uint32_t,3> counts{};
    for(const auto& quad:composition.overlays.draws)++counts.at(size_t(quad.phase));
    require(counts==composition.phase_quad_counts,"Enhanced overlay phase counts differ");
    for(float value:composition.screen_clear_rgba)require(std::isfinite(value)&&value>=0&&value<=1,
        "Invalid enhanced screen clear colour");
    require(composition.screen_clear_rgba[3]==1,"Enhanced screen clear alpha must be opaque");
    const auto& lens=composition.lens;
    require(lens.operation<=SceneGLLensOperation::NotDrawn&&lens.requested_type<=4,"Invalid enhanced lens operation");
    if(!lens.capture_began)require(composition.screen_clear_rgba==world.clear_rgba,"Screen world and composition clears differ");
    const bool lens_draw=lens.operation>=SceneGLLensOperation::Copy&&lens.operation<=SceneGLLensOperation::Overlay;
    if(!lens.requested_active)require(lens==SceneGLLensTrace{},"Inactive enhanced lens contains execution data");
    else if(!lens_draw)require(lens.operation==SceneGLLensOperation::NotDrawn&&!lens.fallback_copy
        &&lens.pass==SceneGLLensPass{}&&lens.resources==std::array<SceneGLUploadStamp,6>{},"Undrawn enhanced lens contains draw inputs");
    else{
        const auto& pass=lens.pass;ValidateLensPass(pass);
        require(lens.capture_began&&uint8_t(pass.kind)==uint8_t(lens.operation)-1
            &&pass.source_width==world.native_width&&pass.target_width==world.native_width
            &&pass.source_height==world.native_height&&pass.target_height==world.native_height
            &&pass.source_x==pass.destination_x
            &&lens.fallback_copy==(lens.operation==SceneGLLensOperation::Copy&&lens.requested_type!=0),
            "Enhanced lens source, target or effective operation differs");
        require((lens.operation!=SceneGLLensOperation::Mist||lens.requested_type==1)
            &&(lens.operation!=SceneGLLensOperation::Remap||lens.requested_type==2||lens.requested_type==3)
            &&(lens.operation!=SceneGLLensOperation::Overlay||lens.requested_type==4),"Enhanced lens request and operation differ");
        const bool mist=lens.operation==SceneGLLensOperation::Mist,overlay=lens.operation==SceneGLLensOperation::Overlay;
        const std::array<bool,6> consumed{mist||overlay,mist,mist||overlay,mist,lens.operation==SceneGLLensOperation::Remap,overlay};
        for(size_t i=0;i<consumed.size();++i)require(consumed[i]
            ? lens.resources[i].generation&&lens.resources[i].revision : lens.resources[i]==SceneGLUploadStamp{},
            "Enhanced lens resource provenance missing or unused");
    }
    uint64_t asset_bytes=atlas.rgba.size()+uint64_t(cluts.size())*1024+uint64_t(world.tiles.size())*1024
        +world.lightmap.size()*2+world.palette_rgba.size()+world.fade.size()
        +uint64_t(composition.overlays.colour_rows.size())*1024+composition.overlays.fade.size();
    for(const auto& source:composition.overlays.sources)asset_bytes+=source.indices.size()+source.initialized_rows.size();
    const auto& p=lens.pass;
    asset_bytes+=uint64_t(p.remap.size())*4+p.overlay.size()+p.density.size()+p.palette_rgba.size()+p.fade.size()+p.rgb6_lookup.size();
    require(asset_bytes<=SceneGLCompositionOwnedByteLimit,"Enhanced composition owned textures exceed 96 MiB");
    r.Idle();
    r.frozen_frames.erase(std::remove_if(r.frozen_frames.begin(),r.frozen_frames.end(),
        [](const auto& weak){return weak.expired();}),r.frozen_frames.end());
    uint64_t resident_bytes=uint64_t(world.native_width)*world.native_height*4;
    for(const auto& weak:r.frozen_frames)if(auto frame=weak.lock())
        resident_bytes+=uint64_t(frame->width)*frame->height*(frame->indices.resource?8:4);
    require(r.frozen_frames.size()<8&&resident_bytes<=512*1024*1024,"Frozen frame resident budget exceeded");
    auto result=std::make_shared<FrozenGpuFrame::State>();
    result->owner=r.frozen_owner;result->width=world.native_width;result->height=world.native_height;
    const Image viewport{world.viewport_width,world.viewport_height,{}};
    auto source=r.RenderOutlineSurface(viewport,atlas,cluts,palette_transform,{},draws,true,&world,
        EnhancedWorldRasterOrigin::Native,&composition.overlays,true);
    if(lens.capture_began){
        LensPass pass=lens.pass;
        if(!lens_draw){
            // The original world was offscreen but no lens draw executed.
            // Only the captured screen clear is visible; no implicit copy.
            pass.source_width=pass.target_width=pass.viewport_width=world.native_width;
            pass.source_height=pass.target_height=pass.viewport_height=world.native_height;
        }
        result->colour=r.RenderLensSurface(pass,nullptr,source.resource.Get(),nullptr,&composition.screen_clear_rgba,lens_draw);
    }else result->colour=std::move(source);
    r.frozen_frames.push_back(result);++r.diagnostics.frozen_preparations;
    FrozenGpuFrame output;output.state=std::move(result);return output;
}

Image Device::CompositeLens(const LensPass& pass,const Image& native_source,const Image& native_target_background)
{
    auto surface=impl->RenderLensSurface(pass,&native_source,nullptr,&native_target_background,nullptr);
    return {pass.target_width,pass.target_height,impl->ReadTexture(surface.resource.Get(),
        pass.target_width,pass.target_height,color_format,D3D12_RESOURCE_STATE_COPY_SOURCE)};
}

void Device::Impl::RecordLens(D3D12_CPU_DESCRIPTOR_HANDLE target, ID3D12DescriptorHeap* heap,
    D3D12_GPU_DESCRIPTOR_HANDLE resources, const LensConstants& constants, bool draw)
{
    commands->SetGraphicsRootSignature(lens_root.Get());
    commands->SetPipelineState(lens_pipeline.Get());
    ID3D12DescriptorHeap* heaps[]{heap}; commands->SetDescriptorHeaps(1, heaps);
    commands->SetGraphicsRootDescriptorTable(0, resources);
    commands->SetGraphicsRoot32BitConstants(1, 20, &constants, 0);
    commands->OMSetRenderTargets(1, &target, FALSE, nullptr);
    const auto& rect = constants.rectangle_kind;
    const auto& native = constants.source_destination;
    const D3D12_VIEWPORT viewport{float(native[2]),float(native[3]),float(rect[0]),float(rect[1]),0,1};
    const D3D12_RECT scissor{LONG(native[2]),LONG(native[3]),LONG(native[2]+rect[0]),LONG(native[3]+rect[1])};
    commands->RSSetViewports(1, &viewport); commands->RSSetScissorRects(1, &scissor);
    commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    if (draw) commands->DrawInstanced(3, 1, 0, 0);
}
Resource Device::Impl::RenderLensSurface(const LensPass& pass,const Image* source_image,ID3D12Resource* resident_source,
    const Image* background,const std::array<float,4>* clear,bool draw)
{
    RequireHealthy();
    auto& r=*this;
    if(!r.initialized)throw std::logic_error("Device has not been initialized");
    // Reject malformed inputs before creating any resources or resetting a list.
    const auto constants=MakeLensConstants(pass);
    const auto matches=[](const Image& image,uint32_t width,uint32_t height){
        return image.width==width&&image.height==height
            &&image.rgba.size()==uint64_t(width)*height*4;
    };
    if(bool(source_image)==bool(resident_source)||bool(background)==bool(clear)
        ||(source_image&&!matches(*source_image,pass.source_width,pass.source_height))
        ||(background&&!matches(*background,pass.target_width,pass.target_height)))
        throw std::invalid_argument("Lens native image dimensions or storage do not match the pass");
    if(resident_source){
        D3D12_RESOURCE_DESC desc{};
#ifdef __MINGW32__
        resident_source->GetDesc(&desc);
#else
        desc=resident_source->GetDesc();
#endif
        if(desc.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||desc.Width!=pass.source_width
            ||desc.Height!=pass.source_height||desc.Format!=color_format||desc.SampleDesc.Count!=1)
            throw std::invalid_argument("Resident lens source extent or format mismatch");
    }
    if(clear)for(float value:*clear)if(!std::isfinite(value)||value<0||value>1)
        throw std::invalid_argument("Invalid lens target clear colour");
    r.Idle();r.CreateLensPipeline();
    auto& frame=r.frames[0];
    Check(frame.allocator->Reset(),"Reset lens allocator");
    Check(r.commands->Reset(frame.allocator.Get(),r.lens_pipeline.Get()),"Reset lens commands");
    ComPtr<ID3D12DescriptorHeap> lens_srv,lens_rtv;
    D3D12_DESCRIPTOR_HEAP_DESC heap{};heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap.NumDescriptors=7;heap.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    Check(r.device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(lens_srv.GetAddressOf())),"Create lens SRV descriptors");
    heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;heap.NumDescriptors=1;heap.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    Check(r.device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(lens_rtv.GetAddressOf())),"Create lens RTV descriptor");
    std::vector<Resource> textures,staging;
    textures.reserve(7);staging.reserve(8);
    const auto upload=[&](uint32_t width,uint32_t height,DXGI_FORMAT format,const uint8_t* pixels,size_t row_bytes,
        D3D12_RESOURCE_STATES state,bool render_target){
        auto desc=TextureDesc(width,height,format);
        if(render_target)desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        auto resource=r.Allocate(desc,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COPY_DEST,
            render_target?L"Owned lens target":L"Owned lens input");
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};UINT64 bytes=0;
        r.device->GetCopyableFootprints(&desc,0,1,0,&footprint,nullptr,nullptr,&bytes);
        auto transfer=r.Allocate(BufferDesc(bytes),D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ,L"Lens staging");
        void* pointer=nullptr;D3D12_RANGE no_read{0,0};
        Check(transfer.resource->Map(0,&no_read,&pointer),"Map lens staging");
        for(uint32_t y=0;y<height;++y)
            std::memcpy(static_cast<uint8_t*>(pointer)+footprint.Offset+size_t(y)*footprint.Footprint.RowPitch,
                pixels+size_t(y)*row_bytes,row_bytes);
        transfer.resource->Unmap(0,nullptr);
        D3D12_TEXTURE_COPY_LOCATION src{},dst{};
        src.pResource=transfer.resource.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;src.PlacedFootprint=footprint;
        dst.pResource=resource.resource.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        r.commands->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
        Transition(r.commands.Get(),resource.resource.Get(),D3D12_RESOURCE_STATE_COPY_DEST,state);
        staging.push_back(std::move(transfer));
        return resource;
    };
    Resource destination;
    if(background)destination=upload(pass.target_width,pass.target_height,color_format,background->rgba.data(),
        size_t(pass.target_width)*4,D3D12_RESOURCE_STATE_RENDER_TARGET,true);
    else{
        auto desc=TextureDesc(pass.target_width,pass.target_height,color_format);desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_CLEAR_VALUE value{};value.Format=color_format;std::copy(clear->begin(),clear->end(),value.Color);
        destination=r.Allocate(desc,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_RENDER_TARGET,L"Owned composition screen",&value);
    }
    r.device->CreateRenderTargetView(destination.resource.Get(),nullptr,CpuHandle(lens_rtv.Get()));
    if(clear)r.commands->ClearRenderTargetView(CpuHandle(lens_rtv.Get()),clear->data(),0,nullptr);
    auto srv_handle=CpuHandle(lens_srv.Get());
    const auto stride=r.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const auto input=[&](uint32_t width,uint32_t height,DXGI_FORMAT format,const uint8_t* pixels,size_t row_bytes){
        ID3D12Resource* resource=nullptr;
        if(pixels){
            textures.push_back(upload(width,height,format,pixels,row_bytes,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,false));
            resource=textures.back().resource.Get();
        }
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};view.Format=format;view.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;view.Texture2D.MipLevels=1;
        // Every slot exists with its shader-declared type. Unused bindings are
        // explicit null SRVs, never stale descriptors from another operator.
        r.device->CreateShaderResourceView(resource,&view,srv_handle);srv_handle.ptr+=stride;
    };
    const auto bytes=[](const std::vector<uint8_t>& values){return values.empty()?nullptr:values.data();};
    if(resident_source){
        Transition(r.commands.Get(),resident_source,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};view.Format=color_format;view.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;view.Texture2D.MipLevels=1;
        r.device->CreateShaderResourceView(resident_source,&view,srv_handle);srv_handle.ptr+=stride;
    }else input(pass.source_width,pass.source_height,color_format,source_image->rgba.data(),size_t(pass.source_width)*4);
    input(256,1,color_format,bytes(pass.palette_rgba),1024);
    input(256,64,DXGI_FORMAT_R8_UNORM,bytes(pass.fade),256);
    input(512,512,DXGI_FORMAT_R8_UNORM,bytes(pass.rgb6_lookup),512);
    input(256,256,DXGI_FORMAT_R8_UNORM,bytes(pass.density),256);
    input(pass.remap_width,pass.remap_height,DXGI_FORMAT_R16G16_UINT,
        pass.remap.empty()?nullptr:reinterpret_cast<const uint8_t*>(pass.remap.data()),size_t(pass.remap_width)*4);
    input(pass.overlay_width,pass.overlay_height,DXGI_FORMAT_R8_UNORM,bytes(pass.overlay),pass.overlay_width);
    r.RecordLens(CpuHandle(lens_rtv.Get()), lens_srv.Get(), GpuHandle(lens_srv.Get()), constants, draw);
    Transition(r.commands.Get(),destination.resource.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE);
    if(resident_source)Transition(r.commands.Get(),resident_source,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_SOURCE);
    Check(r.commands->Close(),"Close lens commands");
    ID3D12CommandList* lists[]={r.commands.Get()};r.Execute(1,lists);
    frame.fence_value=r.Signal();r.Wait(frame.fence_value);r.CollectMessages();
    // All uploads, source owners and descriptor heaps remain alive through the
    // fence. The caller explicitly chooses diagnostic readback or presentation.
    return destination;
}

Image Device::CompositeWorldOverlays(const WorldOverlayPass& pass, const Image& native_background,
    const std::vector<OutlineDepthTriangle>& terrain)
{
    if (!impl->initialized) throw std::logic_error("Device has not been initialized");
    ValidateWorldOverlayPass(pass);
    if (native_background.width != pass.native_width || native_background.height != pass.native_height
        || native_background.rgba.size() != uint64_t(pass.native_width) * pass.native_height * 4)
        throw std::invalid_argument("World overlay native background extent or byte count mismatch");
    const Image unused_atlas{1, 1, {0, 0, 0, 0}};
    const std::array<float, 9> identity{1,0,0,0,1,0,0,0,1};
    auto surface = impl->RenderOutlineSurface(native_background, unused_atlas, {}, identity, terrain, {}, false,
        nullptr, EnhancedWorldRasterOrigin::ViewportLocal, &pass);
    return {pass.native_width, pass.native_height, impl->ReadTexture(surface.resource.Get(), pass.native_width,
        pass.native_height, color_format, D3D12_RESOURCE_STATE_COPY_SOURCE)};
}

void Device::Impl::RecordSurfaceBegin(const WorldSurfaceTarget& target, const float* clear_rgba)
{
    commands->ClearDepthStencilView(target.write_depth, D3D12_CLEAR_FLAG_DEPTH, 1, 0, 0, nullptr);
    commands->RSSetViewports(1, &target.viewport); commands->RSSetScissorRects(1, &target.scissor);
    if (clear_rgba) commands->ClearRenderTargetView(target.colour, clear_rgba, 0, nullptr);
}

void Device::Impl::RecordEnhancedWorld(const WorldSurfaceTarget& target, ID3D12DescriptorHeap* heap,
    D3D12_GPU_DESCRIPTOR_HANDLE table, const std::array<float,16>& constants,
    const D3D12_VERTEX_BUFFER_VIEW& terrain, const D3D12_VERTEX_BUFFER_VIEW& shadows,
    bool finish_terrain, bool terrain_ready)
{
    commands->SetGraphicsRootSignature(enhanced_world_root.Get());
    ID3D12DescriptorHeap* heaps[]{heap}; commands->SetDescriptorHeaps(1, heaps);
    commands->SetGraphicsRootDescriptorTable(0, table);
    commands->SetGraphicsRoot32BitConstants(1, UINT(constants.size()), constants.data(), 0);
    commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    if (!terrain_ready) {
        commands->IASetVertexBuffers(0, 1, &terrain); commands->SetPipelineState(enhanced_terrain.Get());
        commands->OMSetRenderTargets(1, &target.colour, FALSE, &target.write_depth);
        if (terrain.SizeInBytes) commands->DrawInstanced(terrain.SizeInBytes / terrain.StrideInBytes, 1, 0, 0);
        if (!finish_terrain) return;
        Transition(commands.Get(), target.depth, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_DEPTH_READ);
    }
    commands->OMSetRenderTargets(1, &target.colour, FALSE, &target.read_depth);
    if (shadows.SizeInBytes) {
        commands->IASetVertexBuffers(0, 1, &shadows); commands->SetPipelineState(enhanced_shadow.Get());
        commands->DrawInstanced(shadows.SizeInBytes / shadows.StrideInBytes, 1, 0, 0);
    }
}

void Device::Impl::RecordOutlineSprites(const WorldSurfaceTarget& target, ID3D12DescriptorHeap* heap,
    D3D12_GPU_DESCRIPTOR_HANDLE table, const std::array<float,12>& matrix,
    const D3D12_VERTEX_BUFFER_VIEW& vertices, UINT depth_vertices, bool depth_ready,
    std::span<const OutlinePassDraw> draws, bool flat)
{
    commands->SetGraphicsRootSignature(outline_root.Get());
    ID3D12DescriptorHeap* heaps[]{heap}; commands->SetDescriptorHeaps(1, heaps);
    commands->SetGraphicsRootDescriptorTable(0, table);
    commands->SetGraphicsRoot32BitConstants(1, UINT(matrix.size()), matrix.data(), 0);
    commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commands->IASetVertexBuffers(0, 1, &vertices);
    if (!depth_ready) {
        commands->SetPipelineState(outline_depth.Get());
        commands->OMSetRenderTargets(0, nullptr, FALSE, &target.write_depth);
        if (depth_vertices) commands->DrawInstanced(depth_vertices, 1, 0, 0);
        Transition(commands.Get(), target.depth, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_DEPTH_READ);
    }
    commands->OMSetRenderTargets(1, &target.colour, FALSE, &target.read_depth);
    for (size_t i = 0; i < draws.size(); ++i) {
        const auto& draw = draws[i];
        auto* pipeline = (draw.kind == OutlinePassKind::VisibleCoverage || draw.kind == OutlinePassKind::NormalSprite)
            ? outline_visible[draw.premultiplied].Get() : outline_hidden[draw.premultiplied].Get();
        if (draw.kind == OutlinePassKind::GlowAdd || draw.kind == OutlinePassKind::GlowDarken)
            pipeline = sprite_glow[draw.premultiplied][draw.kind == OutlinePassKind::GlowDarken].Get();
        if(flat) pipeline=(draw.kind==OutlinePassKind::GlowAdd||draw.kind==OutlinePassKind::GlowDarken)
            ?cursor_glow[draw.premultiplied][draw.kind==OutlinePassKind::GlowDarken].Get():cursor_visible[draw.premultiplied].Get();
        commands->SetPipelineState(pipeline); commands->DrawInstanced(6, 1, depth_vertices + UINT(i) * 6, 0);
    }
}

uint32_t Device::Impl::RecordWorldOverlays(const WorldSurfaceTarget& target, ID3D12DescriptorHeap* heap,
    D3D12_GPU_DESCRIPTOR_HANDLE sources, D3D12_GPU_DESCRIPTOR_HANDLE tables, UINT stride, UINT null_source,
    const D3D12_VERTEX_BUFFER_VIEW& vertices, uint32_t native_width, uint32_t native_height,
    std::span<const WorldOverlayQuad> draws, const RuntimePostDraw* post)
{
    commands->IASetVertexBuffers(0, 1, &vertices);
    commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commands->SetGraphicsRootSignature(world_overlay_root.Get());
    ID3D12DescriptorHeap* heaps[]{heap}; commands->SetDescriptorHeaps(1, heaps);
    commands->SetGraphicsRootDescriptorTable(1, tables);
    const D3D12_VIEWPORT viewport{0,0,float(native_width),float(native_height),0,1};
    commands->RSSetViewports(1, &viewport);
    commands->OMSetRenderTargets(1, &target.colour, FALSE, &target.read_depth);
    uint32_t recorded = 0;
    for (size_t i = 0; i < draws.size(); ++i) {
        const auto& quad = draws[i];
        D3D12_RECT scissor{0,0,LONG(native_width),LONG(native_height)};
        if (quad.scissor_enabled) {
            const auto clip = [](int64_t value, uint32_t limit) { return LONG(std::clamp<int64_t>(value, 0, limit)); };
            scissor = {clip(quad.scissor_x, native_width), clip(quad.scissor_y, native_height),
                clip(int64_t(quad.scissor_x) + quad.scissor_w, native_width),
                clip(int64_t(quad.scissor_y) + quad.scissor_h, native_height)};
        }
        // Preserve the GL empty result without submitting a D3D12 empty scissor.
        if (scissor.left == scissor.right || scissor.top == scissor.bottom) continue;
        commands->RSSetScissorRects(1, &scissor);
        auto source = sources;
        source.ptr += UINT64(quad.source == NoWorldOverlayResource ? null_source : quad.source) * stride;
        commands->SetGraphicsRootDescriptorTable(0, source);
        WorldOverlayConstants constants{{float(native_width),float(native_height)},
            uint32_t(quad.mode),quad.colour_row,quad.fade_row,{}};
        if(post && post->kind==RuntimePostKind::ImageZoom) {
            constants.zoom_map={post->zoom[0],post->zoom[1]};
            constants.zoom_screen={post->zoom[2],post->zoom[3]};constants.zoom_scale=post->zoom[4];
        }
        commands->SetGraphicsRoot32BitConstants(2, 16, &constants, 0);
        commands->SetPipelineState(quad.depth_test ? world_overlay_depth.Get() : world_overlay_flat.Get());
        commands->DrawInstanced(6, 1, UINT(i * 6), 0);
        ++recorded;
    }
    return recorded;
}

Resource Device::Impl::RenderOutlineSurface(const Image& background, const Image& atlas,
    const std::vector<SpriteColourRow>& cluts, const std::array<float, 9>& palette_transform,
    const std::vector<OutlineDepthTriangle>& terrain, const std::vector<OutlinePassDraw>& draws, bool enhanced,
    const EnhancedWorldPass* world,EnhancedWorldRasterOrigin raster_origin, const WorldOverlayPass* overlays,bool retain_native)
{
    RequireHealthy();
    auto& r = *this;
    const bool native_origin=raster_origin==EnhancedWorldRasterOrigin::Native;
    if((native_origin&&!world)||(retain_native&&!native_origin)||(raster_origin!=EnhancedWorldRasterOrigin::ViewportLocal&&!native_origin))
        throw std::invalid_argument("Invalid enhanced world raster origin");
    const auto valid_image = [](const Image& image) {
        return image.width && image.height && image.width <= 16384 && image.height <= 16384
            && uint64_t(image.width) * image.height * 4 <= 64 * 1024 * 1024
            && image.rgba.size() == size_t(image.width) * image.height * 4;
    };
    if(world){
        ValidateEnhancedWorldPass(*world,atlas.width,atlas.height,atlas.rgba.size(),cluts.size());
        if(!enhanced||!terrain.empty()||!background.rgba.empty()||background.width!=world->viewport_width||background.height!=world->viewport_height)
            throw std::invalid_argument("Inconsistent enhanced world render target");
    }
    if (!r.initialized || (!world && !valid_image(background)) || !valid_image(atlas)
        || terrain.size() > 262144 || draws.size() > 196608 || cluts.size() > 16384
        || atlas.rgba.size() + uint64_t(cluts.size()) * sizeof(SpriteColourRow) > 64 * 1024 * 1024)
        throw std::invalid_argument("Invalid outline diagnostic input");
    const uint32_t target_width=native_origin?world->native_width:background.width;
    const uint32_t target_height=native_origin?world->native_height:background.height;
    if (overlays) {
        ValidateWorldOverlayPass(*overlays);
        if (overlays->native_width != target_width || overlays->native_height != target_height)
            throw std::invalid_argument("World overlay target must retain the original native extent");
    }
    const uint64_t surface_bytes=uint64_t(target_width)*target_height*8
        +(native_origin&&!retain_native?uint64_t(background.width)*background.height*4:0);
    if(surface_bytes>192*1024*1024)
        throw std::invalid_argument("Enhanced world render surfaces exceed diagnostic budget");
    for (float value : palette_transform)
        if (!std::isfinite(value) || std::abs(value) > 1024)
            throw std::invalid_argument("Invalid sprite palette transform");
    struct OutlineVertex {
        float x = 0, y = 0, z = 0, w = 1, local_x = 0, local_y = 0;
        int32_t phase_x = 0, phase_y = 0, step_x = 1, step_y = 1;
        uint32_t mask_x = 0, mask_y = 0, source_w = 0, source_h = 0;
        uint32_t content_h = 0, flip = 0, kind = 0, premultiplied = 1;
        std::array<float, 4> colour{};
        uint32_t lookup_row = 0, texture_fallback = 0;
        float source_alpha = 1;
    };
    static_assert(sizeof(OutlineVertex) == 100);
    const size_t vertex_count = terrain.size() * 3 + draws.size() * 6;
    const uint64_t world_vertex_bytes=world?(world->terrain.size()*3+world->shadows.size()*6)*40:0;
    const uint64_t overlay_vertex_bytes=overlays?uint64_t(overlays->draws.size())*6*sizeof(WorldOverlayVertex):0;
    if (vertex_count * sizeof(OutlineVertex)+world_vertex_bytes+overlay_vertex_bytes > 128 * 1024 * 1024)
        throw std::invalid_argument("Outline geometry exceeds diagnostic budget");
    std::vector<OutlineVertex> vertices;
    vertices.reserve(vertex_count);
    for (const auto& triangle : terrain) for (const auto& point : triangle.vertices) {
        if (!std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.z) || !std::isfinite(point.w)
            || std::abs(point.x) > 65536 || std::abs(point.y) > 65536 || std::abs(point.z) > 16
            || point.w < 1 || point.w > 2147483648.0f || std::floor(point.w) != point.w)
            throw std::invalid_argument("Invalid outline depth vertex");
        vertices.push_back({point.x, point.y, point.z, point.w});
    }
    const UINT terrain_vertices = UINT(vertices.size());
    // The entire diagnostic atlas is a binary coverage image. Validate once,
    // rather than rescanning repeated animation frames for every draw.
    for (size_t i = 1; i < atlas.rgba.size(); i += 4)
        if (atlas.rgba[i] != 0 && atlas.rgba[i] != 255)
            throw std::invalid_argument("Outline coverage is not binary");
    for (const auto& draw : draws) {
        if (uint32_t(draw.kind) > (enhanced ? 5u : 2u) || draw.width <= 0 || draw.height <= 0 || draw.width > 16384 || draw.height > 16384
            || draw.x < -65536 || draw.x > 65536 || draw.y < -65536 || draw.y > 65536
            || draw.phase_x < -16777216 || draw.phase_x > 16777216 || draw.phase_y < -16777216 || draw.phase_y > 16777216
            || draw.step_x <= 0 || draw.step_x > 16777216 || draw.step_y <= 0 || draw.step_y > 16777216
            || !draw.source_width || !draw.source_height || draw.source_width > 1024u
            || draw.source_height > 1024u
            || !draw.content_height || draw.content_height > draw.source_height
            || uint64_t(draw.mask_x) + draw.source_width > atlas.width || uint64_t(draw.mask_y) + draw.source_height > atlas.height
            || !std::isfinite(draw.z_ndc) || std::abs(draw.z_ndc) > 16)
            throw std::invalid_argument("Invalid outline sprite mapping");
        const bool normal = draw.kind == OutlinePassKind::NormalSprite;
        if ((draw.texture_fallback && draw.premultiplied) || (!enhanced && draw.texture_fallback)
            || !std::isfinite(draw.source_alpha) || draw.source_alpha < 0 || draw.source_alpha > 1
            || (normal ? draw.lookup_row >= cluts.size() : draw.lookup_row != NoSpriteColourRow)
            || (!normal && draw.source_alpha != 1))
            throw std::invalid_argument("Inconsistent sprite path or colour binding");
        for (float channel : draw.colour) if (!std::isfinite(channel) || channel < 0 || channel > 1)
            throw std::invalid_argument("Invalid outline colour");
        const auto vertex = [&](float local_x, float local_y) {
            return OutlineVertex{2.0f * (draw.x + local_x) / background.width - 1.0f,
                1.0f - 2.0f * (draw.y + local_y) / background.height, draw.z_ndc, 1, local_x, local_y,
                draw.phase_x, draw.phase_y, draw.step_x, draw.step_y,
                draw.mask_x, draw.mask_y, draw.source_width, draw.source_height,
                draw.content_height, uint32_t(draw.flip), uint32_t(draw.kind), uint32_t(draw.premultiplied), draw.colour,
                draw.lookup_row, uint32_t(draw.texture_fallback), draw.source_alpha};
        };
        const OutlineVertex quad[] = {vertex(0,0),vertex(float(draw.width),0),
            vertex(float(draw.width),float(draw.height)),vertex(0,float(draw.height))};
        // Production instancing uses a TL,TR,BL,BR strip; fallback submits
        // TL,TR,BR / TL,BR,BL. Preserve the interpolation seam of each path.
        const std::array<int, 6> corners = draw.premultiplied
            ? std::array<int, 6>{0,1,3,3,1,2} : std::array<int, 6>{0,1,2,0,2,3};
        for (int corner : corners) vertices.push_back(quad[corner]);
    }
    r.Idle(); r.CreateOutlinePipelines();if(world)r.CreateEnhancedWorldPipelines();
    if (overlays && !overlays->draws.empty()) r.CreateWorldOverlayPipelines();
    auto& frame = r.frames[0];
    Check(frame.allocator->Reset(), "Reset outline allocator");
    Check(r.commands->Reset(frame.allocator.Get(), r.outline_depth.Get()), "Reset outline commands");
    auto colour_desc = TextureDesc(target_width, target_height, color_format);
    colour_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE colour_clear = {}; colour_clear.Format = color_format;
    if (world) std::copy(world->clear_rgba.begin(), world->clear_rgba.end(), colour_clear.Color);
    auto destination = r.Allocate(colour_desc, D3D12_HEAP_TYPE_DEFAULT,
        world?D3D12_RESOURCE_STATE_RENDER_TARGET:D3D12_RESOURCE_STATE_COPY_DEST, L"Owned enhanced colour",
        world ? &colour_clear : nullptr);
    const auto atlas_desc = TextureDesc(atlas.width, atlas.height, color_format);
    auto source = r.Allocate(atlas_desc, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST, L"Owned outline coverage");
    Resource lookup;
    const auto lookup_desc = TextureDesc(256, UINT(std::max<size_t>(1, cluts.size())), color_format);
    if (!cluts.empty())
        lookup = r.Allocate(lookup_desc, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST, L"Resolved sprite colour rows");
    auto depth_desc = TextureDesc(target_width, target_height, DXGI_FORMAT_D24_UNORM_S8_UINT);
    depth_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE depth_clear = {}; depth_clear.Format = depth_desc.Format; depth_clear.DepthStencil.Depth = 1;
    auto depth = r.Allocate(depth_desc, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_DEPTH_WRITE, L"Outline D24 terrain depth", &depth_clear);
    std::vector<Resource> staging;
    const auto upload_bytes = [&](ID3D12Resource* target, const D3D12_RESOURCE_DESC& desc,
        const uint8_t* pixels,size_t row_bytes,uint32_t height,D3D12_RESOURCE_STATES state) {
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {}; UINT64 bytes = 0;
        r.device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
        auto upload = r.Allocate(BufferDesc(bytes), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, L"Outline staging");
        void* mapped = nullptr; D3D12_RANGE no_read = {0,0};
        Check(upload.resource->Map(0, &no_read, &mapped), "Map outline staging");
        for (uint32_t y = 0; y < height; ++y)
            std::memcpy(static_cast<uint8_t*>(mapped) + footprint.Offset + size_t(y) * footprint.Footprint.RowPitch,
                pixels+size_t(y)*row_bytes,row_bytes);
        upload.resource->Unmap(0, nullptr);
        D3D12_TEXTURE_COPY_LOCATION src = {}, dst = {};
        src.pResource = upload.resource.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint = footprint;
        dst.pResource = target; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        r.commands->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        Transition(r.commands.Get(), target, D3D12_RESOURCE_STATE_COPY_DEST, state);
        staging.push_back(std::move(upload));
    };
    const auto upload_image = [&](ID3D12Resource* target,const D3D12_RESOURCE_DESC& desc,
        const Image& image,D3D12_RESOURCE_STATES state){
        upload_bytes(target,desc,image.rgba.data(),size_t(image.width)*4,image.height,state);
    };
    if(!world)upload_image(destination.resource.Get(), colour_desc, background, D3D12_RESOURCE_STATE_RENDER_TARGET);
    upload_image(source.resource.Get(), atlas_desc, atlas, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    if (!cluts.empty()) {
        Image lookup_image{256, UINT(cluts.size()), std::vector<uint8_t>(cluts.size() * sizeof(SpriteColourRow))};
        std::memcpy(lookup_image.rgba.data(), cluts.data(), lookup_image.rgba.size());
        upload_image(lookup.resource.Get(), lookup_desc, lookup_image, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
    auto vertex_buffer = r.Allocate(BufferDesc(std::max<size_t>(1, vertices.size()) * sizeof(OutlineVertex)), D3D12_HEAP_TYPE_UPLOAD,
        D3D12_RESOURCE_STATE_GENERIC_READ, L"Outline diagnostic vertices");
    void* mapped = nullptr; D3D12_RANGE no_read = {0,0};
    Check(vertex_buffer.resource->Map(0, &no_read, &mapped), "Map outline vertices");
    if (!vertices.empty()) std::memcpy(mapped, vertices.data(), vertices.size() * sizeof(OutlineVertex));
    vertex_buffer.resource->Unmap(0, nullptr);
    ComPtr<ID3D12DescriptorHeap> rtv_heap, dsv_heap, srv_heap;
    const auto heap = [&](D3D12_DESCRIPTOR_HEAP_TYPE type, UINT count, bool visible, ComPtr<ID3D12DescriptorHeap>& target) {
        D3D12_DESCRIPTOR_HEAP_DESC desc = {}; desc.Type = type; desc.NumDescriptors = count;
        desc.Flags = visible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        Check(r.device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(target.GetAddressOf())), "Create outline descriptors");
    };
    heap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, false, rtv_heap);
    heap(D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 2, false, dsv_heap);
    heap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2, true, srv_heap);
    auto colour_handle = CpuHandle(rtv_heap.Get()), write_depth = CpuHandle(dsv_heap.Get()), read_depth = write_depth;
    read_depth.ptr += r.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    r.device->CreateRenderTargetView(destination.resource.Get(), nullptr, colour_handle);
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv = {}; dsv.Format = depth_desc.Format; dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    r.device->CreateDepthStencilView(depth.resource.Get(), &dsv, write_depth);
    dsv.Flags = D3D12_DSV_FLAG_READ_ONLY_DEPTH | D3D12_DSV_FLAG_READ_ONLY_STENCIL;
    r.device->CreateDepthStencilView(depth.resource.Get(), &dsv, read_depth);
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {}; srv.Format = color_format; srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; srv.Texture2D.MipLevels = 1;
    r.device->CreateShaderResourceView(source.resource.Get(), &srv, CpuHandle(srv_heap.Get()));
    auto lookup_handle = CpuHandle(srv_heap.Get());
    lookup_handle.ptr += r.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    r.device->CreateShaderResourceView(lookup.resource.Get(), &srv, lookup_handle);
    D3D12_VIEWPORT viewport = {0,0,float(background.width),float(background.height),0,1};
    D3D12_RECT scissor = {0,0,LONG(background.width),LONG(background.height)};
    if(native_origin){
        viewport.TopLeftX=float(world->viewport_x);viewport.TopLeftY=float(world->viewport_y);
        scissor.left=world->viewport_x;scissor.top=world->viewport_y;
        scissor.right+=world->viewport_x;scissor.bottom+=world->viewport_y;
    }
    const WorldSurfaceTarget target{depth.resource.Get(), colour_handle, write_depth, read_depth, viewport, scissor};
    r.RecordSurfaceBegin(target, world ? world->clear_rgba.data() : nullptr);
    // These resources stay owned until this command list's fence completes.
    std::vector<Resource> world_textures;
    Resource world_vertices,shadow_vertices;
    ComPtr<ID3D12DescriptorHeap> world_heap;
    if(world){
        heap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,5,true,world_heap);
        auto handle=CpuHandle(world_heap.Get());
        const auto stride=r.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        const auto add_texture=[&](uint32_t width,uint32_t height,DXGI_FORMAT format,const uint8_t* pixels,size_t row_bytes,
            D3D12_RESOURCE_STATES state){
            const auto desc=TextureDesc(width,height,format);
            auto resource=r.Allocate(desc,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COPY_DEST,L"Owned enhanced world texture");
            upload_bytes(resource.resource.Get(),desc,pixels,row_bytes,height,state);
            D3D12_SHADER_RESOURCE_VIEW_DESC view{};view.Format=format;view.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
            view.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;view.Texture2D.MipLevels=1;
            r.device->CreateShaderResourceView(resource.resource.Get(),&view,handle);handle.ptr+=stride;
            world_textures.push_back(std::move(resource));
        };
        const uint32_t tile_height=uint32_t(std::max<size_t>(1,(world->tiles.size()+127)/128)*32);
        std::vector<uint8_t> tiles(size_t(4096)*tile_height);
        for(size_t tile=0;tile<world->tiles.size();++tile)for(size_t y=0;y<32;++y)
            std::memcpy(tiles.data()+(tile/128*32+y)*4096+tile%128*32,world->tiles[tile].data()+y*32,32);
        add_texture(4096,tile_height,DXGI_FORMAT_R8_UNORM,tiles.data(),4096,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        add_texture(256,1,color_format,world->palette_rgba.data(),1024,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        add_texture(511,511,DXGI_FORMAT_R16_UINT,reinterpret_cast<const uint8_t*>(world->lightmap.data()),511*2,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE|D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        add_texture(256,64,DXGI_FORMAT_R8_UNORM,world->fade.data(),256,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        r.device->CreateShaderResourceView(source.resource.Get(),&srv,handle);
        struct TerrainVertex {OutlineDepthVertex position;float u,v,shade,sx,sy;uint32_t tile;};
        struct ShadowVertex {float x,y,u,v,z,darken;uint32_t mask_x,mask_y,width,height;};
        static_assert(sizeof(TerrainVertex)==40&&sizeof(ShadowVertex)==40);
        std::vector<TerrainVertex> geometry;geometry.reserve(world->terrain.size()*3);
        for(const auto& triangle:world->terrain)for(const auto& v:triangle.vertices)
            geometry.push_back({v.position,v.u,v.v,v.shade,v.stl_x,v.stl_y,triangle.tile});
        std::vector<ShadowVertex> shadows;shadows.reserve(world->shadows.size()*6);
        constexpr int corners[]={0,1,2,0,2,3};
        for(const auto& shadow:world->shadows)for(int corner:corners){
            const auto& v=shadow.vertices[corner];
            shadows.push_back({v.x,v.y,v.u,v.v,shadow.z_ndc,shadow.darken,
                shadow.mask_x,shadow.mask_y,shadow.source_width,shadow.source_height});
        }
        const auto upload_vertices=[&](Resource& resource,const void* data,size_t bytes){
            resource=r.Allocate(BufferDesc(std::max<size_t>(1,bytes)),D3D12_HEAP_TYPE_UPLOAD,
                D3D12_RESOURCE_STATE_GENERIC_READ,L"Enhanced world geometry");
            void* pointer=nullptr;D3D12_RANGE no_read{0,0};
            Check(resource.resource->Map(0,&no_read,&pointer),"Map enhanced world geometry");
            if(bytes)std::memcpy(pointer,data,bytes);
            resource.resource->Unmap(0,nullptr);
        };
        upload_vertices(world_vertices,geometry.data(),geometry.size()*sizeof(TerrainVertex));
        upload_vertices(shadow_vertices,shadows.data(),shadows.size()*sizeof(ShadowVertex));
        std::array<float,16> constants{};
        for(size_t i=0;i<8;++i)constants[i]=world->shade_settings[i];
        constants[8]=world->shade_settings[8];constants[9]=world->time;
        constants[10]=float(world->fade_addressing);
        constants[11]=float(world->lighting_remaster | r.options.lighting_remaster);
        constants[12]=float(world->viewport_width);constants[13]=float(world->viewport_height);
        constants[14]=native_origin?0.0f:float(world->viewport_x);
        constants[15]=float(native_origin?world->native_height:world->native_height-uint32_t(world->viewport_y));
        const D3D12_VERTEX_BUFFER_VIEW terrain_view{world_vertices.resource->GetGPUVirtualAddress(),
            UINT(geometry.size()*sizeof(TerrainVertex)),sizeof(TerrainVertex)};
        const D3D12_VERTEX_BUFFER_VIEW shadow_view{shadow_vertices.resource->GetGPUVirtualAddress(),
            UINT(shadows.size()*sizeof(ShadowVertex)),sizeof(ShadowVertex)};
        r.RecordEnhancedWorld(target, world_heap.Get(), GpuHandle(world_heap.Get()), constants, terrain_view, shadow_view);
    }
    std::array<float, 12> matrix{};
    for (size_t row = 0; row < 3; ++row) for (size_t col = 0; col < 3; ++col)
        matrix[row * 4 + col] = palette_transform[row * 3 + col];
    const D3D12_VERTEX_BUFFER_VIEW vertex_view{vertex_buffer.resource->GetGPUVirtualAddress(),
        UINT(vertices.size() * sizeof(OutlineVertex)), sizeof(OutlineVertex)};
    r.RecordOutlineSprites(target, srv_heap.Get(), GpuHandle(srv_heap.Get()), matrix,
        vertex_view, terrain_vertices, world != nullptr, draws);
    // These owners outlive the submitted fence, just like the world resources.
    std::vector<Resource> overlay_textures;
    Resource overlay_vertices;
    ComPtr<ID3D12DescriptorHeap> overlay_heap;
    if (overlays && !overlays->draws.empty()) {
        const auto& pass = *overlays;
        const UINT null_source = UINT(pass.sources.size()), table_slot = null_source + 1;
        heap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, table_slot + 2, true, overlay_heap);
        const UINT stride = r.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        auto descriptor = CpuHandle(overlay_heap.Get());
        overlay_textures.reserve(pass.sources.size() + 2);
        const auto texture = [&](UINT width, UINT height, DXGI_FORMAT format, const uint8_t* pixels, size_t row_bytes) {
            ID3D12Resource* resource = nullptr;
            if (pixels) {
                const auto desc = TextureDesc(width, height, format);
                overlay_textures.push_back(r.Allocate(desc, D3D12_HEAP_TYPE_DEFAULT,
                    D3D12_RESOURCE_STATE_COPY_DEST, L"Owned world overlay lookup"));
                resource = overlay_textures.back().resource.Get();
                upload_bytes(resource, desc, pixels, row_bytes, height, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            }
            D3D12_SHADER_RESOURCE_VIEW_DESC view{};
            view.Format = format; view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; view.Texture2D.MipLevels = 1;
            r.device->CreateShaderResourceView(resource, &view, descriptor); descriptor.ptr += stride;
        };
        for (const auto& source : pass.sources)
            texture(source.width, source.height, DXGI_FORMAT_R8_UNORM, source.indices.data(), source.width);
        texture(1, 1, DXGI_FORMAT_R8_UNORM, nullptr, 1); // Typed null for solid draws.
        texture(256, UINT(pass.colour_rows.size()), color_format, pass.colour_rows.empty() ? nullptr
            : reinterpret_cast<const uint8_t*>(pass.colour_rows.data()), 1024);
        texture(256, 64, DXGI_FORMAT_R8_UNORM, pass.fade.empty() ? nullptr : pass.fade.data(), 256);
        std::vector<WorldOverlayVertex> geometry;
        geometry.reserve(pass.draws.size() * 6);
        for (const auto& quad : pass.draws) {
            const WorldOverlayVertex corners[] = {
                {quad.x0,quad.y0,quad.u0,quad.v0,quad.colour,quad.z_ndc},
                {quad.x1,quad.y0,quad.u1,quad.v0,quad.colour,quad.z_ndc},
                {quad.x1,quad.y1,quad.u1,quad.v1,quad.colour,quad.z_ndc},
                {quad.x0,quad.y1,quad.u0,quad.v1,quad.colour,quad.z_ndc}};
            for (unsigned corner : {0u,1u,2u,0u,2u,3u}) geometry.push_back(corners[corner]);
        }
        overlay_vertices = r.Allocate(BufferDesc(geometry.size() * sizeof(WorldOverlayVertex)),
            D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, L"Owned world overlay vertices");
        void* pointer = nullptr; D3D12_RANGE no_read{0,0};
        Check(overlay_vertices.resource->Map(0, &no_read, &pointer), "Map world overlay vertices");
        std::memcpy(pointer, geometry.data(), geometry.size() * sizeof(WorldOverlayVertex));
        overlay_vertices.resource->Unmap(0, nullptr);
        const D3D12_VERTEX_BUFFER_VIEW view{overlay_vertices.resource->GetGPUVirtualAddress(),
            UINT(geometry.size() * sizeof(WorldOverlayVertex)), sizeof(WorldOverlayVertex)};
        auto tables = GpuHandle(overlay_heap.Get()); tables.ptr += UINT64(table_slot) * stride;
        r.RecordWorldOverlays(target, overlay_heap.Get(), GpuHandle(overlay_heap.Get()), tables,
            stride, null_source, view, pass.native_width, pass.native_height, pass.draws);
    }
    Transition(r.commands.Get(), destination.resource.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Resource cropped;
    if(native_origin&&!retain_native){
        cropped=r.Allocate(TextureDesc(background.width,background.height,color_format),D3D12_HEAP_TYPE_DEFAULT,
            D3D12_RESOURCE_STATE_COPY_DEST,L"Owned enhanced native viewport crop");
        D3D12_TEXTURE_COPY_LOCATION src{},dst{};
        src.pResource=destination.resource.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.pResource=cropped.resource.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        const D3D12_BOX box{UINT(world->viewport_x),UINT(world->viewport_y),0,
            UINT(world->viewport_x)+background.width,UINT(world->viewport_y)+background.height,1};
        r.commands->CopyTextureRegion(&dst,0,0,0,&src,&box);
        Transition(r.commands.Get(),cropped.resource.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COPY_SOURCE);
    }
    Check(r.commands->Close(), "Close outline commands");
    ID3D12CommandList* lists[] = {r.commands.Get()}; r.Execute(1, lists);
    frame.fence_value = r.Signal(); r.Wait(frame.fence_value);
    r.CollectMessages();
    return native_origin&&!retain_native?std::move(cropped):std::move(destination);
}

void Device::WaitIdle() { impl->RequireHealthy(); impl->Idle(); impl->CollectMessages(); }
int32_t Device::DiagnosticRemoveDevice()
{
    ComPtr<ID3D12Device5> device5;
    Check(impl->device.As(&device5), "Diagnostic RemoveDevice unsupported");
    device5->RemoveDevice();
    return static_cast<int32_t>(impl->device->GetDeviceRemovedReason());
}
PresentationDiagnostics Device::GetPresentationDiagnostics()
{
    auto& r = *impl;
    auto result = r.presentation_diagnostics;
    result.enabled = r.options.presentation_diagnostics;
    result.waitable = r.options.waitable_presentation; result.permit_held = r.presentation_permit;
    if (!result.enabled || !r.swapchain) return result;
    const auto status = [](HRESULT hr) {
        if (hr == S_OK) return PresentationQueryStatus::Valid;
        if (hr == DXGI_ERROR_FRAME_STATISTICS_DISJOINT) return PresentationQueryStatus::Disjoint;
        if (hr == DXGI_ERROR_UNSUPPORTED || hr == E_NOTIMPL || hr == E_NOINTERFACE)
            return PresentationQueryStatus::Unsupported;
        return PresentationQueryStatus::Failed;
    };
    ComPtr<IDXGIOutput> output;
    HRESULT hr = r.swapchain->GetContainingOutput(output.GetAddressOf());
    DXGI_OUTPUT_DESC desc{};
    if (SUCCEEDED(hr)) hr = output->GetDesc(&desc);
    result.output_hresult = static_cast<int32_t>(hr); result.output_status = status(hr);
    if (hr == S_OK) {
        result.output_name = Utf8(desc.DeviceName);
        result.desktop_rect = {desc.DesktopCoordinates.left, desc.DesktopCoordinates.top,
            desc.DesktopCoordinates.right, desc.DesktopCoordinates.bottom};
        result.attached_to_desktop = desc.AttachedToDesktop != FALSE;
        DEVMODEW mode{}; mode.dmSize = sizeof(mode);
        // This is the OS mode's integer nominal refresh, not measured scanout.
        if (EnumDisplaySettingsExW(desc.DeviceName, ENUM_CURRENT_SETTINGS, &mode, 0)
            && (mode.dmFields & DM_DISPLAYFREQUENCY) && mode.dmDisplayFrequency > 1) {
            result.refresh_available = true; result.refresh_hz = mode.dmDisplayFrequency;
        }
    }
    DXGI_FRAME_STATISTICS statistics{};
    hr = r.swapchain->GetFrameStatistics(&statistics);
    result.frame_statistics_hresult = static_cast<int32_t>(hr); result.frame_statistics_status = status(hr);
    if (hr == S_OK) {
        result.present_count = statistics.PresentCount;
        result.present_refresh_count = statistics.PresentRefreshCount;
        result.sync_refresh_count = statistics.SyncRefreshCount;
        result.sync_qpc_time = statistics.SyncQPCTime.QuadPart;
        result.sync_gpu_time = statistics.SyncGPUTime.QuadPart;
    }
    UINT count = 0;
    hr = r.swapchain->GetLastPresentCount(&count);
    result.last_count_hresult = static_cast<int32_t>(hr); result.last_count_status = status(hr);
    if (hr == S_OK) result.last_present_count = count;
    return result;
}
Diagnostics Device::GetDiagnostics()
{
    auto& r = *impl;
    r.CollectMessages();
    if (r.memory) {
        D3D12MA::TotalStatistics statistics = {};
        r.memory->CalculateStatistics(&statistics);
        r.diagnostics.allocation_bytes = statistics.Total.Stats.AllocationBytes;
        r.diagnostics.allocation_count = statistics.Total.Stats.AllocationCount;
    }
    return r.diagnostics;
}
std::vector<std::string> Device::TakeValidationMessages()
{
    impl->CollectMessages();
    std::vector<std::string> result;
    result.swap(impl->messages);
    return result;
}
std::string Device::DeviceRemovedDetails() const
{
    auto& r = *impl;
    if (!r.device) return "No device was created";
    char reason[80];
    std::snprintf(reason, sizeof(reason), "Device removal HRESULT: 0x%08lx", static_cast<unsigned long>(r.device->GetDeviceRemovedReason()));
    std::string output = reason;
    ComPtr<ID3D12DeviceRemovedExtendedData1> dred;
    if (SUCCEEDED(r.device.As(&dred))) {
        D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 crumbs = {};
        if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput1(&crumbs))) {
            UINT count = 0;
            for (auto* node = crumbs.pHeadAutoBreadcrumbNode; node && count++ < 64; node = node->pNext)
                output += "\nBreadcrumb " + std::to_string(count) + ": " +
                    std::to_string(node->pLastBreadcrumbValue ? *node->pLastBreadcrumbValue : 0) + "/" + std::to_string(node->BreadcrumbCount);
        }
        D3D12_DRED_PAGE_FAULT_OUTPUT1 fault = {};
        if (SUCCEEDED(dred->GetPageFaultAllocationOutput1(&fault)))
            output += "\nPage fault address: " + std::to_string(fault.PageFaultVA);
    }
    return output;
}
} // namespace kfx::render::dx12
