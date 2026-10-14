// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
// World-space presentation data. No engine headers, simulation pointers or GPU types.
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace kfx::render {

// An identity is (scene epoch, kind, slot, incarnation). It is local presentation
// state, never saved or synchronized. Loading/resyncing starts another epoch.
struct SceneObjectId {
    uint64_t incarnation = 0;
    uint32_t slot = 0;
    bool operator==(const SceneObjectId&) const = default;
};

struct ScenePosition {
    int32_t x = 0, y = 0, z = 0;
    bool operator==(const ScenePosition&) const = default;
};

enum class SceneView : uint8_t { Overhead, Front, Possession };

struct SceneCamera {
    SceneView view = SceneView::Overhead;
    ScenePosition position;
    int32_t yaw = 0, pitch = 0, roll = 0; // 2048 angle units per revolution.
    int32_t scaled_zoom = 0, horizontal_fov = 0, lens = 0;
    int32_t viewport_x = 0, viewport_y = 0, viewport_w = 0, viewport_h = 0;
    int32_t pixel_size = 1, view_distance = 0;
    uint8_t view_mode = 0, lens_mode = 0, short_walls = 0;
    // Exact legacy fixed-point rotation coefficients, for projection parity.
    // Front view uses its quadrant/zoom instead; has_rotation is false there.
    bool has_rotation = false;
    std::array<int32_t, 12> rotation{};
};

struct SceneCubeMaterial {
    std::array<uint16_t, 6> textures{}; // Legacy face order; logical tile IDs.
    uint8_t properties = 0; // Lava/water/abyss etc., copied from cube definitions.
};

// Unit-cube column instances are the world geometry, before legacy projection,
// clipping, wibble or triangulation. One subtile is 256 world units; Z is up.
struct SceneColumn {
    uint32_t cell = 0; // y * map_width + x; stable within the scene epoch.
    uint16_t x = 0, y = 0;
    std::array<uint16_t, 8> cubes{};
    uint16_t solid_mask = 0, floor_texture = 0;
    uint8_t column_flags = 0, orientation = 0, texture_variation = 0;
    uint8_t map_flags = 0, wibble = 0, ceiling_subtiles = 0;
    bool revealed = false, in_view = false;
    // Four corner samples in order (0,0), (1,0), (0,1), (1,1).
    // Hidden corners carry zero; neighbors are captured for meshing boundaries.
    std::array<uint16_t, 4> light{};
};

struct SceneObject {
    SceneObjectId id;
    ScenePosition position; // The interpolated position already used this frame.
    ScenePosition simulation_position;
    int32_t floor_height = 0;
    uint16_t model = 0, animation = 0, size = 0;
    int16_t yaw = 0, pitch = 0;
    uint8_t owner = 0, class_id = 0, draw_class = 0, frame = 0;
    uint8_t render_flags = 0, tint = 0;
};

struct SceneLight {
    SceneObjectId id;
    ScenePosition position, previous_position;
    uint16_t radius = 0, random_intensity = 0, previous_random_intensity = 0;
    uint8_t intensity = 0, flags = 0;
    bool reset_interpolation = false;
    int32_t last_turn_moved = 0, last_turn_randomized = 0;
};

// Optional receipt metadata for doors, whose visible geometry is column terrain.
// This is never serialized into the frozen capture or consulted by rendering.
struct SceneReferenceDoor {
    SceneObjectId id;
    uint16_t slab_x = 0, slab_y = 0, model = 0;
    int16_t open_amount = 0;
    uint8_t owner = 0, orientation = 0, state = 0, locked = 0;
};

// Ephemeral local input receipts. Not part of Game or the scene capture codec.
struct SceneReferencePacket {
    bool valid=false;
    uint64_t epoch=0,serial=0;
    int64_t consumed_turn=0,packet_turn=0;
    int32_t x=0,y=0;
    uint32_t control=0;
    uint8_t additional=0,context=0,action=0;
    std::array<int32_t,4> parameters{};
};
struct SceneReferenceInput {
    bool valid=false,map_point_valid=false;
    std::array<int32_t,2> native_mouse{},native_size{},window_size{},drawable_size{};
    std::array<int32_t,4> viewport{},present_rect{},top{},block{}; // Pick XY then fractional XY.
    std::array<int32_t,2> floor{}; // Existing floor pick has no separate fraction globals.
    int32_t view_type=0,work_state=0;
    SceneObjectId local_hover{},player_hover{}; // Input state at seal, before this frame's sprite callbacks.
    SceneReferencePacket consumed,last_button;
};

struct SceneWibble {
    ScenePosition offset;
    int32_t lightness_offset = 0;
};

struct WorldScene {
    uint64_t epoch = 0, sequence = 0;
    int32_t game_turn = 0, player = 0;
    uint16_t map_width = 0, map_height = 0;
    uint32_t texture_generation = 0;
    int32_t ambient_light = 0;
    bool lights_enabled = false;
    float interpolation = 0;
    float lava_scroll = 0, water_scroll = 0;
    std::array<SceneWibble, 128> wibble{};
    SceneCamera camera;
    std::vector<SceneColumn> columns;
    std::vector<SceneObject> objects;
    std::vector<SceneLight> lights;
    std::vector<SceneCubeMaterial> cube_materials;
    std::vector<int16_t> top_cubes;
    std::vector<uint16_t> ceiling_textures;
    // Each logical tile maps to the byte offset of its currently selected tile
    // in block_mem, or -1. Rows use texture_row_stride, not a packed tile stride.
    // No pointer into mutable animated block_ptrs survives.
    // SceneCapture owns matching pixel storage for opt-in frozen scenes. Normal
    // snapshots alone still require producer-side asset resolution; keys must
    // include epoch AND texture_generation, never just the tile number.
    std::vector<int32_t> texture_frames;
    int32_t texture_dimension = 0, texture_row_stride = 0;
    double capture_ms = 0; // CPU work in snapshot capture only, not legacy drawing.
    // Opt-in diagnostic, attached to the exact sealed frame for its screenshot
    // or owned capture. Never serialized or supplied by a decoded capture.
    // Empty in normal play; consumers must not capture a different live frame.
    std::string reference_image;
    std::vector<SceneReferenceDoor> reference_doors;
    SceneReferenceInput reference_input;

    void Clear()
    {
        columns.clear(); objects.clear(); lights.clear();
        cube_materials.clear(); top_cubes.clear(); ceiling_textures.clear(); texture_frames.clear();
        capture_ms = 0;
        reference_image.clear();
        reference_doors.clear();
        reference_input={};
        lava_scroll = water_scroll = 0;
    }
    size_t PayloadBytes() const
    {
        return sizeof(*this) + columns.size() * sizeof(SceneColumn)
            + objects.size() * sizeof(SceneObject) + lights.size() * sizeof(SceneLight)
            + cube_materials.size() * sizeof(SceneCubeMaterial)
            + top_cubes.size() * sizeof(int16_t) + ceiling_textures.size() * sizeof(uint16_t)
            + texture_frames.size() * sizeof(int32_t) + reference_image.size()
            + reference_doors.size() * sizeof(SceneReferenceDoor);
    }
};

// Only the producer can mutate a buffer. A slot is reused only when neither a
// renderer nor diagnostics holds it; retained frames survive load/reset safely.
class WorldScenePool {
    std::array<std::shared_ptr<WorldScene>, 3> slots;
public:
    std::shared_ptr<WorldScene> Acquire()
    {
        for (auto& slot : slots) {
            if (!slot || slot.use_count() == 1) {
                if (!slot) slot = std::make_shared<WorldScene>();
                slot->Clear();
                return slot;
            }
        }
        // A consumer retaining more than the normal two frames must not stall
        // gameplay or cause an in-flight frame to be overwritten.
        return std::make_shared<WorldScene>();
    }
};

// Allocation hooks catch same-tick reuse, including objects never rendered.
class SceneIncarnations {
    std::vector<uint64_t> slots;
    uint64_t serial = 0;
public:
    explicit SceneIncarnations(size_t count) : slots(count) {}
    void Allocate(uint32_t slot) { if (slot > 0 && slot < slots.size()) slots[slot] = ++serial; }
    void Reset() { for (auto& slot : slots) slot = 0; }
    SceneObjectId Peek(uint32_t slot) const
    {
        if (slot == 0 || slot >= slots.size() || slots[slot] == 0) return {};
        return {slots[slot], slot};
    }
    SceneObjectId Get(uint32_t slot)
    {
        if (slot == 0 || slot >= slots.size()) return {};
        if (slots[slot] == 0) Allocate(slot); // Existing objects after load/resync.
        return {slots[slot], slot};
    }
};

// Pure validation: safe on either thread, with no calls back into the engine.
bool ValidateWorldScene(const WorldScene& scene);
}
