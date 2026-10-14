// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#include "pre_inc.h"
#include "kfx/renderer/SceneAssetCapture.h"
#include "engine_render.h"
#include "engine_buckets.h"
#include "engine_textures.h"
#include "creature_graphics.h"
#include "front_simple.h"
#include "vidmode.h"
#include "bflib_video.h"
#include "thing_data.h"
#include "thing_list.h"
#include "thing_doors.h"
#include "slab_data.h"
#include "game_legacy.h"
#include "config_settings.h"
#include "config_terrain.h"
#include "kfx/lense/LensManager.h"
#include "kfx/renderer/RendererSettings.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>
#include <cstring>
#include <limits>
#include <atomic>
#include <mutex>
#include "post_inc.h"

using namespace kfx::render;
static_assert(SceneOrdinarySpriteKind == QK_JontySprite);
static_assert(SceneSpinningKeySpriteKind == QK_JontyISOSprite && ODC_Default == 2 && ODC_SpinningKey == 6);
static_assert(PVM_IsoWibbleView == 2 && PVM_IsoStraightView == 8 && PVM_FrontView == 5);
static_assert(QK_PolygonStandard == 0 && QK_PolygonNearFP == 1 && QK_TextureQuad == 6);
static_assert(VM_QuadTextured == 5 && VM_SolidColor == 7);
static_assert(QK_CreatureShadow == 3 && VM_SpriteTranslucent == 10);
static_assert(sizeof(decltype(Thing::index)) == sizeof(decltype(BucketKindCreatureShadow::caster_slot)));
struct WorldSceneOutlineCapture {
    std::shared_ptr<SceneCapture> capture;
    std::filesystem::path path;
    std::atomic<bool> cancelled{false};
    std::mutex publication;
    bool begun = false, failed = false, finished = false;
    char error[256] = {};
    std::map<std::array<uint8_t, 1024>, uint32_t> colour_rows;
    std::map<std::array<uint8_t, 1024>, uint32_t> tiles;
    std::map<std::tuple<uint16_t, uint16_t, std::vector<uint8_t>>, uint32_t> shadow_sources;
    size_t world_asset_bytes = 0;
    size_t overlay_asset_bytes = 0;
};
namespace {
bool pending = false;
bool pending_receipt = false;
struct NearPlaneProof {
    bool active = false;
    uint64_t epoch = 0, sequence = 0, intersections = 0;
    EngineReferenceSelectorProof selector = {};
    EngineReferenceHoverProof hover = {};
    EngineReferenceRoomProof room = {};
} near_plane_proof;
NearPlaneProof trace_reference_proof;
bool trace_reference_metadata = false;
void Require(bool ok, const char* error) { if (!ok) throw std::runtime_error(error); }
std::shared_ptr<SceneCapture> tracing;
std::filesystem::path trace_path;
std::map<uint32_t, const SceneObject*> trace_objects;
std::map<uint32_t, uint32_t> trace_sprites;
std::set<uint32_t> trace_textures;
std::set<std::pair<uint32_t, size_t>> trace_verified_textures;
const SceneObject* trace_object = nullptr;
struct ShadowCapture {
    bool begun = false;
    const SceneObject* caster = nullptr;
    const unsigned char* data = nullptr;
    uint32_t entry = 0, mask = 0;
    unsigned triangles = 0;
} trace_shadow;
size_t trace_pixel_bytes = 0;
bool trace_started = false, trace_ended = false, trace_failed = false;
char trace_error[256] = {};
uint32_t trace_outline_sprite = SceneOutlineNoDraw;
WorldSceneOutlineSession pending_outline;
std::weak_ptr<WorldSceneOutlineCapture> outstanding_outline;
// Local proof only. Pointers identify the actual initialized target and never
// enter owned captures. A proof is consumed by one world walk or frame end.
struct SoftwareTargetProof {
    const unsigned char* data = nullptr;
    SceneIndexedTarget target;
} software_target;

void FinishShadowEntry()
{
    if (tracing->format_version < 5 || tracing->trace.backend != 0 || tracing->trace.entries.empty()
        || tracing->trace.entries.back().kind != QK_CreatureShadow) return;
    Require(trace_shadow.begun, "Shadow bucket did not resolve its caster");
    if (trace_shadow.caster)
        Require(trace_shadow.data && trace_shadow.triangles == 2, "Incomplete shadow mask or triangle pair");
}

template<class F> void TraceGuard(F&& operation)
{
    if (!tracing || trace_failed) return;
    try { operation(); }
    catch (const std::exception& e) {
        trace_failed = true;
        std::snprintf(trace_error, sizeof(trace_error), "%s", e.what());
    } catch (...) {
        trace_failed = true;
        std::snprintf(trace_error, sizeof(trace_error), "Unknown draw trace failure");
    }
}

void SaveReferenceMetadata(const WorldScene& scene, const std::filesystem::path& path, const NearPlaneProof& proof)
{
    std::map<unsigned, size_t> slabs, classes, owners, draw_flags, draw_classes, object_models, variations;
    std::set<unsigned> counted_slabs, visible_slab_centres;
    size_t hidden = 0, visible = 0;
    size_t marked_land = 0, marked_valuable = 0;
    for (const auto& column : scene.columns) {
        if (!column.in_view) continue;
        if (!column.revealed) { ++hidden; continue; }
        ++visible;
        if (column.map_flags & SlbAtFlg_Unexplored) ++marked_land;
        if (column.map_flags & SlbAtFlg_TaggedValuable) ++marked_valuable;
        ++variations[column.texture_variation];
        const auto x = column.x / STL_PER_SLB, y = column.y / STL_PER_SLB;
        if (column.x == slab_subtile_center(x) && column.y == slab_subtile_center(y))
            visible_slab_centres.insert(get_slab_number(x, y));
        if (counted_slabs.insert(get_slab_number(x, y)).second) {
            const auto* slab = get_slabmap_block(x, y);
            if (!slabmap_block_invalid(slab)) ++slabs[slab->kind];
        }
    }
    for (const auto& object : scene.objects) {
        ++classes[object.class_id]; ++owners[object.owner]; ++draw_flags[object.render_flags];
        ++draw_classes[object.draw_class];
        if (object.class_id == TCls_Object) ++object_models[object.model];
    }
    const auto temporary = path.string() + ".tmp";
    std::ofstream out(temporary, std::ios::binary);
    out << "{\"epoch\":" << scene.epoch << ",\"sequence\":" << scene.sequence
        << ",\"turn\":" << scene.game_turn << ",\"view\":" << unsigned(scene.camera.view)
        << ",\"lens\":" << scene.camera.lens << ",\"lensMode\":" << unsigned(scene.camera.lens_mode)
        << ",\"eyeLens\":" << LensManager::GetInstance()->GetAppliedLens()
        << ",\"viewport\":[" << scene.camera.viewport_x << ',' << scene.camera.viewport_y
        << ',' << scene.camera.viewport_w << ',' << scene.camera.viewport_h << ']'
        << ",\"camera\":[" << scene.camera.position.x << ',' << scene.camera.position.y << ',' << scene.camera.position.z << ']'
        << ",\"angles\":[" << scene.camera.yaw << ',' << scene.camera.pitch << ',' << scene.camera.roll << ']'
        << ",\"scaledZoom\":" << scene.camera.scaled_zoom << ",\"fov\":" << scene.camera.horizontal_fov
        << ",\"pixelSize\":" << scene.camera.pixel_size << ",\"viewDistance\":" << scene.camera.view_distance
        << ",\"shortWalls\":" << unsigned(scene.camera.short_walls)
        << ",\"nearPlaneIntersections\":" << proof.intersections
        << ",\"markedLandColumns\":" << marked_land << ",\"markedValuableColumns\":" << marked_valuable
        << ",\"selector\":{\"active\":" << proof.selector.active
        << ",\"slabX\":" << proof.selector.slab_x << ",\"slabY\":" << proof.selector.slab_y
        << ",\"width\":" << proof.selector.width << ",\"height\":" << proof.selector.height
        << ",\"valid\":" << proof.selector.valid_slabs << ",\"invalid\":" << proof.selector.invalid_slabs
        << ",\"colour\":" << proof.selector.colour << '}'
        << ",\"selectorGuard\":{\"drawing\":" << proof.selector.drawing
        << ",\"displayed\":" << proof.selector.displayed << ",\"minimapDrag\":" << proof.selector.minimap_drag
        << ",\"observer\":" << proof.selector.observer << ",\"guiBusy\":" << proof.selector.gui_busy
        << ",\"guiOverride\":" << proof.selector.gui_override << '}'
        << ",\"hiddenColumns\":" << hidden << ",\"revealedColumns\":" << visible
        << ",\"objects\":" << scene.objects.size() << ",\"lights\":" << scene.lights.size();
    if (proof.room.active) {
        const auto& room = proof.room;
        out << ",\"roomPreview\":{\"active\":" << room.active << ",\"roomKind\":" << room.room_kind
            << ",\"busy\":" << room.busy << ",\"valid\":" << room.valid_slabs << ",\"invalid\":" << room.invalid_slabs
            << ",\"colour\":" << room.colour << ",\"hiddenSlabs\":" << room.hidden_slabs
            << ",\"processCalls\":" << room.process_calls << ",\"busyBranches\":" << room.busy_branches
            << ",\"basicCalls\":" << room.basic_calls << ",\"fancyCalls\":" << room.fancy_calls
            << ",\"emittedLines\":" << room.emitted_lines << '}';
    }
    if (proof.hover.active) {
        const auto& hover = proof.hover;
        out << ",\"hover\":{\"active\":" << hover.active << ",\"targetSlot\":" << hover.target_slot
            << ",\"targetIncarnation\":\"" << hover.target_incarnation << "\",\"targetCreationTurn\":" << hover.target_creation_turn
            << ",\"owner\":" << hover.owner << ",\"model\":" << hover.model
            << ",\"hovered\":" << hover.hovered << ",\"blinkOn\":" << hover.blink_on
            << ",\"actualHit\":" << hover.actual_hit << ",\"lastTurnDamaged\":" << hover.last_turn_damaged
            << ",\"renderFlags\":" << hover.render_flags << ",\"considered\":" << hover.considered
            << ",\"white\":" << hover.white << ",\"red\":" << hover.red << ",\"normal\":" << hover.normal;
        const auto table = [&](const char* name, const unsigned char* remap) {
            static constexpr char hex[] = "0123456789abcdef";
            out << ",\"" << name << "\":\"";
            for (size_t i = 0; i < 256; ++i) out << hex[remap[i] >> 4] << hex[remap[i] & 15];
            out << '\"';
        };
        table("whiteRemap", hover.white_remap); table("redRemap", hover.red_remap);
        out << '}';
    }
    const auto counts = [&](const char* name, const auto& values) {
        out << ",\"" << name << "\":{";
        bool comma = false;
        for (const auto& [key, value] : values) {
            out << (comma ? "," : "") << '\"' << key << "\":" << value; comma = true;
        }
        out << '}';
    };
    counts("slabKinds", slabs); counts("objectClasses", classes);
    counts("objectOwners", owners); counts("renderFlags", draw_flags);
    counts("drawClasses", draw_classes); counts("objectModels", object_models);
    counts("textureVariations", variations);
    out << ",\"settings\":{\"lighting\":" << g_renderer_settings.lighting_mode
        << ",\"shadow\":" << g_renderer_settings.shadow_type << ",\"shadowCount\":" << unsigned(settings.video_shadows)
        << ",\"outline\":" << g_renderer_settings.creature_outline_mode
        << ",\"darkness\":" << g_renderer_settings.darkness_mode << ",\"tileFilter\":" << g_renderer_settings.tile_filter
        << ",\"lensColour\":" << g_renderer_settings.lens_color_mode << ",\"glowBlend\":" << g_renderer_settings.glow_blend_mode << '}';
    out << ",\"objectLocations\":[";
    bool comma = false;
    for (const auto& object : scene.objects) {
        Coord3d pos{};
        pos.x.val = object.position.x; pos.y.val = object.position.y; pos.z.val = object.position.z;
        int32_t x = 0, y = 0;
        const bool projected = map_to_screen(&pos, &x, &y);
        out << (comma ? "," : "") << "{\"slot\":" << object.id.slot << ",\"class\":" << unsigned(object.class_id)
            << ",\"model\":" << object.model << ",\"drawClass\":" << unsigned(object.draw_class)
            << ",\"owner\":" << unsigned(object.owner)
            << ",\"position\":[" << pos.x.val << ',' << pos.y.val << ',' << pos.z.val << ']'
            << ",\"screen\":[" << x << ',' << y << "],\"projected\":" << (projected ? "true" : "false") << '}';
        comma = true;
    }
    out << ']';
    // Doors render as animated terrain rather than ordinary scene sprites.
    // Read only revealed, in-view slabs for diagnostic phase assertions.
    out << ",\"doorStates\":[";
    comma = false;
    for (const auto slab_number : visible_slab_centres) {
        const auto x = slb_num_decode_x(slab_number), y = slb_num_decode_y(slab_number);
        auto* door = get_door_for_position(slab_subtile_center(x), slab_subtile_center(y));
        if (!thing_exists(door) || door_is_hidden_to_player(door, scene.player)) continue;
        int32_t screen_x = 0, screen_y = 0;
        const bool projected = map_to_screen(&door->mappos, &screen_x, &screen_y);
        out << (comma ? "," : "") << "{\"slab\":[" << x << ',' << y << ']'
            << ",\"model\":" << door->model << ",\"owner\":" << unsigned(door->owner)
            << ",\"orientation\":" << unsigned(door->door.orientation)
            << ",\"state\":" << unsigned(door->active_state)
            << ",\"openAmount\":" << door->door.closing_counter
            << ",\"locked\":" << unsigned(door->door.is_locked)
            << ",\"screen\":[" << screen_x << ',' << screen_y << ']'
            << ",\"projected\":" << (projected ? "true" : "false") << '}';
        comma = true;
    }
    out << ']';
    out << "}\n";
    out.close();
    Require(bool(out), "Reference metadata write failed");
    std::filesystem::rename(temporary, path);
}
}

