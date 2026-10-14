// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#pragma once
#include "Dx12Device.h"
#include "EnhancedWorldPass.h"
#include "kfx/renderer/SceneGLCompositionTrace.h"

namespace kfx::render::dx12 {
Image RenderEnhancedWorldGLReference(const EnhancedWorldPass& world, const Image& atlas,
    const std::vector<SpriteColourRow>& cluts, const std::array<float,9>& palette_transform,
    const std::vector<OutlinePassDraw>& draws);
// Internal oracle stage hook. The shared isolated-context runner supplies the
// native RGBA8+D24 target; these unchanged production shaders draw into it.
void DrawEnhancedWorldGLStages(const EnhancedWorldPass& world,const Image& atlas);
// Full native output. World, keeper sprites, UI overlays and the resolved lens
// run in one isolated GL context; no rendered image is supplied as an input.
Image RenderEnhancedCompositionGLReference(const EnhancedWorldPass& world,const Image& atlas,
    const std::vector<SpriteColourRow>& cluts,const std::array<float,9>& palette_transform,
    const std::vector<OutlinePassDraw>& draws,const SceneGLCompositionTrace& composition);
// Internal hooks for the shared production-shader runner. Validation precedes
// context/resource creation. Finish uses its currently bound native colour+D24
// framebuffer and the supplied colour texture, which remain owned by the runner.
void ValidateEnhancedCompositionGLInputs(const EnhancedWorldPass& world,uint64_t atlas_bytes,size_t clut_count,
    const SceneGLCompositionTrace& composition);
Image FinishEnhancedCompositionGLReference(const SceneGLCompositionTrace& composition,unsigned native_colour_texture);
}
