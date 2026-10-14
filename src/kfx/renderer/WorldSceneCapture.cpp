// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#include "pre_inc.h"
#include "kfx/renderer/WorldSceneCapture.h"
#include "kfx/renderer/WorldScene.h"
#include "kfx/renderer/SceneAssetCapture.h"
#include "engine_render.h"
#include "engine_camera.h"
#include "engine_arrays.h"
#include "engine_textures.h"
#include "bflib_video.h"
#include "roomspace_prediction.h"
#include "thing_objects.h"
#include "thing_doors.h"
#include "thing_list.h"
#include "packets.h"
#include "player_data.h"
#include "local_camera.h"
#include "kjm_input.h"
#include "keeperfx.hpp"
#include "kfx/platform/IPlatform.h"
#include "kfx/platform/IWindowSystem.h"
#include "kfx/renderer/RendererManager.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cinttypes>
#include <cstring>
#include <string>
#include "post_inc.h"

extern float interpolate_time;

using namespace kfx::render;
namespace {
WorldScenePool pool;
SceneIncarnations things(THINGS_COUNT), lights(LIGHTS_COUNT);
std::shared_ptr<WorldScene> writing;
std::shared_ptr<const WorldScene> latest;
uint64_t epoch = 1, sequence = 0;
std::atomic<uint64_t> consumed_sequence{0}, consumed_errors{0};
std::vector<int32_t> cell_indices;
std::array<bool, THINGS_COUNT> captured_objects{};
SceneReferencePacket consumed_packet,last_button_packet;
uint64_t packet_receipt_serial=0;

// Timing is opt-in: normal play does not query the clock once per column.
bool diagnostics()
{
    static const bool enabled = std::getenv("KFX_SCENE_TEST_DIR") != nullptr;
    return enabled;
}
struct CaptureTimer {
    std::chrono::steady_clock::time_point start;
    CaptureTimer() { if (diagnostics()) start = std::chrono::steady_clock::now(); }
    ~CaptureTimer() {
        if (diagnostics() && writing) writing->capture_ms +=
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    }
};
ScenePosition position(const Coord3d& p) { return {p.x.val, p.y.val, p.z.val}; }

void capture_cell(int32_t x, int32_t y, bool in_view)
{
    auto& s = *writing;
    if (x < 0 || y < 0 || x >= s.map_width || y >= s.map_height) return;
    const uint32_t index = y * s.map_width + x;
    if (cell_indices[index] >= 0) {
        s.columns[cell_indices[index]].in_view |= in_view;
        return;
    }
    const Map* map = get_map_block_at(x, y);
    if (map_block_invalid(map)) return;
    SceneColumn c;
    c.cell = index; c.x = x; c.y = y; c.in_view = in_view;
    c.revealed = map_block_revealed(map, s.player);
    const Column* column = c.revealed ? get_map_column(map) : get_column(game.unrevealed_column_idx);
    if (column_invalid(column)) return;
    std::copy_n(column->cubes, c.cubes.size(), c.cubes.begin());
    c.solid_mask = column->solidmask;
    c.floor_texture = column->floor_texture;
    c.column_flags = column->bitfields;
    c.orientation = column->orient;
    if (c.revealed) {
        c.texture_variation = game.slab_ext_data[get_slab_number(subtile_slab(x), subtile_slab(y))] & 31;
        c.map_flags = get_local_dig_prediction_render_flags(x, y, map->flags);
        c.wibble = map->wibble_value;
        c.ceiling_subtiles = map->filled_subtiles;
        for (int dy = 0; dy < 2; ++dy) for (int dx = 0; dx < 2; ++dx) {
            // Match the legacy four-neighbor concealment at each light corner.
            const int cx = x + dx, cy = y + dy;
            if (cx > 0 && cy > 0 && cx < s.map_width && cy < s.map_height
                && subtile_revealed(cx, cy, s.player) && subtile_revealed(cx-1, cy, s.player)
                && subtile_revealed(cx, cy-1, s.player) && subtile_revealed(cx-1, cy-1, s.player))
                c.light[dy * 2 + dx] = game.lish.subtile_lightness[cy * s.map_width + cx];
        }
    }
    cell_indices[index] = static_cast<int32_t>(s.columns.size());
    s.columns.push_back(c);
}
}

