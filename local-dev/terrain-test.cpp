// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#include "kfx/renderer/TerrainMesh.h"
#include <algorithm>
#include <bit>
#include <cassert>
#include <cmath>
#include <iostream>
#include <iomanip>
#include <limits>
#include <stdexcept>

using namespace kfx::render;
namespace {
SceneCapture Fixture(unsigned width = 5)
{
    SceneCapture c;
    auto& s = c.scene;
    s.epoch = 1; s.sequence = 2; s.map_width = width; s.map_height = 5;
    s.camera.viewport_w = 3840; s.camera.viewport_h = 2160;
    s.camera.has_rotation = true; s.camera.rotation[8] = 8192; s.camera.rotation[10] = -8192;
    s.texture_generation = 3; s.texture_dimension = 32; s.texture_row_stride = 256;
    s.texture_frames.assign(32 * 1544, 0); s.texture_frames[4] = 32;
    s.cube_materials.resize(3);
    s.cube_materials[1].textures = {0,1,2,3,4,5};
    s.cube_materials[2].textures = {7,8,9,10,11,12};
    s.top_cubes.assign(1544, 0); s.ceiling_textures.assign(1544, 13);
    for (unsigned y = 0; y < 5; ++y) for (unsigned x = 0; x < width; ++x) {
        SceneColumn col; col.x = x; col.y = y; col.cell = y * width + x;
        col.floor_texture = 6; col.revealed = true; col.light = {8192, 12288, 4096, 16128};
        col.cubes.fill(1); s.columns.push_back(col);
    }
    c.assets.texture_pixels.resize(8192);
    for (size_t i = 0; i < 8192; ++i) c.assets.texture_pixels[i] = uint8_t(i % 256);
    for (size_t i = 0; i < 768; ++i) c.assets.display_palette[i] = c.assets.base_palette[i] = i % 64;
    for (size_t i = 0; i < c.assets.fade.size(); ++i) c.assets.fade[i] = uint8_t(i % 256);
    return c;
}
SceneColumn& Cell(SceneCapture& c, int x = 2, int y = 2) { return c.scene.columns[y * c.scene.map_width + x]; }
void CheckGeometry(const TerrainMesh& m)
{
    assert(ValidateTerrainMesh(m));
    for (const auto& q : m.quads) {
        const auto& a = m.vertices[q.first_vertex];
        const auto& b = m.vertices[q.first_vertex + 1];
        const auto& c = m.vertices[q.first_vertex + 2];
        const float ux = b.x-a.x, uy = b.y-a.y, uz = b.z-a.z;
        const float vx = c.x-a.x, vy = c.y-a.y, vz = c.z-a.z;
        assert((uy*vz-uz*vy)*a.nx + (uz*vx-ux*vz)*a.ny + (ux*vy-uy*vx)*a.nz > 0);
        assert(a.u == 0 && a.v == 0 && b.u == 1 && b.v == 0 && c.u == 1 && c.v == 1);
        for (size_t i = 0; i < 4; ++i) {
            const auto& v = m.vertices[q.first_vertex + i];
            assert(v.material == a.material);
            if (!q.revealed) assert(v.shade == 0);
        }
    }
    for (const auto& chunk : m.chunks) for (uint32_t i = 0; i < chunk.index_count; ++i) {
        const auto& v = m.vertices[m.indices[chunk.first_index + i]];
        assert(v.x >= chunk.minimum[0] && v.x <= chunk.maximum[0]);
        assert(v.y >= chunk.minimum[1] && v.y <= chunk.maximum[1]);
        assert(v.z >= chunk.minimum[2] && v.z <= chunk.maximum[2]);
    }
}
uint64_t Fingerprint(const TerrainMesh& mesh)
{
    uint64_t hash = 14695981039346656037ull;
    auto add = [&](uint32_t value) { for (unsigned i = 0; i < 4; ++i) { hash ^= uint8_t(value >> (i*8)); hash *= 1099511628211ull; } };
    for (const auto& v : mesh.vertices) {
        for (auto f : {v.x,v.y,v.z,v.nx,v.ny,v.nz,v.u,v.v,v.shade}) add(std::bit_cast<uint32_t>(f));
        add(v.material);
    }
    for (auto i : mesh.indices) add(i);
    for (const auto& m : mesh.materials) { add(m.tile); add(m.variation); add(m.pixel_offset); }
    for (const auto& q : mesh.quads) { add(q.cell); add(q.first_vertex); add(uint32_t(q.face)); add(q.revealed); }
    return hash;
}
void Reject(auto callback) { bool rejected = false; try { callback(); } catch (const std::exception&) { rejected = true; } assert(rejected); }
void Tests()
{
    auto c = Fixture(); auto& center = Cell(c); center.in_view = true; center.solid_mask = 1;
    auto m = BuildTerrainMesh(c); CheckGeometry(m);
    assert(m.quads.size() == 5 && m.materials.size() == 5 && m.missing_neighbors == 0);
    for (const auto& q : m.quads) assert(m.materials[m.vertices[q.first_vertex].material].tile == unsigned(q.face));
    assert(m.vertices[0].x == 3 && m.vertices[0].y == 2 && m.vertices[0].z == 1); // north UV origin
    assert(m.vertices[0].shade == 36); // 48 * .75 legacy directional shade
    auto atlas = BuildTerrainAtlas(c, m);
    for (size_t i = 0; i < m.materials.size(); ++i) {
        auto [x,y] = atlas.origins[i];
        assert(atlas.rgba[(y*atlas.width+x)*4] == c.assets.texture_pixels[m.materials[i].pixel_offset]);
    }
    const auto before = Fingerprint(m);
    std::reverse(c.scene.columns.begin(), c.scene.columns.end());
    assert(Fingerprint(BuildTerrainMesh(c)) == before); // order-independent output
    c = Fixture(); Cell(c).in_view = true; Cell(c).solid_mask = 3;
    m = BuildTerrainMesh(c); CheckGeometry(m); assert(m.quads.size() == 9 && m.maximum[2] == 2);
    c = Fixture(); Cell(c).in_view = true; Cell(c).solid_mask = 1; Cell(c,3).solid_mask = 1;
    m = BuildTerrainMesh(c); assert(m.quads.size() == 4 && m.culled_sides == 1); // halo culls but emits nothing
    Cell(c,3).in_view = true; m = BuildTerrainMesh(c); CheckGeometry(m);
    assert(m.quads.size() == 8 && m.culled_sides == 2);
    c = Fixture(20); Cell(c,15).in_view = Cell(c,16).in_view = true;
    Cell(c,15).solid_mask = Cell(c,16).solid_mask = 1; Cell(c,16).texture_variation = 17;
    m = BuildTerrainMesh(c); CheckGeometry(m); assert(m.chunks.size() == 2 && m.quads.size() == 8 && m.culled_sides == 2);
    c = Fixture(); Cell(c).in_view = true; Cell(c).solid_mask = 49; Cell(c).cubes[5] = 2;
    m = BuildTerrainMesh(c); CheckGeometry(m); assert(m.quads.size() == 15);
    auto underside = std::find_if(m.quads.begin(),m.quads.end(),[](const auto& q){return q.face==TerrainFace::Bottom;});
    assert(underside != m.quads.end()); assert(m.materials[m.vertices[underside->first_vertex].material].tile == 12);
    c = Fixture(); Cell(c).in_view = true; Cell(c).solid_mask = 31; Cell(c).cubes[4] = 2;
    c.scene.camera.short_walls = 1; m = BuildTerrainMesh(c); CheckGeometry(m);
    assert(m.quads.size() == 9 && m.maximum[2] == 2);
    assert(m.materials[m.vertices.back().material].tile == 11); // original wall cap
    Cell(c).map_flags = 2; assert(BuildTerrainMesh(c).maximum[2] == 5); // room exemption
    Cell(c).map_flags = 64; assert(BuildTerrainMesh(c).maximum[2] == 5); // door exemption
    Cell(c).map_flags = 0; Cell(c).column_flags = 2; assert(BuildTerrainMesh(c).maximum[2] == 5); // lintel exemption
    Cell(c).column_flags = 0; Cell(c).revealed = false; Cell(c).light = {};
    m = BuildTerrainMesh(c); CheckGeometry(m); assert(m.maximum[2] == 2 && m.hidden_columns == 1);
    c = Fixture(); Cell(c).in_view = true; Cell(c).map_flags = 4;
    m = BuildTerrainMesh(c); assert(m.quads.size() == 1 && m.materials[0].tile == 578 && m.vertices[0].shade == 44);
    Cell(c).solid_mask = 1; Cell(c).map_flags = 128; m = BuildTerrainMesh(c);
    assert(m.materials[m.vertices.back().material].tile == 579);
    c = Fixture(); Cell(c).in_view = true; c.scene.top_cubes[7] = 2; c.scene.cube_materials[2].properties = 16;
    Cell(c,3).in_view = true; Cell(c,3).floor_texture = 7;
    m = BuildTerrainMesh(c); CheckGeometry(m); assert(m.quads.size() == 7 && m.minimum[2] == -6);
    assert(m.vertices.back().shade == 0); // abyss skirt fades out
    c = Fixture(); Cell(c).in_view = true; c.scene.camera.view = SceneView::Possession;
    for (auto& col : c.scene.columns) col.ceiling_subtiles = 7;
    m = BuildTerrainMesh(c); CheckGeometry(m); assert(m.quads.size() == 2 && m.ceiling_fallbacks == 0);
    assert(m.quads.back().face == TerrainFace::Ceiling && m.materials.back().tile == 13);
    c = Fixture(); Cell(c).in_view = true; Cell(c).solid_mask = 1;
    Reject([&]{ (void)BuildTerrainMesh(c, {TerrainWalls::Captured, true, 1}); });
    auto invalid = c; Cell(invalid).solid_mask = 256; Reject([&]{ (void)BuildTerrainMesh(invalid); });
    invalid = c; invalid.scene.columns.push_back(Cell(invalid)); Reject([&]{ (void)BuildTerrainMesh(invalid); });
    invalid = c; invalid.scene.texture_frames[0] = -1; Reject([&]{ (void)BuildTerrainMesh(invalid); });
    m = BuildTerrainMesh(c); invalid = c; invalid.scene.epoch++; Reject([&]{ (void)BuildTerrainAtlas(invalid,m); });
    auto bad_mesh = m; bad_mesh.quads[0].first_vertex = uint32_t(m.vertices.size()); assert(!ValidateTerrainMesh(bad_mesh));
    bad_mesh = m; bad_mesh.vertices[0].u = std::numeric_limits<float>::quiet_NaN(); assert(!ValidateTerrainMesh(bad_mesh));
    bad_mesh = m; bad_mesh.chunks[0].maximum[0] = bad_mesh.chunks[0].minimum[0]; assert(!ValidateTerrainMesh(bad_mesh));
    bad_mesh = m; bad_mesh.indices[0] = 1; assert(!ValidateTerrainMesh(bad_mesh));
    const auto matrix = TerrainInspectionTransform(m,16.0f/9,.4f,.8f);
    float dot = 0; for (int i=0;i<3;++i) dot += matrix[i*4+1]*matrix[i*4+2]; assert(std::abs(dot)<1e-7f);
    std::cout << "{\"passed\":true,\"fixtureFingerprint\":\"" << std::hex << Fingerprint(m) << "\"}\n";
}
}
int main(int argc, char** argv)
{
    try {
        if (argc == 1) { Tests(); return 0; }
        const auto capture = LoadSceneCapture(argv[1]);
        const auto m = BuildTerrainMesh(*capture); CheckGeometry(m);
        const auto atlas = BuildTerrainAtlas(*capture,m);
        auto reversed = *capture; std::reverse(reversed.scene.columns.begin(),reversed.scene.columns.end());
        assert(Fingerprint(BuildTerrainMesh(reversed)) == Fingerprint(m));
        std::cout << "{\"passed\":true,\"view\":" << unsigned(capture->scene.camera.view)
            << ",\"columns\":" << m.columns << ",\"hiddenColumns\":" << m.hidden_columns
            << ",\"faces\":" << m.quads.size() << ",\"triangles\":" << m.indices.size()/3
            << ",\"chunks\":" << m.chunks.size() << ",\"materials\":" << m.materials.size()
            << ",\"culledSides\":" << m.culled_sides << ",\"missingNeighbors\":" << m.missing_neighbors
            << ",\"ceilingFallbacks\":" << m.ceiling_fallbacks << ",\"wibbleColumns\":" << m.wibble_columns
            << ",\"atlasBytes\":" << atlas.rgba.size() << ",\"fingerprint\":\"" << std::hex << Fingerprint(m) << "\"}\n";
        return 0;
    } catch(const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
