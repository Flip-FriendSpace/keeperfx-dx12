// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#include "RuntimeProcessBudgetTests.h"
#include "EnhancedSpriteTestImages.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <functional>
#include <iomanip>
#include <sstream>
#include <thread>
#include <type_traits>

namespace kfx::render::dx12 {
namespace {
using namespace enhanced_test;
using Bytes = std::vector<uint8_t>;
using Clock = std::chrono::steady_clock;
constexpr uint64_t MiB = 1024 * 1024;
constexpr int32_t FailedQuery = -2147467259;

void Pump()
{
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        Require(message.message != WM_QUIT, "Process-budget window closed");
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
}
std::array<uint8_t, 4> Ink(unsigned index, uint64_t revision)
{
    return {uint8_t(index * 13 + revision), uint8_t(index * 29 + revision * 3),
        uint8_t(index * 53 + revision * 7), 255};
}
Bytes Palette(uint64_t revision)
{
    Bytes bytes(1024);
    for (unsigned index = 0; index < 256; ++index) {
        const auto colour = Ink(index, revision);
        std::copy(colour.begin(), colour.end(), bytes.begin() + index * 4);
    }
    return bytes;
}
Bytes Indices(unsigned width, unsigned height, uint64_t revision)
{
    Bytes bytes(size_t(width) * height);
    for (unsigned y = 0; y < height; ++y)
        for (unsigned x = 0; x < width; ++x)
            bytes[size_t(y) * width + x] = uint8_t(x * 17 + y * 31 + revision);
    bytes[0] = 0;
    bytes[1] = 255;
    return bytes;
}
Bytes Grid(uint16_t value)
{
    Bytes bytes(511 * 511 * 2);
    for (size_t offset = 0; offset < bytes.size(); offset += 2) {
        bytes[offset] = uint8_t(value);
        bytes[offset + 1] = uint8_t(value >> 8);
    }
    return bytes;
}
uint16_t Light(const Bytes& bytes, unsigned x, unsigned y)
{
    const auto offset = (size_t(y) * 511 + x) * 2;
    return uint16_t(bytes[offset] | unsigned(bytes[offset + 1]) << 8);
}
void ChangeLight(Bytes& bytes, unsigned x, unsigned y, uint16_t value)
{
    const auto offset = (size_t(y) * 511 + x) * 2;
    bytes[offset] = uint8_t(value);
    bytes[offset + 1] = uint8_t(value >> 8);
}
RuntimeAssetLease Publish(Device& device, RuntimeAssetKind kind, uint64_t id,
    uint64_t revision, uint32_t width, uint32_t height, const Bytes& bytes)
{
    return device.PublishRuntimeAsset({1, id, 1, revision, kind}, {width, height},
        std::make_shared<const Bytes>(bytes));
}
struct WorldFixture {
    RuntimeWorldFrame packet;
    Bytes light;
    uint64_t revision = 1;
    uint8_t tile_index = 0;
};
void WorldRevision(Device& device, WorldFixture& fixture, uint64_t revision, uint16_t light)
{
    fixture.revision = revision;
    fixture.light = Grid(light);
    fixture.packet.lightmap = Publish(device, RuntimeAssetKind::LightmapR16, 3,
        revision, 511, 511, fixture.light);
    auto tile = Publish(device, RuntimeAssetKind::TileR8, 10, revision,
        32, 32, Bytes(1024, fixture.tile_index));
    for (auto& triangle : fixture.packet.terrain) triangle.tile = tile;
    const auto indices = Indices(17, 9, revision);
    Bytes source(indices.size() * 4);
    for (size_t index = 0; index < indices.size(); ++index) {
        source[index * 4] = indices[index];
        source[index * 4 + 1] = 255;
    }
    auto& sprite = fixture.packet.sprites[0];
    sprite.source = Publish(device, RuntimeAssetKind::SpriteRGBA8, 11, revision, 17, 9, source);
    sprite.colour_row = Publish(device, RuntimeAssetKind::ColourRowRGBA8, 12,
        revision, 256, 1, Palette(revision));
    const float shade = float(light) / 16384;
    const EnhancedTerrainVertex a{{-1, 1, 0, 1}, 0, 0, shade, 0, 0};
    const EnhancedTerrainVertex b{{1, 1, 0, 1}, 0, 0, shade, 64, 0};
    const EnhancedTerrainVertex c{{1, -1, 0, 1}, 0, 0, shade, 64, 64};
    const EnhancedTerrainVertex e{{-1, -1, 0, 1}, 0, 0, shade, 0, 64};
    fixture.packet.terrain[0].vertices = {a, b, c};
    fixture.packet.terrain[1].vertices = {a, c, e};
}
WorldFixture World(Device& device, uint32_t side = 64)
{
    WorldFixture fixture;
    auto& packet = fixture.packet;
    packet.native_width = packet.native_height = side;
    packet.viewport_width = packet.viewport_height = 64;
    packet.shade_settings = {0, 0, 1, 1, 1, 0, 1, 0, 0};
    Bytes palette(1024, 128);
    for (size_t offset = 3; offset < palette.size(); offset += 4) palette[offset] = 255;
    palette[4] = palette[5] = palette[6] = 160;
    packet.palette = Publish(device, RuntimeAssetKind::PaletteRGBA8, 1, 1, 256, 1, palette);
    packet.fade = Publish(device, RuntimeAssetKind::FadeR8, 2, 1, 256, 64, Bytes(16384));
    packet.terrain.resize(2);
    RuntimeWorldSprite sprite;
    sprite.draw.kind = OutlinePassKind::NormalSprite;
    sprite.draw.x = 7;
    sprite.draw.y = 9;
    sprite.draw.width = sprite.draw.source_width = 17;
    sprite.draw.height = sprite.draw.source_height = sprite.draw.content_height = 9;
    sprite.draw.lookup_row = 0;
    sprite.draw.z_ndc = -0.5f;
    packet.sprites.push_back(sprite);
    WorldRevision(device, fixture, 1, 8192);
    return fixture;
}
Image WorldExpected(const WorldFixture& fixture)
{
    const auto side = fixture.packet.native_width;
    Image result{side, side, Bytes(size_t(side) * side * 4)};
    for (size_t offset = 3; offset < result.rgba.size(); offset += 4) result.rgba[offset] = 255;
    const unsigned palette = fixture.tile_index ? 160 : 128;
    for (unsigned y = 0; y < 64; ++y) {
        for (unsigned x = 0; x < 64; ++x) {
            const uint64_t sum = unsigned(Light(fixture.light, x, y)) + Light(fixture.light, x + 1, y)
                + Light(fixture.light, x, y + 1) + Light(fixture.light, x + 1, y + 1);
            const auto value = uint8_t(std::min<uint64_t>(palette, (sum * palette + 32768) / 65536));
            const auto offset = (size_t(y) * side + x) * 4;
            result.rgba[offset] = result.rgba[offset + 1] = result.rgba[offset + 2] = value;
        }
    }
    const auto source = Indices(17, 9, fixture.revision);
    for (unsigned y = 0; y < 9; ++y) for (unsigned x = 0; x < 17; ++x) {
        const auto colour = Ink(source[y * 17 + x], fixture.revision);
        std::copy(colour.begin(), colour.end(), result.rgba.begin() + ((size_t(y) + 9) * side + x + 7) * 4);
    }
    return result;
}
struct IndexedFixture {
    RuntimeIndexedFrame packet;
    uint64_t source_revision = 1, palette_revision = 1;
    uint64_t source_id = 100, palette_id = 101;
};
void IndexedRevision(Device& device, IndexedFixture& fixture, uint64_t revision)
{
    fixture.source_revision = fixture.palette_revision = revision;
    fixture.packet.indices = Publish(device, RuntimeAssetKind::Indexed8,
        fixture.source_id, revision, 19, 11, Indices(19, 11, revision));
    fixture.packet.palette = Publish(device, RuntimeAssetKind::PaletteRGBA8,
        fixture.palette_id, revision, 256, 1, Palette(revision));
}
IndexedFixture Indexed(Device& device, uint64_t source = 100, uint64_t palette = 101)
{
    IndexedFixture fixture;
    fixture.source_id = source;
    fixture.palette_id = palette;
    fixture.packet.native_width = fixture.packet.native_height = 64;
    fixture.packet.x = 3;
    fixture.packet.y = 5;
    fixture.packet.width = 19;
    fixture.packet.height = 11;
    fixture.packet.clear_index = 17;
    IndexedRevision(device, fixture, 1);
    return fixture;
}
Image IndexedExpected(const IndexedFixture& fixture)
{
    Image result{64, 64, Bytes(64 * 64 * 4)};
    const auto clear = Ink(17, fixture.palette_revision);
    for (size_t offset = 0; offset < result.rgba.size(); offset += 4)
        std::copy(clear.begin(), clear.end(), result.rgba.begin() + offset);
    const auto source = Indices(19, 11, fixture.source_revision);
    for (unsigned y = 0; y < 11; ++y) for (unsigned x = 0; x < 19; ++x) {
        const auto colour = Ink(source[y * 19 + x], fixture.palette_revision);
        std::copy(colour.begin(), colour.end(), result.rgba.begin() + ((y + 5) * 64 + x + 3) * 4);
    }
    return result;
}
RuntimeProcessBudgetOverride Samples(int32_t result, uint64_t budget, uint64_t usage)
{
    RuntimeProcessBudgetOverride provider;
    for (auto& sample : provider.samples) sample = {result, budget, usage, 0, 0};
    return provider;
}


void Resources(std::ostream& output, const RuntimeResourceDiagnostics& value)
{
    output << "{\"submissions\":" << value.submissions << ",\"presentations\":" << value.presentations
        << ",\"heldSubmissions\":" << value.held_submissions << ",\"defaultBytes\":" << value.default_bytes
        << ",\"uploadBytes\":" << value.upload_bytes << ",\"readbackBytes\":" << value.readback_bytes
        << ",\"resourceAllocations\":" << value.resource_allocations << ",\"descriptorHeapAllocations\":" << value.descriptor_heap_allocations
        << ",\"worldPages\":" << value.world_pages << ",\"lightmapImages\":" << value.lightmap_images
        << ",\"stagingPools\":" << value.staging_pools << ",\"worldUploadBytes\":" << value.world_upload_bytes
        << ",\"sourceUploadBytes\":" << value.source_upload_bytes << ",\"paletteUploadBytes\":" << value.palette_upload_bytes
        << ",\"alignedUploadBytes\":" << value.aligned_upload_bytes << ",\"imageReadbacks\":" << value.image_readbacks
        << ",\"globalIdleCalls\":" << value.global_idle_calls << ",\"fenceWaits\":" << value.fence_waits
        << ",\"readerHoldSlots\":" << value.reader_hold_slots << ",\"failed\":" << (value.failed ? "true" : "false")
        << ",\"quarantined\":" << (value.quarantined ? "true" : "false") << '}';
}
void Process(std::ostream& output, const RuntimeProcessBudgetDiagnostics& value)
{
    output << std::setprecision(17) << "{\"uma\":" << (value.uma ? "true" : "false")
        << ",\"overrideActive\":" << (value.override_active ? "true" : "false")
        << ",\"notificationRegistered\":" << (value.notification_registered ? "true" : "false")
        << ",\"notificationHRESULT\":" << value.notification_hresult
        << ",\"unavailablePolicy\":" << unsigned(value.config.unavailable)
        << ",\"reserveBytes\":[" << value.config.reserve_bytes[0] << ',' << value.config.reserve_bytes[1] << ']'
        << ",\"growthRefreshes\":" << value.growth_refreshes << ",\"telemetryRefreshes\":" << value.telemetry_refreshes
        << ",\"existingBlockProbes\":" << value.existing_block_probes << ",\"existingBlockHits\":" << value.existing_block_hits
        << ",\"existingBlockMisses\":" << value.existing_block_misses
        << ",\"existingBlockUnsupported\":" << value.existing_block_unsupported
        << ",\"committedGrowths\":" << value.committed_growths << ",\"committedGrowthBytes\":" << value.committed_growth_bytes
        << ",\"budgetDenials\":" << value.budget_denials << ",\"unavailableDenials\":" << value.unavailable_denials
        << ",\"allocationDenials\":" << value.allocation_denials << ",\"compatibilityGrowths\":" << value.compatibility_growths
        << ",\"pressureTrims\":" << value.pressure_trims << ",\"pressureRecoveries\":" << value.pressure_recoveries
        << ",\"worldBootstraps\":" << value.world_bootstraps << ",\"indexedBootstraps\":" << value.indexed_bootstraps
        << ",\"bootstrapMs\":" << value.bootstrap_ms << ",\"lastAllocationHRESULT\":" << value.last_allocation_hresult
        << ",\"lastSegment\":" << value.last_segment << ",\"lastRequestedBytes\":" << value.last_requested_bytes
        << ",\"segments\":[";
    for (unsigned index = 0; index < 2; ++index) {
        if (index) output << ',';
        const auto& segment = value.segments[index];
        output << "{\"applicable\":" << (segment.applicable ? "true" : "false")
            << ",\"available\":" << (segment.available ? "true" : "false")
            << ",\"injected\":" << (segment.injected ? "true" : "false")
            << ",\"HRESULT\":" << segment.sample.hresult << ",\"budgetBytes\":" << segment.sample.budget_bytes
            << ",\"usageBytes\":" << segment.sample.usage_bytes
            << ",\"availableForReservationBytes\":" << segment.sample.available_for_reservation_bytes
            << ",\"reservationBytes\":" << segment.sample.reservation_bytes
            << ",\"sampleSerial\":" << segment.sample_serial << ",\"refreshes\":" << segment.refreshes
            << ",\"failures\":" << segment.failures << ",\"ageMs\":" << segment.age_ms
            << ",\"allocatorBlockBytes\":" << segment.allocator_block_bytes
            << ",\"allocatorAllocationBytes\":" << segment.allocator_allocation_bytes
            << ",\"allocatorBlockCount\":" << segment.allocator_block_count
            << ",\"allocatorAllocationCount\":" << segment.allocator_allocation_count
            << ",\"allocatorEstimatedUsageBytes\":" << segment.allocator_estimated_usage_bytes
            << ",\"allocatorEstimatedBudgetBytes\":" << segment.allocator_estimated_budget_bytes
            << ",\"allocatorBlockHighWater\":" << segment.allocator_block_high_water << '}';
    }
    output << "]}";
}
struct Snapshot {
    RuntimeResourceDiagnostics resources;
    RuntimeProcessBudgetDiagnostics process;
    RuntimeTimingDiagnostics timing;
};
void SnapshotJson(std::ostream& output, const Snapshot& value)
{
    output << "{\"resources\":";
    Resources(output, value.resources);
    output << ",\"process\":";
    Process(output, value.process);
    output << ",\"cpuStatus\":{";
    bool first = true;
    for (unsigned path = 0; path < 2; ++path) {
        const auto& timing = path ? value.timing.indexed : value.timing.world;
        for (unsigned bucket = 0; bucket < 3; ++bucket) {
            if (!first) output << ',';
            first = false;
            const auto& part = bucket == 0 ? timing.normal : bucket == 1 ? timing.diagnostic : timing.held;
            output << Json(std::string(path ? "indexed" : "world") + (bucket == 0 ? ".normal" : bucket == 1 ? ".diagnostic" : ".held"))
                << ":{\"submitted\":" << part.cpu_submitted.samples << ",\"wouldBlock\":" << part.cpu_would_block.samples
                << ",\"failed\":" << part.cpu_failed.samples << ",\"present\":" << part.cpu_present.samples << '}';
        }
    }
    output << "}}";
}
struct Runner {
    Device& device;
    std::filesystem::path output;
    RuntimeProcessBudgetTestResult result;
    Diagnostics baseline;
    std::vector<std::string> cases, measurements, comparisons, messages;
    std::atomic<bool> watchdog{false};
    std::string active;

