// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#include "kfx/renderer/SceneCamera.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace kfx::render {
namespace {
void Require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
}
CapturedCamera::CapturedCamera(const SceneCamera& c) : source(c)
{
    Require(c.viewport_w > 0 && c.viewport_h > 0 && c.pixel_size == 1,
        "Captured camera requires a positive native-resolution viewport");
    perspective = c.view == SceneView::Possession;
    Require(!perspective || ((c.lens_mode == 1 || c.lens_mode == 2) && c.lens > 0 && c.view_distance > 0),
        "Only standard and circular possession projection are supported; fisheye is deferred");
    if (c.view == SceneView::Front) {
        const unsigned q = (unsigned(c.yaw + 256) / 512) & 3;
        constexpr double right[4][2] = {{1,0},{0,1},{-1,0},{0,-1}};
        constexpr double down[4][2] = {{0,1},{-1,0},{0,-1},{1,0}};
        view[0] = {right[q][0]*256, right[q][1]*256, 0, 0};
        view[1] = {-down[q][0]*256, -down[q][1]*256, 128, 0};
        view[2] = {-down[q][0]*256, -down[q][1]*256, -256, 0};
        for (auto& row : view) row[3] = -(row[0]*c.position.x + row[1]*c.position.y)/256;
        scale = double(c.scaled_zoom / 8) / 65536;
    } else {
        Require(c.has_rotation, "Captured camera rotation is absent");
        for (size_t i = 0; i < 3; ++i) {
            // Legacy input axes are (world X, world Z, minus world Y).
            const auto* r = &c.rotation[i*4];
            view[i] = {r[0]/64.0, -r[2]/64.0, r[1]/64.0,
                (double(r[0])*r[1]-r[3])/16384.0};
            view[i][3] -= (view[i][0]*c.position.x + view[i][1]*c.position.y + view[i][2]*c.position.z)/256;
        }
        scale = perspective ? c.lens : double(c.scaled_zoom) / 65536;
    }
    Require(std::isfinite(scale) && scale > 0, "Invalid captured camera scale");
    const auto& a = view;
    const double det = a[0][0]*(a[1][1]*a[2][2]-a[1][2]*a[2][1])
        - a[0][1]*(a[1][0]*a[2][2]-a[1][2]*a[2][0]) + a[0][2]*(a[1][0]*a[2][1]-a[1][1]*a[2][0]);
    Require(std::isfinite(det) && std::abs(det) > 1, "Degenerate captured camera basis");
    reverse_winding = det > 0;
    for (size_t i = 0; i < 3; ++i) for (size_t j = 0; j < 3; ++j)
        inverse[i][j] = (a[(j+1)%3][(i+1)%3]*a[(j+2)%3][(i+2)%3]
            - a[(j+1)%3][(i+2)%3]*a[(j+2)%3][(i+1)%3])/det;
    const double near = 32, far = std::max(512.0, double(c.view_distance)*256);
    for (size_t i = 0; i < 4; ++i) {
        transform[i*4] = float(view[0][i]*2*scale/c.viewport_w);
        transform[i*4+1] = float(view[1][i]*2*scale/c.viewport_h);
        transform[i*4+2] = float(view[2][i]*(perspective ? far/(far-near) : 1.0/524288));
        transform[i*4+3] = perspective ? float(view[2][i]) : 0;
    }
    if (perspective) transform[14] -= float(far*near/(far-near));
    else { transform[14] += .5f; transform[15] = 1; }
}
SceneProjectedPoint CapturedCamera::Project(double x, double y, double z) const
{
    double p[3];
    for (size_t i = 0; i < 3; ++i) p[i] = view[i][0]*x + view[i][1]*y + view[i][2]*z + view[i][3];
    SceneProjectedPoint result;
    result.depth = p[2]; result.in_front = !perspective || p[2] >= 32;
    if (!result.in_front) return result;
    result.pixels_per_unit = perspective ? scale/p[2] : scale;
    result.x = source.viewport_x + source.viewport_w*.5 + p[0]*result.pixels_per_unit;
    result.y = source.viewport_y + source.viewport_h*.5 - p[1]*result.pixels_per_unit;
    return result;
}
std::array<float, 3> CapturedCamera::OffsetOnViewPlane(const ScenePosition& anchor, double dx, double dy) const
{
    auto p = Project(anchor.x/256.0, anchor.y/256.0, anchor.z/256.0);
    Require(p.in_front && std::isfinite(dx) && std::isfinite(dy), "Invalid sprite camera-plane offset");
    const double position[3] = {anchor.x/256.0, anchor.y/256.0, anchor.z/256.0};
    std::array<float, 3> result;
    for (size_t i = 0; i < 3; ++i)
        result[i] = float(position[i] + (inverse[i][0]*dx - inverse[i][1]*dy)/p.pixels_per_unit);
    return result;
}
std::array<float, 3> CapturedCamera::OffsetOnSpritePlane(const ScenePosition& anchor, double dx, double dy) const
{
    const auto flat = OffsetOnViewPlane(anchor,dx,dy);
    const double position[3] = {anchor.x/256.0,anchor.y/256.0,anchor.z/256.0};
    const double nx = view[2][0], ny = view[2][1];
    // Directly overhead there is no useful upright facing plane.
    if (nx*nx+ny*ny < 1) return flat;
    std::array<float,3> result;
    if (!perspective) {
        const double divisor = nx*inverse[0][2]+ny*inverse[1][2];
        if (std::abs(divisor)<1e-6) return flat;
        const double depth = -(nx*(flat[0]-position[0])+ny*(flat[1]-position[1]))/divisor;
        for (size_t i=0;i<3;++i) result[i] = float(flat[i]+inverse[i][2]*depth);
    } else {
        double eye[3]{};
        for (size_t i=0;i<3;++i) for (size_t j=0;j<3;++j) eye[i] -= inverse[i][j]*view[j][3];
        const double divisor = nx*(flat[0]-eye[0])+ny*(flat[1]-eye[1]);
        if (std::abs(divisor)<1e-6) return flat;
        const double t = (nx*(position[0]-eye[0])+ny*(position[1]-eye[1]))/divisor;
        if (t<=0) return flat;
        for (size_t i=0;i<3;++i) result[i] = float(eye[i]+(flat[i]-eye[i])*t);
    }
    return result;
}
CameraComparison CompareCapturedCamera(const SceneCapture& capture)
{
    CapturedCamera camera(capture.scene.camera);
    CameraComparison result; result.passed = true;
    double squares = 0;
    for (const auto& r : capture.assets.projection_reference) {
        auto p = camera.Project(r.position.x/256.0, r.position.y/256.0, r.position.z/256.0);
        if (!r.visible || !p.in_front) continue;
        double error = std::max(std::abs(p.x-r.screen_x), std::abs(p.y-r.screen_y));
        // The old projector truncates rotated coordinates and then pixels.
        const double tolerance = 2.5 + 3*p.pixels_per_unit;
        result.passed &= error <= tolerance;
        result.maximum_error = std::max(result.maximum_error, error);
        squares += error*error; ++result.checked;
    }
    result.passed &= result.checked > 0;
    if (result.checked) result.rms_error = std::sqrt(squares/result.checked);
    return result;
}
}