extern "C" void WorldSceneReset(void)
{
    ++epoch;
    things.Reset(); lights.Reset();
    writing.reset(); latest.reset();
    WorldSceneCancelCapture();
    consumed_packet={};last_button_packet={};
}
extern "C" int WorldSceneReferenceInputEnabled(void)
{
    static const bool enabled=[] {
        const char* dir=std::getenv("KFX_SCENE_TEST_DIR");
        const char* reference=std::getenv("KFX_SCENE_REFERENCE");
        return dir && *dir && reference && std::strcmp(reference,"1")==0;
    }();
    return enabled && !(local_system_flags & GSF_NetworkActive);
}
extern "C" void WorldSceneReferencePacketConsumed(const Packet* packet,uint64_t original_epoch)
{
    if(!packet || !WorldSceneReferenceInputEnabled() || original_epoch!=epoch || packet_receipt_serial==UINT64_MAX)return;
    SceneReferencePacket value;value.valid=true;value.epoch=epoch;value.serial=++packet_receipt_serial;
    // Completion turn is observed after dispatch. Packet turn is the original
    // input-lag/replay turn carried by the packet; the two need not be equal.
    value.consumed_turn=get_gameturn();value.packet_turn=packet->turn;
    value.x=packet->pos_x;value.y=packet->pos_y;value.control=packet->control_flags;
    value.additional=packet->additional_packet_values;value.context=(packet->additional_packet_values&PCAdV_ContextMask)>>1;
    value.action=packet->action;value.parameters={packet->actn_par1,packet->actn_par2,packet->actn_par3,packet->actn_par4};
    consumed_packet=value;
    if(packet->control_flags&(PCtr_LBtnClick|PCtr_RBtnClick|PCtr_LBtnRelease|PCtr_RBtnRelease))last_button_packet=value;
}
extern "C" uint64_t WorldSceneEpoch(void) { return epoch; }
extern "C" void WorldSceneThingAllocated(uint32_t slot) { things.Allocate(slot); }
extern "C" uint64_t WorldSceneThingIncarnation(uint32_t slot) { return things.Peek(slot).incarnation; }
extern "C" void WorldSceneLightAllocated(uint32_t slot) { lights.Allocate(slot); }

extern "C" void WorldSceneBegin(const Camera* cam, const M33* rotation, int front_view, int short_walls)
{
    WorldSceneDiscardDrawTrace();
    writing.reset();
    writing = pool.Acquire();
    CaptureTimer timer;
    auto& s = *writing;
    s.epoch = epoch; s.sequence = ++sequence; s.game_turn = get_gameturn(); s.player = my_player_number;
    WorldSceneNearPlaneCaptureBegin(s.epoch, s.sequence);
    s.map_width = game.map_subtiles_x + 1; s.map_height = game.map_subtiles_y + 1;
    s.texture_generation = level_textures_generation;
    s.ambient_light = game.lish.global_ambient_light;
    s.lights_enabled = game.lish.light_enabled;
    s.interpolation = (game.operation_flags & GOF_Paused) ? 1.0f : interpolate_time;
    s.camera = {};
    auto& c = s.camera;
    c.view = front_view ? SceneView::Front : lens_mode ? SceneView::Possession : SceneView::Overhead;
    c.position = position(cam->mappos);
    c.yaw = cam->rotation_angle_x; c.pitch = cam->rotation_angle_y; c.roll = cam->rotation_angle_z;
    c.scaled_zoom = cam->zoom; c.horizontal_fov = cam->horizontal_fov;
    c.view_mode = cam->view_mode; c.lens_mode = front_view ? 0 : lens_mode; c.lens = front_view ? 0 : lens;
    c.viewport_x = lbDisplay.GraphicsWindowX; c.viewport_y = lbDisplay.GraphicsWindowY;
    c.viewport_w = lbDisplay.GraphicsWindowWidth; c.viewport_h = lbDisplay.GraphicsWindowHeight;
    c.pixel_size = pixel_size; c.view_distance = cells_away; c.short_walls = short_walls;
    c.has_rotation = rotation != nullptr;
    if (rotation) for (int row = 0; row < 3; ++row) for (int col = 0; col < 4; ++col)
        c.rotation[row * 4 + col] = rotation->r[row].v[col];
    cell_indices.assign(size_t(s.map_width) * s.map_height, -1);
    captured_objects.fill(false);
    s.cube_materials.resize(CUBE_ITEMS_MAX);
    for (int i = 0; i < CUBE_ITEMS_MAX; ++i) {
        const auto& cube = game.conf.cube_conf.cube_cfgstats[i];
        std::copy_n(cube.texture_id, 6, s.cube_materials[i].textures.begin());
        s.cube_materials[i].properties = cube.properties_flags;
    }
    s.top_cubes.assign(std::begin(game.top_cube), std::end(game.top_cube));
    s.ceiling_textures.assign(std::begin(floor_to_ceiling_map), std::end(floor_to_ceiling_map));
    static_assert(WIBBLE_TABLE_SIZE == 128);
    for (size_t i = 0; i < s.wibble.size(); ++i) {
        const auto& w = wibble_table[i];
        s.wibble[i] = {{w.offset_x, -w.offset_z, w.offset_y}, w.lightness_offset};
    }
    s.texture_dimension = block_dimension;
    s.texture_row_stride = block_dimension * block_count_per_row;
    s.texture_frames.resize(TEXTURE_VARIATIONS_COUNT * TEXTURE_BLOCKS_COUNT);
    const uintptr_t base = reinterpret_cast<uintptr_t>(block_mem);
    for (size_t i = 0; i < s.texture_frames.size(); ++i) {
        const uintptr_t ptr = reinterpret_cast<uintptr_t>(block_ptrs[i]);
        s.texture_frames[i] = ptr >= base && ptr - base < sizeof(block_mem) ? static_cast<int32_t>(ptr - base) : -1;
    }
}

