// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#pragma once
#include "kfx/renderer/SceneGLLensPass.h"

namespace kfx::render::dx12 {
using LensPassKind = SceneGLLensPassKind;
using LensPass = SceneGLLensPass;
inline void ValidateLensPass(const LensPass& pass) { ValidateSceneGLLensPass(pass); }

// Exact b0 layout used by Lens.hlsl (five 16-byte registers). Integration must
// additionally check that the supplied source GPU texture matches its extent.
struct LensConstants {
    std::array<uint32_t, 4> source_destination{}; // source W/H, destination X/Y
    std::array<uint32_t, 4> rectangle_kind{}; // viewport W/H, kind, mist lightness
    std::array<uint32_t, 4> resources{}; // remap W/H, overlay W/H
    std::array<int32_t, 4> address{}; // source X/Y, overlay alpha, zero
    std::array<float, 4> mist_offsets{};
};
static_assert(sizeof(LensConstants) == 80);
static_assert(sizeof(std::array<uint16_t, 2>) == 4);
inline LensConstants MakeLensConstants(const LensPass& pass)
{
    ValidateLensPass(pass);
    return {{pass.source_width, pass.source_height, uint32_t(pass.destination_x), uint32_t(pass.destination_y)},
        {pass.viewport_width, pass.viewport_height, uint32_t(pass.kind), pass.mist_lightness},
        {pass.remap_width, pass.remap_height, pass.overlay_width, pass.overlay_height},
        {pass.source_x, pass.source_y, int32_t(pass.overlay_alpha), 0}, pass.mist_offsets};
}
}