    Runner(Device& target, const std::filesystem::path& path)
        : device(target), output(path), baseline(target.GetDiagnostics()) {}
    void Check(bool value, const char* reason) { ++result.checks; Require(value, reason); }
    template<class Exception = std::runtime_error, class Function>
    void Reject(Function&& function, const char* reason)
    {
        bool rejected = false;
        try { function(); } catch (const Exception&) { rejected = true; }
        Check(rejected, reason);
    }
    void Validation()
    {
        auto observed = device.TakeValidationMessages();
        messages.insert(messages.end(), observed.begin(), observed.end());
        const auto diagnostics = device.GetDiagnostics();
        Check(diagnostics.validation_errors == baseline.validation_errors
            && diagnostics.validation_warnings == baseline.validation_warnings, "Process-budget GPU validation diagnostics");
    }
    Snapshot Read()
    {
        Snapshot value{device.GetRuntimeResourceDiagnostics(), device.GetRuntimeProcessBudgetDiagnostics(), device.GetRuntimeTimingDiagnostics()};
        Check(std::isfinite(value.process.bootstrap_ms) && value.process.bootstrap_ms >= 0, "Invalid bootstrap timing");
        for (const auto& segment : value.process.segments)
            Check(std::isfinite(segment.age_ms) && segment.age_ms >= 0, "Invalid budget sample age");
        return value;
    }
    void Record(const std::string& name, const Snapshot& before, const Snapshot& after)
    {
        std::ostringstream record;
        record << "{\"name\":" << Json(name) << ",\"before\":";
        SnapshotJson(record, before);
        record << ",\"after\":";
        SnapshotJson(record, after);
        record << '}';
        measurements.push_back(record.str());
    }
    void NoWait(const Snapshot& before)
    {
        const auto after = device.GetRuntimeResourceDiagnostics();
        Check(after.global_idle_calls == before.resources.global_idle_calls
            && after.fence_waits == before.resources.fence_waits, "Process-budget path inserted GPU wait/idle");
    }
    void Configure(uint32_t side = 64)
    {
        device.ClearRuntimeResources();
        device.Resize(side, side);
        device.ConfigureRuntimeResources({});
        device.ConfigureRuntimeProcessBudget({});
    }
    void Case(const std::string& name, const std::function<void()>& function)
    {
        active = name;
        Validation();
        try {
            function();
            Validation();
            ++result.cases;
            cases.push_back("{\"name\":" + Json(name) + ",\"passed\":true,\"exercised\":true}");
        } catch (const std::exception& error) {
            cases.push_back("{\"name\":" + Json(name) + ",\"passed\":false,\"exercised\":true,\"error\":" + Json(error.what()) + '}');
            throw;
        }
    }
    void Skip(const std::string& name, const std::string& reason)
    {
        cases.push_back("{\"name\":" + Json(name) + ",\"exercised\":false,\"reason\":" + Json(reason) + '}');
    }
    template<class Packet> RuntimeFrameTicket Submit(const Packet& packet, bool readback = false)
    {
        const auto deadline = Clock::now() + std::chrono::seconds(25);
        for (;;) {
            RuntimeSubmitResult submitted;
            if constexpr (std::is_same_v<Packet, RuntimeWorldFrame>) submitted = device.TrySubmitRuntimeWorldFrame(packet, readback);
            else submitted = device.TrySubmitRuntimeIndexedFrame(packet, readback);
            if (submitted.status == RuntimeSubmitStatus::Submitted) {
                ++result.submitted_frames;
                Check(submitted.ticket.Valid() && submitted.ticket.HasReadback() == readback, "Process-budget ticket mismatch");
                return submitted.ticket;
            }
            Check(submitted.status == RuntimeSubmitStatus::WouldBlock && !submitted.ticket.Valid(), "Unexpected process-budget submit status");
            Require(Clock::now() < deadline, "Process-budget submission remained blocked");
            device.CollectRuntimeResources();
            Pump();
        }
    }
    void Complete(RuntimeFrameTicket& ticket)
    {
        const auto deadline = Clock::now() + std::chrono::seconds(25);
        while (!device.IsRuntimeFrameComplete(ticket)) {
            Require(Clock::now() < deadline, "Process-budget fence timeout");
            Pump();
        }
        ticket.Reset();
        device.CollectRuntimeResources();
    }
    void Compare(RuntimeFrameTicket& ticket, const Image& expected, const std::string& name, bool retain = false)
    {
        Image actual;
        const auto deadline = Clock::now() + std::chrono::seconds(25);
        while (!device.TryReadbackRuntimeFrame(ticket, actual)) {
            Require(Clock::now() < deadline, "Process-budget readback timeout");
            Pump();
        }
        const auto differences = Differences(actual, expected);
        const auto stem = "runtime-process-budget-" + name;
        Save(output / (stem + "-gpu"), actual);
        Save(output / (stem + "-expected"), expected);
        result.rgba_byte_checks += actual.rgba.size();
        comparisons.push_back("{\"name\":" + Json(name) + ",\"gpuStem\":" + Json(stem + "-gpu")
            + ",\"expectedStem\":" + Json(stem + "-expected") + ",\"width\":" + std::to_string(actual.width)
            + ",\"height\":" + std::to_string(actual.height) + ",\"rgbaBytes\":" + std::to_string(actual.rgba.size())
            + ",\"differingBytes\":" + std::to_string(differences) + '}');
        Check(differences == 0, "Process-budget pixels differ from independent analytic fixture");
        if (!retain) ticket.Reset();
        device.CollectRuntimeResources();
        Validation();
    }
    void Warm(WorldFixture& world, IndexedFixture& indexed)
    {
        // Six alternating pairs rotate both paths over all three Present slots.
        // Full changed lightmaps also warm the maximum staging footprint.
        for (uint64_t revision = 1; revision <= 6; ++revision) {
            if (revision > 1) {
                WorldRevision(device, world, revision, revision % 2 ? 8192 : 8704);
                IndexedRevision(device, indexed, revision);
            }
            auto first = Submit(world.packet);
            Complete(first);
            auto second = Submit(indexed.packet);
            Complete(second);
        }
        Check(device.GetRuntimeResourceDiagnostics().staging_pools == 3, "Mixed warmup did not create three staging pools");
    }
    void OuterStatus(const Snapshot& before, const Snapshot& after, bool world, bool accepted)
    {
        const auto& a = world ? before.timing.world.diagnostic : before.timing.indexed.diagnostic;
        const auto& b = world ? after.timing.world.diagnostic : after.timing.indexed.diagnostic;
        Check(b.cpu_submitted.samples == a.cpu_submitted.samples + unsigned(accepted)
            && b.cpu_would_block.samples == a.cpu_would_block.samples + unsigned(!accepted)
            && b.cpu_failed.samples == a.cpu_failed.samples, "Bounded retry recorded more than one outer CPU status");
    }
    void Write(bool passed, const std::string& error, const Snapshot& before)
    {
        std::ofstream report(output / "runtime-process-budget-tests.json");
        report << "{\"schema\":1,\"scope\":\"Injected process-budget admission over real runtime allocations, mixed resident progress, retained readers and allocator backing; real OS observations remain separate\",\"passed\":" << (passed ? "true" : "false")
            << ",\"gate3Accepted\":false,\"liveProducerTested\":false,\"osPressureForced\":false,\"releaseFpsQualified\":false,\"readbackUsedAsInput\":false"
            << ",\"physicalProcessPeakMeasured\":false,\"checks\":" << result.checks << ",\"casesPassed\":" << result.cases
            << ",\"successfulSubmissions\":" << result.submitted_frames << ",\"rgbaByteChecks\":" << result.rgba_byte_checks
            << ",\"comparisonCount\":" << comparisons.size() << ",\"watchdogFired\":" << (watchdog.load() ? "true" : "false")
            << ",\"error\":" << Json(error) << ",\"activeCase\":" << Json(active) << ",\"beforeCleanup\":";
        SnapshotJson(report, before);
        report << ",\"afterCleanup\":";
        SnapshotJson(report, Read());
        const auto array = [&](const char* name, const std::vector<std::string>& records) {
            report << ",\"" << name << "\":[";
            for (size_t index = 0; index < records.size(); ++index) {
                if (index) report << ',';
                report << records[index];
            }
            report << ']';
        };
        array("cases", cases);
        array("measurements", measurements);
        array("comparisons", comparisons);
        report << ",\"validationMessages\":[";
        for (size_t index = 0; index < messages.size(); ++index) {
            if (index) report << ',';
            report << Json(messages[index]);
        }
        report << "]}\n";
        Require(bool(report), "Cannot retain process-budget evidence");
    }
};
struct Hold {
    RuntimeReaderHold handle;
    std::jthread watchdog_thread;
    std::atomic<bool>& fired;
    Hold(Device& device, std::atomic<bool>& flag) : fired(flag)
    {
        const auto deadline = Clock::now() + std::chrono::seconds(25);
        while (!device.TryBeginRuntimeReaderHold(handle)) {
            Require(Clock::now() < deadline, "Cannot arm process-budget hold");
            device.CollectRuntimeResources();
            Pump();
        }
        watchdog_thread = std::jthread([this](std::stop_token stop) {
            const auto deadline = Clock::now() + std::chrono::seconds(15);
            while (!stop.stop_requested()) {
                if (Clock::now() >= deadline) {
                    fired.store(true);
                    handle.Release();
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        });
    }
    void Release()
    {
        const bool released = handle.Release();
        watchdog_thread.request_stop();
        if (watchdog_thread.joinable()) watchdog_thread.join();
        Require(released && !fired.load(), "Process-budget hold watchdog intervened");
    }
    ~Hold()
    {
        watchdog_thread.request_stop();
        if (watchdog_thread.joinable()) watchdog_thread.join();
        handle.Release();
    }
};
}


RuntimeProcessBudgetTestResult RunRuntimeProcessBudgetTests(Device& device, const Options& options,
    const std::filesystem::path& output)
{
    (void)options;
    Runner runner(device, output);
    try {
        runner.Case("real-os-observations-and-1000-mixed-resident-frames", [&] {
            runner.Configure();
            auto world = World(device);
            auto indexed = Indexed(device);
            runner.Warm(world, indexed);
            const auto before = runner.Read();
            runner.Check(!before.process.override_active && !before.process.segments[0].injected,
                "Real observations unexpectedly injected");
            for (unsigned index = 0; index < 1000; ++index) {
                auto ticket = index % 2 ? runner.Submit(indexed.packet) : runner.Submit(world.packet);
                runner.Complete(ticket);
            }
            const auto after = runner.Read();
            runner.Check(after.resources.submissions == before.resources.submissions + 1000
                && after.resources.presentations == before.resources.presentations + 1000
                && after.resources.resource_allocations == before.resources.resource_allocations
                && after.resources.descriptor_heap_allocations == before.resources.descriptor_heap_allocations
                && after.resources.world_upload_bytes == before.resources.world_upload_bytes
                && after.resources.source_upload_bytes == before.resources.source_upload_bytes
                && after.resources.palette_upload_bytes == before.resources.palette_upload_bytes
                && after.process.committed_growths == before.process.committed_growths,
                "Resident loop grew/reuploaded resources");
            runner.NoWait(before);
            runner.Record("real-resident1000", before, after);
            auto first = runner.Submit(world.packet, true);
            runner.Compare(first, WorldExpected(world), "real-world");
            auto second = runner.Submit(indexed.packet, true);
            runner.Compare(second, IndexedExpected(indexed), "real-indexed");
        });
        runner.Case("denied-growth-preserves-all-three-mixed-slots", [&] {
            runner.Configure();
            auto world = World(device);
            auto indexed = Indexed(device);
            runner.Warm(world, indexed);
            auto anchor = runner.Submit(indexed.packet, true);
            runner.Compare(anchor, IndexedExpected(indexed), "shrink-anchor", true);
            // Distinct keys leave warmed baseline inputs admissible after refusal.
            auto candidate = Indexed(device, 200, 201);
            RuntimeProcessBudgetConfig config;
            config.unavailable = RuntimeBudgetUnavailablePolicy::FailClosed;
            device.ConfigureRuntimeProcessBudget(config);
            auto scope = device.OverrideRuntimeProcessBudgetForDiagnostics(Samples(0, 0, 1));
            const auto before = runner.Read();
            auto denied = device.TrySubmitRuntimeIndexedFrame(candidate.packet, true);
            const auto after = runner.Read();
            runner.Check(denied.status == RuntimeSubmitStatus::WouldBlock
                && denied.pressure == RuntimePressureReason::ProcessBudgetLocal && !denied.ticket.Valid(),
                "Zero growth headroom admitted required committed texture");
            runner.Check(candidate.packet.indices.Valid() && candidate.packet.palette.Valid()
                && after.resources.submissions == before.resources.submissions
                && after.resources.source_upload_bytes == before.resources.source_upload_bytes
                && after.resources.palette_upload_bytes == before.resources.palette_upload_bytes
                && after.process.pressure_trims == before.process.pressure_trims + 1
                && after.process.committed_growths == before.process.committed_growths,
                "Denied growth consumed input or exceeded bounded trim");
            runner.OuterStatus(before, after, false, false);
            runner.Record("denied-required-growth", before, after);
            const auto ordinary_before = runner.Read();
            for (unsigned index = 0; index < 3; ++index) {
                auto first = runner.Submit(world.packet);
                runner.Complete(first);
                auto second = runner.Submit(indexed.packet);
                runner.Complete(second);
            }
            const auto ordinary_after = runner.Read();
            runner.Check(ordinary_after.resources.resource_allocations == ordinary_before.resources.resource_allocations
                && ordinary_after.resources.descriptor_heap_allocations == ordinary_before.resources.descriptor_heap_allocations
                && ordinary_after.process.committed_growths == ordinary_before.process.committed_growths,
                "Process trim evicted a warmed ordinary slot");
            runner.Record("post-trim-three-slot-ordinary", ordinary_before, ordinary_after);
            const auto exact_before = runner.Read();
            for (unsigned index = 0; index < 3; ++index) {
                auto first = runner.Submit(world.packet, true);
                runner.Compare(first, WorldExpected(world), "shrink-world-" + std::to_string(index));
                auto second = runner.Submit(indexed.packet, true);
                runner.Compare(second, IndexedExpected(indexed), "shrink-indexed-" + std::to_string(index));
            }
            runner.Check(runner.Read().process.committed_growths == exact_before.process.committed_growths,
                "Resident diagnostic frames needed new backing");
            ChangeLight(world.light, 17, 19, 9216);
            world.packet.lightmap = Publish(device, RuntimeAssetKind::LightmapR16, 3, 7, 511, 511, world.light);
            const auto fitting_before = runner.Read();
            auto fitting = runner.Submit(world.packet, true);
            runner.Compare(fitting, WorldExpected(world), "shrink-fitting-lightmap");
            const auto fitting_after = runner.Read();
            runner.Check(fitting_after.process.committed_growths == fitting_before.process.committed_growths
                && fitting_after.resources.world_upload_bytes == fitting_before.resources.world_upload_bytes + 2,
                "Fitting lightmap update allocated backing or retransmitted full image");
            runner.NoWait(before);
            runner.Record("shrink-fitting-update", fitting_before, fitting_after);
            scope.Reset();
            const auto recovery_before = runner.Read();
            auto recovered = runner.Submit(candidate.packet, true);
            runner.Compare(recovered, IndexedExpected(candidate), "growth-recovered");
            const auto recovery_after = runner.Read();
            runner.Check(recovery_after.resources.source_upload_bytes == recovery_before.resources.source_upload_bytes + 209
                && recovery_after.resources.palette_upload_bytes == recovery_before.resources.palette_upload_bytes + 1024,
                "Recovery omitted required source/palette uploads");
            runner.Record("required-growth-recovered", recovery_before, recovery_after);
            anchor.Reset();
        });
        runner.Case("unavailable-genuine-zero-and-explicit-compatibility", [&] {
            runner.Configure();
            auto world = World(device);
            auto indexed = Indexed(device);
            runner.Warm(world, indexed);
            auto candidate = Indexed(device, 200, 201);
            RuntimeProcessBudgetConfig config;
            config.unavailable = RuntimeBudgetUnavailablePolicy::FailClosed;
            device.ConfigureRuntimeProcessBudget(config);
            auto scope = device.OverrideRuntimeProcessBudgetForDiagnostics(Samples(FailedQuery, UINT64_MAX, 0));
            runner.Reject<std::logic_error>([&] {
                auto nested = device.OverrideRuntimeProcessBudgetForDiagnostics({});
            }, "Nested diagnostic provider accepted");
            const auto before = runner.Read();
            auto denied = device.TrySubmitRuntimeIndexedFrame(candidate.packet, true);
            const auto after = runner.Read();
            runner.Check(denied.status == RuntimeSubmitStatus::WouldBlock
                && denied.pressure == RuntimePressureReason::ProcessBudgetUnavailable
                && !after.process.segments[0].available && after.process.segments[0].injected,
                "Unavailable measurement promoted stale numbers");
            runner.OuterStatus(before, after, false, false);
            runner.Record("failclosed-unavailable", before, after);
            scope.Reset();
            config.unavailable = RuntimeBudgetUnavailablePolicy::ConfiguredCapsCompatibility;
            device.ConfigureRuntimeProcessBudget(config);
            scope = device.OverrideRuntimeProcessBudgetForDiagnostics(Samples(FailedQuery, 0, UINT64_MAX));
            const auto fallback_before = runner.Read();
            auto accepted = runner.Submit(candidate.packet, true);
            runner.Compare(accepted, IndexedExpected(candidate), "compatibility-growth");
            const auto fallback_after = runner.Read();
            runner.Check(fallback_after.process.compatibility_growths > fallback_before.process.compatibility_growths
                && !fallback_after.process.segments[0].available,
                "Configured caps fallback was labelled measured budget enforcement");
            runner.Record("configured-caps-compatibility", fallback_before, fallback_after);
            scope.Reset();
            config.unavailable = RuntimeBudgetUnavailablePolicy::FailClosed;
            device.ConfigureRuntimeProcessBudget(config);
            auto zero = device.OverrideRuntimeProcessBudgetForDiagnostics(Samples(0, 0, 0));
            auto fresh = Indexed(device, 210, 211);
            const auto zero_before = runner.Read();
            auto refused = device.TrySubmitRuntimeIndexedFrame(fresh.packet, true);
            const auto zero_after = runner.Read();
            runner.Check(refused.status == RuntimeSubmitStatus::WouldBlock
                && refused.pressure == RuntimePressureReason::ProcessBudgetLocal
                && zero_after.process.segments[0].available && zero_after.process.segments[0].sample.budget_bytes == 0,
                "Successful zero budget was confused with unavailable query");
            runner.Record("successful-zero", zero_before, zero_after);
            runner.NoWait(before);
        });


        runner.Case("partial-preparation-backing-credit-and-before-execute-rollback", [&] {
            runner.Configure();
            auto world = World(device);
            auto indexed = Indexed(device);
            runner.Warm(world, indexed);
            auto calibration = indexed;
            calibration.source_id = 300;
            calibration.source_revision = 7;
            calibration.packet.indices = Publish(device, RuntimeAssetKind::Indexed8, 300, 1,
                19, 11, Indices(19, 11, 7));
            const auto calibration_before = runner.Read();
            auto measured = runner.Submit(calibration.packet, true);
            runner.Compare(measured, IndexedExpected(calibration), "source-cost-calibration");
            const auto calibration_after = runner.Read();
            const uint64_t cost = calibration_after.process.committed_growth_bytes - calibration_before.process.committed_growth_bytes;
            runner.Check(cost > 0 && calibration_after.process.committed_growths == calibration_before.process.committed_growths + 1,
                "Source backing calibration included unrelated growth");
            auto candidate = Indexed(device, 200, 201);
            RuntimeProcessBudgetConfig config;
            config.unavailable = RuntimeBudgetUnavailablePolicy::FailClosed;
            device.ConfigureRuntimeProcessBudget(config);
            auto provider = Samples(0, MiB + cost, MiB);
            provider.samples[1] = {0, UINT64_MAX, 0, 0, 0};
            auto scope = device.OverrideRuntimeProcessBudgetForDiagnostics(provider);
            const auto before = runner.Read();
            auto denied = device.TrySubmitRuntimeIndexedFrame(candidate.packet, true);
            const auto after = runner.Read();
            runner.Check(denied.status == RuntimeSubmitStatus::WouldBlock
                && denied.pressure == RuntimePressureReason::ProcessBudgetLocal
                && after.process.committed_growths == before.process.committed_growths + 2
                && after.process.committed_growth_bytes == before.process.committed_growth_bytes + cost * 2
                && after.process.pressure_trims == before.process.pressure_trims + 1,
                "Partial preparation failed to charge positive backing across one bounded retry");
            runner.Check(after.resources.submissions == before.resources.submissions
                && after.resources.source_upload_bytes == before.resources.source_upload_bytes
                && after.resources.palette_upload_bytes == before.resources.palette_upload_bytes
                && after.resources.default_bytes == before.resources.default_bytes
                && after.process.segments[0].allocator_block_high_water >= before.process.segments[0].allocator_block_bytes + cost,
                "Partial rollback consumed bytes or omitted old/new backing peak");
            runner.OuterStatus(before, after, false, false);
            runner.Record("partial-backing-refusal", before, after);
            scope.Reset();
            const auto rollback_before = runner.Read();
            device.SetRuntimeFailureInjection(RuntimeFailurePoint::BeforeExecute);
            runner.Reject([&] { device.TrySubmitRuntimeIndexedFrame(candidate.packet, true); },
                "BeforeExecute rollback injection absent");
            const auto rollback_after = runner.Read();
            runner.Check(rollback_after.resources.submissions == rollback_before.resources.submissions
                && rollback_after.resources.default_bytes == rollback_before.resources.default_bytes
                && rollback_after.resources.source_upload_bytes == rollback_before.resources.source_upload_bytes,
                "Preexecute rollback committed candidate uploads");
            runner.Record("before-execute-rollback", rollback_before, rollback_after);
            auto retry = runner.Submit(candidate.packet, true);
            runner.Compare(retry, IndexedExpected(candidate), "partial-retry");
            runner.NoWait(before);
        });
        const auto uma = device.GetRuntimeProcessBudgetDiagnostics().uma;
        if (uma) {
            runner.Skip("all-selected-pages-localzero-nonlocal-retry",
                "UMA has no relevant nonlocal segment; CPU mapping and other actual cases still exercised");
        } else {
            runner.Case("all-selected-pages-localzero-nonlocal-retry", [&] {
                // One maximal readback cannot fit the pinned 64MiB allocator block.
                // World viewport stays64x64; outside pixels prove native clear too.
                runner.Configure(4096);
                auto world = World(device, 4096);
                for (uint64_t revision = 1; revision <= 3; ++revision) {
                    if (revision > 1) WorldRevision(device, world, revision, revision % 2 ? 8192 : 8704);
                    auto ticket = runner.Submit(world.packet);
                    runner.Complete(ticket);
                }
                runner.Check(device.GetRuntimeResourceDiagnostics().staging_pools == 3,
                    "Large native warmup missed a persistent slot");
                world.tile_index = 1;
                WorldRevision(device, world, 4, 8192);
                ChangeLight(world.light, 17, 19, 8704);
                world.packet.lightmap = Publish(device, RuntimeAssetKind::LightmapR16, 3, 5, 511, 511, world.light);
                RuntimeProcessBudgetConfig config;
                config.unavailable = RuntimeBudgetUnavailablePolicy::FailClosed;
                device.ConfigureRuntimeProcessBudget(config);
                auto blocked_scope = device.OverrideRuntimeProcessBudgetForDiagnostics(Samples(0, 0, 1));
                const auto before = runner.Read();
                auto denied = device.TrySubmitRuntimeWorldFrame(world.packet, true);
                const auto blocked = runner.Read();
                runner.Check(denied.status == RuntimeSubmitStatus::WouldBlock
                    && denied.pressure == RuntimePressureReason::ProcessBudgetNonlocal
                    && blocked.process.existing_block_probes > before.process.existing_block_probes
                    && blocked.process.committed_growths == before.process.committed_growths,
                    "Large readback did not force relevant new nonlocal backing denial");
                runner.OuterStatus(before, blocked, true, false);
                runner.Record("large-readback-nonlocal-denied", before, blocked);
                blocked_scope.Reset();
                auto provider = Samples(0, 0, 1);
                provider.replace_after_trim = true;
                provider.after_trim = provider.samples;
                provider.after_trim[1] = {0, UINT64_MAX, 0, 0, 0};
                auto recovered_scope = device.OverrideRuntimeProcessBudgetForDiagnostics(provider);
                const auto retry_before = runner.Read();
                auto result = device.TrySubmitRuntimeWorldFrame(world.packet, true);
                const auto submitted = runner.Read();
                runner.Check(result.status == RuntimeSubmitStatus::Submitted && result.ticket.Valid(),
                    "Process retry evicted a selected existing DEFAULT page");
                ++runner.result.submitted_frames;
                runner.Check(submitted.process.pressure_trims == retry_before.process.pressure_trims + 1
                    && submitted.process.pressure_recoveries == retry_before.process.pressure_recoveries + 1
                    && submitted.process.committed_growths == retry_before.process.committed_growths + 1
                    && submitted.resources.world_pages == retry_before.resources.world_pages
                    && submitted.resources.default_bytes == retry_before.resources.default_bytes
                    && submitted.resources.lightmap_images == retry_before.resources.lightmap_images,
                    "Retry recreated selected page/target/lightmap under localzero");
                runner.OuterStatus(retry_before, submitted, true, true);
                runner.Compare(result.ticket, WorldExpected(world), "all-pages-preserved-native4096");
                runner.NoWait(before);
                runner.Record("all-selected-pages-retry", retry_before, runner.Read());
            });
        }
        runner.Case("held-readers-remain-protected-and-postexecute-fails-closed", [&] {
            runner.Configure();
            auto world = World(device);
            auto indexed = Indexed(device);
            runner.Warm(world, indexed);
            auto anchor = runner.Submit(indexed.packet, true);
            runner.Compare(anchor, IndexedExpected(indexed), "held-anchor", true);
            Hold gate(device, runner.watchdog);
            std::array<RuntimeFrameTicket, 3> tickets{runner.Submit(world.packet, true),
                runner.Submit(indexed.packet, true), runner.Submit(world.packet, true)};
            for (const auto& ticket : tickets)
                runner.Check(!device.IsRuntimeFrameComplete(ticket), "Controlled reader completed before hold release");
            auto scope = device.OverrideRuntimeProcessBudgetForDiagnostics(Samples(0, 0, 1));
            const auto before = runner.Read();
            auto denied = device.TrySubmitRuntimeIndexedFrame(indexed.packet, true);
            runner.Check(denied.status == RuntimeSubmitStatus::WouldBlock
                && denied.pressure == RuntimePressureReason::FrameSlot,
                "Process budget bypassed three pending reader slots");
            device.CollectRuntimeResources(true);
            const auto retained = runner.Read();
            runner.Check(retained.resources.reader_hold_slots == 3
                && retained.resources.default_bytes == before.resources.default_bytes
                && retained.resources.upload_bytes == before.resources.upload_bytes
                && retained.resources.readback_bytes == before.resources.readback_bytes,
                "Pressure collection reclaimed pending readers");
            runner.NoWait(before);
            runner.Record("held-reader-protection", before, retained);
            gate.Release();
            runner.Compare(tickets[0], WorldExpected(world), "held-world0");
            runner.Compare(tickets[1], IndexedExpected(indexed), "held-indexed1");
            runner.Compare(tickets[2], WorldExpected(world), "held-world2");
            scope.Reset();
            anchor.Reset();
            Hold failing_gate(device, runner.watchdog);
            device.SetRuntimeFailureInjection(RuntimeFailurePoint::AfterExecute);
            runner.Reject([&] { device.TrySubmitRuntimeIndexedFrame(indexed.packet, true); },
                "PostExecute failure injection absent");
            const auto quarantined = runner.Read();
            runner.Check(quarantined.resources.failed && quarantined.resources.quarantined,
                "Executed failure was converted to retryable pressure");
            runner.Reject<std::logic_error>([&] { device.TrySubmitRuntimeIndexedFrame(indexed.packet); },
                "Quarantine accepted another submit");
            runner.Check(runner.Read().process.pressure_trims == quarantined.process.pressure_trims,
                "PostExecute failure entered pressure retry");
            failing_gate.Release();
            device.ClearRuntimeResources();
            runner.Record("postexecute-quarantine-drained", quarantined, runner.Read());
        });
        const auto before = runner.Read();
        device.ClearRuntimeResources();
        device.ConfigureRuntimeProcessBudget({});
        runner.Check(!runner.Read().process.override_active, "Teardown retained diagnostic provider");
        runner.Validation();
        runner.Write(true, "", before);
        return runner.result;
    } catch (const std::exception& error) {
        const auto before = runner.Read();
        try { device.ClearRuntimeResources(); } catch (...) {}
        runner.Write(false, error.what(), before);
        throw;
    }
}
}
