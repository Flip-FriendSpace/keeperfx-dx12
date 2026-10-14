// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#include "kfx/renderer/TerrainMesh.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <stdexcept>

namespace kfx::render {
namespace {
constexpr uint8_t IsRoom = 2, Marked = 4, IsDoor = 64, MarkedGold = 128;
constexpr uint8_t Liquid = 3, Abyss = 16;
using Point = std::array<float, 3>;
void Require(bool ok, const char* text) { if (!ok) throw std::runtime_error(text); }
int FloorHeight(unsigned mask) { int z = 0; while (z < 8 && (mask & (1u << z))) ++z; return z; }
Point EmptyMin() { return {1e9f, 1e9f, 1e9f}; }
Point EmptyMax() { return {-1e9f, -1e9f, -1e9f}; }
void Expand(Point& minimum, Point& maximum, const Point& p) {
    for (size_t i = 0; i < 3; ++i) { minimum[i] = std::min(minimum[i], p[i]); maximum[i] = std::max(maximum[i], p[i]); }
}
}

TerrainMesh BuildTerrainMesh(const SceneCapture& capture, TerrainOptions options)
{
    Require(ValidateSceneCapture(capture), "Invalid terrain capture");
    Require(options.max_faces > 0 && options.max_faces <= 1000000, "Invalid terrain face budget");
    const auto& s = capture.scene;
    TerrainMesh mesh;
    mesh.epoch = s.epoch; mesh.sequence = s.sequence; mesh.texture_generation = s.texture_generation;
    mesh.minimum = mesh.focus_minimum = EmptyMin(); mesh.maximum = mesh.focus_maximum = EmptyMax();
    std::vector<const SceneColumn*> grid(size_t(s.map_width) * s.map_height, nullptr);
    std::map<uint32_t, std::vector<const SceneColumn*>> chunks;
    for (const auto& c : s.columns) {
        Require(c.solid_mask <= 255, "Column exceeds the eight-cube terrain stack");
        grid[c.cell] = &c;
        if (c.in_view) chunks[(c.y / 16) * 32 + c.x / 16].push_back(&c);
    }
    auto column = [&](int x, int y) -> const SceneColumn* {
        return x < 0 || y < 0 || x >= s.map_width || y >= s.map_height ? nullptr : grid[y * s.map_width + x];
    };
    const bool short_walls = options.walls == TerrainWalls::Short
        || (options.walls == TerrainWalls::Captured && s.camera.short_walls != 0);
    auto mask = [&](const SceneColumn& c) -> unsigned {
        if (short_walls && (!c.revealed || (c.solid_mask >= 8
            && !(c.map_flags & (IsRoom | IsDoor)) && !(c.column_flags & 14)))) return c.solid_mask & 3;
        return c.solid_mask;
    };
    auto properties = [&](const SceneColumn& c) -> uint8_t {
        const auto cube = s.top_cubes.at(c.floor_texture);
        return cube >= 0 ? s.cube_materials.at(cube).properties : 0;
    };
    std::map<uint32_t, uint32_t> material_ids;
    auto material = [&](const SceneColumn& c, uint16_t tile) {
        Require(tile < 1544, "Terrain material outside texture table");
        const uint32_t key = uint32_t(c.texture_variation) * 1544 + tile;
        const int32_t offset = s.texture_frames.at(key);
        Require(offset >= 0, "Terrain material is missing from the capture");
        auto [entry, inserted] = material_ids.emplace(key, uint32_t(mesh.materials.size()));
        if (inserted) mesh.materials.push_back({tile, c.texture_variation, offset});
        return entry->second;
    };
    auto emit = [&](const SceneColumn& c, TerrainChunk& chunk, TerrainFace face,
        uint16_t tile, const std::array<Point, 4>& points, Point normal, float boost = 0) {
        Require(mesh.quads.size() < options.max_faces, "Terrain face budget exceeded");
        const uint32_t first = static_cast<uint32_t>(mesh.vertices.size());
        const uint32_t mat = material(c, tile);
        float direction = 1;
        if (normal[2] == 0) {
            // The legacy normal shading derives from row 2 of its camera matrix.
            float a = 0;
            if (s.camera.has_rotation) a = normal[0] ? -normal[0] * s.camera.rotation[8] : normal[1] * s.camera.rotation[10];
            direction = (std::clamp(a, 0.0f, 16384.0f) + 16384) / 32768;
        }
        for (size_t i = 0; i < 4; ++i) {
            const auto& p = points[i];
            const unsigned corner = (p[0] > c.x ? 1 : 0) + (p[1] > c.y ? 2 : 0);
            float shade = std::min<float>(c.light[corner], 16128) / 256 * direction + boost;
            if (face == TerrainFace::Abyss) shade *= .7f * std::max(0.0f, 1 + p[2] / 6);
            if (!c.revealed) shade = 0;
            mesh.vertices.push_back({p[0], p[1], p[2], normal[0], normal[1], normal[2],
                i == 1 || i == 2 ? 1.0f : 0.0f, i >= 2 ? 1.0f : 0.0f, std::clamp(shade, 0.0f, 63.0f), mat});
            Expand(chunk.minimum, chunk.maximum, p); Expand(mesh.minimum, mesh.maximum, p);
            if (c.revealed) Expand(mesh.focus_minimum, mesh.focus_maximum, p);
        }
        for (auto i : {0u, 1u, 2u, 0u, 2u, 3u}) mesh.indices.push_back(first + i);
        mesh.quads.push_back({c.cell, first, face, c.revealed});
    };
    for (auto& [key, cells] : chunks) {
        std::sort(cells.begin(), cells.end(), [](auto a, auto b) { return a->cell < b->cell; });
        TerrainChunk chunk;
        chunk.x = key % 32; chunk.y = key / 32;
        chunk.first_index = static_cast<uint32_t>(mesh.indices.size()); chunk.minimum = EmptyMin(); chunk.maximum = EmptyMax();
        for (const auto* cp : cells) {
            const auto& c = *cp;
            ++mesh.columns; mesh.hidden_columns += !c.revealed;
            mesh.wibble_columns += c.wibble != 0; mesh.liquid_columns += (properties(c) & Liquid) != 0;
            const float x = c.x, y = c.y;
            const unsigned occupied = mask(c);
            const bool abyss = c.revealed && (properties(c) & Abyss);
            auto horizontal = [&](float z, uint16_t tile, bool bottom, TerrainFace face, float boost = 0) {
                const std::array<Point, 4> points = bottom
                    ? std::array<Point, 4>{{{x,y+1,z}, {x+1,y+1,z}, {x+1,y,z}, {x,y,z}}}
                    : std::array<Point, 4>{{{x,y,z}, {x+1,y,z}, {x+1,y+1,z}, {x,y+1,z}}};
                emit(c, chunk, face, tile, points, {0,0,bottom ? -1.0f : 1.0f}, boost);
            };
            auto side = [&](unsigned direction, float low, float high, uint16_t tile, TerrainFace face) {
                const std::array<std::array<Point, 4>, 4> points{{
                    {{{x+1,y,high}, {x,y,high}, {x,y,low}, {x+1,y,low}}},
                    {{{x+1,y+1,high}, {x+1,y,high}, {x+1,y,low}, {x+1,y+1,low}}},
                    {{{x,y+1,high}, {x+1,y+1,high}, {x+1,y+1,low}, {x,y+1,low}}},
                    {{{x,y,high}, {x,y+1,high}, {x,y+1,low}, {x,y,low}}}
                }};
                constexpr std::array<Point, 4> normals{{{0,-1,0}, {1,0,0}, {0,1,0}, {-1,0,0}}};
                emit(c, chunk, face, tile, points[direction], normals[direction]);
            };
            constexpr int dx[] = {0, 1, 0, -1}, dy[] = {-1, 0, 1, 0};
            std::array<const SceneColumn*, 4> neighbors{};
            for (size_t d = 0; d < 4; ++d) {
                neighbors[d] = column(c.x + dx[d], c.y + dy[d]);
                mesh.missing_neighbors += neighbors[d] == nullptr;
            }
            for (unsigned z = 0; z < 8; ++z) {
                if (!(occupied & (1u << z))) continue;
                const auto& cube = s.cube_materials.at(c.cubes[z]);
                for (unsigned d = 0; d < 4; ++d) {
                    if (neighbors[d] && (mask(*neighbors[d]) & (1u << z))) { ++mesh.culled_sides; continue; }
                    side(d, float(z), float(z + 1), cube.textures[d], static_cast<TerrainFace>(d));
                }
                if (z == 7 || !(occupied & (1u << (z + 1)))) {
                    auto tile = cube.textures[4];
                    // Cut-down walls keep the original cap artwork at the new height.
                    if (short_walls && int(z + 1) == FloorHeight(occupied) && FloorHeight(c.solid_mask) > int(z + 1))
                        tile = s.cube_materials.at(c.cubes[FloorHeight(c.solid_mask) - 1]).textures[4];
                    float boost = 0;
                    if (int(z + 1) == FloorHeight(occupied)) {
                        if (c.map_flags & Marked) { tile = 578; boost = 12; }
                        else if (c.map_flags & MarkedGold) { tile = 579; boost = 12; }
                    }
                    horizontal(float(z + 1), tile, false, TerrainFace::Top, boost);
                }
                if (z > 0 && !(occupied & (1u << (z - 1)))) {
                    unsigned top = z;
                    while (top < 7 && (occupied & (1u << (top + 1)))) ++top;
                    horizontal(float(z), s.cube_materials.at(c.cubes[top]).textures[5], true, TerrainFace::Bottom);
                }
            }
            if (!(occupied & 1) && !abyss)
                horizontal(0, c.map_flags & Marked ? 578 : c.floor_texture, false, TerrainFace::Floor, c.map_flags & Marked ? 12 : 0);
            if (!abyss && c.revealed) for (unsigned d = 0; d < 4; ++d) {
                if (!neighbors[d] || !neighbors[d]->revealed || !(properties(*neighbors[d]) & Abyss)) continue;
                Require(!s.ceiling_textures.empty(), "Abyss wall material is absent");
                auto tile = s.ceiling_textures[0];
                if (properties(c) & Liquid) {
                    int top = FloorHeight(c.solid_mask);
                    const auto cube = top ? c.cubes[top - 1] : s.top_cubes[c.floor_texture];
                    if (cube >= 0) tile = s.cube_materials.at(cube).textures[d];
                }
                for (int depth = 0; depth < 6; ++depth) side(d, float(-depth - 1), float(-depth), tile, TerrainFace::Abyss);
            }
            if (options.ceilings && s.camera.view == SceneView::Possession && c.revealed
                && c.ceiling_subtiles > FloorHeight(occupied)) {
                Require(c.floor_texture < s.ceiling_textures.size(), "Ceiling material is absent");
                std::array<Point, 4> points{{{x,y+1,0}, {x+1,y+1,0}, {x+1,y,0}, {x,y,0}}};
                for (auto& p : points) {
                    const auto* corner = column(int(p[0]), int(p[1]));
                    const bool valid = corner && corner->revealed && corner->ceiling_subtiles;
                    if (!valid) ++mesh.ceiling_fallbacks;
                    p[2] = valid ? corner->ceiling_subtiles : c.ceiling_subtiles;
                }
                emit(c, chunk, TerrainFace::Ceiling, s.ceiling_textures[abyss ? 0 : c.floor_texture], points, {0,0,-1});
            }
        }
        chunk.index_count = static_cast<uint32_t>(mesh.indices.size()) - chunk.first_index;
        if (chunk.index_count) mesh.chunks.push_back(chunk);
    }
    Require(!mesh.indices.empty(), "Capture contains no drawable terrain");
    if (mesh.focus_minimum[0] > mesh.focus_maximum[0]) { mesh.focus_minimum = mesh.minimum; mesh.focus_maximum = mesh.maximum; }
    Require(ValidateTerrainMesh(mesh), "Generated invalid terrain");
    return mesh;
}

bool ValidateTerrainMesh(const TerrainMesh& m)
{
    if (m.quads.empty() || m.quads.size() > 1000000 || m.vertices.size() != m.quads.size() * 4
        || m.indices.size() != m.quads.size() * 6 || m.materials.empty()) return false;
    auto bounds = [](const Point& minimum, const Point& maximum) {
        for (size_t i = 0; i < 3; ++i)
            if (!std::isfinite(minimum[i]) || !std::isfinite(maximum[i]) || minimum[i] > maximum[i]) return false;
        return true;
    };
    if (!bounds(m.minimum, m.maximum) || !bounds(m.focus_minimum, m.focus_maximum)) return false;
    for (auto i : m.indices) if (i >= m.vertices.size()) return false;
    for (const auto& v : m.vertices) {
        if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z) || !std::isfinite(v.shade)
            || !std::isfinite(v.nx) || !std::isfinite(v.ny) || !std::isfinite(v.nz)
            || !std::isfinite(v.u) || !std::isfinite(v.v)
            || v.shade < 0 || v.shade > 63 || v.material >= m.materials.size()) return false;
    }
    for (size_t i = 0; i < m.quads.size(); ++i) {
        const auto& q = m.quads[i];
        if (q.first_vertex != i * 4 || q.face > TerrainFace::Abyss) return false;
        constexpr unsigned corners[] = {0, 1, 2, 0, 2, 3};
        for (size_t j = 0; j < 6; ++j) if (m.indices[i * 6 + j] != q.first_vertex + corners[j]) return false;
        for (size_t j = 0; j < 4; ++j) {
            const auto& v = m.vertices[q.first_vertex + j];
            if (v.material != m.vertices[q.first_vertex].material || (!q.revealed && v.shade != 0)) return false;
        }
    }
    uint32_t next = 0;
    for (const auto& c : m.chunks) {
        if (c.first_index != next || !c.index_count || c.index_count % 6 || c.index_count > m.indices.size() - next
            || !bounds(c.minimum, c.maximum)) return false;
        for (uint32_t i = 0; i < c.index_count; ++i) {
            const auto& v = m.vertices[m.indices[next + i]];
            if (v.x < c.minimum[0] || v.x > c.maximum[0] || v.y < c.minimum[1] || v.y > c.maximum[1]
                || v.z < c.minimum[2] || v.z > c.maximum[2]) return false;
        }
        next += c.index_count;
    }
    return next == m.indices.size();
}

