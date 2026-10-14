// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#pragma once
#include "Dx12Device.h"
#include "WorldOverlayPass.h"
#include "OutlinePass.h"
#include <string>

namespace kfx::render::dx12 {
Image RenderWorldOverlayGLReference(const WorldOverlayPass& pass,const Image& native_background,
    const std::vector<OutlineDepthTriangle>& terrain);
std::string WorldOverlayGLReferenceDevice();
}
