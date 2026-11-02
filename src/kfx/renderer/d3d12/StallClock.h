// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <chrono>
#include <optional>

namespace kfx::render::dx12 {
// Times how long the running game has been blocked. A long gap between checks means the
// process itself was stopped (sleep, debugger, window drag), which is not a renderer stall.
class StallClock {
public:
    using Clock = std::chrono::steady_clock;
    bool Expired(Clock::time_point now, Clock::duration limit)
    {
        if (!since_ || now - last_ > MaxGap) since_ = now;
        last_ = now;
        return now - *since_ >= limit;
    }
    void reset() noexcept { since_.reset(); }
private:
    static constexpr Clock::duration MaxGap = std::chrono::seconds(2);
    std::optional<Clock::time_point> since_;
    Clock::time_point last_{};
};
}