TerrainAtlas BuildTerrainAtlas(const SceneCapture& capture, const TerrainMesh& mesh)
{
    Require(ValidateSceneCapture(capture) && ValidateTerrainMesh(mesh), "Invalid terrain atlas input");
    Require(mesh.epoch == capture.scene.epoch && mesh.sequence == capture.scene.sequence
        && mesh.texture_generation == capture.scene.texture_generation, "Terrain/capture identity mismatch");
    TerrainAtlas atlas;
    atlas.width = 256;
    while (uint64_t(atlas.width / 32) * (atlas.width / 32) < mesh.materials.size()) atlas.width *= 2;
    atlas.height = 64 + static_cast<uint32_t>((mesh.materials.size() + atlas.width / 32 - 1) / (atlas.width / 32)) * 32;
    Require(atlas.width <= 16384 && atlas.height <= 16384, "Terrain atlas exceeds texture limits");
    atlas.rgba.resize(size_t(atlas.width) * atlas.height * 4, 0);
    for (size_t i = 3; i < atlas.rgba.size(); i += 4) atlas.rgba[i] = 255;
    for (unsigned shade = 0; shade < 64; ++shade) for (unsigned index = 0; index < 256; ++index) {
        const auto remapped = capture.assets.fade[shade * 256 + index];
        for (unsigned channel = 0; channel < 3; ++channel)
            atlas.rgba[(size_t(shade) * atlas.width + index) * 4 + channel] = capture.assets.display_palette[remapped * 3 + channel] * 255 / 63;
    }
    for (size_t i = 0; i < mesh.materials.size(); ++i) {
        const auto& mat = mesh.materials[i];
        Require(mat.variation < 32 && mat.tile < 1544 && mat.pixel_offset >= 0
            && mat.pixel_offset == capture.scene.texture_frames[mat.variation * 1544 + mat.tile], "Terrain material mapping changed");
        const uint32_t ox = uint32_t(i % (atlas.width / 32)) * 32, oy = 64 + uint32_t(i / (atlas.width / 32)) * 32;
        atlas.origins.push_back({ox, oy});
        for (unsigned y = 0; y < 32; ++y) for (unsigned x = 0; x < 32; ++x)
            atlas.rgba[(size_t(oy + y) * atlas.width + ox + x) * 4] = capture.assets.texture_pixels.at(
                size_t(mat.pixel_offset) + size_t(y) * capture.scene.texture_row_stride + x);
    }
    return atlas;
}