std::shared_ptr<SceneCapture> CaptureWorldSceneAssets(const WorldScene& scene)
{
    Require(level_textures_ready && scene.texture_generation == level_textures_generation,
        "Texture generation changed during capture");
    Require(engine_palette != nullptr && fade_tables_ready, "Palette/fade tables unavailable");
    auto capture = std::make_shared<SceneCapture>();
    capture->scene = scene;
    auto& a = capture->assets;
    std::copy_n(engine_palette, a.base_palette.size(), a.base_palette.begin());
    Require(LbPaletteGet(a.display_palette.data()) == Lb_SUCCESS, "Display palette unavailable");
    std::copy_n(pixmap.fade_tables, a.fade.size(), a.fade.begin());
    std::copy_n(pixmap.ghost, a.ghost.size(), a.ghost.begin());
    a.texture_pixels.assign(std::begin(block_mem), std::end(block_mem));
    std::map<uint32_t, uint32_t> sprites;
    std::set<std::pair<uint16_t, uint8_t>> animations;
    size_t pixel_bytes = 0;
    for (const auto& o : scene.objects) {
        auto* thing = thing_get(o.id.slot);
        Require(thing_exists(thing) && thing->model == o.model && thing->class_id == o.class_id,
            "Object changed during asset capture");
        const auto* base = keepersprite_array(o.animation);
        Require(base != nullptr, "Sprite presentation metadata unavailable");
        unsigned lightness = (o.render_flags & TRF_Unshaded)
            ? (scene.camera.view == SceneView::Front ? 8192 : game.conf.rules[o.owner].gameplay.thing_minimum_illumination << 8)
            : get_thing_shade(thing);
        const int offset = scene.camera.view == SceneView::Front ? 0 : 256 * ((thing->flags & TAF_ROTATED_MASK) >> TAF_ROTATED_SHIFT);
        a.presentation.push_back({o.id, static_cast<uint16_t>(std::min(lightness, 16639u)), static_cast<int16_t>(offset), base->Rotable != 0});
    }
    auto reference = [&](ScenePosition p) {
        if (a.projection_reference.size() >= 4096) return;
        Coord3d pos{}; pos.x.val = p.x; pos.y.val = p.y; pos.z.val = p.z;
        SceneProjectionReference ref; ref.position = p;
        ref.visible = map_to_screen(&pos, &ref.screen_x, &ref.screen_y);
        a.projection_reference.push_back(ref);
    };
    for (const auto& col : scene.columns) if (col.in_view && col.revealed && a.projection_reference.size() < 1024) {
        reference({col.x * 256, col.y * 256, 0});
        reference({col.x * 256, col.y * 256, 768});
    }
    for (const auto& o : scene.objects) reference(o.position);
    for (const auto& o : scene.objects) {
        if (!animations.emplace(o.animation, o.frame).second) continue;
        const auto* base = keepersprite_array(o.animation);
        Require(base && base->FramesCount && (base->Rotable == 0 || base->Rotable == 2), "Invalid sprite animation");
        const auto start = keepersprite_index(o.animation);
        const size_t count = base->FramesCount * (base->Rotable ? 5 : 1);
        if (start >= KEEPERSPRITE_ADD_OFFSET) {
            Require(start - KEEPERSPRITE_ADD_OFFSET + count <= KEEPERSPRITE_ADD_NUM, "Custom animation outside sprite table");
        } else {
            Require(start + count < creature_table_length && start + count <= KEEPSPRITE_LENGTH,
                "Animation outside sprite table");
            for (size_t i = start; i < start + count; ++i)
                Require(creature_table[i + 1].DataOffset >= creature_table[i].DataOffset
                    && creature_table[i + 1].DataOffset - creature_table[i].DataOffset <= 4 * 1024 * 1024,
                    "Invalid sprite storage extent");
        }
        // All eight headings for the object's current animation frame. These
        // allow camera rotation of a frozen scene without reading the game.
        for (uint8_t direction = 0; direction < 8; ++direction) {
            int32_t id = -1; const uint8_t* data = nullptr; int w = 0, h = 0;
            const KeeperSprite* meta = nullptr;
            Require(resolve_keepersprite_draw_data(o.animation, direction * 256, o.frame,
                &id, &data, &w, &h, &meta), "Sprite frame unavailable");
            Require(id >= 0 && meta && w >= 0 && h >= 0 && w <= 1024 && h <= 1024, "Invalid sprite dimensions");
            auto found = sprites.find(id);
            uint32_t index;
            if (found == sprites.end()) {
                size_t length = 0;
                if (id >= KEEPERSPRITE_ADD_OFFSET) {
                    Require(id - KEEPERSPRITE_ADD_OFFSET < KEEPERSPRITE_ADD_NUM, "Custom sprite outside table");
                    length = keepersprite_add_bytes[id - KEEPERSPRITE_ADD_OFFSET];
                } else {
                    Require(size_t(id + 1) < creature_table_length, "Sprite outside table");
                    length = creature_table[id + 1].DataOffset - creature_table[id].DataOffset;
                }
                Require((length || !w || !h) && length <= 4 * 1024 * 1024, "Sprite storage bound unavailable");
                SceneSprite sprite;
                sprite.id = id; sprite.width = w; sprite.height = h;
                sprite.frame_width = meta->FrameWidth; sprite.frame_height = meta->FrameHeight;
                sprite.frame_x = meta->FrameOffsW; sprite.frame_y = meta->FrameOffsH;
                sprite.offset_x = meta->offset_x; sprite.offset_y = meta->offset_y;
                sprite.shadow_offset = meta->shadow_offset; sprite.flags = meta->frame_flags;
                pixel_bytes += size_t(w) * h * 2;
                Require(pixel_bytes <= 64 * 1024 * 1024, "Capture sprite budget exceeded");
                // Some effect frames are intentionally empty; preserve their
                // metadata without reading an otherwise unused RLE stream.
                if (w && h) Require(DecodeSceneSprite({data, length}, w, h, sprite.pixels), "Malformed sprite RLE");
                index = static_cast<uint32_t>(a.sprites.size());
                sprites.emplace(id, index); a.sprites.push_back(std::move(sprite));
            } else index = found->second;
            a.bindings.push_back({o.animation, o.frame, direction, index, base->Rotable == 2 && direction >= 5,
                base->FrameWidth, base->FrameHeight, base->offset_x, base->offset_y});
        }
    }
    Require(ValidateSceneCapture(*capture), "Captured scene assets failed validation");
    return capture;
}

