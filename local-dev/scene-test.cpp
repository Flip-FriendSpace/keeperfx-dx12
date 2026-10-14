// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
// Exercises the production capture against a small engine-data fixture. These
// lookup adapters provide the fixture; projection/rendering is tested in-game.
#include "kfx/renderer/WorldScene.h"
#include "kfx/renderer/WorldSceneCapture.h"
#include "engine_render.h"
#include "engine_camera.h"
#include "engine_arrays.h"
#include "engine_textures.h"
#include "bflib_video.h"
#include "roomspace_prediction.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <algorithm>

Game game{};
decltype(local_state) local_state{};
decltype(lbDisplay) lbDisplay{};
decltype(my_player_number) my_player_number = 0;
decltype(local_system_flags) local_system_flags = 0;
unsigned short pixel_size = 1;
long cells_away = 16, lens = 320;
unsigned char lens_mode = 0;
float interpolate_time = 0.5f;
unsigned char block_mem[TEXTURE_VARIATIONS_COUNT * TEXTURE_BLOCKS_STAT_COUNT * 32 * 32]{};
unsigned char* block_ptrs[TEXTURE_VARIATIONS_COUNT * TEXTURE_BLOCKS_COUNT]{};
unsigned short floor_to_ceiling_map[TEXTURE_BLOCKS_COUNT]{};
WibbleTable wibble_table[WIBBLE_TABLE_SIZE]{};
volatile unsigned int level_textures_generation = 1;
long block_dimension = 32, block_count_per_row = 8;
Map bad_map_block{};
static TrapConfigStats trap{};

GameTurn get_gameturn() { return 20; }
Map* get_map_block_at(MapSubtlCoord x, MapSubtlCoord y) {
    return x < 0 || y < 0 || x > game.map_subtiles_x || y > game.map_subtiles_y
        ? &bad_map_block : &game.map[y * (game.map_subtiles_x + 1) + x];
}
TbBool map_block_invalid(const Map* m) { return m == &bad_map_block; }
TbBool map_block_revealed(const Map* m, PlayerNumber p) { return m->revealed & (1 << p); }
TbBool subtile_revealed(MapSubtlCoord x, MapSubtlCoord y, PlayerNumber p) { return map_block_revealed(get_map_block_at(x, y), p); }
Column* get_column(long i) { return &game.columns_data[i]; }
Column* get_map_column(const Map* m) { return get_column(m->col_idx); }
TbBool column_invalid(const Column* c) { return c == &game.columns_data[0]; }
SlabCodedCoords get_slab_number(MapSlabCoord x, MapSlabCoord y) { return y * 2 + x; }
unsigned char get_local_dig_prediction_render_flags(MapSubtlCoord, MapSubtlCoord, unsigned char flags) { return flags; }
TbBool thing_exists(const Thing* t) { return t && (t->alloc_flags & TAlF_Exists); }
TrapConfigStats* get_trap_model_stats(int) { return &trap; }
unsigned short get_render_animation_sprite(unsigned short animation) { return animation; }

