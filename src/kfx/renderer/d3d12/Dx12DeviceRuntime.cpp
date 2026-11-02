// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
// KeeperFX renderer performance changes: 2026-10-19.
// Runtime assets, residency, budgets and diagnostics.
#include "Dx12DeviceImpl.h"

namespace kfx::render::dx12 {
using Microsoft::WRL::ComPtr;

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
        if (length > RuntimeMaxNativePixels) throw std::invalid_argument("Runtime lens remap exceeds the native limit");
        length *= 4;
    } else if (key.kind != RuntimeAssetKind::Indexed8 || length > RuntimeMaxNativePixels)
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

} // namespace kfx::render::dx12
