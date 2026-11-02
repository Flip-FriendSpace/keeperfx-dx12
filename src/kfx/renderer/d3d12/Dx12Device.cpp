// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
// KeeperFX renderer performance changes: 2026-10-19.
// Device setup, presentation and frozen frame rendering.
#include "Dx12DeviceImpl.h"

namespace kfx::render::dx12 {
using Microsoft::WRL::ComPtr;

bool FrozenGpuFrame::Valid() const noexcept
{
    return state && !state->owner.expired() && state->colour.resource;
}
uint32_t FrozenGpuFrame::Width() const noexcept { return Valid() ? state->width : 0; }
uint32_t FrozenGpuFrame::Height() const noexcept { return Valid() ? state->height : 0; }
bool RuntimeAssetLease::Valid() const noexcept { return state && state->current && !state->owner.expired(); }
RuntimeAssetKey RuntimeAssetLease::Key() const noexcept { return state ? state->key : RuntimeAssetKey{}; }
RuntimeAssetDesc RuntimeAssetLease::Description() const noexcept { return state ? state->desc : RuntimeAssetDesc{}; }
bool RuntimeFrameTicket::Valid() const noexcept { return state && state->fence_value && !state->owner.expired(); }
uint64_t RuntimeFrameTicket::FenceValue() const noexcept { return Valid() ? state->fence_value : 0; }
bool RuntimeFrameTicket::HasReadback() const noexcept { return Valid() && state->readback.resource; }
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
RuntimeProcessBudgetScope::~RuntimeProcessBudgetScope() = default;
RuntimeProcessBudgetScope::RuntimeProcessBudgetScope(RuntimeProcessBudgetScope&&) noexcept = default;
RuntimeProcessBudgetScope& RuntimeProcessBudgetScope::operator=(RuntimeProcessBudgetScope&&) noexcept = default;
void RuntimeProcessBudgetScope::Reset() noexcept { state.reset(); }
bool RuntimeProcessBudgetScope::Valid() const noexcept { return bool(state); }

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
