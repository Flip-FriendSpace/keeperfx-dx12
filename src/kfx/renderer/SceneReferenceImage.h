// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
// Opt-in reference screenshots. Called only after the captured frame is drawn.
#pragma once
#include <SDL3_image/SDL_image.h>
#include "kfx/renderer/WorldScene.h"
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <string>

// Copied from the accepted owned packet, never from the currently building frame.
struct SceneReferencePresentation {
    uint64_t epoch = 0, sequence = 0;
};

inline void WriteSceneReferenceReceipt(std::ostream& out, const kfx::render::WorldScene& scene,
    int width, int height, const SceneReferencePresentation* presentation = nullptr)
{
    out.imbue(std::locale::classic());
    out << std::setprecision(std::numeric_limits<float>::max_digits10);
    auto position = [&](const kfx::render::ScenePosition& p) { out << '[' << p.x << ',' << p.y << ',' << p.z << ']'; };
    out << "{\"schema\":1,\"epoch\":" << scene.epoch << ",\"sequence\":" << scene.sequence
        << ",\"turn\":" << scene.game_turn << ",\"player\":" << scene.player
        << ",\"imageWidth\":" << width << ",\"imageHeight\":" << height;
    if (presentation) out << ",\"backend\":\"DX12\",\"presentation\":{\"epoch\":" << presentation->epoch
        << ",\"sequence\":" << presentation->sequence << '}';
    out << ",\"input\":";
    const auto& input=scene.reference_input;
    if(!input.valid)out << "null";
    else {
        const auto array=[&](const auto& values) {
            out << '[';for(size_t i=0;i<values.size();++i)out << (i?",":"") << values[i];out << ']';
        };
        const auto id=[&](const kfx::render::SceneObjectId& value) {
            out << "{\"slot\":" << value.slot << ",\"incarnation\":" << value.incarnation << '}';
        };
        const auto packet=[&](const kfx::render::SceneReferencePacket& value) {
            if(!value.valid){out << "null";return;}
            out << "{\"epoch\":" << value.epoch << ",\"serial\":" << value.serial
                << ",\"consumedTurn\":" << value.consumed_turn << ",\"packetTurn\":" << value.packet_turn
                << ",\"position\":[" << value.x << ',' << value.y << "],\"control\":" << value.control
                << ",\"additional\":" << unsigned(value.additional) << ",\"context\":" << unsigned(value.context)
                << ",\"action\":" << unsigned(value.action) << ",\"parameters\":";array(value.parameters);out << '}';
        };
        out << "{\"nativeMouse\":";array(input.native_mouse);
        out << ",\"nativeSize\":";array(input.native_size);
        out << ",\"windowSize\":";array(input.window_size);
        out << ",\"drawableSize\":";array(input.drawable_size);
        out << ",\"viewport\":";array(input.viewport);
        out << ",\"presentRect\":";array(input.present_rect);
        out << ",\"mapPointValid\":" << (input.map_point_valid?"true":"false");
        out << ",\"top\":";array(input.top);
        out << ",\"block\":";array(input.block);
        out << ",\"floor\":";array(input.floor);
        out << ",\"viewType\":" << input.view_type << ",\"workState\":" << input.work_state;
        out << ",\"localHover\":";id(input.local_hover);
        out << ",\"playerHover\":";id(input.player_hover);
        out << ",\"consumed\":";packet(input.consumed);
        out << ",\"lastButton\":";packet(input.last_button);out << '}';
    }
    out << ",\"interpolation\":" << scene.interpolation << ",\"camera\":{\"view\":" << unsigned(scene.camera.view)
        << ",\"position\":";
    const auto& c = scene.camera;
    position(c.position);
    out << ",\"angles\":[" << c.yaw << ',' << c.pitch << ',' << c.roll << "]"
        << ",\"scaledZoom\":" << c.scaled_zoom << ",\"fov\":" << c.horizontal_fov << ",\"lens\":" << c.lens
        << ",\"viewport\":[" << c.viewport_x << ',' << c.viewport_y << ',' << c.viewport_w << ',' << c.viewport_h << ']'
        << ",\"pixelSize\":" << c.pixel_size << ",\"viewDistance\":" << c.view_distance
        << ",\"viewMode\":" << unsigned(c.view_mode) << ",\"lensMode\":" << unsigned(c.lens_mode)
        << ",\"shortWalls\":" << unsigned(c.short_walls) << ",\"hasRotation\":" << (c.has_rotation ? "true" : "false")
        << ",\"rotation\":[";
    for (size_t i = 0; i < c.rotation.size(); ++i) out << (i ? "," : "") << c.rotation[i];
    out << "]},\"objects\":[";
    bool comma = false;
    for (const auto& o : scene.objects) {
        out << (comma ? "," : "") << "{\"slot\":" << o.id.slot << ",\"incarnation\":" << o.id.incarnation
            << ",\"position\":";
        position(o.position);
        out << ",\"simulationPosition\":"; position(o.simulation_position);
        out << ",\"floorHeight\":" << o.floor_height << ",\"model\":" << o.model << ",\"animation\":" << o.animation
            << ",\"frame\":" << unsigned(o.frame) << ",\"size\":" << o.size << ",\"yaw\":" << o.yaw << ",\"pitch\":" << o.pitch
            << ",\"owner\":" << unsigned(o.owner) << ",\"class\":" << unsigned(o.class_id)
            << ",\"drawClass\":" << unsigned(o.draw_class) << ",\"renderFlags\":" << unsigned(o.render_flags)
            << ",\"tint\":" << unsigned(o.tint) << '}';
        comma = true;
    }
    out << "],\"columns\":[";
    comma = false;
    for (const auto& column : scene.columns) {
        out << (comma ? "," : "") << "{\"x\":" << column.x << ",\"y\":" << column.y << ",\"cubes\":[";
        for (size_t i = 0; i < column.cubes.size(); ++i) out << (i ? "," : "") << column.cubes[i];
        out << "],\"solidMask\":" << column.solid_mask << ",\"floorTexture\":" << column.floor_texture
            << ",\"mapFlags\":" << unsigned(column.map_flags)
            << ",\"revealed\":" << (column.revealed ? "true" : "false")
            << ",\"inView\":" << (column.in_view ? "true" : "false") << '}';
        comma = true;
    }
    out << "],\"doors\":[";
    comma = false;
    for (const auto& door : scene.reference_doors) {
        out << (comma ? "," : "") << "{\"slot\":" << door.id.slot << ",\"incarnation\":" << door.id.incarnation
            << ",\"slab\":[" << door.slab_x << ',' << door.slab_y << "]"
            << ",\"model\":" << door.model << ",\"owner\":" << unsigned(door.owner)
            << ",\"orientation\":" << unsigned(door.orientation) << ",\"state\":" << unsigned(door.state)
            << ",\"openAmount\":" << door.open_amount << ",\"locked\":" << unsigned(door.locked) << '}';
        comma = true;
    }
    out << "],\"lighting\":{\"enabled\":" << (scene.lights_enabled ? "true" : "false")
        << ",\"ambient\":" << scene.ambient_light << ",\"lights\":[";
    comma = false;
    for (const auto& l : scene.lights) {
        out << (comma ? "," : "") << "{\"slot\":" << l.id.slot << ",\"incarnation\":" << l.id.incarnation
            << ",\"position\":";
        position(l.position);
        out << ",\"previousPosition\":"; position(l.previous_position);
        out << ",\"radius\":" << l.radius << ",\"intensity\":" << unsigned(l.intensity)
            << ",\"randomIntensity\":" << l.random_intensity << ",\"previousRandomIntensity\":" << l.previous_random_intensity
            << ",\"flags\":" << unsigned(l.flags) << ",\"resetInterpolation\":" << (l.reset_interpolation ? "true" : "false")
            << ",\"lastTurnMoved\":" << l.last_turn_moved << ",\"lastTurnRandomized\":" << l.last_turn_randomized << '}';
        comma = true;
    }
    out << "]},\"surface\":{\"lavaScroll\":" << scene.lava_scroll << ",\"waterScroll\":" << scene.water_scroll
        << ",\"textureGeneration\":" << scene.texture_generation << ",\"textureDimension\":" << scene.texture_dimension
        << ",\"textureRowStride\":" << scene.texture_row_stride << ",\"textureFrames\":[";
    for (size_t i = 0; i < scene.texture_frames.size(); ++i) out << (i ? "," : "") << scene.texture_frames[i];
    out << "],\"wibble\":[";
    for (size_t i = 0; i < scene.wibble.size(); ++i) {
        out << (i ? "," : "") << "{\"offset\":"; position(scene.wibble[i].offset);
        out << ",\"lightnessOffset\":" << scene.wibble[i].lightness_offset << '}';
    }
    out << "]}}\n";
}

inline bool SaveSceneReferenceImage(SDL_Surface* surface, const kfx::render::WorldScene& scene,
    const SceneReferencePresentation* presentation = nullptr)
{
    const auto& path = scene.reference_image;
    SDL_IOStream* stream = SDL_IOFromFile(path.c_str(), "wb");
    if (!stream) return false;
    const bool saved = IMG_SavePNG_IO(surface, stream, false);
    const bool closed = SDL_CloseIO(stream);
    if (!saved || !closed) return false;
    // Publish a completion receipt only after the PNG stream has closed. Every
    // metadata field comes from the retained scene and accepted presentation;
    // GPU completion must not query the later live simulation or its projector.
    const auto receipt = path + ".ready.json";
    const auto temporary = receipt + ".tmp";
    std::ofstream out(temporary, std::ios::binary);
    WriteSceneReferenceReceipt(out, scene, surface->w, surface->h, presentation);
    out.close();
    if (!out) return SDL_SetError("Reference receipt write failed");
    std::error_code error;
    std::filesystem::rename(temporary, receipt, error);
    return !error || SDL_SetError("Reference receipt publication failed: %s", error.message().c_str());
}