using namespace kfx::render;
// Asset adapter has its own bounded-decoder/codec suite and live capture tests.
void WorldSceneProcessCapture(WorldScene&) {}
void WorldSceneCancelCapture() {}
int main()
{
    game.map_subtiles_x = game.map_subtiles_y = 5;
    game.unrevealed_column_idx = 1;
    game.columns_data[1].solidmask = 31;
    game.columns_data[1].floor_texture = 3;
    game.columns_data[1].cubes[0] = 7;
    game.columns_data[2].solidmask = 1;
    game.columns_data[2].floor_texture = 4;
    game.columns_data[2].cubes[0] = 8;
    for (int i = 0; i < 36; ++i) {
        game.map[i].col_idx = 2;
        game.map[i].revealed = 1;
        game.map[i].wibble_value = 9;
        game.lish.subtile_lightness[i] = 200 + i;
    }
    game.map[2 * 6 + 3].revealed = 0;
    game.slab_ext_data[0] = 17;
    game.slab_ext_data[1] = 23;
    game.conf.cube_conf.cube_cfgstats[8].texture_id[4] = 600;
    game.lish.light_enabled = 1;
    auto& light = game.lish.lights[1];
    light.flags = LgtF_Allocated | LgtF_CanTurnOff;
    light.mappos.x.val = light.mappos.y.val = 512;
    game.lish.lights[2] = light; game.lish.lights[2].mappos.x.val = 3 * 256; // concealed
    game.lish.lights[3] = light; game.lish.lights[3].flags = LgtF_Allocated; // switched off
    block_ptrs[600] = block_mem + 32;
    wibble_table[64].offset_y = 7;
    lbDisplay.GraphicsWindowWidth = 3840; lbDisplay.GraphicsWindowHeight = 2160;
    Camera camera{}; camera.zoom = 6000; camera.mappos.x.val = 512; camera.mappos.y.val = 512;
    M33 rotation{}; rotation.r[1].v[2] = 1234;
    ThingInterpolateResult interp{}; interp.mappos.x.val = 500; interp.mappos.y.val = 512;
    auto& object = game.things_data[1];
    object.alloc_flags = TAlF_Exists; object.index = 1; object.draw_class = ODC_Default;
    object.class_id = TCls_Creature; object.mappos.x.val = 512;
    object.anim_sprite = 52; object.current_frame = 3;
    auto before = std::make_unique<Game>();
    std::memcpy(before.get(), &game, sizeof(game));
    WorldSceneBegin(&camera, &rotation, 0, 0);
    WorldSceneCaptureSurfaceMotion(3.5f, 8.5f);
    WorldSceneCaptureCell(2, 2); WorldSceneCaptureCell(2, 2); // deduplicate
    WorldSceneCaptureCell(-1, -1); // bounds
    WorldSceneCaptureObject(&object, &interp); WorldSceneCaptureObject(&object, &interp);
    auto first = WorldSceneSeal();
    assert(std::memcmp(before.get(), &game, sizeof(game)) == 0); // capture is read-only
    assert(ValidateWorldScene(*first));
    assert(first->columns.size() == 5 && first->objects.size() == 1 && first->lights.size() == 1);
    assert(first->camera.rotation[6] == 1234 && first->camera.viewport_w == 3840);
    assert(first->objects[0].position.x == 500 && first->objects[0].simulation_position.x == 512);
    const auto original_id = first->objects[0].id;
    const auto hidden = std::find_if(first->columns.begin(), first->columns.end(), [](auto& c) { return !c.revealed; });
    assert(hidden != first->columns.end() && hidden->cubes[0] == 7 && hidden->solid_mask == 31);
    assert(hidden->texture_variation == 0 && hidden->wibble == 0);
    assert(first->texture_frames[600] == 32);
    assert(first->wibble[64].offset.z == 7 && first->lava_scroll == 3.5f && first->water_scroll == 8.5f);
    // Reuse the slot in the SAME turn; retain the old snapshot through reset.
    WorldSceneThingAllocated(1); object.current_frame = 4; block_ptrs[600] = block_mem + 64;
    WorldSceneBegin(&camera, nullptr, 1, 0); WorldSceneCaptureCell(2, 2);
    WorldSceneCaptureObject(&object, &interp);
    auto next = WorldSceneSeal();
    assert(next->objects[0].id != original_id && next->camera.view == SceneView::Front);
    assert(next->texture_frames[600] == 64 && first->texture_frames[600] == 32);
    assert(first->objects[0].frame == 3 && next->objects[0].frame == 4);
    // Hidden trap and invisible object never enter the world snapshot.
    object.class_id = TCls_Trap; object.owner = 1; trap.hidden = 1;
    WorldSceneBegin(&camera, nullptr, 0, 0); WorldSceneCaptureObject(&object, &interp);
    assert(WorldSceneSeal()->objects.empty());
    object.class_id = TCls_Creature; object.rendering_flags = TRF_Invisible;
    WorldSceneBegin(&camera, nullptr, 0, 0); WorldSceneCaptureObject(&object, &interp);
    assert(WorldSceneSeal()->objects.empty());
    object.rendering_flags = 0;
    WorldSceneReset(); lens_mode = 1;
    WorldSceneBegin(&camera, &rotation, 0, 0); WorldSceneCaptureObject(&object, &interp);
    auto loaded = WorldSceneSeal();
    assert(loaded->epoch != first->epoch && loaded->camera.view == SceneView::Possession);
    assert(first->objects[0].id == original_id && first->texture_frames[600] == 32);
    // Frame pool never reuses storage while a consumer holds it.
    WorldScenePool pool;
    auto a = pool.Acquire(), b = pool.Acquire(), c = pool.Acquire(), d = pool.Acquire();
    assert(a.get() != b.get() && b.get() != c.get() && c.get() != d.get());
    auto reusable = a.get(); a->reference_image = "diagnostic-only.png"; a.reset();
    auto reused = pool.Acquire(); assert(reused.get() == reusable && reused->reference_image.empty());
    // Validation rejects dangerous geometry and concealment corruption.
    auto invalid = *first; invalid.columns[0].cell = 0xffffffff;
    assert(!ValidateWorldScene(invalid));
    invalid = *first; invalid.columns.push_back(invalid.columns[0]);
    assert(!ValidateWorldScene(invalid));
    invalid = *first; invalid.columns[0].cubes[0] = 65535;
    assert(!ValidateWorldScene(invalid));
    invalid = *first; invalid.objects.push_back(invalid.objects[0]);
    assert(!ValidateWorldScene(invalid));
    std::puts("Scene capture: purity, concealment, animation, identities, lifetime and three camera modes passed");
}
