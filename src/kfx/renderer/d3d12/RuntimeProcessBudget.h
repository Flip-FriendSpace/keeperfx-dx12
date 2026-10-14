// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#pragma once
#include <array>
#include <cstdint>
#include <memory>

namespace kfx::render::dx12 {
enum class RuntimeBudgetUnavailablePolicy : uint8_t { ConfiguredCapsCompatibility, FailClosed };
enum class RuntimeProcessMemoryHeap : uint8_t { Default, Upload, Readback };
enum class RuntimeProcessBudgetDecision : uint8_t { Allow, OverBudget, Unavailable };
struct RuntimeProcessBudgetConfig {
    std::array<uint64_t, 2> reserve_bytes{}; // local, nonlocal; zero is experimental, not measured driver headroom.
    RuntimeBudgetUnavailablePolicy unavailable = RuntimeBudgetUnavailablePolicy::ConfiguredCapsCompatibility;
};
struct RuntimeProcessBudgetSample {
    int32_t hresult = 0;
    uint64_t budget_bytes = 0, usage_bytes = 0;
    uint64_t available_for_reservation_bytes = 0, reservation_bytes = 0;
};
inline unsigned RuntimeProcessSegment(bool uma, RuntimeProcessMemoryHeap heap) noexcept
{ return uma || heap == RuntimeProcessMemoryHeap::Default ? 0u : 1u; }
inline RuntimeProcessBudgetDecision EvaluateRuntimeProcessBudget(const RuntimeProcessBudgetSample& sample,
    uint64_t reserve, uint64_t growth, RuntimeBudgetUnavailablePolicy unavailable) noexcept
{
    if (!growth) return RuntimeProcessBudgetDecision::Allow;
    if (sample.hresult < 0) return unavailable == RuntimeBudgetUnavailablePolicy::FailClosed
        ? RuntimeProcessBudgetDecision::Unavailable : RuntimeProcessBudgetDecision::Allow;
    if (sample.usage_bytes > sample.budget_bytes) return RuntimeProcessBudgetDecision::OverBudget;
    const auto available = sample.budget_bytes - sample.usage_bytes;
    return reserve > available || growth > available - reserve
        ? RuntimeProcessBudgetDecision::OverBudget : RuntimeProcessBudgetDecision::Allow;
}
struct RuntimeProcessBudgetOverride {
    std::array<RuntimeProcessBudgetSample, 2> samples{};
    bool replace_after_trim = false;
    std::array<RuntimeProcessBudgetSample, 2> after_trim{};
};
// Diagnostic provider override only. Real allocation, recording and fences remain active.
// Scope destruction restores real queries, including if Device was already destroyed.
class RuntimeProcessBudgetScope {
public:
    RuntimeProcessBudgetScope() = default;
    ~RuntimeProcessBudgetScope();
    RuntimeProcessBudgetScope(RuntimeProcessBudgetScope&&) noexcept;
    RuntimeProcessBudgetScope& operator=(RuntimeProcessBudgetScope&&) noexcept;
    RuntimeProcessBudgetScope(const RuntimeProcessBudgetScope&) = delete;
    RuntimeProcessBudgetScope& operator=(const RuntimeProcessBudgetScope&) = delete;
    void Reset() noexcept;
    bool Valid() const noexcept;
private:
    struct State;
    std::shared_ptr<State> state;
    friend class Device;
};
struct RuntimeProcessSegmentDiagnostics {
    bool applicable = true, available = false, injected = false;
    RuntimeProcessBudgetSample sample{};
    uint64_t sample_serial = 0, refreshes = 0, failures = 0;
    double age_ms = 0;
    uint64_t allocator_block_bytes = 0, allocator_allocation_bytes = 0;
    uint64_t allocator_block_count = 0, allocator_allocation_count = 0;
    // These public allocator values may be cached or estimated. Never label them direct DXGI data.
    uint64_t allocator_estimated_usage_bytes = 0, allocator_estimated_budget_bytes = 0;
    uint64_t allocator_block_high_water = 0;
};
struct RuntimeProcessBudgetDiagnostics {
    bool uma = false, notification_registered = false, override_active = false;
    int32_t notification_hresult = 0;
    RuntimeProcessBudgetConfig config{};
    std::array<RuntimeProcessSegmentDiagnostics, 2> segments{};
    uint64_t notifications = 0, telemetry_refreshes = 0, growth_refreshes = 0;
    uint64_t existing_block_probes = 0, existing_block_hits = 0, existing_block_misses = 0, existing_block_unsupported = 0;
    uint64_t committed_growths = 0, committed_growth_bytes = 0;
    uint64_t budget_denials = 0, unavailable_denials = 0, allocation_denials = 0;
    uint64_t compatibility_growths = 0, pressure_trims = 0, pressure_recoveries = 0;
    uint64_t world_bootstraps = 0, indexed_bootstraps = 0;
    double bootstrap_ms = 0;
    int32_t last_allocation_hresult = 0;
    uint32_t last_segment = 0;
    uint64_t last_requested_bytes = 0;
};
}
