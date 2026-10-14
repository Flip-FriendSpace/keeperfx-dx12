// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#pragma once
#include <cmath>
#include <limits>

// Window event coordinates are window units; the fitted rectangle is drawable
// pixels. Output is absolute native position, before the mouse window clips it.
struct WindowMouseMapping
{
    int window_w = 0, window_h = 0, drawable_w = 0, drawable_h = 0;
    int present_x = 0, present_y = 0, present_w = 0, present_h = 0;
    int native_w = 0, native_h = 0;
    bool operator==(const WindowMouseMapping&) const = default;
    bool Convert(float x, float y, int& native_x, int& native_y) const noexcept
    {
        if (window_w <= 0 || window_h <= 0 || drawable_w <= 0 || drawable_h <= 0
            || present_w <= 0 || present_h <= 0 || native_w <= 0 || native_h <= 0
            || !std::isfinite(x) || !std::isfinite(y))
            return false;
        const double gx = std::floor((double(x) * drawable_w / window_w - present_x) * native_w / present_w);
        const double gy = std::floor((double(y) * drawable_h / window_h - present_y) * native_h / present_h);
        if (gx < (std::numeric_limits<int>::min)() || gx > (std::numeric_limits<int>::max)()
            || gy < (std::numeric_limits<int>::min)() || gy > (std::numeric_limits<int>::max)())
            return false;
        native_x = int(gx);
        native_y = int(gy);
        return true;
    }
};
