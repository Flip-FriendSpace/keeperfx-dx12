// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#pragma once
#include "RenderBenchmark.h"
#include "d3d12/Dx12Device.h"
#include <algorithm>
#include <array>
#include <stdexcept>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <vector>

// Opt-in bounded observations. No GPU wait, image readback or simulation writes.
class DX12LiveBenchmark {
    using Clock = std::chrono::steady_clock;
    struct Row {
        uint64_t id, epoch, sequence;
        double producer, cpu, present, admission_interval, present_interval, gpu = -1;
        uint32_t width, height;
        double admission_time = 0, admission_to_next = -1;
        bool replay, diagnostic, resolved = false, held = false;
    };
    static constexpr size_t capacity = 65536;
    std::vector<Row> rows;
    bool active = false, draining = false;
    double started = 0, warmup = 30, duration = 120, stopped = 0;
    double admission = 0, previous_admission = 0, admission_interval = -1;
    double producer = 0, pending_producer = 0, pending_admission_interval = -1;
    double submit_cpu = 0, previous_present = 0;
    uint64_t overflow = 0, retries = 0, initial_queue_overflow = 0;
    kfx::render::dx12::RuntimeResourceDiagnostics before{};
    kfx::render::dx12::RuntimeProcessBudgetDiagnostics budget_before{};
    kfx::render::dx12::PresentationDiagnostics presentation_before{};
    static const char* QueryStatus(kfx::render::dx12::PresentationQueryStatus value) {
        using Status = kfx::render::dx12::PresentationQueryStatus;
        switch (value) {
        case Status::Disabled: return "disabled";
        case Status::Valid: return "valid";
        case Status::Disjoint: return "disjoint";
        case Status::Unsupported: return "unsupported";
        default: return "failed";
        }
    }
    static void PresentationGap(std::ostream& out, const kfx::render::dx12::PresentationGapHistogram& h) {
        out << "{\"count\":" << h.count << ",\"lt2Ms\":" << h.buckets[0]
            << ",\"from2To8Ms\":" << h.buckets[1] << ",\"from8To20Ms\":" << h.buckets[2]
            << ",\"ge20Ms\":" << h.buckets[3] << ",\"maximumMs\":" << h.maximum_ms << '}';
    }
    static void Presentation(std::ostream& out, const kfx::render::dx12::PresentationDiagnostics& p) {
        out << "{\"enabled\":" << p.enabled << ",\"presentOk\":" << p.present_ok
            << ",\"presentOccluded\":" << p.present_occluded << ",\"presentOther\":" << p.present_other
            << ",\"waitable\":" << p.waitable << ",\"permitHeld\":" << p.permit_held
            << ",\"admissionPolls\":" << p.admission_polls << ",\"admissionGrants\":" << p.admission_grants << ",\"admissionNotReady\":" << p.admission_not_ready
            << ",\"pendingPollGaps\":";
        PresentationGap(out, p.pending_poll_gaps);
        out << ",\"grantPollGaps\":"; PresentationGap(out, p.grant_poll_gaps);
        out << ",\"pendingWaitDurations\":"; PresentationGap(out, p.pending_wait_durations);
        out << ",\"lastPresentHRESULT\":" << p.last_present_hresult
            << ",\"outputStatus\":" << std::quoted(QueryStatus(p.output_status)) << ",\"outputHRESULT\":" << p.output_hresult
            << ",\"outputName\":" << std::quoted(p.output_name) << ",\"attachedToDesktop\":" << p.attached_to_desktop
            << ",\"desktopRect\":[" << p.desktop_rect[0] << ',' << p.desktop_rect[1] << ',' << p.desktop_rect[2] << ',' << p.desktop_rect[3]
            << "],\"nominalRefreshAvailable\":" << p.refresh_available << ",\"nominalRefreshHz\":" << p.refresh_hz
            << ",\"frameStatisticsStatus\":" << std::quoted(QueryStatus(p.frame_statistics_status))
            << ",\"frameStatisticsHRESULT\":" << p.frame_statistics_hresult
            << ",\"presentCount\":" << p.present_count << ",\"presentRefreshCount\":" << p.present_refresh_count
            << ",\"syncRefreshCount\":" << p.sync_refresh_count << ",\"syncQPCTime\":" << p.sync_qpc_time
            << ",\"syncGPUTime\":" << p.sync_gpu_time << ",\"lastCountStatus\":" << std::quoted(QueryStatus(p.last_count_status))
            << ",\"lastCountHRESULT\":" << p.last_count_hresult << ",\"lastPresentCount\":" << p.last_present_count << '}';
    }
    static double Seconds(const char* name, double fallback, double maximum) {
        const char* text = std::getenv(name);
        if (!text) return fallback;
        char* end = nullptr;
        const long value = std::strtol(text, &end, 10);
        return end != text && *end == '\0' && value >= 1 && value <= maximum ? double(value) : fallback;
    }
    static void Memory(std::ostream& out, const kfx::render::dx12::RuntimeResourceDiagnostics& r,
        const kfx::render::dx12::RuntimeProcessBudgetDiagnostics& b) {
        out << "{\"assetCpuBytes\":" << r.cpu_bytes << ",\"terrainCpuBytes\":" << r.terrain_cpu_bytes
            << ",\"defaultBytes\":" << r.default_bytes << ",\"uploadBytes\":" << r.upload_bytes
            << ",\"readbackBytes\":" << r.readback_bytes << ",\"defaultHighWaterBytes\":" << r.default_high_water << ",\"uploadHighWaterBytes\":" << r.upload_high_water << ",\"globalIdleCalls\":" << r.global_idle_calls << ",\"fenceWaits\":" << r.fence_waits << ",\"uma\":" << b.uma << ",\"segments\":[";
        for (size_t i = 0; i < b.segments.size(); ++i) {
            if (i) out << ',';
            const auto& s = b.segments[i];
            out << "{\"applicable\":" << s.applicable << ",\"available\":" << s.available
                << ",\"injected\":" << s.injected << ",\"ageMs\":" << s.age_ms
                << ",\"budgetBytes\":" << s.sample.budget_bytes << ",\"usageBytes\":" << s.sample.usage_bytes
                << ",\"allocatorBlockBytes\":" << s.allocator_block_bytes
                << ",\"allocatorAllocationBytes\":" << s.allocator_allocation_bytes << '}';
        }
        out << "]}";
    }
public:
    const bool enabled = RenderBenchmarkEnabled() != 0;
    bool debug = true, gpu_validation = false, vsync = false;
    static double Now() { return std::chrono::duration<double>(Clock::now().time_since_epoch()).count(); }
    void Cancel() noexcept {
        if (!enabled) return;
        if (active) {
            try {
                std::ofstream out(std::filesystem::path(RenderBenchmarkOutputDirectory()) / "dx12-live.json");
                out << "{\"passed\":false,\"aborted\":true,\"reason\":\"renderer shutdown before benchmark completion\"}\n";
            } catch (...) {}
        }
        active = draining = false;
        RenderBenchmarkComplete();
    }
    void Admit() {
        if (!enabled) return;
        if (!enabled) return;
        const double now = Now();
        admission_interval = previous_admission ? (now - previous_admission) * 1000 : -1;
        if (active) {
            for (auto row = rows.rbegin(); row != rows.rend(); ++row) {
                if (!row->replay) {
                    if (row->admission_time == previous_admission) row->admission_to_next = (now - previous_admission) * 1000;
                    break;
                }
            }
        }
        previous_admission = admission = now;
    }
    void FinishProducer() { if (!enabled) return; producer = admission ? (Now() - admission) * 1000 : -1; }
    void Sealed() { if (!enabled) return; pending_producer = producer; pending_admission_interval = admission_interval; submit_cpu = 0; }
    void Replay() { if (!enabled) return; pending_producer = -1; pending_admission_interval = -1; submit_cpu = 0; }
    void Attempt(double elapsed_ms, const kfx::render::dx12::RuntimeSubmitResult* result) {
        if (!enabled) return;
        submit_cpu += std::max(0.0, elapsed_ms - (result ? result->cpu_present_ms : 0));
        if (active && (!result || result->status != kfx::render::dx12::RuntimeSubmitStatus::Submitted)) ++retries;
    }
    void Submitted(const kfx::render::dx12::RuntimeSubmitResult& result, uint64_t epoch, uint64_t sequence,
        uint32_t width, uint32_t height, bool replay, bool diagnostic) {
        if (!enabled) return;
        const double now = Now();
        const double present_time = result.present_completed_seconds;
        const double interval = present_time && previous_present ? (present_time - previous_present) * 1000 : -1;
        if (present_time) previous_present = present_time;
        if (!active || draining || now < started + warmup || now >= started + warmup + duration) return;
        if (rows.size() == capacity) { ++overflow; return; }
        rows.push_back({result.submission_id, epoch, sequence, pending_producer, submit_cpu,
            result.cpu_present_ms, pending_admission_interval, interval, -1, width, height, replay ? 0 : admission, -1, replay, diagnostic, false});
    }
    void Poll(kfx::render::dx12::Device& device) {
        if (!enabled) return;
        std::array<kfx::render::dx12::RuntimeTimingCompletion, 256> completed;
        const size_t count = device.TakeRuntimeTimingCompletions(completed);
        for (size_t i = 0; i < count; ++i) {
            const auto& sample = completed[i];
            auto row = std::lower_bound(rows.begin(), rows.end(), sample.submission_id,
                [](const Row& a, uint64_t id) { return a.id < id; });
            if (row != rows.end() && row->id == sample.submission_id) {
                row->gpu = sample.gpu_upload_render_ms; row->resolved = true; row->held = sample.held;
            }
        }
        if (!active && RenderBenchmarkTakeRequest(1) == 3) {
            rows.clear(); rows.reserve(capacity); overflow = retries = 0;
            warmup = Seconds("KFX_RENDER_BENCHMARK_WARMUP_SECONDS", 30, 120);
            duration = Seconds("KFX_RENDER_BENCHMARK_SAMPLE_SECONDS", 120, 300);
            started = Now(); active = true; draining = false;
            before = device.GetRuntimeResourceDiagnostics(); budget_before = device.GetRuntimeProcessBudgetDiagnostics();
            presentation_before = device.GetPresentationDiagnostics();
            initial_queue_overflow = device.GetRuntimeTimingDiagnostics().completion_overflows;
        }
        if (!active) return;
        if (!draining && Now() >= started + warmup + duration) { draining = true; stopped = Now(); }
        if (!draining) return;
        const bool unresolved = std::any_of(rows.begin(), rows.end(), [](const Row& r) { return !r.resolved; });
        if (unresolved && Now() - stopped < 5) return;
        const auto timing = device.GetRuntimeTimingDiagnostics();
        const auto after = device.GetRuntimeResourceDiagnostics();
        const auto budget_after = device.GetRuntimeProcessBudgetDiagnostics();
        const auto presentation_after = device.GetPresentationDiagnostics();
        const auto base = std::filesystem::path(RenderBenchmarkOutputDirectory());
        std::ofstream csv(base / "dx12-live.csv");
        csv << std::fixed << std::setprecision(6)
            << "submission_id,epoch,sequence,producer_wall_ms,preparation_submit_cpu_ms,present_cpu_ms,admission_interval_ms,admission_to_next_ms,present_completion_interval_ms,gpu_upload_render_ms,native_width,native_height,palette_replay,diagnostic\n";
        size_t missing = 0, held = 0;
        for (const auto& r : rows) {
            missing += r.gpu < 0; held += r.held;
            csv << r.id << ',' << r.epoch << ',' << r.sequence << ',' << r.producer << ',' << r.cpu << ',' << r.present
                << ',' << r.admission_interval << ',' << r.admission_to_next << ',' << r.present_interval << ',' << r.gpu << ',' << r.width << ',' << r.height
                << ',' << r.replay << ',' << r.diagnostic << '\n';
        }
        csv.close();
        if (!csv) throw std::runtime_error("Cannot write DX12 benchmark CSV");
        std::ofstream json(base / "dx12-live.json");
        json << std::boolalpha << std::setprecision(12) << "{\"passed\":"
            << (!rows.empty() && !missing && !held && !overflow && timing.completion_overflows == initial_queue_overflow)
            << ",\"debug\":" << debug << ",\"gpuValidation\":" << gpu_validation << ",\"vsync\":" << vsync
            << ",\"heldRows\":" << held << ",\"rows\":" << rows.size() << ",\"missingGpu\":" << missing << ",\"overflow\":" << overflow
            << ",\"completionOverflows\":" << timing.completion_overflows - initial_queue_overflow
            << ",\"retryAttempts\":" << retries << ",\"warmupSeconds\":" << warmup << ",\"sampleSeconds\":" << duration
            << ",\"timestampFrequency\":" << timing.timestamp_frequency
            << ",\"scope\":\"Application admission and successful Present return intervals, not scanout FPS. Producer wall includes gaps between draw calls; CPU submission includes bridge preparation and retries, excludes Present. GPU upload/render excludes queue latency and Present. Missing values are -1. Memory endpoints are not total-process peaks.\",\"memoryBefore\":";
        Memory(json, before, budget_before); json << ",\"memoryAfter\":"; Memory(json, after, budget_after);
        json << ",\"presentationEndpointScope\":\"Cumulative runtime Present counters, pending-poll histograms and DXGI snapshots at request-before-warmup and after timing/drain; histogram samples are TIMEOUT-to-TIMEOUT, last-TIMEOUT-to-grant and first-TIMEOUT-to-grant respectively, excluding immediate grants and resetting continuity at Present/cancel/resize/suspend/VSync bypass; deltas include warmup/drain and are not sample-window display FPS. HRESULTs are signed32; unavailable query values are not observations.\",\"presentationBefore\":";
        Presentation(json, presentation_before); json << ",\"presentationAfter\":"; Presentation(json, presentation_after); json << "}\n";
        json.close();
        if (!json) throw std::runtime_error("Cannot write DX12 benchmark manifest");
        active = false; RenderBenchmarkComplete();
    }
};