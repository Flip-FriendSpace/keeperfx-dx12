// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#include "RuntimeReclamationTests.h"
#include "EnhancedSpriteTestImages.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <sstream>
#include <thread>
#include <type_traits>

namespace kfx::render::dx12 {
namespace {
using namespace enhanced_test;
using Clock = std::chrono::steady_clock;
constexpr uint64_t KiB = 1024, MiB = 1024 * KiB;
constexpr uint32_t Side = 64;
using Colour = std::array<uint8_t, 4>;
Colour Ink(uint8_t n) { return {uint8_t(n * 13), uint8_t(n * 29), uint8_t(n * 53), 255}; }
void Pump()
{
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        Require(message.message != WM_QUIT, "Reclamation window closed");
        TranslateMessage(&message); DispatchMessageW(&message);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
}
RuntimeAssetKey Key(RuntimeAssetKind kind, uint64_t id, uint64_t revision = 1) { return {1, id, 1, revision, kind}; }
RuntimeAssetLease Publish(Device& d, RuntimeAssetKind kind, uint64_t id, uint32_t width, uint32_t height,
    const std::vector<uint8_t>& bytes, uint64_t revision = 1)
{
    return d.PublishRuntimeAsset(Key(kind, id, revision), {width, height}, std::make_shared<const std::vector<uint8_t>>(bytes));
}
std::vector<uint8_t> Palette()
{
    std::vector<uint8_t> bytes(1024);
    for (unsigned i = 0; i < 256; ++i) { const auto c = Ink(uint8_t(i)); std::copy(c.begin(), c.end(), bytes.begin() + i * 4); }
    return bytes;
}
std::vector<uint8_t> Artwork(uint32_t width, uint32_t height, uint8_t index)
{
    std::vector<uint8_t> bytes(size_t(width) * height * 4);
    for (size_t i = 0; i < bytes.size(); i += 4) { bytes[i] = index; bytes[i + 1] = 255; }
    return bytes;
}
RuntimeAssetLease Sprite(Device& d, uint64_t id, uint8_t index, uint64_t revision = 1,
    uint32_t width = 256, uint32_t height = 256)
{
    return Publish(d, RuntimeAssetKind::SpriteRGBA8, id, width, height, Artwork(width, height, index), revision);
}
RuntimeWorldFrame World(Device& d)
{
    RuntimeWorldFrame p; p.native_width = p.native_height = p.viewport_width = p.viewport_height = Side;
    p.clear_rgba = {8.0f / 255, 16.0f / 255, 24.0f / 255, 1};
    p.shade_settings = {0, 0, 1, 1, 0, 1, 1, 0, 0};
    p.palette = Publish(d, RuntimeAssetKind::PaletteRGBA8, 1, 256, 1, Palette());
    std::vector<uint8_t> fade(256 * 64); for (size_t i = 0; i < fade.size(); ++i) fade[i] = uint8_t(i);
    p.fade = Publish(d, RuntimeAssetKind::FadeR8, 2, 256, 64, fade);
    std::vector<uint8_t> light(511 * 511 * 2); for (size_t i = 1; i < light.size(); i += 2) light[i] = 32;
    p.lightmap = Publish(d, RuntimeAssetKind::LightmapR16, 3, 511, 511, light);
    return p;
}
RuntimeAssetLease Row(Device& d) { return Publish(d, RuntimeAssetKind::ColourRowRGBA8, 4, 256, 1, Palette()); }
RuntimeWorldSprite Draw(const RuntimeAssetLease& source, const RuntimeAssetLease& row,
    int x, int y, int width = 4, int height = 8, float z = -0.5f)
{
    OutlinePassDraw draw; draw.kind = OutlinePassKind::NormalSprite; draw.lookup_row = 0;
    draw.x = x; draw.y = y; draw.width = width; draw.height = height; draw.z_ndc = z;
    draw.source_width = source.Description().width; draw.source_height = draw.content_height = source.Description().height;
    return {draw, source, row};
}
Image Blank(Colour colour = {8, 16, 24, 255})
{
    Image image{Side, Side, std::vector<uint8_t>(Side * Side * 4)};
    for (size_t i = 0; i < image.rgba.size(); i += 4) std::copy(colour.begin(), colour.end(), image.rgba.begin() + i);
    return image;
}
void Paint(Image& image, int x, int y, int width, int height, Colour colour, bool behind = false)
{
    for (int j = std::max(0, y); j < std::min(int(Side), y + height); ++j)
        for (int i = std::max(0, x); i < std::min(int(Side), x + width); ++i) {
            if (behind && i >= 32 && i < 48 && j >= 32 && j < 48) continue;
            std::copy(colour.begin(), colour.end(), image.rgba.begin() + (j * Side + i) * 4);
        }
}
void Stats(std::ostream& o, const RuntimeResourceDiagnostics& s)
{
    o << "{\"cpuBytes\":" << s.cpu_bytes << ",\"defaultBytes\":" << s.default_bytes
      << ",\"uploadBytes\":" << s.upload_bytes << ",\"readbackBytes\":" << s.readback_bytes
      << ",\"descriptors\":" << s.descriptors << ",\"assetCount\":" << s.asset_count
      << ",\"worldPages\":" << s.world_pages << ",\"worldRegions\":" << s.world_regions
      << ",\"worldRegionAllocations\":" << s.world_region_allocations << ",\"worldRegionsReclaimed\":" << s.world_regions_reclaimed
      << ",\"pressureCollections\":" << s.pressure_collections << ",\"pressureRecoveries\":" << s.pressure_recoveries
      << ",\"evictions\":" << s.evictions << ",\"resourceAllocations\":" << s.resource_allocations
      << ",\"descriptorHeapAllocations\":" << s.descriptor_heap_allocations << ",\"worldUploadBytes\":" << s.world_upload_bytes
      << ",\"alignedUploadBytes\":" << s.aligned_upload_bytes << ",\"sourceUploadBytes\":" << s.source_upload_bytes
      << ",\"paletteUploadBytes\":" << s.palette_upload_bytes << ",\"submissions\":" << s.submissions
      << ",\"presentations\":" << s.presentations << ",\"heldSubmissions\":" << s.held_submissions
      << ",\"wouldBlock\":" << s.would_block << ",\"fenceWaits\":" << s.fence_waits
      << ",\"globalIdleCalls\":" << s.global_idle_calls << ",\"imageReadbacks\":" << s.image_readbacks
      << ",\"defaultHighWater\":" << s.default_high_water << ",\"uploadHighWater\":" << s.upload_high_water
      << ",\"readbackHighWater\":" << s.readback_high_water << ",\"descriptorHighWater\":" << s.descriptor_high_water
      << ",\"readerHoldSlots\":" << s.reader_hold_slots << ",\"readerHoldActive\":" << (s.reader_hold_active ? "true" : "false")
      << ",\"failed\":" << (s.failed ? "true" : "false") << ",\"quarantined\":" << (s.quarantined ? "true" : "false") << '}';
}
struct Runner {
    Device& device; std::filesystem::path output; RuntimeReclamationTestResult result;
    RuntimeResourceBudgets budgets; Diagnostics initial;
    std::vector<std::string> cases, comparisons, messages, pressure_probes;
    std::atomic<bool> watchdog{false}; std::string active;
    Runner(Device& d, const std::filesystem::path& p) : device(d), output(p), initial(d.GetDiagnostics()) {}
    void Check(bool ok, const char* why) { ++result.checks; Require(ok, why); }
    template<class Exception = std::invalid_argument, class F> void Reject(F&& fn, const char* why)
    { bool rejected = false; try { fn(); } catch (const Exception&) { rejected = true; } Check(rejected, why); }
    void Validation()
    {
        auto fresh = device.TakeValidationMessages(); messages.insert(messages.end(), fresh.begin(), fresh.end());
        const auto s = device.GetDiagnostics();
        Check(s.validation_errors == initial.validation_errors && s.validation_warnings == initial.validation_warnings,
            "Reclamation emitted GPU validation diagnostics");
    }
    void Bound()
    {
        const auto s = device.GetRuntimeResourceDiagnostics();
        Check(s.cpu_bytes <= budgets.cpu_bytes && s.default_bytes <= budgets.default_bytes && s.upload_bytes <= budgets.upload_bytes
            && s.readback_bytes <= budgets.readback_bytes && s.descriptors <= budgets.descriptors && s.asset_count <= budgets.assets
            && s.diagnostic_tickets <= budgets.diagnostic_tickets, "Reclamation exceeded configured actual allocation bounds");
    }
    void Configure(uint64_t default_bytes)
    {
        device.ClearRuntimeResources(); device.Resize(Side, Side); budgets = {}; budgets.default_bytes = default_bytes;
        device.ConfigureRuntimeResources(budgets);
    }
    void Case(const std::string& name, const std::function<void()>& body)
    {
        active = name; Validation(); const auto before = device.GetRuntimeResourceDiagnostics();
        try {
            body(); Validation(); ++result.cases; std::ostringstream o;
            o << "{\"name\":" << Json(name) << ",\"passed\":true,\"before\":"; Stats(o, before);
            o << ",\"after\":"; Stats(o, device.GetRuntimeResourceDiagnostics()); o << ",\"defaultByteCap\":" << budgets.default_bytes << ",\"uploadByteCap\":" << budgets.upload_bytes << ",\"readbackByteCap\":" << budgets.readback_bytes << ",\"descriptorCap\":" << budgets.descriptors << '}'; cases.push_back(o.str());
        } catch (const std::exception& e) {
            cases.push_back("{\"name\":" + Json(name) + ",\"passed\":false,\"error\":" + Json(e.what()) + "}"); throw;
        }
    }
    template<class Packet> RuntimeFrameTicket Submit(const Packet& p, bool diagnostic = true)
    {
        const auto until = Clock::now() + std::chrono::seconds(25);
        for (;;) {
            RuntimeSubmitResult submitted;
            if constexpr (std::is_same_v<Packet, RuntimeWorldFrame>) submitted = device.TrySubmitRuntimeWorldFrame(p, diagnostic);
            else submitted = device.TrySubmitRuntimeIndexedFrame(p, diagnostic);
            Bound(); Check(p.palette.Valid(), "Reclamation consumed producer palette");
            if (submitted.status == RuntimeSubmitStatus::Submitted) {
                ++result.submitted_frames; Check(submitted.ticket.Valid(), "Reclamation submission omitted ticket"); return submitted.ticket;
            }
            Check(submitted.status == RuntimeSubmitStatus::WouldBlock && !submitted.ticket.Valid(), "Unexpected reclamation submission status");
            Require(Clock::now() < until, "Reclamation submission remained blocked");
            device.CollectRuntimeResources(); Pump();
        }
    }
    void Complete(RuntimeFrameTicket& ticket)
    {
        const auto until = Clock::now() + std::chrono::seconds(25);
        while (!device.IsRuntimeFrameComplete(ticket)) { Require(Clock::now() < until, "Reclamation fence timeout"); Pump(); }
        ticket.Reset(); device.CollectRuntimeResources();
    }
    void Compare(RuntimeFrameTicket& ticket, const Image& wanted, const std::string& name)
    {
        Image image; const auto until = Clock::now() + std::chrono::seconds(25);
        while (!device.TryReadbackRuntimeFrame(ticket, image)) { Require(Clock::now() < until, "Reclamation readback timeout"); Pump(); }
        const auto different = Differences(image, wanted); const auto stem = "runtime-reclamation-" + name;
        Save(output / (stem + "-gpu"), image); Save(output / (stem + "-expected"), wanted);
        result.rgba_byte_checks += image.rgba.size();
        comparisons.push_back("{\"name\":" + Json(name) + ",\"gpuStem\":" + Json(stem + "-gpu")
            + ",\"expectedStem\":" + Json(stem + "-expected") + ",\"width\":64,\"height\":64,\"rgbaBytes\":"
            + std::to_string(image.rgba.size()) + ",\"differingBytes\":" + std::to_string(different) + "}");
        Validation(); Check(different == 0, "Reclamation differs from analytic output"); ticket.Reset(); device.CollectRuntimeResources();
    }
    void NoWait(const RuntimeResourceDiagnostics& before)
    {
        const auto after = device.GetRuntimeResourceDiagnostics();
        Check(after.global_idle_calls == before.global_idle_calls && after.fence_waits == before.fence_waits,
            "Reclamation inserted a global idle or internal GPU wait");
    }
    void Write(bool passed, const std::string& error, const RuntimeResourceDiagnostics& before_cleanup)
    {
        std::ofstream o(output / "runtime-reclamation-tests.json");
        o << "{\"schema\":1,\"scope\":\"Public-API bounded world page region reuse, cache-pressure retry, pending-reader safety and cross-page painter order; analytic expected pixels\""
          << ",\"passed\":" << (passed ? "true" : "false") << ",\"gate3Accepted\":false,\"manualEvictionUsed\":false"
          << ",\"readbackUsedAsInput\":false,\"frozenReferenceUsed\":false,\"pageRegionReclamationTested\":true,\"mixedSizeCoalescingTested\":true,\"overlaysTested\":false,\"lensesTested\":false"
          << ",\"liveProducerTested\":false,\"uploadRingReuseTested\":false,\"processDxgiBudgetTested\":false,\"gpuTimestampsTested\":false"
          << ",\"dirtyLightmapRectanglesTested\":false,\"checks\":" << result.checks << ",\"casesPassed\":" << result.cases
          << ",\"successfulSubmissions\":" << result.submitted_frames << ",\"rgbaByteChecks\":" << result.rgba_byte_checks
          << ",\"comparisonCount\":" << comparisons.size() << ",\"rawFiles\":" << comparisons.size() * 2
          << ",\"bmpFiles\":" << comparisons.size() * 2 << ",\"watchdogFired\":" << (watchdog.load() ? "true" : "false")
          << ",\"highWaterCountersScope\":\"Cumulative shared Device, including earlier runtime-world suite; per-case caps checked on actual owned bytes\""
          << ",\"error\":" << Json(error) << ",\"activeCase\":" << Json(active) << ",\"beforeCleanup\":"; Stats(o, before_cleanup);
        o << ",\"afterCleanup\":"; Stats(o, device.GetRuntimeResourceDiagnostics());
        const auto array = [&](const char* name, const std::vector<std::string>& values) {
            o << ",\"" << name << "\":["; for (size_t i = 0; i < values.size(); ++i) { if (i) o << ','; o << values[i]; } o << ']';
        };
        array("cases", cases); array("comparisons", comparisons); array("pressureProbes", pressure_probes);
        o << ",\"validationMessages\":["; for (size_t i = 0; i < messages.size(); ++i) { if (i) o << ','; o << Json(messages[i]); }
        o << "]}\n"; Require(bool(o), "Cannot retain reclamation report");
    }
};
struct Hold {
    RuntimeReaderHold handle; std::jthread timer; std::atomic<bool>& fired;
    Hold(Device& device, std::atomic<bool>& flag) : fired(flag)
    {
        const auto until = Clock::now() + std::chrono::seconds(25);
        while (!device.TryBeginRuntimeReaderHold(handle)) { Require(Clock::now() < until, "Cannot arm reclamation hold"); device.CollectRuntimeResources(); Pump(); }
        timer = std::jthread([this](std::stop_token stop) {
            const auto until = Clock::now() + std::chrono::seconds(15);
            while (!stop.stop_requested()) { if (Clock::now() >= until) { fired.store(true); handle.Release(); } std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
        });
    }
    void Release() { const bool ok = handle.Release(); timer.request_stop(); if (timer.joinable()) timer.join(); Require(ok && !fired.load(), "Reclamation hold release/watchdog failure"); }
    ~Hold() { timer.request_stop(); if (timer.joinable()) timer.join(); handle.Release(); }
};
}

RuntimeReclamationTestResult RunRuntimeReclamationTests(Device& device, const std::filesystem::path& output)
{
    Runner r(device, output);
    try {
        r.Case("one-page-survivor-64-revisions-without-manual-eviction", [&] {
            r.Configure(6 * MiB); auto packet = World(device); auto row = Row(device); std::vector<RuntimeAssetLease> sources;
            auto expected = Blank();
            for (unsigned i = 0; i < 16; ++i) { sources.push_back(Sprite(device, 1000 + i, uint8_t(i + 1))); packet.sprites.push_back(Draw(sources.back(), row, int(i * 4), 0)); Paint(expected, int(i * 4), 0, 4, 8, Ink(uint8_t(i + 1))); }
            auto first = r.Submit(packet); r.Compare(first, expected, "full-page");
            for (unsigned i = 0; i < 6; ++i) { auto t = r.Submit(packet, false); r.Complete(t); }
            packet.sprites.resize(1); sources.resize(1); const auto before = device.GetRuntimeResourceDiagnostics();
            r.Check(before.world_pages == 5, "Six-MiB fixture did not contain exactly one sprite page and mandatory tables");
            for (uint64_t revision = 2; revision <= 65; ++revision) {
                const uint8_t index = uint8_t(32 + revision); auto changed = Sprite(device, 1001, index, revision);
                packet.sprites.resize(1); packet.sprites.push_back(Draw(changed, row, 12, 12, 12, 12)); changed.Reset();
                auto wanted = Blank(); Paint(wanted, 0, 0, 4, 8, Ink(1)); Paint(wanted, 12, 12, 12, 12, Ink(index));
                auto ticket = r.Submit(packet, revision == 2 || revision % 8 == 0 || revision == 65);
                if (ticket.HasReadback()) r.Compare(ticket, wanted, "reuse-" + std::to_string(revision)); else r.Complete(ticket);
                r.Bound(); r.Check(sources[0].Valid() && device.GetRuntimeResourceDiagnostics().world_pages == before.world_pages,
                    "Live survivor invalidated or sprite page count grew during hole reuse");
            }
            const auto after = device.GetRuntimeResourceDiagnostics();
            r.Check(after.world_upload_bytes == before.world_upload_bytes + 64 * 256 * 256 * 4, "Region reuse uploaded unrelated source bytes");
            r.Check(after.world_regions_reclaimed >= before.world_regions_reclaimed + 64, "Completed replaced regions were not reclaimed");
            r.Reject([&] { Sprite(device, 1001, 2, 1); }, "Stale reclaimed asset revision was admitted"); r.NoWait(before);
        });
        r.Case("pending-reader-hole-protection-and-before-execute-rollback", [&] {
            r.Configure(6 * MiB); auto packet = World(device); auto row = Row(device); std::vector<RuntimeAssetLease> sources; auto expected = Blank();
            for (unsigned i = 0; i < 16; ++i) { sources.push_back(Sprite(device, 2000 + i, uint8_t(i + 1))); packet.sprites.push_back(Draw(sources.back(), row, int(i * 4), 0)); Paint(expected, int(i * 4), 0, 4, 8, Ink(uint8_t(i + 1))); }
            Hold hold(device, r.watchdog); auto pending = r.Submit(packet); const auto held = device.GetRuntimeResourceDiagnostics();
            auto changed = Sprite(device, 2001, 70, 2); packet.sprites.resize(1); packet.sprites.push_back(Draw(changed, row, 12, 12, 12, 12)); changed.Reset(); sources.resize(1);
            const auto denied = device.TrySubmitRuntimeWorldFrame(packet, true); r.Bound();
            r.Check(denied.status == RuntimeSubmitStatus::WouldBlock && denied.pressure == RuntimePressureReason::DefaultBytes && !denied.ticket.Valid(),
                "Pending full page was overwritten or pressure reason did not identify missing default capacity");
            r.Check(!device.IsRuntimeFrameComplete(pending) && packet.sprites[1].source.Valid(), "Pressure consumed packet or completed held reader");
            r.Check(device.GetRuntimeResourceDiagnostics().world_regions == held.world_regions, "Pressure reclaimed a region protected by pending reader");
            hold.Release(); r.Compare(pending, expected, "held-old-page"); device.CollectRuntimeResources(); const auto before = device.GetRuntimeResourceDiagnostics();
            device.SetRuntimeFailureInjection(RuntimeFailurePoint::BeforeExecute);
            r.Reject<std::runtime_error>([&] { device.TrySubmitRuntimeWorldFrame(packet, true); }, "Reclaimed-hole failure injection not reached");
            const auto rolled = device.GetRuntimeResourceDiagnostics();
            r.Check(rolled.world_regions == before.world_regions && rolled.world_region_allocations == before.world_region_allocations
                && rolled.world_upload_bytes == before.world_upload_bytes && rolled.submissions == before.submissions,
                "Abandoned reclaimed-hole reservation published region occupancy or submission");
            auto wanted = Blank(); Paint(wanted, 0, 0, 4, 8, Ink(1)); Paint(wanted, 12, 12, 12, 12, Ink(70));
            auto retry = r.Submit(packet); r.Compare(retry, wanted, "hole-rollback-retry"); r.NoWait(held);
        });
        r.Case("mixed-size-adjacent-holes-coalesce-with-live-neighbours", [&] {
            r.Configure(6 * MiB); auto packet = World(device); auto row = Row(device); std::vector<RuntimeAssetLease> sources;
            for (unsigned i = 0; i < 32; ++i) { sources.push_back(Sprite(device, 4000 + i, uint8_t(i + 1), 1, 128, 256)); packet.sprites.push_back(Draw(sources.back(), row, -16, -16)); }
            auto first = r.Submit(packet); r.Compare(first, Blank(), "coalesce-full-page");
            packet.sprites.clear(); sources[0].Reset(); sources[1].Reset();
            // Keep all other30 leases, so the only available contiguous256x256 is the union of these two128x256 holes.
            auto changed = Sprite(device, 4999, 99); packet.sprites.push_back(Draw(sources[2], row, 0, 0, 12, 12));
            packet.sprites.push_back(Draw(changed, row, 20, 20, 12, 12)); changed.Reset();
            auto expected = Blank(); Paint(expected, 0, 0, 12, 12, Ink(3)); Paint(expected, 20, 20, 12, 12, Ink(99));
            const auto before = device.GetRuntimeResourceDiagnostics(); auto t = r.Submit(packet); r.Compare(t, expected, "coalesced-hole");
            const auto after = device.GetRuntimeResourceDiagnostics();
            r.Check(after.world_pages == before.world_pages && after.world_regions_reclaimed >= before.world_regions_reclaimed + 2,
                "Adjacent retired rectangles were not coalesced into a bounded256x256 allocation");
            r.Check(after.pressure_recoveries > before.pressure_recoveries, "Current mixed-size cached regions were not evicted on pressure");
            for (size_t i = 2; i < sources.size(); ++i) r.Check(sources[i].Valid(), "Coalescing invalidated a neighbouring producer lease");
            r.NoWait(before);
        });
        r.Case("world-current-cache-pressure-recovery-and-tombstone", [&] {
            r.Configure(MiB); auto packet = World(device);
            for (unsigned i = 0; i < 6; ++i) { auto t = r.Submit(packet, false); r.Complete(t); }
            const auto before = device.GetRuntimeResourceDiagnostics(); std::vector<uint8_t> light(511 * 511 * 2, 0);
            packet.lightmap = Publish(device, RuntimeAssetKind::LightmapR16, 33, 511, 511, light);
            auto t = r.Submit(packet); r.Compare(t, Blank(), "world-pressure"); const auto after = device.GetRuntimeResourceDiagnostics();
            r.Check(after.pressure_collections > before.pressure_collections && after.pressure_recoveries > before.pressure_recoveries,
                "World did not recover automatically from default-cache pressure");
            r.Reject([&] { Publish(device, RuntimeAssetKind::LightmapR16, 3, 511, 511, light); }, "Evicted current world key ignored tombstone"); r.NoWait(before);
        });
        r.Case("indexed-current-cache-pressure-recovery-and-exact-output", [&] {
            r.Configure(320 * KiB); RuntimeIndexedFrame packet; packet.native_width = packet.native_height = packet.width = packet.height = Side;
            packet.palette = Publish(device, RuntimeAssetKind::PaletteRGBA8, 1, 256, 1, Palette());
            packet.indices = Publish(device, RuntimeAssetKind::Indexed8, 40, Side, Side, std::vector<uint8_t>(Side * Side, 7));
            for (unsigned i = 0; i < 6; ++i) { auto t = r.Submit(packet, false); r.Complete(t); }
            const auto before = device.GetRuntimeResourceDiagnostics();
            // Driver texture allocation sizes can be much smaller than64KiB.
            // Keep the cap fixed and fill current cache entries with distinct IDs;
            // normal stale-version collection cannot make this pressure disappear.
            unsigned admitted = 0; uint8_t index = 0; bool recovered = false;
            for (unsigned i = 1; i <= 128; ++i) {
                index = uint8_t(8 + i);
                packet.indices = Publish(device, RuntimeAssetKind::Indexed8, 40 + i, Side, Side, std::vector<uint8_t>(Side * Side, index));
                auto submitted = r.Submit(packet, false); r.Complete(submitted); admitted = i; r.Bound();
                const auto current = device.GetRuntimeResourceDiagnostics();
                if (current.pressure_recoveries > before.pressure_recoveries) { recovered = true; break; }
            }
            const auto after_pressure = device.GetRuntimeResourceDiagnostics();
            std::ostringstream probe; probe << "{\"name\":\"indexed-current-cache-default-pressure\",\"maximumNewSources\":128,\"newSourcesAdmitted\":"
                << admitted << ",\"sourceLogicalBytes\":4096,\"defaultByteCap\":" << r.budgets.default_bytes
                << ",\"recovered\":" << (recovered ? "true" : "false") << ",\"before\":"; Stats(probe, before);
            probe << ",\"afterPressure\":"; Stats(probe, after_pressure); probe << '}'; r.pressure_probes.push_back(probe.str());
            r.Check(recovered && after_pressure.pressure_collections > before.pressure_collections,
                "Indexed current cache did not recover under bounded forced default-byte pressure");
            auto t = r.Submit(packet); r.Compare(t, Blank(Ink(index)), "indexed-pressure");
            r.Reject([&] { Publish(device, RuntimeAssetKind::Indexed8, 40, Side, Side, std::vector<uint8_t>(Side * Side, 7)); }, "Evicted indexed key ignored tombstone"); r.NoWait(before);
        });
        r.Case("two-page-adjacent-only-painter-order-and-terrain-occlusion", [&] {
            r.Configure(12 * MiB); auto packet = World(device); auto row = Row(device); std::vector<RuntimeAssetLease> sources;
            for (unsigned i = 0; i < 17; ++i) { sources.push_back(Sprite(device, 3000 + i, uint8_t(i + 1))); packet.sprites.push_back(Draw(sources.back(), row, -16, -16)); }
            auto tile = Publish(device, RuntimeAssetKind::TileR8, 90, 32, 32, std::vector<uint8_t>(1024, 50));
            const EnhancedTerrainVertex a{{0,0,0,1},0,0,1,-1,-1}, b{{0.5f,0,0,1},0,0,1,-1,-1}, c{{0.5f,-0.5f,0,1},0,0,1,-1,-1}, e{{0,-0.5f,0,1},0,0,1,-1,-1};
            packet.terrain = {{{a,b,c},tile},{{a,c,e},tile}};
            auto wanted = Blank(); Paint(wanted, 32, 32, 16, 16, Ink(50));
            packet.sprites.push_back(Draw(sources[0], row, 4, 4, 24, 24)); Paint(wanted, 4, 4, 24, 24, Ink(1));
            packet.sprites.push_back(Draw(sources[16], row, 8, 8, 24, 24)); Paint(wanted, 8, 8, 24, 24, Ink(17));
            packet.sprites.push_back(Draw(sources[1], row, 12, 12, 24, 24)); Paint(wanted, 12, 12, 24, 24, Ink(2));
            packet.sprites.push_back(Draw(sources[16], row, 28, 28, 24, 24, 0.5f)); Paint(wanted, 28, 28, 24, 24, Ink(17), true);
            const auto before = device.GetRuntimeResourceDiagnostics(); auto t = r.Submit(packet); r.Compare(t, wanted, "two-page-order");
            r.Check(device.GetRuntimeResourceDiagnostics().world_pages == 7, "Order fixture did not allocate two sprite pages plus mandatory tables/tile"); r.NoWait(before);
        });
        const auto before = device.GetRuntimeResourceDiagnostics(); device.ClearRuntimeResources(); const auto after = device.GetRuntimeResourceDiagnostics();
        r.Check(!after.cpu_bytes && !after.default_bytes && !after.upload_bytes && !after.readback_bytes && !after.world_pages && !after.world_regions && !after.descriptors,
            "Reclamation cleanup retained resources"); r.Validation(); r.Write(true, "", before); return r.result;
    } catch (const std::exception& e) {
        const auto before = device.GetRuntimeResourceDiagnostics(); try { device.ClearRuntimeResources(); } catch (...) {}
        r.Write(false, e.what(), before); throw;
    }
}
}
