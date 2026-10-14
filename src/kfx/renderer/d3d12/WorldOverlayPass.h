// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#pragma once
#include "kfx/renderer/SceneGLOverlayTrace.h"
#include <cstddef>

namespace kfx::render::dx12 {
using WorldOverlayPhase=SceneGLOverlayPhase;
using WorldOverlayMode=SceneGLOverlayMode;
using IndexedOverlaySource=SceneGLOverlaySource;
using WorldOverlayQuad=SceneGLOverlayQuad;
using WorldOverlayPass=SceneGLOverlayPass;
using WorldOverlayColourRow=SceneGLOverlayColourRow;
inline constexpr uint32_t NoWorldOverlayResource=NoSceneGLOverlayResource;
inline void ValidateWorldOverlayPass(const WorldOverlayPass& pass){ValidateSceneGLOverlayPass(pass);}

// Matches production UI_VERTEX_SHADER: float XY, UV, RGBA and original GL NDC Z.
// Expand each quad as TL/TR/BR, TL/BR/BL, retaining the actual float endpoints.
struct WorldOverlayVertex {
    float x=0,y=0,u=0,v=0;
    std::array<float,4> colour{};
    float z_ndc=0;
};
static_assert(sizeof(WorldOverlayVertex)==36);
static_assert(offsetof(WorldOverlayVertex,colour)==16&&offsetof(WorldOverlayVertex,z_ndc)==32);
// Exact b0 layout. Each draw binds its original-dimension R8 source at t0,
// all RGBA colour rows at t1, and the actual R8 fade table at t2. A point/clamp
// sampler is s0. Solid draws need no source; irrelevant descriptors may be null.
struct WorldOverlayConstants {
    std::array<float,2> native_size{};
    uint32_t mode=0,colour_row=NoWorldOverlayResource;
    uint32_t fade_row=0;
    std::array<uint32_t,3> reserved{};
    std::array<float,2> zoom_map{},zoom_screen{};
    float zoom_scale=0;
    std::array<uint32_t,3> zoom_reserved{};
};
static_assert(sizeof(WorldOverlayConstants)==64);
inline WorldOverlayConstants MakeWorldOverlayConstants(const WorldOverlayPass& pass,const WorldOverlayQuad& q)
{
    return {{float(pass.native_width),float(pass.native_height)},uint32_t(q.mode),q.colour_row,q.fade_row,{}};
}
// Caller validates the complete pass before allocations, preserves existing
// colour and D24, and uses the full-native viewport. Scissors are native top-
// down intersections. Depth is LEQUAL/read-only or disabled per quad. RGB AND
// alpha blend with SRC_ALPHA/INV_SRC_ALPHA ADD (no premultiplied conversion).
}
