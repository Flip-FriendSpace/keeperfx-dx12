// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
// Standalone CPU check; no window, GPU, runtime assets or engine build required.
// Example: g++ -std=c++20 -O2 -Wall -Wextra -Werror -pedantic -Isrc
//     local-dev/runtime-page-regions-test.cpp -o /tmp/runtime-page-regions-test
#include "kfx/renderer/d3d12/RuntimePageRegions.h"
#include <cstdlib>
#include <iostream>
#include <new>
#include <random>
#include <string>
#include <type_traits>

// Exercise the allocator's actual vector allocation failure paths. Tests run
// single-threaded; the hook is disabled except around one allocator operation.
#if defined(__GNUC__)
#define KFX_TEST_NOINLINE __attribute__((noinline))
#else
#define KFX_TEST_NOINLINE
#endif
static int fail_after = -1;
KFX_TEST_NOINLINE void* operator new(std::size_t size)
{
    if (fail_after == 0) { fail_after = -1; throw std::bad_alloc(); }
    if (fail_after > 0) --fail_after;
    if (void* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
KFX_TEST_NOINLINE void operator delete(void* p) noexcept { std::free(p); }
KFX_TEST_NOINLINE void operator delete(void* p, std::size_t) noexcept { std::free(p); }

using namespace kfx::render::dx12;
static uint64_t checks = 0, random_operations = 0;
static void Require(bool ok, const char* why)
{
    ++checks;
    if (!ok) throw std::runtime_error(why);
}
static bool Same(const RuntimePageRegions& a, const RuntimePageRegions& b)
{
    return a.Width() == b.Width() && a.Height() == b.Height() && a.FreeArea() == b.FreeArea()
        && std::ranges::equal(a.FreeRectangles(), b.FreeRectangles())
        && std::ranges::equal(a.AllocatedRegions(), b.AllocatedRegions());
}
static RuntimePageRegion Allocate(RuntimePageRegions& page, uint32_t width, uint32_t height)
{
    RuntimePageRegion r;
    Require(page.TryAllocate(width, height, r), "Expected rectangle did not fit");
    return r;
}
// Independently rasterize both partitions. Every page pixel must be covered
// exactly once, and live handles must match the test's separate ownership list.
static void Verify(const RuntimePageRegions& page, const std::vector<RuntimePageRegion>& live)
{
    Require(page.Valid(), "Internal occupancy invariants failed");
    Require(live.size() == page.AllocationCount(), "Live allocation count differs");
    std::vector<uint8_t> pixels(size_t(page.Width()) * page.Height());
    uint64_t used = 0, free = 0;
    const auto paint = [&](const RuntimePageRegion& r) {
        Require(r.width && r.height && uint64_t(r.x) + r.width <= page.Width()
            && uint64_t(r.y) + r.height <= page.Height(), "Rectangle escaped page bounds");
        for (uint32_t y = r.y; y < r.y + r.height; ++y)
            for (uint32_t x = r.x; x < r.x + r.width; ++x)
                Require(++pixels[size_t(y) * page.Width() + x] == 1, "Page partitions overlap");
    };
    for (const auto& r : page.AllocatedRegions()) {
        Require(r.id && std::count(live.begin(), live.end(), r) == 1, "Unknown or duplicate live handle");
        used += uint64_t(r.width) * r.height; paint(r);
    }
    for (const auto& r : page.FreeRectangles()) {
        Require(!r.id, "Free rectangle has allocation ID");
        free += uint64_t(r.width) * r.height; paint(r);
    }
    Require(used + free == pixels.size() && free == page.FreeArea(), "Page area accounting differs");
    Require(std::all_of(pixels.begin(), pixels.end(), [](uint8_t n) { return n == 1; }), "Lost page area");
}
static void BoundsAndIdentity()
{
    RuntimePageRegions empty;
    Require(empty.Valid() && !empty.FreeArea(), "Default page is not inert");
    RuntimePageRegion output{9, 8, 7, 6, 5};
    const auto sentinel = output;
    Require(!empty.TryAllocate(1, 1, output) && output == sentinel, "Empty allocation changed output");
    for (auto dimensions : {std::pair{0u, 8u}, {8u, 0u}, {16385u, 1u}, {1u, UINT32_MAX}}) {
        bool rejected = false;
        try { RuntimePageRegions invalid(dimensions.first, dimensions.second); }
        catch (const std::invalid_argument&) { rejected = true; }
        Require(rejected, "Invalid page dimension admitted");
    }
    RuntimePageRegions page(8, 8);
    const auto before = page;
    for (auto dimensions : {std::pair{0u, 1u}, {1u, 0u}, {9u, 1u}, {1u, UINT32_MAX}})
        Require(!page.TryAllocate(dimensions.first, dimensions.second, output)
            && output == sentinel && Same(page, before), "Failed allocation mutated state");
    auto first = Allocate(page, 8, 8);
    auto wrong = first; ++wrong.x;
    Require(!page.Release(wrong) && page.AllocationCount() == 1, "Modified rectangle was released");
    Require(page.Release(first) && !page.Release(first), "Double release admitted");
    auto second = Allocate(page, 8, 8);
    Require(second.id > first.id && second.x == first.x && second.y == first.y, "Reset reused allocation ID");
    Require(!page.Release(first) && page.AllocationCount() == 1, "Stale handle released replacement");
    Verify(page, {second});
}
static void MixedAndCoalescing()
{
    RuntimePageRegions page(8, 8);
    auto a = Allocate(page, 4, 4), b = Allocate(page, 4, 4), c = Allocate(page, 8, 4);
    Require(a.x == 0 && a.y == 0 && b.x == 4 && b.y == 0 && c.x == 0 && c.y == 4,
        "First-fit placement is not deterministic");
    Require(page.Release(a) && page.Release(b), "Horizontal release failed");
    auto wide = Allocate(page, 8, 4);
    Require(wide.x == 0 && wide.y == 0, "Horizontal holes did not merge");
    Verify(page, {c, wide});
    Require(page.Release(c) && page.Release(wide), "Emptying page failed");
    Require(page.FreeRectangles().size() == 1 && page.FreeArea() == 64, "Empty page did not fully reset");
    a = Allocate(page, 4, 8); b = Allocate(page, 4, 4); c = Allocate(page, 4, 4);
    Require(page.Release(b) && page.Release(c), "Vertical release failed");
    auto tall = Allocate(page, 4, 8);
    Require(tall.x == 4 && tall.y == 0, "Vertical holes did not merge");
    Verify(page, {a, tall});
    RuntimePageRegions fragmented(8, 8);
    a = Allocate(fragmented, 4, 4); b = Allocate(fragmented, 4, 4);
    c = Allocate(fragmented, 4, 4); auto d = Allocate(fragmented, 4, 4);
    Require(fragmented.Release(a) && fragmented.Release(d), "Diagonal release failed");
    RuntimePageRegion unchanged{1, 2, 3, 4, 5}; const auto sentinel = unchanged;
    Require(fragmented.FreeArea() == 32 && !fragmented.TryAllocate(8, 4, unchanged)
        && unchanged == sentinel, "Fragmented area incorrectly treated as contiguous");
    Verify(fragmented, {b, c});
    Require(fragmented.Release(b) && fragmented.Release(c), "Fragmented reset failed");
    auto full = Allocate(fragmented, 8, 8); Verify(fragmented, {full});
}
static void TransactionsAndExceptions()
{
    static_assert(std::is_nothrow_swappable_v<RuntimePageRegions>);
    static_assert(std::is_nothrow_move_constructible_v<RuntimePageRegions>);
    static_assert(std::is_nothrow_move_assignable_v<RuntimePageRegions>);
    RuntimePageRegions page(16, 16);
    auto original = Allocate(page, 4, 4); const auto before = page;
    {
        auto discarded = page;
        Require(discarded.Release(original), "Plan release failed");
        Allocate(discarded, 16, 16);
    }
    Require(Same(page, before), "Discarded plan changed committed page");
    auto plan = page;
    auto next = Allocate(plan, 3, 7);
    page.Swap(plan);
    Verify(page, {original, next});
    Require(Same(plan, before), "Swap did not preserve prior occupancy in old plan");
    auto moved = std::move(plan);
    Require(plan.Valid() && plan.Width() == 0 && Same(moved, before), "Move did not preserve valid states");
    RuntimePageRegions assigned;
    assigned = moved;
    Require(Same(assigned, moved), "Copy assignment differs");
    for (int fail = 0; fail < 2; ++fail) {
        RuntimePageRegions failing(8, 8); const auto baseline = failing;
        RuntimePageRegion output{1, 2, 3, 4, 5}; const auto sentinel = output;
        bool rejected = false;
        fail_after = fail;
        try { failing.TryAllocate(4, 4, output); }
        catch (const std::bad_alloc&) { rejected = true; }
        fail_after = -1;
        Require(rejected && Same(failing, baseline) && output == sentinel && failing.Valid(),
            "Allocation failure did not roll back occupancy");
        Require(Allocate(failing, 4, 4).id == 1, "Failed allocation consumed an ID");
    }
    RuntimePageRegions releasing(8, 8);
    auto region = Allocate(releasing, 4, 4); const auto baseline = releasing;
    bool rejected = false;
    fail_after = 0;
    try { releasing.Release(region); } catch (const std::bad_alloc&) { rejected = true; }
    fail_after = -1;
    Require(rejected && Same(releasing, baseline) && releasing.Valid(), "Release allocation failure mutated occupancy");
    Require(releasing.Release(region) && releasing.FreeArea() == 64, "Release retry did not recover");
}
static void FixedShapes()
{
    for (bool rows : {false, true}) {
        RuntimePageRegions page(rows ? 256 : 4096, 256);
        std::vector<RuntimePageRegion> live;
        const uint32_t width = rows ? 256 : 32, height = rows ? 1 : 32;
        const size_t count = rows ? 256 : 1024;
        for (size_t i = 0; i < count; ++i) live.push_back(Allocate(page, width, height));
        RuntimePageRegion output;
        Require(!page.TryAllocate(width, height, output) && !page.FreeArea(), "Full fixed-shape page admitted overflow");
        Verify(page, live);
        // Keep most of the page pinned while individual regions are reclaimed.
        for (size_t i = 0; i < live.size(); i += 3) {
            const auto old = live[i];
            Require(page.Release(old), "Fixed shape release failed");
            auto replacement = Allocate(page, width, height);
            Require(replacement.x == old.x && replacement.y == old.y && replacement.id > old.id,
                "Fixed shape hole was not reused");
            Require(!page.Release(old), "Fixed shape stale ID was admitted");
            live[i] = replacement;
        }
        Verify(page, live);
        for (const auto& r : live) Require(page.Release(r), "Fixed shape cleanup failed");
        Verify(page, {});
        Require(page.FreeRectangles().size() == 1, "Fixed shape page did not reset");
    }
}
static void Randomized()
{
    for (uint32_t seed : {1u, 17u, 392939u, 0xdecafbadU}) {
        std::mt19937 random(seed);
        RuntimePageRegions page(37, 29);
        std::vector<RuntimePageRegion> live;
        for (unsigned step = 0; step < 6000; ++step) {
            ++random_operations;
            if (!live.empty() && random() % 100 < 44) {
                const size_t i = random() % live.size();
                Require(page.Release(live[i]), "Random live release rejected");
                Require(!page.Release(live[i]), "Random double release accepted");
                live.erase(live.begin() + i);
            } else {
                const uint32_t width = 1 + random() % 12, height = 1 + random() % 11;
                const auto before = page;
                RuntimePageRegion output{77, 66, 55, 44, 33}; const auto sentinel = output;
                if (page.TryAllocate(width, height, output)) live.push_back(output);
                else {
                    Require(Same(page, before) && output == sentinel, "Random no-fit changed occupancy");
                    for (const auto& r : page.FreeRectangles())
                        Require(r.width < width || r.height < height, "First-fit missed a suitable free rectangle");
                }
            }
            Verify(page, live);
            // Identical committed states must make identical reservation choices.
            if (step % 127 == 0) {
                auto a = page, b = page;
                RuntimePageRegion ra, rb;
                Require(a.TryAllocate(3, 5, ra) == b.TryAllocate(3, 5, rb) && ra == rb && Same(a, b),
                    "Reservation plan is nondeterministic");
            }
        }
        std::shuffle(live.begin(), live.end(), random);
        for (const auto& r : live) Require(page.Release(r), "Random final release rejected");
        Verify(page, {});
        auto full = Allocate(page, 37, 29); Verify(page, {full});
    }
}
int main()
{
    try {
        BoundsAndIdentity(); MixedAndCoalescing(); TransactionsAndExceptions(); FixedShapes(); Randomized();
        std::cout << "PASS runtime-page-regions: " << checks << " checks, " << random_operations
            << " randomized operations; CPU only, no GPU/fence qualification\n";
        return 0;
    } catch (const std::exception& error) {
        fail_after = -1;
        std::cerr << "FAIL runtime-page-regions: " << error.what() << '\n'; return 1;
    }
}
