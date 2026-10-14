// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#include "kfx/platform/WindowMouseMapping.h"
#include <cassert>
#include <iostream>
#include <limits>

static void Expect(const WindowMouseMapping& m,float x,float y,int expected_x,int expected_y)
{
    int gx=123,gy=456;assert(m.Convert(x,y,gx,gy));assert(gx==expected_x && gy==expected_y);
}
int main()
{
    // Actual maximized geometry, with native rendering independent of client size.
    WindowMouseMapping maximized{3840,2054,3840,2054,94,0,3652,2054,1920,1080};
    Expect(maximized,94,0,0,0);Expect(maximized,1920,1027,960,540);
    Expect(maximized,3746,2054,1920,1080);Expect(maximized,93,0,-1,0);
    // Same physical picture at two drawable pixels per window coordinate unit.
    auto dpi=maximized;dpi.window_w=1920;dpi.window_h=1027;
    Expect(dpi,47,0,0,0);Expect(dpi,960,513.5f,960,540);Expect(dpi,1873,1027,1920,1080);
    // A 1280x900 client has 90px top/bottom bars. Boundary points remain
    // outside native space until MouseStateHandler performs its normal clipping.
    WindowMouseMapping resized{1280,900,1280,900,0,90,1280,720,1920,1080};
    Expect(resized,0,90,0,0);Expect(resized,640,450,960,540);
    Expect(resized,0,89,0,-2);Expect(resized,1280,810,1920,1080);
    // The old delta-only path would keep a stale origin when the stationary
    // pointer's presentation mapping changed. Absolute rebase gives the new point.
    WindowMouseMapping original{1920,1080,1920,1080,0,0,1920,1080,1920,1080};
    Expect(original,640,450,640,450);Expect(resized,640,450,960,540);
    // Event positions, not latest SDL state, distinguish a click from a later
    // queued movement; returning from possession cannot retain virtual centre.
    Expect(resized,320,270,480,270);Expect(resized,960,630,1440,810);
    auto invalid=resized;invalid.present_w=0;int x=17,y=23;
    assert(!invalid.Convert(1,1,x,y) && x==17 && y==23);
    assert(!resized.Convert(std::numeric_limits<float>::infinity(),0,x,y) && x==17 && y==23);
    std::cout << "Window mouse native, scaled, DPI, letterbox, resize and queued-coordinate checks passed\n";
}
