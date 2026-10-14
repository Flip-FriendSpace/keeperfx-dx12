// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
// Captured game-camera projection, independent of engine and graphics APIs.
#pragma once
#include "kfx/renderer/SceneAssets.h"

namespace kfx::render {
struct SceneProjectedPoint {
    double x = 0, y = 0, depth = 0, pixels_per_unit = 0;
    bool in_front = false;
};
class CapturedCamera {
public:
    explicit CapturedCamera(const SceneCamera& camera);
    // Absolute pixels in the captured viewport; world inputs are subtiles.
    SceneProjectedPoint Project(double x, double y, double z) const;
    std::array<float, 3> OffsetOnViewPlane(const ScenePosition& anchor, double dx, double dy) const;
    // Upright billboard: projected size is unchanged, but upper pixels have
    // physical height/depth so rear pillars cannot slice through their tops.
    std::array<float, 3> OffsetOnSpritePlane(const ScenePosition& anchor, double dx, double dy) const;
    std::array<float, 16> transform{};
    bool reverse_winding = false; // Legacy basis can reflect mesh winding.
    const SceneCamera source;
private:
    std::array<std::array<double, 4>, 3> view{};
    std::array<std::array<double, 3>, 3> inverse{};
    double scale = 0;
    bool perspective = false;
};
struct CameraComparison {
    uint32_t checked = 0;
    double maximum_error = 0, rms_error = 0;
    bool passed = false;
};
CameraComparison CompareCapturedCamera(const SceneCapture& capture);
}