std::array<float, 16> TerrainInspectionTransform(const TerrainMesh& m, float aspect, float yaw, float pitch, float zoom)
{
    Require(std::isfinite(aspect) && aspect > 0 && std::isfinite(yaw) && std::isfinite(pitch)
        && std::isfinite(zoom) && zoom > 0, "Invalid terrain inspection camera");
    Point center;
    for (size_t i = 0; i < 3; ++i) center[i] = (m.focus_minimum[i] + m.focus_maximum[i]) * .5f;
    const float radius = std::max({m.focus_maximum[0] - m.focus_minimum[0], m.focus_maximum[1] - m.focus_minimum[1], 8.0f});
    const Point right{std::cos(yaw), std::sin(yaw), 0};
    const Point up{-std::sin(yaw) * std::sin(pitch), std::cos(yaw) * std::sin(pitch), std::cos(pitch)};
    const Point forward{-std::sin(yaw) * std::cos(pitch), std::cos(yaw) * std::cos(pitch), -std::sin(pitch)};
    const float sy = 1.65f * zoom * std::min(1.0f, aspect) / radius, sx = sy / aspect, sz = 1.0f / 2048;
    std::array<float, 16> transform{};
    transform[15] = 1; transform[14] = .5f;
    for (size_t i = 0; i < 3; ++i) {
        transform[i * 4] = right[i] * sx; transform[i * 4 + 1] = up[i] * sy; transform[i * 4 + 2] = forward[i] * sz;
        transform[12] -= center[i] * right[i] * sx;
        transform[13] -= center[i] * up[i] * sy;
        transform[14] -= center[i] * forward[i] * sz;
    }
    return transform;
}
}