extern "C" int WorldSceneRequestCapture(void)
{
    const char* directory = std::getenv("KFX_SCENE_TEST_DIR");
    if (!directory || !*directory || pending || tracing || !outstanding_outline.expired()
        || (local_system_flags & GSF_NetworkActive)) return false;
    pending_receipt = false;
    pending = true;
    return true;
}
extern "C" int WorldSceneRequestReceipt(void)
{
    const char* reference = std::getenv("KFX_SCENE_REFERENCE");
    if (!reference || std::strcmp(reference, "1") != 0 || !WorldSceneRequestCapture()) return false;
    pending_receipt = true;
    return true;
}
extern "C" void WorldSceneSoftwareTargetEnd(void)
{
    software_target = {};
}
extern "C" void WorldSceneSoftwareClear(const unsigned char* data, int width, int height, int pitch, unsigned char colour)
{
    software_target = {};
    if (!data || width <= 0 || height <= 0 || width > 16384 || height > 16384
        || pitch < width || pitch > 16384 || uint64_t(pitch) * unsigned(height) > 64 * 1024 * 1024) return;
    software_target.data = data;
    auto& t = software_target.target;
    t.kind = SceneIndexedTargetKind::Screen;
    t.clear_index = colour;
    t.width = uint32_t(width); t.height = uint32_t(height); t.pitch = uint32_t(pitch);
}
extern "C" void WorldSceneSoftwareLensBegin(const unsigned char* data, int width, int height, unsigned int lens_id)
{
    WorldSceneSoftwareClear(data, width, height, width, 0);
    if (!software_target.data || lens_id == 0 || lens_id > 255) {
        software_target = {};
        return;
    }
    software_target.target.kind = SceneIndexedTargetKind::LensSource;
    software_target.target.lens_id = uint16_t(lens_id);
}
extern "C" void WorldSceneNearPlaneCaptureBegin(uint64_t epoch, uint64_t sequence)
{
    near_plane_proof = {pending, epoch, sequence, 0, {}, {}, {}};
    if (pending) near_plane_proof.selector = get_engine_reference_selector_proof();
}

extern "C" void WorldSceneNearPlaneIntersection(void)
{
    if (near_plane_proof.active) ++near_plane_proof.intersections;
}

