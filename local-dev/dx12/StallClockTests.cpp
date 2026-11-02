// SPDX-License-Identifier: GPL-2.0-or-later
#include "kfx/renderer/d3d12/StallClock.h"
#include <cstdio>
#include <cstdlib>
using kfx::render::dx12::StallClock;
using namespace std::chrono_literals;
static void Check(bool value, const char* what)
{
    if (!value) { std::fprintf(stderr, "Failed: %s\n", what); std::exit(1); }
}
int main()
{
    const auto t0 = StallClock::Clock::time_point{} + 1h;
    {
        StallClock clock;
        bool expired = false;
        for (auto t = 0s; t < 10s; t += 1s) expired = clock.Expired(t0 + t, 10s);
        Check(!expired, "Nine seconds of polling is below the limit");
        Check(clock.Expired(t0 + 10s, 10s), "Ten polled seconds expire");
    }
    {
        StallClock clock;
        Check(!clock.Expired(t0, 10s), "The first check starts the clock");
        Check(!clock.Expired(t0 + 1h, 10s), "A sleeping process is not a stall");
        Check(!clock.Expired(t0 + 1h + 9s, 10s) , "The gap restarts the clock");
    }
    {
        StallClock clock;
        for (auto t = 0s; t < 9s; t += 1s) (void)clock.Expired(t0 + t, 10s);
        clock.reset();
        Check(!clock.Expired(t0 + 10s, 10s), "Progress restarts the clock");
    }
    std::puts("PASS stall clock");
}
