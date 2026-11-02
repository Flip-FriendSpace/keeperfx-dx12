// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
// KeeperFX renderer performance changes: 2026-10-19.
// Private to the Dx12Device*.cpp files.
#pragma once
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
namespace device_detail {
constexpr UINT frame_count = 3;
constexpr DXGI_FORMAT color_format = DXGI_FORMAT_R8G8B8A8_UNORM;

inline void Check(HRESULT hr, const char* operation)
{
    if (FAILED(hr)) {
        char text[256];
        std::snprintf(text, sizeof(text), "%s failed (0x%08lx)", operation, static_cast<unsigned long>(hr));
        throw std::runtime_error(text);
    }
}
inline std::string Utf8(const wchar_t* text)
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
inline std::string ResizeWarningDescription(const ScopedResizeWarningCallback& scope,
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
inline std::vector<char> ReadShader(const std::filesystem::path& path)
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
inline D3D12_CPU_DESCRIPTOR_HANDLE CpuHandle(ID3D12DescriptorHeap* heap)
{
#ifdef __MINGW32__
    D3D12_CPU_DESCRIPTOR_HANDLE handle;
    heap->GetCPUDescriptorHandleForHeapStart(&handle);
    return handle;
#else
    return heap->GetCPUDescriptorHandleForHeapStart();
#endif
}
inline D3D12_GPU_DESCRIPTOR_HANDLE GpuHandle(ID3D12DescriptorHeap* heap)
{
#ifdef __MINGW32__
    D3D12_GPU_DESCRIPTOR_HANDLE handle;
    heap->GetGPUDescriptorHandleForHeapStart(&handle);
    return handle;
#else
    return heap->GetGPUDescriptorHandleForHeapStart();
#endif
}
inline D3D12_RESOURCE_DESC BufferDesc(UINT64 bytes)
{
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes; desc.Height = 1; desc.DepthOrArraySize = 1; desc.MipLevels = 1;
    desc.SampleDesc.Count = 1; desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return desc;
}
inline D3D12_RESOURCE_DESC TextureDesc(UINT width, UINT height, DXGI_FORMAT format)
{
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width; desc.Height = height; desc.DepthOrArraySize = 1; desc.MipLevels = 1;
    desc.Format = format; desc.SampleDesc.Count = 1;
    return desc;
}
inline void Transition(ID3D12GraphicsCommandList* commands, ID3D12Resource* resource,
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
} // namespace device_detail
using namespace device_detail;

struct FrozenGpuFrame::State {
    std::weak_ptr<uint8_t> owner;
    uint32_t width = 0, height = 0;
    Resource indices, colour;
};
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
struct RuntimeFrameTicket::State {
    std::weak_ptr<uint8_t> owner;
    uint64_t fence_value = 0;
    uint32_t width = 0, height = 0;
    Resource readback;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    uint64_t copy_bytes = 0;
};
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
struct RuntimeProcessBudgetScope::State {
    RuntimeProcessBudgetOverride provider;
    bool trimmed = false;
};

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
} // namespace kfx::render::dx12
