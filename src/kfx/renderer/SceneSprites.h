// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#pragma once
#include "kfx/renderer/SceneCamera.h"
#include "kfx/renderer/TerrainMesh.h"

namespace kfx::render {
struct SceneSpriteVertex { float x, y, z, u, v; };
struct SceneSpriteQuad {
    SceneObjectId id;
    uint32_t sprite = 0;
    uint8_t direction = 0;
    bool flip = false;
    std::array<SceneSpriteVertex, 4> vertices{};
};
struct SceneSpriteMesh {
    // Terrain atlas is copied into the top left; its texel origins stay valid.
    TerrainAtlas atlas;
    std::vector<SceneSpriteQuad> quads;
    uint32_t empty = 0, clipped = 0, deferred_effects = 0;
};
SceneSpriteMesh BuildSceneSprites(const SceneCapture& capture, const TerrainMesh& terrain);
}