void WorldSceneDiscardDrawTrace()
{
    near_plane_proof = {};
    trace_reference_proof = {};
    trace_reference_metadata = false;
    trace_object = nullptr; trace_objects.clear(); trace_sprites.clear();
    trace_shadow = {};
    trace_textures.clear(); trace_verified_textures.clear();
    tracing.reset(); trace_path.clear(); trace_pixel_bytes = 0;
    trace_started = trace_ended = trace_failed = false; trace_error[0] = 0;
    trace_outline_sprite = SceneOutlineNoDraw;
}
void WorldSceneCancelCapture()
{
    clear_engine_reference_selector();
    clear_engine_reference_hover();
    WorldSceneSoftwareTargetEnd();
    pending = false;
    pending_receipt = false;
    if (auto session = outstanding_outline.lock()) {
        // Serialize cancellation with final file publication. If publication
        // already holds this lock, that completed frame precedes cancellation.
        std::lock_guard lock(session->publication);
        session->cancelled.store(true);
    }
    pending_outline.reset();
    WorldSceneDiscardDrawTrace();
}
void WorldSceneProcessCapture(WorldScene& scene)
{
    const auto clipping = near_plane_proof;
    near_plane_proof = {};
    if (!pending) return;
    const bool receipt = pending_receipt;
    pending = false;
    pending_receipt = false;
    const char* directory = std::getenv("KFX_SCENE_TEST_DIR");
    if (!directory || !*directory || (local_system_flags & GSF_NetworkActive)) return;
    try {
        if (receipt) {
            const char* reference = std::getenv("KFX_SCENE_REFERENCE");
            if (!reference || std::strcmp(reference, "1") != 0) return;
            // The renderer retains this exact immutable scene until its accepted
            // screenshot completes. No GL/software asset trace is started.
            scene.reference_image = (std::filesystem::path(directory)
                / ("temporal-" + std::to_string(scene.epoch) + "-" + std::to_string(scene.sequence) + ".png")).string();
            return;
        }
        Require(clipping.active && clipping.epoch == scene.epoch && clipping.sequence == scene.sequence,
            "Near-plane diagnostic does not belong to sealed scene");
        tracing = CaptureWorldSceneAssets(scene);
        tracing->format_version = 3;
        for (auto offset : tracing->scene.texture_frames)
            if (offset >= 0) trace_textures.insert(uint32_t(offset));
        for (const auto& object : tracing->scene.objects) trace_objects.emplace(object.id.slot, &object);
        for (uint32_t i = 0; i < tracing->assets.sprites.size(); ++i) {
            trace_sprites.emplace(tracing->assets.sprites[i].id, i);
            trace_pixel_bytes += tracing->assets.sprites[i].pixels.size();
        }
        const auto name = "scene-" + std::to_string(scene.epoch) + "-" + std::to_string(scene.sequence) + ".kfxscene";
        trace_path = std::filesystem::path(directory) / name;
        if (const char* reference = std::getenv("KFX_SCENE_REFERENCE"); reference && std::string(reference) == "1") {
            trace_reference_proof = clipping;
            trace_reference_metadata = true;
            scene.reference_image = (std::filesystem::path(directory) / (name + ".png")).string();
        }
    } catch (const std::exception& e) {
        ERRORLOG("Scene capture failed: %s", e.what());
        WorldSceneDiscardDrawTrace();
        // No partial final file is published; runners time out and retain logs.
    }
}

void WorldSceneRejectPendingCapture(const char* reason)
{
    if (pending || tracing) {
        ERRORLOG("Scene capture rejected: %s", reason);
        pending = false;
        pending_receipt = false;
        WorldSceneDiscardDrawTrace();
    }
}

