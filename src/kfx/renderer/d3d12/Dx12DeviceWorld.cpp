// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
// KeeperFX renderer performance changes: 2026-10-19.
// Runtime world and indexed frame submission.
#include "Dx12DeviceImpl.h"

namespace kfx::render::dx12 {
using Microsoft::WRL::ComPtr;

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
            // A dropped frame can have carried the capture request, so an unknown newer ID captures now.
            if(r.map_fade_current&&m.transition_id<=r.map_fade_current->id)
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
    // Over-doubling must not reject a request that fits exactly, or admission would stall for good.
    if (new_heap && heap_capacity > descriptors
        && uint64_t(r.runtime_stats.descriptors) + heap_capacity + 4 > r.runtime_budgets.descriptors)
        heap_capacity = std::max<uint32_t>(64, descriptors);
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
        || uint64_t(packet.native_width) * packet.native_height > RuntimeMaxNativePixels
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

} // namespace kfx::render::dx12
