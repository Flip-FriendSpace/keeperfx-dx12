// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#include "kfx/renderer/WorldScene.h"
#include <algorithm>
#include <cmath>

namespace kfx::render {
bool ValidateWorldScene(const WorldScene& s)
{
    if (!s.epoch || !s.sequence || !s.map_width || !s.map_height
        || s.map_width > 512 || s.map_height > 512
        || s.camera.viewport_w <= 0 || s.camera.viewport_h <= 0
        || !std::isfinite(s.capture_ms) || s.capture_ms < 0) return false;
    std::vector<bool> cells(size_t(s.map_width) * s.map_height);
    for (const auto& c : s.columns) {
        if (c.x >= s.map_width || c.y >= s.map_height
            || c.cell != uint32_t(c.y) * s.map_width + c.x || cells[c.cell]
            || c.texture_variation >= 32 || c.floor_texture >= s.top_cubes.size()) return false;
        cells[c.cell] = true;
        for (auto cube : c.cubes) if (cube >= s.cube_materials.size()) return false;
        if (!c.revealed && (c.texture_variation || c.map_flags || c.wibble || c.ceiling_subtiles
            || std::any_of(c.light.begin(), c.light.end(), [](uint16_t l) { return l != 0; }))) return false;
    }
    std::vector<uint32_t> ids;
    for (const auto& o : s.objects) {
        if (!o.id.incarnation || !o.id.slot) return false;
        ids.push_back(o.id.slot);
    }
    std::sort(ids.begin(), ids.end());
    if (std::adjacent_find(ids.begin(), ids.end()) != ids.end()) return false;
    ids.clear();
    for (const auto& l : s.lights) {
        if (!l.id.incarnation || !l.id.slot) return false;
        ids.push_back(l.id.slot);
    }
    std::sort(ids.begin(), ids.end());
    return std::adjacent_find(ids.begin(), ids.end()) == ids.end();
}
}