extern "C" void WorldSceneCaptureSurfaceMotion(float lava_scroll, float water_scroll)
{
    if (!writing) return;
    writing->lava_scroll = lava_scroll;
    writing->water_scroll = water_scroll;
}

extern "C" void WorldSceneCaptureCell(int32_t x, int32_t y)
{
    if (!writing) return;
    CaptureTimer timer;
    capture_cell(x, y, true);
    capture_cell(x-1, y, false); capture_cell(x+1, y, false);
    capture_cell(x, y-1, false); capture_cell(x, y+1, false);
}

extern "C" void WorldSceneCaptureObject(const Thing* thing, const ThingInterpolateResult* interp)
{
    if (!writing || !thing_exists(thing) || thing->index >= captured_objects.size()
        || captured_objects[thing->index] || (thing->rendering_flags & TRF_Invisible)) return;
    // The map walk has already filtered unexplored cells. A hidden enemy trap
    // is filtered later by legacy sprite drawing; apply that rule here too.
    if (thing->class_id == TCls_Trap && thing->owner != writing->player
        && !thing->trap.revealed && get_trap_model_stats(thing->model)->hidden) return;
    // Room flags/prices are UI, not world objects. Their legacy commands remain.
    if (thing->draw_class != ODC_Default && thing->draw_class != ODC_SpinningKey) return;
    CaptureTimer timer;
    SceneObject o;
    o.id = things.Get(thing->index);
    o.position = position(interp->mappos); o.simulation_position = position(thing->mappos);
    o.floor_height = interp->floor_height;
    o.model = thing->model; o.animation = get_render_animation_sprite(thing->anim_sprite);
    o.size = thing->sprite_size; o.yaw = thing->move_angle_xy; o.pitch = thing->move_angle_z;
    o.owner = thing->owner; o.class_id = thing->class_id; o.draw_class = thing->draw_class;
    o.frame = thing->current_frame; o.render_flags = thing->rendering_flags; o.tint = thing->tint_colour;
    captured_objects[thing->index] = true;
    writing->objects.push_back(o);
}

