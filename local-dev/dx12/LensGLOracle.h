// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
// Isolated standard-lens reference using unchanged production OpenGL shaders.
#pragma once
#include "Dx12Device.h"
#include "LensPass.h"
#include <string>

namespace kfx::render::dx12 {
// Both images are complete native, top-down RGBA8 images. The target outside
// the destination rectangle is preserved. Inputs are never borrowed from a
// different renderer's result for cross-renderer comparisons.
Image RenderLensGLReference(const LensPass& pass, const Image& native_source,
    const Image& native_target_background);
std::string LensGLReferenceDevice();
}