extern "C" void WorldSceneTraceBegin(int backend)
{
    const auto target_proof = software_target;
    WorldSceneSoftwareTargetEnd();
    TraceGuard([&] {
        Require(!trace_started && !trace_ended && (backend == 0 || backend == 1), "Invalid draw trace begin");
        Require(render_alpha && render_ghost, "Draw lookup tables unavailable");
        Require(std::equal(tracing->assets.ghost.begin(), tracing->assets.ghost.end(), render_ghost), "Ghost table changed after scene snapshot");
        Require(render_fade_tables && std::equal(tracing->assets.fade.begin(), tracing->assets.fade.end(), render_fade_tables),
            "Fade table changed after scene snapshot");
        static_assert(sizeof(alpha_sprite_table) == 65536);
        tracing->assets.additive.assign(render_alpha, render_alpha + 65536);
        tracing->trace.backend = uint8_t(backend);
        // Only software calls the final C terrain routines. GL's projected
        // triangle batches have different subdivision and shading semantics.
        tracing->format_version = backend == 0 ? 8 : 11;
        if (backend == 0) {
            Require(target_proof.data && target_proof.data == lbDisplay.WScreen,
                "Software world target has no matching successful clear");
            auto target = target_proof.target;
            Require(target.pitch == uint32_t(lbDisplay.GraphicsScreenWidth)
                && target.height == uint32_t(lbDisplay.GraphicsScreenHeight), "Software target dimensions changed after clear");
            target.window_x = lbDisplay.GraphicsWindowX; target.window_y = lbDisplay.GraphicsWindowY;
            target.window_w = lbDisplay.GraphicsWindowWidth; target.window_h = lbDisplay.GraphicsWindowHeight;
            const auto& camera = tracing->scene.camera;
            Require(target.window_x >= 0 && target.window_y >= 0 && target.window_w > 0 && target.window_h > 0
                && int64_t(target.window_x) + target.window_w <= target.width
                && int64_t(target.window_y) + target.window_h <= target.height
                && target.window_x == camera.viewport_x && target.window_y == camera.viewport_y
                && target.window_w == camera.viewport_w && target.window_h == camera.viewport_h,
                "Software target window differs from captured camera or initialized extent");
            Require(lbDisplay.GraphicsWindowPtr == target_proof.data + size_t(target.window_y) * target.pitch + target.window_x,
                "Software target window pointer differs from cleared source");
            tracing->target = target;
        }
        trace_started = true; trace_object = nullptr;
    });
}
extern "C" void WorldSceneTraceBucket(int32_t bucket, unsigned int kind)
{
    trace_object = nullptr;
    trace_outline_sprite = SceneOutlineNoDraw;
    TraceGuard([&] {
        Require(trace_started && !trace_ended, "Bucket outside active draw trace");
        FinishShadowEntry();
        trace_shadow = {};
        Require(bucket >= 0 && bucket < BUCKETS_COUNT && kind <= 255, "Invalid draw bucket");
        auto& entries = tracing->trace.entries;
        Require(entries.size() < 262144 && (entries.empty() || bucket <= entries.back().bucket), "Draw bucket order or budget invalid");
        entries.push_back({bucket, uint8_t(kind)});
    });
}
extern "C" void WorldSceneTraceObject(uint32_t slot)
{
    trace_object = nullptr;
    TraceGuard([&] {
        Require(trace_started && !trace_ended && !tracing->trace.entries.empty(), "Object outside draw bucket");
        const auto found = trace_objects.find(slot);
        // Identity must already belong to the visibility-filtered owned scene.
        // UI and hidden entities cannot acquire identity through this trace.
        if (found == trace_objects.end()) return;
        const auto kind = tracing->trace.entries.back().kind;
        const bool ordinary = found->second->draw_class == ODC_Default && kind == QK_JontySprite;
        const bool spinning_key = tracing->format_version >= 6
            && found->second->draw_class == ODC_SpinningKey && kind == QK_JontyISOSprite;
        if (spinning_key) {
            const auto& camera = tracing->scene.camera;
            Require((camera.view == SceneView::Overhead
                    && (camera.view_mode == PVM_IsoWibbleView || camera.view_mode == PVM_IsoStraightView))
                || (camera.view == SceneView::Front && camera.view_mode == PVM_FrontView),
                "Spinning-key sprite submitted outside its supported camera");
        }
        if (ordinary || spinning_key) trace_object = found->second;
    });
}
extern "C" void WorldSceneTraceShadowBegin(uint32_t slot)
{
    if (!tracing || trace_failed || tracing->format_version < 5 || tracing->trace.backend != 0) return;
    TraceGuard([&] {
        Require(trace_started && !trace_ended && !tracing->trace.entries.empty()
            && tracing->trace.entries.back().kind == QK_CreatureShadow && !trace_shadow.begun,
            "Shadow caster outside its bucket");
        trace_shadow.begun = true;
        trace_shadow.entry = uint32_t(tracing->trace.entries.size() - 1);
        const auto found = trace_objects.find(slot);
        if (found != trace_objects.end() && found->second->draw_class == ODC_Default
            && found->second->class_id == TCls_Creature) {
            trace_shadow.caster = found->second;
        } else {
            ++tracing->trace.raster.excluded_shadow_entries;
        }
    });
}
extern "C" void WorldSceneTraceShadowMask(const unsigned char* data, int width, int height)
{
    if (!tracing || trace_failed || tracing->format_version < 5 || tracing->trace.backend != 0) return;
    TraceGuard([&] {
        Require(trace_started && !trace_ended && trace_shadow.begun
            && trace_shadow.entry + 1 == tracing->trace.entries.size()
            && tracing->trace.entries.back().kind == QK_CreatureShadow,
            "Decoded shadow mask outside its bucket");
        if (!trace_shadow.caster) return;
        Require(!trace_shadow.data && trace_shadow.triangles == 0, "Shadow mask decoded more than once");
        Require(data && width > 0 && height > 0 && width <= 256 && height <= 256,
            "Shadow decoder extent outside scratch row bounds");
        auto& masks = tracing->trace.raster.shadow_masks;
        const size_t bytes = size_t(width) * height;
        Require(masks.size() < 32768 && trace_pixel_bytes <= 64 * 1024 * 1024
            && bytes <= 64 * 1024 * 1024 - trace_pixel_bytes,
            "Combined sprite and shadow mask budget exceeded");
        SceneShadowMask mask;
        mask.width = uint16_t(width); mask.height = uint16_t(height);
        mask.coverage.resize(bytes);
        for (int y = 0; y < height; ++y)
            std::copy_n(data + y * 256, width, mask.coverage.begin() + size_t(y) * width);
        Require(std::all_of(mask.coverage.begin(), mask.coverage.end(), [](uint8_t value) {
            return value == 0 || value == 255;
        }), "Shadow decoder produced nonbinary coverage");
        trace_shadow.mask = uint32_t(masks.size());
        masks.push_back(std::move(mask));
        trace_pixel_bytes += bytes;
        trace_shadow.data = data;
    });
}
extern "C" void WorldSceneTraceSprite(const SpriteScale* scale, const unsigned char* data,
    int src_w, int src_h, int32_t content_h, unsigned int flags,
    const unsigned char* remap, int32_t sprite_id, int alpha)
{
    trace_outline_sprite = SceneOutlineNoDraw;
    TraceGuard([&] {
        Require(trace_started && !trace_ended, "Sprite outside active draw trace");
        if (!trace_object) { ++tracing->trace.excluded_sprite_calls; return; }
        Require(scale && data && src_w > 0 && src_h > 0 && src_w <= 1024 && src_h <= 1024
            && content_h > 0 && content_h <= src_h, "Invalid submitted sprite extent");
        Require(tracing->trace.draws.size() < 65536, "Draw trace sprite budget exceeded");
        Require(((flags & Lb_SPRITE_REMAP) != 0) == (remap != nullptr), "Submitted remap state inconsistent");
        uint32_t index;
        const auto found = trace_sprites.find(sprite_id);
        if (found != trace_sprites.end()) {
            index = found->second;
            const auto& sprite = tracing->assets.sprites[index];
            Require(sprite.width == src_w && sprite.height == src_h, "Submitted sprite asset changed dimensions");
        } else {
            // Composite flame frames may not occur in object animation bindings.
            // Decode the already resolved bytes: never reload the sprite heap
            // while the renderer still holds its submitted source pointer.
            const KeeperSprite* metadata = nullptr; size_t length = 0;
            if (sprite_id >= KEEPERSPRITE_ADD_OFFSET) {
                const auto offset = sprite_id - KEEPERSPRITE_ADD_OFFSET;
                Require(offset < KEEPERSPRITE_ADD_NUM, "Submitted custom sprite outside table");
                metadata = &creature_table_add[offset]; length = keepersprite_add_bytes[offset];
            } else {
                Require(sprite_id >= 0 && sprite_id < KEEPSPRITE_LENGTH && size_t(sprite_id + 1) < creature_table_length,
                    "Submitted sprite outside table");
                metadata = &creature_table[sprite_id];
                Require(creature_table[sprite_id + 1].DataOffset >= metadata->DataOffset, "Invalid submitted sprite storage");
                length = creature_table[sprite_id + 1].DataOffset - metadata->DataOffset;
            }
            Require(length > 0 && length <= 4 * 1024 * 1024, "Submitted sprite storage bound unavailable");
            Require(metadata->SWidth == src_w && metadata->SHeight == src_h, "Submitted sprite metadata differs");
            SceneSprite sprite;
            sprite.id = sprite_id; sprite.width = src_w; sprite.height = src_h;
            sprite.frame_width = metadata->FrameWidth; sprite.frame_height = metadata->FrameHeight;
            sprite.frame_x = metadata->FrameOffsW; sprite.frame_y = metadata->FrameOffsH;
            sprite.offset_x = metadata->offset_x; sprite.offset_y = metadata->offset_y;
            sprite.shadow_offset = metadata->shadow_offset; sprite.flags = metadata->frame_flags;
            trace_pixel_bytes += size_t(src_w) * src_h * 2;
            Require(trace_pixel_bytes <= 64 * 1024 * 1024, "Draw trace asset budget exceeded");
            Require(DecodeSceneSprite({data, length}, src_w, src_h, sprite.pixels), "Malformed submitted sprite RLE");
            index = uint32_t(tracing->assets.sprites.size());
            tracing->assets.sprites.push_back(std::move(sprite)); trace_sprites.emplace(sprite_id, index);
        }
        SceneSpriteDraw draw;
        draw.object = trace_object->id; draw.entry = uint32_t(tracing->trace.entries.size() - 1); draw.sprite = index;
        draw.scale = *scale; draw.content_height = content_h; draw.flags = flags;
        draw.alpha = alpha != 0; draw.has_remap = remap != nullptr;
        draw.blend = ResolveSceneSpriteBlend(flags, draw.alpha);
        if (remap) std::copy_n(remap, draw.remap.size(), draw.remap.begin());
        if (tracing->format_version >= 4 && tracing->trace.backend == 0) {
            auto& commands = tracing->trace.raster.commands;
            Require(commands.size() < 327680, "Mixed draw trace command budget exceeded");
            commands.push_back({SceneDrawCommandKind::Sprite, uint32_t(tracing->trace.draws.size())});
        }
        tracing->trace.draws.push_back(draw);
        if (tracing->trace.backend == 1) trace_outline_sprite = uint32_t(tracing->trace.draws.size() - 1);
    });
}
extern "C" void WorldSceneTraceTriangle(unsigned int rasterizer, const PolyPoint* a,
    const PolyPoint* b, const PolyPoint* c, unsigned int mode,
    unsigned int colour, const unsigned char* texture)
{
    // The software routines also render UI and are used by standalone
    // diagnostics. Do not inspect their inputs without an active software walk.
    if (!tracing || trace_failed || !trace_started || trace_ended || tracing->format_version < 4
        || tracing->trace.backend != 0) return;
    TraceGuard([&] {
        Require(!tracing->trace.entries.empty(), "Terrain outside draw bucket");
        const auto kind = tracing->trace.entries.back().kind;
        if (kind == QK_CreatureShadow && tracing->format_version >= 5) {
            Require(trace_shadow.begun && trace_shadow.entry + 1 == tracing->trace.entries.size(),
                "Shadow triangle outside its caster bucket");
            if (!trace_shadow.caster) return;
            Require(trace_shadow.data && texture == trace_shadow.data && trace_shadow.triangles < 2,
                "Shadow triangle lacks its freshly decoded mask");
            Require(rasterizer == unsigned(SceneRasterizer::Trig) && mode == VM_SpriteTranslucent
                && a && b && c && colour >= 16 && colour <= 31, "Invalid shadow rasterizer arguments");
            auto& raster = tracing->trace.raster;
            Require(raster.shadows.size() < 65536 && raster.commands.size() < 327680,
                "Shadow draw trace budget exceeded");
            const auto& mask = raster.shadow_masks.at(trace_shadow.mask);
            SceneShadowTriangle shadow;
            shadow.caster = trace_shadow.caster->id; shadow.entry = trace_shadow.entry;
            shadow.mask = trace_shadow.mask; shadow.shade = uint8_t(colour);
            const PolyPoint* points[] = {a, b, c};
            for (size_t i = 0; i < shadow.vertices.size(); ++i) {
                const auto* point = points[i];
                Require(point->X >= -16777216 && point->X <= 16777216
                    && point->Y >= -16777216 && point->Y <= 16777216,
                    "Shadow position outside bounded capture range");
                Require(point->U >= 0 && point->U <= (int32_t(mask.width) - 1) * 65536
                    && point->V >= 0 && point->V <= (int32_t(mask.height) - 1) * 65536,
                    "Shadow UV outside initialized decoder rectangle");
                shadow.vertices[i] = {int32_t(point->X), int32_t(point->Y), int32_t(point->U), int32_t(point->V), 0};
            }
            if (trace_shadow.triangles) {
                const auto& first = raster.shadows.back();
                const auto same = [](const SceneRasterVertex& p, const SceneRasterVertex& q) {
                    return p.x == q.x && p.y == q.y && p.u == q.u && p.v == q.v;
                };
                Require(first.shade == shadow.shade && same(first.vertices[0], shadow.vertices[0])
                    && same(first.vertices[2], shadow.vertices[1]), "Shadow triangle pair changed shared inputs");
            }
            raster.commands.push_back({SceneDrawCommandKind::Shadow, uint32_t(raster.shadows.size())});
            raster.shadows.push_back(shadow);
            ++trace_shadow.triangles;
            return;
        }
        if (kind != QK_PolygonStandard && kind != QK_PolygonNearFP && kind != QK_TextureQuad) return;
        Require(a && b && c && texture, "Missing submitted terrain data");
        const bool gpoly = rasterizer == unsigned(SceneRasterizer::Gpoly) && mode == VM_QuadTextured;
        const bool trig = rasterizer == unsigned(SceneRasterizer::Trig) && mode == VM_SolidColor && kind == QK_PolygonNearFP;
        Require(gpoly || trig, "Unsupported terrain rasterizer or mode");
        Require(tracing->scene.texture_row_stride == 256, "Terrain trace needs original texture bank stride");
        const size_t span = gpoly ? 31 * 256 + 32 : (colour == 32 ? 256 * 256 : 32 * 256);
        const uintptr_t base = reinterpret_cast<uintptr_t>(block_mem);
        const uintptr_t address = reinterpret_cast<uintptr_t>(texture);
        Require(address >= base && address - base < sizeof(block_mem), "Terrain texture outside owned bank");
        const uint32_t offset = uint32_t(address - base);
        Require(trace_textures.contains(offset) && uint64_t(offset) + span <= tracing->assets.texture_pixels.size(),
            "Terrain texture range outside captured frames");
        if (trace_verified_textures.emplace(offset, span).second)
            Require(std::equal(texture, texture + span, tracing->assets.texture_pixels.begin() + offset),
                "Terrain texture changed after scene snapshot");
        Require(!trig || colour < 64, "Terrain constant shade outside captured fade table");
        auto& raster = tracing->trace.raster;
        Require(raster.triangles.size() < 262144 && raster.commands.size() < 327680,
            "Terrain draw trace budget exceeded");
        SceneTerrainTriangle triangle;
        triangle.entry = uint32_t(tracing->trace.entries.size() - 1);
        triangle.rasterizer = gpoly ? SceneRasterizer::Gpoly : SceneRasterizer::Trig;
        triangle.mode = uint8_t(mode); triangle.colour = trig ? uint8_t(colour) : 0;
        triangle.texture_offset = offset;
        const PolyPoint* points[] = {a, b, c};
        const auto integer = [](long value) {
            Require(value >= std::numeric_limits<int32_t>::min() && value <= std::numeric_limits<int32_t>::max(),
                "Terrain vertex outside portable capture range");
            return int32_t(value);
        };
        for (size_t i = 0; i < triangle.vertices.size(); ++i) {
            const auto* point = points[i];
            auto& vertex = triangle.vertices[i];
            vertex.x = integer(point->X); vertex.y = integer(point->Y);
            vertex.u = integer(point->U); vertex.v = integer(point->V);
            // Mode 7 ignores S; subdivision does not initialize that field.
            // Do not even read it when constructing the canonical owned value.
            vertex.s = gpoly ? integer(point->S) : 0;
            Require(vertex.x >= -16777216 && vertex.x <= 16777216
                && vertex.y >= -16777216 && vertex.y <= 16777216,
                "Terrain position outside bounded capture range");
            Require(vertex.s >= 0 && vertex.s < 64 * 65536, "Terrain vertex shade outside captured fade table");
        }
        raster.commands.push_back({SceneDrawCommandKind::Triangle, uint32_t(raster.triangles.size())});
        raster.triangles.push_back(triangle);
    });
}
extern "C" void WorldSceneTraceEnd(void)
{
    trace_object = nullptr;
    TraceGuard([&] {
        Require(trace_started && !trace_ended, "Invalid draw trace end");
        FinishShadowEntry();
        if (trace_reference_metadata) {
            // Sample actual remap decisions before the private draw scope ends.
            // Initial clipping/selector proof remains the sealed-frame snapshot.
            trace_reference_proof.hover = get_engine_reference_hover_proof();
            trace_reference_proof.room = get_engine_reference_room_proof();
            auto metadata_path = trace_path;
            metadata_path += ".json";
            SaveReferenceMetadata(tracing->scene, metadata_path, trace_reference_proof);
        }
        trace_ended = true;
    });
}
void WorldSceneCompleteCapture()
{
    if (!tracing) return;
    try {
        Require(!trace_failed, trace_error);
        Require(trace_started && trace_ended, "Scene draw trace did not complete");
        Require(!(local_system_flags & GSF_NetworkActive), "Network became active during scene capture");
        Require(tracing->scene.texture_generation == level_textures_generation, "Texture generation changed during draw capture");
        tracing->trace.complete = true;
        if (tracing->trace.backend == 1) {
            Require(!pending_outline && outstanding_outline.expired(), "Previous GL capture still outstanding");
            auto session = std::make_shared<WorldSceneOutlineCapture>();
            session->capture = tracing; session->path = trace_path;
            pending_outline = session; outstanding_outline = session;
        } else {
            SaveSceneCapture(trace_path, *tracing);
            SYNCLOG("Owned scene and draw trace written: %s", trace_path.filename().string().c_str());
        }
    } catch (const std::exception& e) {
        ERRORLOG("Scene draw trace failed: %s", e.what());
    }
    WorldSceneDiscardDrawTrace();
}