std::shared_ptr<const WorldScene> WorldSceneSeal()
{
    if (!writing) return {};
    {
        CaptureTimer timer;
        // Copy active sources only when their origin is revealed. Hidden sources
        // cannot disclose positions through later shadow/lighting passes.
        for (int i = 1; i < LIGHTS_COUNT; ++i) {
            const auto& light = game.lish.lights[i];
            if ((light.flags & (LgtF_Allocated | LgtF_CanTurnOff)) != (LgtF_Allocated | LgtF_CanTurnOff)
                || !subtile_revealed(light.mappos.x.stl.num, light.mappos.y.stl.num, writing->player)) continue;
            SceneLight l;
            l.id = lights.Get(i); l.position = position(light.mappos);
            l.previous_position = position(light.previous_mappos);
            l.radius = light.radius; l.intensity = light.intensity; l.flags = light.flags;
            l.random_intensity = light.intensity_random; l.previous_random_intensity = light.previous_intensity_random;
            l.reset_interpolation = light.reset_interpolation;
            l.last_turn_moved = light.last_turn_moved; l.last_turn_randomized = light.last_turn_randomized;
            writing->lights.push_back(l);
        }
    }
    WorldSceneProcessCapture(*writing);
    if (!writing->reference_image.empty() && WorldSceneReferenceInputEnabled()) {
        auto& input=writing->reference_input;input.valid=true;
        input.native_mouse={GetMouseX(),GetMouseY()};
        input.native_size={RendererPhysicalWidth(),RendererPhysicalHeight()};
        const auto& camera=writing->camera;
        input.viewport={camera.viewport_x,camera.viewport_y,camera.viewport_w,camera.viewport_h};
        if(auto* platform=GetPlatform())if(auto* window=platform->GetWindowSystem()) {
            window->GetWindowSize(&input.window_size[0],&input.window_size[1]);
            window->GetDrawableSize(&input.drawable_size[0],&input.drawable_size[1]);
            window->GetPresentRect(&input.present_rect[0],&input.present_rect[1],&input.present_rect[2],&input.present_rect[3]);
        }
        input.map_point_valid=me_pointed_at && !map_block_invalid(me_pointed_at);
        input.top={top_pointed_at_x,top_pointed_at_y,top_pointed_at_frac_x,top_pointed_at_frac_y};
        input.block={block_pointed_at_x,block_pointed_at_y,pointed_at_frac_x,pointed_at_frac_y};
        input.floor={int32_t(floor_pointed_at_x),int32_t(floor_pointed_at_y)};
        const auto identity=[](const Thing* thing) {
            if(!thing_exists(thing))return SceneObjectId{};
            auto id=things.Peek(thing->index);id.slot=thing->index;return id;
        };
        input.local_hover=identity(thing_get(local_state.local_thing_under_hand));
        if(const auto* player=get_my_player();!player_invalid(player)) {
            input.view_type=get_local_view_type(player);input.work_state=player->work_state;
            input.player_hover=identity(thing_get(player->thing_under_hand));
        }
        input.consumed=consumed_packet;input.last_button=last_button_packet;
        // Snapshot only revealed slab centres in this exact frame. Completion
        // may occur after a door changes or a level resets, so it cannot read
        // these values from the live simulation at screenshot completion.
        for (const auto& column : writing->columns) {
            if (!column.in_view || !column.revealed) continue;
            const auto x = subtile_slab(column.x), y = subtile_slab(column.y);
            if (column.x != slab_subtile_center(x) || column.y != slab_subtile_center(y)) continue;
            auto* door = get_door_for_position(column.x, column.y);
            if (!thing_exists(door) || door_is_hidden_to_player(door, writing->player)) continue;
            SceneReferenceDoor d;
            d.id = things.Get(door->index);
            d.slab_x = x; d.slab_y = y; d.model = door->model;
            d.owner = door->owner; d.orientation = door->door.orientation;
            d.state = door->active_state; d.open_amount = door->door.closing_counter;
            d.locked = door->door.is_locked;
            writing->reference_doors.push_back(d);
        }
    }
    latest = writing;
    writing.reset();
    return latest;
}

extern "C" int WorldSceneCheck(void)
{
    const char* dir = std::getenv("KFX_SCENE_TEST_DIR");
    if (!dir || !*dir || !latest || (local_system_flags & GSF_NetworkActive)) return false;
    const auto& s = *latest;
    if (!ValidateWorldScene(s) || s.columns.empty() || consumed_errors.load() != 0) return false;
    const std::string path = std::string(dir) + "/scene.json";
    FILE* out = std::fopen(path.c_str(), "wb");
    if (!out) return false;
    size_t hidden = 0;
    for (const auto& c : s.columns) hidden += !c.revealed;
    std::fprintf(out, "{\"epoch\":%" PRIu64 ",\"sequence\":%" PRIu64 ",\"turn\":%d,\"view\":%d,"
        "\"columns\":%zu,\"hidden\":%zu,\"objects\":%zu,\"lights\":%zu,\"bytes\":%zu,\"captureMs\":%.6f,"
        "\"consumedSequence\":%" PRIu64 ",\"consumedErrors\":%" PRIu64 ",\"ids\":[",
        s.epoch, s.sequence, s.game_turn, int(s.camera.view), s.columns.size(), hidden,
        s.objects.size(), s.lights.size(), s.PayloadBytes(), s.capture_ms, consumed_sequence.load(), consumed_errors.load());
    for (size_t i = 0; i < s.objects.size(); ++i) {
        const auto& id = s.objects[i].id;
        std::fprintf(out, "%s[%u,%" PRIu64 "]", i ? "," : "", id.slot, id.incarnation);
    }
    std::fprintf(out, "]}\n");
    return std::fclose(out) == 0;
}

void WorldSceneValidateConsumed(const WorldScene& scene)
{
    if (!diagnostics()) return;
    if (!ValidateWorldScene(scene)) ++consumed_errors;
    consumed_sequence.store(scene.sequence);
}
