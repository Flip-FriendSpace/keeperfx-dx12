// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
// Independent arithmetic boundary and segment-policy tests. No GPU or engine assets.
#include "kfx/renderer/d3d12/RuntimeProcessBudget.h"
#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace kfx::render::dx12;
namespace {
uint64_t checks = 0;
uint64_t numeric_cases = 0;
constexpr int32_t Unavailable = -2147467259;

void Check(bool value, const char* reason)
{
    ++checks;
    if (!value) throw std::runtime_error(reason);
}

void Expect(const RuntimeProcessBudgetSample& sample, uint64_t reserve,
    uint64_t growth, RuntimeBudgetUnavailablePolicy policy,
    RuntimeProcessBudgetDecision expected, const char* reason)
{
    Check(EvaluateRuntimeProcessBudget(sample, reserve, growth, policy) == expected, reason);
}

void SegmentMapping()
{
    for (auto heap : {RuntimeProcessMemoryHeap::Default,
        RuntimeProcessMemoryHeap::Upload, RuntimeProcessMemoryHeap::Readback}) {
        Check(RuntimeProcessSegment(true, heap) == 0, "UMA heap was charged to nonlocal memory");
    }
    Check(RuntimeProcessSegment(false, RuntimeProcessMemoryHeap::Default) == 0,
        "Discrete DEFAULT heap was charged to nonlocal memory");
    Check(RuntimeProcessSegment(false, RuntimeProcessMemoryHeap::Upload) == 1,
        "Discrete UPLOAD heap was charged to local memory");
    Check(RuntimeProcessSegment(false, RuntimeProcessMemoryHeap::Readback) == 1,
        "Discrete READBACK heap was charged to local memory");
}

void ExactAndZeroBoundaries()
{
    const auto policy = RuntimeBudgetUnavailablePolicy::FailClosed;
    RuntimeProcessBudgetSample sample{0, 100, 90, 0, 0};
    Expect(sample, 3, 7, policy, RuntimeProcessBudgetDecision::Allow,
        "Exact headroom equality was rejected");
    Expect(sample, 3, 8, policy, RuntimeProcessBudgetDecision::OverBudget,
        "One byte past measured headroom was admitted");
    Expect(sample, 11, 1, policy, RuntimeProcessBudgetDecision::OverBudget,
        "Operational reserve larger than headroom was admitted");
    sample.available_for_reservation_bytes = UINT64_MAX;
    sample.reservation_bytes = UINT64_MAX;
    Expect(sample, 0, 11, policy, RuntimeProcessBudgetDecision::OverBudget,
        "Reservation metadata was invented as spendable headroom");
    sample = {0, 0, 0, UINT64_MAX, 0};
    Expect(sample, 0, 1, policy, RuntimeProcessBudgetDecision::OverBudget,
        "Successful zero budget was treated as unavailable or infinite capacity");
    Expect(sample, UINT64_MAX, 0, policy, RuntimeProcessBudgetDecision::Allow,
        "Zero-backing resident work was denied by reserve");
    sample = {0, 10, 11, 0, 0};
    Expect(sample, 0, 1, policy, RuntimeProcessBudgetDecision::OverBudget,
        "Usage over budget gained unsigned-underflow headroom");
    Expect(sample, 0, 0, policy, RuntimeProcessBudgetDecision::Allow,
        "Budget shrink blocked zero-backing work");
}

void UnavailableIsNotLastGood()
{
    RuntimeProcessBudgetSample failed{Unavailable, UINT64_MAX, 0, UINT64_MAX, 0};
    Expect(failed, 0, 1, RuntimeBudgetUnavailablePolicy::FailClosed,
        RuntimeProcessBudgetDecision::Unavailable,
        "Failed query promoted retained numerical fields to measured data");
    Expect(failed, UINT64_MAX, UINT64_MAX,
        RuntimeBudgetUnavailablePolicy::ConfiguredCapsCompatibility,
        RuntimeProcessBudgetDecision::Allow,
        "Explicit configured-cap compatibility fallback was denied");
    Expect(failed, UINT64_MAX, 0, RuntimeBudgetUnavailablePolicy::FailClosed,
        RuntimeProcessBudgetDecision::Allow,
        "Unavailable query blocked resident zero-backing work");
    RuntimeProcessBudgetSample local{0, 100, 1, 0, 0};
    Expect(local, 0, 99, RuntimeBudgetUnavailablePolicy::FailClosed,
        RuntimeProcessBudgetDecision::Allow,
        "Unrelated nonlocal failure erased successful local policy input");
    Expect(failed, 0, 1, RuntimeBudgetUnavailablePolicy::FailClosed,
        RuntimeProcessBudgetDecision::Unavailable,
        "Relevant failed segment was ignored");
}

void UnsignedExtremes()
{
    const auto policy = RuntimeBudgetUnavailablePolicy::FailClosed;
    RuntimeProcessBudgetSample sample{0, UINT64_MAX, 0, 0, 0};
    Expect(sample, 0, UINT64_MAX, policy, RuntimeProcessBudgetDecision::Allow,
        "Maximum representable exact headroom was rejected");
    Expect(sample, 1, UINT64_MAX, policy, RuntimeProcessBudgetDecision::OverBudget,
        "Reserve plus growth overflow wrapped into admission");
    Expect(sample, UINT64_MAX, 1, policy, RuntimeProcessBudgetDecision::OverBudget,
        "Maximum reserve overflow granted headroom");
    sample.usage_bytes = UINT64_MAX - 1;
    Expect(sample, 0, 1, policy, RuntimeProcessBudgetDecision::Allow,
        "One final byte of headroom was rejected");
    Expect(sample, 0, 2, policy, RuntimeProcessBudgetDecision::OverBudget,
        "Near-maximum usage subtraction overflow admitted growth");
    sample.budget_bytes = UINT64_MAX - 1;
    sample.usage_bytes = UINT64_MAX;
    Expect(sample, 0, 1, policy, RuntimeProcessBudgetDecision::OverBudget,
        "Maximum usage above budget wrapped into free space");
}

void IndependentSmallIntegerModel()
{
    // Signed arithmetic over a small domain is an independent mathematical
    // model: all requested growth and reserve must fit budget minus usage.
    // The production helper's checked unsigned subtraction is not reused.
    for (int64_t budget = 0; budget <= 17; ++budget) {
        for (int64_t usage = 0; usage <= 19; ++usage) {
            for (int64_t reserve = 0; reserve <= 21; ++reserve) {
                for (int64_t growth = 0; growth <= 23; ++growth) {
                    RuntimeProcessBudgetSample sample{0, uint64_t(budget), uint64_t(usage), 0, 0};
                    const bool fits = growth == 0 || budget - usage - reserve - growth >= 0;
                    const auto expected = fits ? RuntimeProcessBudgetDecision::Allow
                        : RuntimeProcessBudgetDecision::OverBudget;
                    Expect(sample, uint64_t(reserve), uint64_t(growth),
                        RuntimeBudgetUnavailablePolicy::FailClosed, expected,
                        "Measured admission disagrees with independent signed model");
                    ++numeric_cases;
                }
            }
        }
    }
}
}

int main()
{
    try {
        SegmentMapping();
        ExactAndZeroBoundaries();
        UnavailableIsNotLastGood();
        UnsignedExtremes();
        IndependentSmallIntegerModel();
        std::cout << "{\"passed\":true,\"checks\":" << checks
            << ",\"numericCases\":" << numeric_cases
            << ",\"umaAndDiscreteMapping\":true,\"genuineZero\":true"
            << ",\"staleQueryFieldsIgnored\":true,\"overflowBoundaries\":true"
            << ",\"gpuTested\":false,\"physicalBackingForecastTested\":false}\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Process-budget CPU test failed: " << error.what() << '\n';
        return 1;
    }
}