uint32_t WorldSceneOutlineSpriteTag() noexcept
{
    return tracing && !trace_failed && tracing->trace.backend == 1 ? trace_outline_sprite : SceneOutlineNoDraw;
}
uint32_t WorldSceneOutlineEntryTag() noexcept
{
    if (!tracing || trace_failed || !trace_started || trace_ended || tracing->trace.backend != 1
        || tracing->trace.entries.empty()) return SceneOutlineNoDraw;
    return uint32_t(tracing->trace.entries.size() - 1);
}
void WorldSceneOutlineFailProducer(const char* reason) noexcept
{
    if (!tracing || tracing->trace.backend != 1 || trace_failed) return;
    trace_failed = true;
    std::snprintf(trace_error, sizeof(trace_error), "%s", reason ? reason : "GL capture producer failure");
}
uint32_t WorldSceneGLShadowTag(uint32_t slot, SceneObjectId& caster) noexcept
{
    uint32_t entry = SceneOutlineNoDraw;
    TraceGuard([&] {
        if (tracing->trace.backend != 1) return;
        const auto found = trace_objects.find(slot);
        Require(found != trace_objects.end() && found->second->draw_class == ODC_Default
            && found->second->class_id == TCls_Creature,
            "Complete GL world cannot capture excluded or unresolved shadow caster");
        Require(!tracing->trace.entries.empty() && tracing->trace.entries.back().kind == QK_CreatureShadow,
            "GL shadow has no matching bucket entry");
        caster = found->second->id; entry = uint32_t(tracing->trace.entries.size() - 1);
    });
    return entry;
}
WorldSceneOutlineSession WorldSceneTakeOutlineCapture(uint64_t epoch, uint64_t sequence) noexcept
{
    auto session = std::move(pending_outline);
    if (!session) return {};
    if (session->cancelled.load() || session->capture->scene.epoch != epoch || session->capture->scene.sequence != sequence) {
        ERRORLOG("GL outline capture handoff frame mismatch or cancellation");
        return {};
    }
    return session;
}
void WorldSceneFailOutlineCapture(const WorldSceneOutlineSession& session, const char* reason) noexcept
{
    if (!session || session->failed || session->finished) return;
    session->failed = true;
    std::snprintf(session->error, sizeof(session->error), "%s", reason ? reason : "GL capture render failure");
    ERRORLOG("GL outline capture failed: %s", session->error);
}
namespace {
template<class F> void OutlineGuard(const WorldSceneOutlineSession& session, F&& operation) noexcept
{
    if (!session || session->failed || session->finished || session->cancelled.load()) return;
    try { operation(); }
    catch (const std::exception& e) { WorldSceneFailOutlineCapture(session, e.what()); }
    catch (...) { WorldSceneFailOutlineCapture(session, "Unknown GL outline capture failure"); }
}
}
void WorldSceneBeginOutlineCapture(const WorldSceneOutlineSession& session, uint8_t mode, float alpha,
    const std::array<float, 9>& transform, const std::array<uint8_t, 768>& palette) noexcept
{
    OutlineGuard(session, [&] {
        Require(!session->begun, "GL capture render pass began more than once");
        Require(palette == session->capture->assets.display_palette, "GL display palette changed after owned capture");
        auto& outline = session->capture->trace.outline;
        outline.present = true; outline.mode = mode; outline.alpha = alpha; outline.palette_transform = transform;
        session->begun = true;
    });
}
void WorldSceneAppendOutlineOccluder(const WorldSceneOutlineSession& session, const SceneOutlineOccluder& triangle) noexcept
{
    OutlineGuard(session, [&] {
        Require(session->begun, "GL occluder before capture begin");
        auto& outline = session->capture->trace.outline;
        Require(outline.occluders.size() < 262144 && outline.sprites.empty() && outline.commands.empty(),
            "GL occluder budget or phase violated");
        outline.occluders.push_back(triangle);
    });
}
void WorldSceneBeginGLWorldCapture(const WorldSceneOutlineSession& session, SceneGLWorldTrace state) noexcept
{
    OutlineGuard(session, [&] {
        Require(session->begun && !session->capture->trace.gl_world.present, "GL world capture began out of order");
        auto& c = *session->capture;
        size_t bytes = c.assets.texture_pixels.size() + c.assets.additive.size()
            + c.assets.base_palette.size() + c.assets.display_palette.size()
            + c.assets.fade.size() + c.assets.ghost.size() + 1024 + 16384 + 511 * 511 * 2;
        for (const auto& sprite : c.assets.sprites) bytes += sprite.pixels.size();
        Require(bytes <= 64 * 1024 * 1024, "GL world shared asset budget exceeded");
        session->world_asset_bytes = bytes;
        state.present = true; state.complete = false;
        c.trace.gl_world = std::move(state);
    });
}
void WorldSceneAppendGLTerrain(const WorldSceneOutlineSession& session, const SceneOutlineOccluder& positions,
    SceneGLTerrainTriangle triangle, const std::array<uint8_t, 1024>* tile) noexcept
{
    OutlineGuard(session, [&] {
        auto& w = session->capture->trace.gl_world;
        auto& o = session->capture->trace.outline;
        Require(w.present && w.terrain.size() < 262144 && w.shadows.empty() && o.sprites.empty(), "GL terrain phase or budget invalid");
        triangle.tile = SceneGLMissingTile;
        if (tile) {
            auto found = session->tiles.find(*tile);
            if (found != session->tiles.end()) triangle.tile = found->second;
            else {
                Require(session->world_asset_bytes + 1024 <= 64 * 1024 * 1024, "GL tile shared asset budget exceeded");
                triangle.tile = uint32_t(w.tiles.size()); w.tiles.push_back(*tile);
                session->tiles.emplace(*tile, triangle.tile); session->world_asset_bytes += 1024;
            }
        }
        o.occluders.push_back(positions); w.terrain.push_back(triangle);
    });
}
void WorldSceneAppendGLShadow(const WorldSceneOutlineSession& session, SceneGLShadow shadow,
    const SceneGLShadowSource& source) noexcept
{
    OutlineGuard(session, [&] {
        auto& w = session->capture->trace.gl_world;
        Require(w.present && session->capture->trace.outline.sprites.empty() && w.shadows.size() < 65536,
            "GL shadow phase or budget invalid");
        auto key = std::make_tuple(source.width, source.height, source.coverage);
        auto found = session->shadow_sources.find(key);
        if (found != session->shadow_sources.end()) shadow.source = found->second;
        else {
            Require(source.coverage.size() <= 65536 && session->world_asset_bytes + source.coverage.size() <= 64 * 1024 * 1024,
                "GL shadow shared asset budget exceeded");
            shadow.source = uint32_t(w.shadow_sources.size()); w.shadow_sources.push_back(source);
            session->shadow_sources.emplace(std::move(key), shadow.source);
            session->world_asset_bytes += source.coverage.size();
        }
        w.shadows.push_back(shadow);
    });
}
uint32_t WorldSceneAppendOutlineSprite(const WorldSceneOutlineSession& session, const SceneOutlineSprite& sprite,
    SceneOutlineColourResolution resolution, const std::array<uint8_t, 1024>* effective_row) noexcept
{
    uint32_t index = SceneOutlineNoDraw;
    OutlineGuard(session, [&] {
        Require(session->begun, "GL sprite before capture begin");
        auto& capture = *session->capture; auto& outline = capture.trace.outline;
        Require(outline.sprites.size() < 65536 && sprite.draw < capture.trace.draws.size(), "GL sprite capture budget or identity invalid");
        const auto& source = capture.assets.sprites.at(capture.trace.draws[sprite.draw].sprite);
        Require(source.width && source.height && source.pixels.size() == size_t(source.width) * source.height * 2,
            "GL owned source coverage unavailable");
        auto owned = sprite; owned.source_border_covered = false;
        for (unsigned y = 0; y < source.height; ++y)
            owned.source_border_covered |= source.pixels[(size_t(y) * source.width + source.width - 1) * 2 + 1] != 0;
        for (unsigned x = 0; x < source.width; ++x)
            owned.source_border_covered |= source.pixels[(size_t(source.height - 1) * source.width + x) * 2 + 1] != 0;
        SceneOutlineColourBinding binding;
        binding.resolution = resolution;
        Require(sprite.additive == (resolution == SceneOutlineColourResolution::Glow)
            && sprite.additive == (effective_row == nullptr), "GL effective colour binding missing");
        if (effective_row) {
            const auto found = session->colour_rows.find(*effective_row);
            if (found != session->colour_rows.end()) binding.lookup_row = found->second;
            else {
                Require(outline.colour_rows.size() < 16384, "GL effective colour row budget exceeded");
                Require(session->world_asset_bytes + 1024 <= 64 * 1024 * 1024, "GL colour shared asset budget exceeded");
                session->world_asset_bytes += 1024;
                binding.lookup_row = uint32_t(outline.colour_rows.size());
                outline.colour_rows.push_back(*effective_row);
                session->colour_rows.emplace(*effective_row, binding.lookup_row);
            }
        }
        outline.colour_bindings.push_back(binding);
        outline.sprites.push_back(owned); index = uint32_t(outline.sprites.size() - 1);
    });
    return index;
}
void WorldSceneAppendOutlineCommand(const WorldSceneOutlineSession& session, const SceneOutlineCommand& command) noexcept
{
    OutlineGuard(session, [&] {
        Require(session->begun, "GL command before capture begin");
        auto& outline = session->capture->trace.outline;
        Require(outline.commands.size() < 196608 && command.sprite < outline.sprites.size(), "GL command capture budget or index invalid");
        outline.commands.push_back(command);
    });
}
void WorldSceneFinishOutlineCapture(const WorldSceneOutlineSession& session) noexcept
{
    OutlineGuard(session, [&] {
        Require(session->begun, "GL capture ended before begin");
        std::lock_guard lock(session->publication);
        session->capture->trace.outline.complete = true;
        Require(session->capture->trace.gl_world.present, "Complete GL world state missing");
        session->capture->trace.gl_world.complete = true;
        if (session->capture->format_version >= 11) return;
        Require(!session->cancelled.load(), "GL capture cancelled before publication");
        SaveSceneCapture(session->path, *session->capture);
        session->finished = true;
        SYNCLOG("Owned scene and GL outline trace written: %s", session->path.filename().string().c_str());
    });
}

