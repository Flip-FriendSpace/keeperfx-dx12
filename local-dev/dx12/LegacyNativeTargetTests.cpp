// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
// Focused CPU-only native-stride oracle checks. Build with the same original-C
// objects as the diagnostic host and --wrap=LbPixelBlockCopyForward.
#include "LegacyWorldOracle.h"
#include "LegacyTerrainOracle.h"
#include <algorithm>
#include <iostream>
#include <stdexcept>

using namespace kfx::render;
using namespace kfx::render::dx12;
namespace {
void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
std::vector<uint8_t> Packed(const SceneCapture& c)
{
    std::vector<uint8_t> result(size_t(c.target.window_w) * c.target.window_h);
    for (size_t i = 0; i < result.size(); ++i) result[i] = uint8_t(i * 17 + 23);
    return result;
}
std::vector<uint8_t> Native(const SceneCapture& c, const std::vector<uint8_t>& packed)
{
    const auto& t = c.target;
    std::vector<uint8_t> result(size_t(t.pitch) * t.height, 0xa7);
    for (size_t y = 0; y < t.height; ++y) std::fill_n(result.begin() + y * t.pitch, t.width, t.clear_index);
    for (int y = 0; y < t.window_h; ++y)
        std::copy_n(packed.begin() + size_t(y) * t.window_w, t.window_w,
            result.begin() + size_t(y + t.window_y) * t.pitch + t.window_x);
    return result;
}
std::vector<uint8_t> Crop(const SceneCapture& c, const std::vector<uint8_t>& native)
{
    auto result = Packed(c); const auto& t = c.target;
    for (int y = 0; y < t.window_h; ++y)
        std::copy_n(native.begin() + size_t(y + t.window_y) * t.pitch + t.window_x,
            t.window_w, result.begin() + size_t(y) * t.window_w);
    return result;
}
SceneSpriteDraw Draw(int x, int y, unsigned flags)
{
    SceneSpriteDraw d;
    d.sprite = 0; d.scale = {x,y,2,2,4,4,0,0,8,6}; d.content_height = 2;
    d.flags = flags; d.blend = ResolveSceneSpriteBlend(flags, false);
    d.has_remap = (flags & 0x0800) != 0;
    if (d.has_remap) for (size_t i = 0; i < d.remap.size(); ++i) d.remap[i] = uint8_t(255 - i);
    return d;
}
}
int main(int argc, char** argv)
{
    try {
        Check(argc == 2, "Pass a validated v8 fixture path");
        SceneCapture c;
        c.format_version = 8; c.scene.camera.viewport_w = 8; c.scene.camera.viewport_h = 6;
        c.target = {SceneIndexedTargetKind::Screen,144,0,14,12,16,3,2,8,6};
        c.assets.additive.resize(65536);
        SceneSprite sprite; sprite.width = sprite.height = 2;
        sprite.pixels = {31,255,67,255,103,255,139,255}; c.assets.sprites.push_back(sprite);
        auto packed = Packed(c); auto native = Native(c, packed);
        auto draw = Draw(2,1,1);
        const auto interior = ReplayLegacySpritesNative(c,native,{&draw,1},false);
        Check(interior.available && interior.guard_changes.empty(), "Padded interior original unavailable");
        Check(Crop(c,interior.pixels) == ReplayLegacySprites(c,packed,{&draw,1}), "Padded interior differs");
        draw = Draw(-1,1,1);
        const auto unknown = ReplayLegacySpritesNative(c,native,{&draw,1},false);
        Check(!unknown.available && unknown.pixels.empty() && unknown.unavailable_reads.size() == 1,
            "Sidebar source read was not blocked");
        const auto repaired = ReplayLegacySpritesNative(c,native,{&draw,1},true);
        Check(repaired.available && repaired.guard_changes.empty(), "Corrected padded draw failed");
        Check(Crop(c,repaired.pixels) == ReplayCorrectedLegacySprites(c,packed,{&draw,1}), "Corrected native differs");
        auto remapped = Draw(-1,1,0x0801);
        Check(!ReplayLegacySpritesNative(c,native,{&remapped,1},false).available, "Remapped sidebar source read was not blocked");
        Check(Crop(c,ReplayLegacySpritesNative(c,native,{&remapped,1},true).pixels)
            == ReplayCorrectedLegacySprites(c,packed,{&remapped,1}), "Corrected remapped native differs");
        // A full-width target really aliases the preceding visible row. This
        // is captured state, so the unchanged original C copy is permitted.
        c.target = {SceneIndexedTargetKind::Screen,144,0,8,6,8,0,0,8,6}; native = Native(c,packed);
        const auto alias = ReplayLegacySpritesNative(c,native,{&draw,1},false);
        Check(alias.available && alias.pixels == ReplayLegacySprites(c,packed,{&draw,1}), "Native full-width alias differs from old path");
        Check(alias.pixels != ReplayCorrectedLegacySprites(c,packed,{&draw,1}), "Alias fixture did not exercise original defect");
        c.target.pitch = 12; native = Native(c,packed);
        Check(!ReplayLegacySpritesNative(c,native,{&draw,1},false).available, "Padding read accepted");
        std::fill(native.begin(),native.end(),0xa7);
        Check(!ReplayLegacySpritesNative(c,native,{&draw,1},false).available, "Same-value unknown read escaped validation");
        c.target = {SceneIndexedTargetKind::Screen,144,0,8,6,8,0,0,8,6}; native = Native(c,packed);
        draw = Draw(-1,0,1);
        Check(!ReplayLegacySpritesNative(c,native,{&draw,1},false).available, "Allocation guard source read accepted");
        Check(ReplayLegacySpritesNative(c,native,{&draw,1},true).available, "Corrected origin draw failed");
        // Top-clipped vertical flips can copy from a captured row into the
        // preceding native row. Retain that write without treating the old
        // clear byte as a captured UI pixel, and prove the correction removes it.
        c.target = {SceneIndexedTargetKind::Screen,144,0,8,10,8,0,2,8,6}; native = Native(c,packed);
        size_t retained = 0;
        for (int y = -3; y <= 0; ++y) {
            draw = Draw(-1,y,3);
            const auto changed = ReplayLegacySpritesNative(c,native,{&draw,1},false);
            if (!changed.available) continue;
            for (const auto& change : changed.guard_changes) {
                Check(change.offset == -1 && change.before == 144, "Unexpected off-window diagnostic write");
                const auto subsequent = Draw(2,1,0);
                const auto later = ReplayLegacySpritesNative(c,changed.pixels,{&subsequent,1},false);
                const size_t native_offset = size_t(c.target.window_y) * c.target.pitch - 1;
                Check(later.available && later.pixels[native_offset] == change.after, "Later sprite lost native side effect");
                ++retained;
            }
            Check(ReplayLegacySpritesNative(c,native,{&draw,1},true).guard_changes.empty(), "Corrected side effect retained");
        }
        Check(retained > 0, "Known native off-window writes were not exercised");

        // Use an independently validated fixture's owned identities/tables;
        // exercise separated sprite runs around an actual original terrain run.
        auto mixed = *LoadSceneCapture(argv[1]);
        const auto identity = mixed.trace.draws.front().object;
        const auto kind = mixed.trace.entries[mixed.trace.draws.front().entry].kind;
        mixed.scene.camera.viewport_x = 3; mixed.scene.camera.viewport_y = 2;
        mixed.scene.camera.viewport_w = 8; mixed.scene.camera.viewport_h = 6;
        mixed.target = {SceneIndexedTargetKind::Screen,144,0,14,12,16,3,2,8,6};
        mixed.assets.sprites[0].width = mixed.assets.sprites[0].height = 2;
        mixed.assets.sprites[0].pixels = sprite.pixels;
        mixed.trace = {}; mixed.trace.complete = true;
        mixed.trace.entries = {{3000,kind},{2000,0},{1000,kind}};
        auto first = Draw(2,1,1); first.object = identity;
        auto last = Draw(4,0,0); last.object = identity; last.entry = 2;
        mixed.trace.draws = {first,last};
        SceneTerrainTriangle triangle; triangle.entry = 1; triangle.mode = 5;
        triangle.vertices = {{{0,0,0,0,65536},{8,0,65536,0,65536},{0,6,0,65536,65536}}};
        mixed.trace.raster.triangles = {triangle};
        mixed.trace.raster.commands = {{SceneDrawCommandKind::Sprite,0},{SceneDrawCommandKind::Triangle,0},{SceneDrawCommandKind::Sprite,1}};
        Check(ValidateSceneCapture(mixed), "Ordered mixed fixture invalid");
        const auto native_mixed = ReplayLegacyWorld(mixed,packed,true);
        auto historical = mixed; historical.format_version = 6; historical.target = {};
        const auto old_mixed = ReplayLegacyWorld(historical,packed,true);
        Check(native_mixed.available && native_mixed.native_stride && !old_mixed.native_stride
            && native_mixed.pixels == old_mixed.pixels, "Ordered terrain/sprite mixed replay differs");
        Check(ReplayLegacyWorld(mixed,packed,false).pixels == ReplayLegacyWorld(historical,packed,false).pixels,
            "Original ordered native mixed replay differs from packed interior");
        mixed.trace.draws.front().scale.frame_x = -1;
        const auto unavailable = ReplayLegacyWorld(mixed,packed,false);
        Check(!unavailable.available && unavailable.pixels.empty() && unavailable.unavailable_reads.front().command == 0,
            "Mixed unavailable original not propagated");
        Check(ReplayLegacyWorld(mixed,packed,true).available, "Independent corrected mixed replay failed");
        std::cout << "Native-stride CPU checks passed: padded interior, solid/remap sidebar/padding/guard reads, same-value read, full-width alias, retained off-window writes, corrected output, ordered mixed replay, historical path.\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
