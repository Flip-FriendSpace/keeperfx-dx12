// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#include "RenderBenchmark.h"
#include <atomic>
#include <cstdlib>
#include <string>
namespace {
std::atomic<int> request{-1};
std::atomic<bool> busy{false};
}
extern "C" const char* RenderBenchmarkOutputDirectory(void)
{
    static const std::string path = [] {
        const char* value = std::getenv("KFX_RENDER_BENCHMARK_DIR");
        return value ? std::string(value) : std::string();
    }();
    return path.c_str();
}
extern "C" int RenderBenchmarkEnabled(void) { return *RenderBenchmarkOutputDirectory() != '\0'; }
extern "C" int RenderBenchmarkRequest(int scene)
{
    if (!RenderBenchmarkEnabled() || scene < 0 || scene > 3) return 0;
    bool expected = false;
    if (!busy.compare_exchange_strong(expected, true)) return 0;
    request.store(scene);
    return 1;
}
extern "C" int RenderBenchmarkTakeRequest(int live)
{
    int value = request.load();
    if (value < 0 || ((value == 3) != (live != 0))) return -1;
    return request.compare_exchange_strong(value, -1) ? value : -1;
}
extern "C" void RenderBenchmarkComplete(void) { request.store(-1); busy.store(false); }