void WorldSceneGLUploadMirror::Record(uint64_t resource, uint32_t id, uint32_t w, uint32_t h,
    const void* data, size_t size) noexcept
{
    try {
        if (!data || !id || !w || !h || size > 64 * 1024 * 1024) { bytes.clear(); return; }
        if (handle != resource || texture != id || width != w || height != h) {
            ++stamp.generation; stamp.revision = 0;
        }
        handle = resource; texture = id; width = w; height = h;
        bytes.assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size);
        ++stamp.revision;
    } catch (...) { bytes.clear(); }
}
void WorldSceneBeginCompositionCapture(const WorldSceneOutlineSession& session, uint32_t width, uint32_t height,
    const std::array<float, 4>& clear) noexcept
{
    OutlineGuard(session, [&] {
        auto& c = session->capture->trace.gl_composition;
        Require(!c.present && width && height && uint64_t(width) * height * 4 <= 64 * 1024 * 1024,
            "Invalid GL composition begin or native extent");
        c.present = true; c.screen_clear_rgba = clear;
        c.overlays.native_width = width; c.overlays.native_height = height;
    });
}
void WorldSceneAppendOverlayQuad(const WorldSceneOutlineSession& session, SceneGLOverlayQuad quad,
    const SceneGLOverlaySource* source, const std::array<uint8_t, 1024>* row, const uint8_t* fade) noexcept
{
    OutlineGuard(session, [&] {
        auto& c = session->capture->trace.gl_composition;
        auto& pass = c.overlays;
        const unsigned phase = unsigned(quad.phase);
        Require(c.present && session->capture->trace.gl_world.complete && phase < 3
            && c.completed_phases == (1u << phase) - 1 && pass.draws.size() < 262144,
            "GL overlay phase or draw budget violated");
        const auto reserve = [&](size_t size) {
            if (size > 64 * 1024 * 1024 || session->overlay_asset_bytes > 64 * 1024 * 1024 - size
                || session->world_asset_bytes > SceneGLCompositionOwnedByteLimit - size) {
                char error[256];
                std::snprintf(error, sizeof(error), "GL overlay asset budget: total=%llu overlay=%llu add=%llu sources=%u limit=%llu",
                    static_cast<unsigned long long>(session->world_asset_bytes),
                    static_cast<unsigned long long>(session->overlay_asset_bytes), static_cast<unsigned long long>(size),
                    unsigned(pass.sources.size()), static_cast<unsigned long long>(SceneGLCompositionOwnedByteLimit));
                throw std::runtime_error(error);
            }
            session->world_asset_bytes += size;
            session->overlay_asset_bytes += size;
        };
        if (source) {
            auto found = std::find(pass.sources.begin(), pass.sources.end(), *source);
            if (found == pass.sources.end()) {
                reserve(source->indices.size() + source->initialized_rows.size());
                quad.source = uint32_t(pass.sources.size()); pass.sources.push_back(*source);
            } else quad.source = uint32_t(found - pass.sources.begin());
        }
        if (row) {
            auto found = std::find(pass.colour_rows.begin(), pass.colour_rows.end(), *row);
            if (found == pass.colour_rows.end()) {
                reserve(row->size()); quad.colour_row = uint32_t(pass.colour_rows.size()); pass.colour_rows.push_back(*row);
            } else quad.colour_row = uint32_t(found - pass.colour_rows.begin());
        }
        if (fade) {
            if (pass.fade.empty()) { reserve(16384); pass.fade.assign(fade, fade + 16384); }
            else Require(std::equal(pass.fade.begin(), pass.fade.end(), fade), "GL overlay fade changed within frame");
        }
        pass.draws.push_back(quad); ++c.phase_quad_counts[phase];
    });
}
void WorldSceneCompleteOverlayPhase(const WorldSceneOutlineSession& session, SceneGLOverlayPhase phase) noexcept
{
    OutlineGuard(session, [&] {
        auto& c = session->capture->trace.gl_composition;
        Require(c.present && unsigned(phase) < 3 && c.completed_phases == (1u << unsigned(phase)) - 1,
            "GL overlay completion order invalid");
        c.completed_phases |= uint8_t(1u << unsigned(phase));
    });
}
void WorldSceneRecordLensCapture(const WorldSceneOutlineSession& session, SceneGLLensTrace resolved_lens) noexcept
{
    OutlineGuard(session, [&] {
        auto& c = session->capture->trace.gl_composition;
        Require(c.present && c.completed_phases == 7, "GL lens completion order invalid");
        const auto& p = resolved_lens.pass;
        const size_t size = p.remap.size() * 4 + p.overlay.size() + p.density.size()
            + p.palette_rgba.size() + p.fade.size() + p.rgb6_lookup.size();
        if (size > 64 * 1024 * 1024 || session->world_asset_bytes > SceneGLCompositionOwnedByteLimit - size) {
            char error[256];
            std::snprintf(error, sizeof(error), "GL lens asset budget: before=%llu lens=%llu total=%llu sources=%u overlay=%llu limit=%llu",
                static_cast<unsigned long long>(session->world_asset_bytes), static_cast<unsigned long long>(size),
                static_cast<unsigned long long>(session->world_asset_bytes + size), unsigned(c.overlays.sources.size()),
                static_cast<unsigned long long>(session->overlay_asset_bytes),
                static_cast<unsigned long long>(SceneGLCompositionOwnedByteLimit));
            throw std::runtime_error(error);
        }
        SYNCLOG("GL composition assets: world=%llu overlay=%llu lens=%llu total=%llu sources=%u phases=%u/%u/%u",
            static_cast<unsigned long long>(session->world_asset_bytes - session->overlay_asset_bytes),
            static_cast<unsigned long long>(session->overlay_asset_bytes), static_cast<unsigned long long>(size),
            static_cast<unsigned long long>(session->world_asset_bytes + size), unsigned(c.overlays.sources.size()),
            c.phase_quad_counts[0], c.phase_quad_counts[1], c.phase_quad_counts[2]);
        session->world_asset_bytes += size; c.lens = std::move(resolved_lens); c.completed_phases = 15;
    });
}
void WorldSceneFinishCompositionCapture(const WorldSceneOutlineSession& session) noexcept
{
    OutlineGuard(session, [&] {
        auto& c = session->capture->trace.gl_composition;
        Require(c.present && c.completed_phases == 15 && session->capture->trace.gl_world.complete,
            "GL composition capture incomplete");
        ValidateSceneGLOverlayPass(c.overlays);
        std::lock_guard lock(session->publication);
        Require(!session->cancelled.load(), "GL composition cancelled before publication");
        c.complete = true; SaveSceneCapture(session->path, *session->capture); session->finished = true;
        SYNCLOG("Owned scene and GL composition written: %s", session->path.filename().string().c_str());
    });
}
