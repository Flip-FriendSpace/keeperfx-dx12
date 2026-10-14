// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
// Independent production-shader reference for the owned outline diagnostic.
#pragma once
#include "Dx12Device.h"
#include <string>

namespace kfx::render::dx12 {
// VisibleCoverage is a declared flat-colour coverage/depth probe, not normal
// sprite artwork. Every source is zero-filled outside its full logical bounds.
Image RenderOutlineGLReference(const Image& background, const Image& coverage_atlas,
    const std::vector<OutlineDepthTriangle>& terrain, const std::vector<OutlinePassDraw>& draws);
// Uses the unchanged production normal/glow shaders as well as outlines.
// Rows are actual resolved lookup inputs; no CPU-rendered sprite pixels.
Image RenderEnhancedSpritesGLReference(const Image& background, const Image& index_coverage_atlas,
    const std::vector<SpriteColourRow>& resolved_cluts, const std::array<float, 9>& palette_transform,
    const std::vector<OutlineDepthTriangle>& terrain, const std::vector<OutlinePassDraw>& draws);
// Device used by the most recent reference call on this thread.
std::string OutlineGLReferenceDevice();
}
