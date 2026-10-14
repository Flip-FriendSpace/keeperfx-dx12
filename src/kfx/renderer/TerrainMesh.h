// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
// Deterministic terrain construction from owned presentation data only.
#pragma once
#include "kfx/renderer/SceneAssets.h"

namespace kfx::render {
enum class TerrainFace : uint8_t { North, East, South, West, Top, Bottom, Floor, Ceiling, Abyss };
enum class TerrainWalls : uint8_t { Captured, Full, Short };
struct TerrainOptions {
    TerrainWalls walls = TerrainWalls::Captured;
    bool ceilings = true; // Universal ceilings are only used for possession captures.
    uint32_t max_faces = 1000000; // Explicit bound before allocating geometry.
};
struct TerrainVertex {
    float x = 0, y = 0, z = 0; // Subtiles: divide captured world coordinates by 256.
    float nx = 0, ny = 0, nz = 0;
    float u = 0, v = 0, shade = 0; // Legacy palette shade row, interpolated then quantized.
    uint32_t material = 0;
};
struct TerrainMaterial {
    uint16_t tile = 0;
    uint8_t variation = 0;
    int32_t pixel_offset = 0; // Captured animation mapping, never resolved in the engine.
};
struct TerrainQuad {
    uint32_t cell = 0, first_vertex = 0;
    TerrainFace face = TerrainFace::Top;
    bool revealed = false;
};
struct TerrainChunk {
    uint16_t x = 0, y = 0; // 16x16-subtile grid, independent of traversal order.
    uint32_t first_index = 0, index_count = 0;
    std::array<float, 3> minimum{}, maximum{};
};
struct TerrainMesh {
    uint64_t epoch = 0, sequence = 0;
    uint32_t texture_generation = 0;
    std::vector<TerrainVertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<TerrainMaterial> materials;
    std::vector<TerrainQuad> quads;
    std::vector<TerrainChunk> chunks;
    uint32_t columns = 0, hidden_columns = 0, culled_sides = 0, missing_neighbors = 0;
    uint32_t liquid_columns = 0, wibble_columns = 0, ceiling_fallbacks = 0;
    std::array<float, 3> minimum{}, maximum{}, focus_minimum{}, focus_maximum{};
};
struct TerrainAtlas {
    uint32_t width = 0, height = 0;
    // Top 64 rows contain palette(fade[row][index]); below are indexed tiles
    // stored in R of RGBA8. Explicit texel loads avoid cross-material bleeding.
    std::vector<uint8_t> rgba;
    std::vector<std::array<uint32_t, 2>> origins;
};
TerrainMesh BuildTerrainMesh(const SceneCapture& capture, TerrainOptions options = {});
TerrainAtlas BuildTerrainAtlas(const SceneCapture& capture, const TerrainMesh& mesh);
bool ValidateTerrainMesh(const TerrainMesh& mesh);
// Inspection camera, not a reconstruction of the legacy projection/lens.
std::array<float, 16> TerrainInspectionTransform(const TerrainMesh& mesh, float aspect,
    float yaw, float elevation, float zoom = 1);
}
