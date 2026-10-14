// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#include "ShadowOperatorTests.h"
#include <algorithm>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace kfx::render::dx12 {
namespace {
void Require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
uint8_t Fade(uint8_t destination, uint8_t shade)
{
    return uint8_t(destination * (shade - 13) + shade * 17);
}
void Texel(Image& atlas, uint32_t x, uint32_t y, uint8_t index)
{
    const size_t p = (size_t(y) * atlas.width + x) * 4;
    atlas.rgba.at(p) = index; atlas.rgba.at(p + 3) = 255;
}
PalettePrimitive Shadow(std::array<PaletteShadowVertex, 3> points, uint8_t shade = 16)
{
    PalettePrimitive result;
    result.kind = PalettePrimitiveKind::Shadow;
    result.shadow.vertices = points;
    result.shadow.mask_y = 320; result.shadow.mask_width = result.shadow.mask_height = 256;
    result.shadow.fade_y = 256; result.shadow.shade = shade;
    return result;
}
std::vector<PalettePrimitive> Square(float left, float top, float right, float bottom, uint8_t shade = 16)
{
    return {Shadow({{{left,top,0,0},{right,top,0,0},{right,bottom,0,0}}}, shade),
        Shadow({{{left,top,0,0},{right,bottom,0,0},{left,bottom,0,0}}}, shade)};
}
struct Runner {
    Device& device;
    std::filesystem::path output;
    bool rov;
    uint64_t checks = 0;
    std::vector<std::string> records;
    std::vector<uint8_t> Render(uint32_t w, uint32_t h, const std::vector<uint8_t>& background,
        const Image& atlas, const std::vector<PalettePrimitive>& commands, bool ordered)
    {
        const auto before = device.GetDiagnostics();
        auto result = device.CompositeIndexed(w, h, background, atlas, commands, ordered);
        const auto after = device.GetDiagnostics();
        Require(before.allocation_count == after.allocation_count && before.allocation_bytes == after.allocation_bytes,
            "Shadow operator retained GPU allocations");
        return result;
    }
    void Exact(const std::string& name, uint32_t w, uint32_t h, const std::vector<uint8_t>& background,
        const Image& atlas, const std::vector<PalettePrimitive>& commands, const std::vector<uint8_t>& expected)
    {
        Require(expected.size() == size_t(w) * h, "Invalid shadow expected image");
        std::vector<uint8_t> serial;
        for (bool ordered : {false, true}) {
            if (ordered && !rov) continue;
            const auto actual = Render(w, h, background, atlas, commands, ordered);
            uint64_t differences = 0;
            for (size_t i = 0; i < actual.size(); ++i) differences += actual[i] != expected[i];
            checks += actual.size();
            if (ordered) { Require(actual == serial, "Shadow ROV/serial mismatch"); checks += actual.size(); }
            else serial = actual;
            records.push_back("{\"name\":\"" + name + "\",\"path\":\"" + (ordered ? "rov" : "serial")
                + "\",\"pixels\":" + std::to_string(actual.size()) + ",\"differences\":" + std::to_string(differences) + "}");
            if (differences) {
                for (const auto& pair : {std::pair{"actual", &actual}, std::pair{"expected", &expected}}) {
                    std::ofstream file(output / ("shadow-" + name + "-" + pair.first + ".idx"), std::ios::binary);
                    file.write(reinterpret_cast<const char*>(pair.second->data()), std::streamsize(pair.second->size()));
                }
                throw std::runtime_error("Shadow operator mismatch: " + name);
            }
        }
    }
    void Rejected(const std::string& name, const Image& atlas, const std::vector<PalettePrimitive>& commands,
        bool gpu_sample_error = false)
    {
        for (bool ordered : {false, true}) {
            if (ordered && !rov) continue;
            const auto before = device.GetDiagnostics();
            bool rejected = false;
            try { device.CompositeIndexed(32, 32, std::vector<uint8_t>(1024, 41), atlas, commands, ordered); }
            catch (const std::invalid_argument&) { if (gpu_sample_error) throw; rejected = true; }
            catch (const std::runtime_error& error) {
                if (!gpu_sample_error || std::string(error.what()) !=
                    "Palette compositor encountered an invalid index or shadow sample outside its initialized mask") throw;
                rejected = true;
            }
            const auto after = device.GetDiagnostics();
            Require(rejected && before.allocation_count == after.allocation_count && before.allocation_bytes == after.allocation_bytes
                && before.validation_errors == after.validation_errors && before.validation_warnings == after.validation_warnings,
                "Invalid shadow input not rejected without retained allocations");
            records.push_back("{\"name\":\"" + name + "\",\"path\":\"" + (ordered ? "rov" : "serial") + "\",\"rejected\":true}");
        }
    }
};
void Fixtures(Runner& run)
{
    Image atlas{512, 576, std::vector<uint8_t>(512 * 576 * 4)};
    for (uint32_t y = 0; y < 256; ++y) for (uint32_t x = 0; x < 256; ++x) {
        Texel(atlas, x, y, uint8_t(x * 3 + y * 7 + 11));
        Texel(atlas, x, y + 320, 255);
    }
    for (uint32_t y = 0; y < 64; ++y) for (uint32_t x = 0; x < 256; ++x) Texel(atlas, x, y + 256, Fade(uint8_t(x), uint8_t(y)));
    Texel(atlas, 300, 320, 0); // Opaque palette-zero sprite remains independent of shadow mask coverage.
    std::vector<uint8_t> destinations(256);
    for (unsigned i = 0; i < 256; ++i) destinations[i] = uint8_t(i);
    for (uint8_t shade = 16; shade < 32; ++shade) {
        auto expected = destinations;
        for (auto& value : expected) value = Fade(value, shade);
        run.Exact("all-destinations-shade-" + std::to_string(shade), 256, 1, destinations, atlas, Square(0,0,256,1,shade), expected);
    }
    constexpr uint32_t w = 32, h = 32;
    std::vector<uint8_t> background(w * h), expected;
    for (size_t i = 0; i < background.size(); ++i) background[i] = uint8_t(i * 13 + 29);
    auto square = Square(2, 2, 30, 30);
    auto remap = [&](std::vector<uint8_t>& image, uint8_t shade) {
        for (uint32_t y = 2; y < 30; ++y) for (uint32_t x = 2; x < 30; ++x) image[y * w + x] = Fade(image[y * w + x], shade);
    };
    expected = background; remap(expected, 16);
    run.Exact("shared-diagonal-once", w, h, background, atlas, square, expected);
    auto twice = square; twice.insert(twice.end(), square.begin(), square.end()); remap(expected, 16);
    run.Exact("repeated-shadow", w, h, background, atlas, twice, expected);
    auto second = Square(2,2,30,30,23);
    auto forward = square; forward.insert(forward.end(), second.begin(), second.end());
    auto reverse = second; reverse.insert(reverse.end(), square.begin(), square.end());
    expected = background; remap(expected,16); remap(expected,23);
    run.Exact("two-shades-forward",w,h,background,atlas,forward,expected);
    auto reversed_expected = background; remap(reversed_expected,23); remap(reversed_expected,16);
    Require(expected != reversed_expected, "Shadow order fixture is commutative");
    run.Exact("two-shades-reversed",w,h,background,atlas,reverse,reversed_expected);
    auto culled = square;
    for (auto& command : culled) std::swap(command.shadow.vertices[1], command.shadow.vertices[2]);
    run.Exact("reverse-winding",w,h,background,atlas,culled,background);
    auto degenerate = square[0]; degenerate.shadow.vertices[2] = degenerate.shadow.vertices[1];
    run.Exact("degenerate",w,h,background,atlas,{degenerate},background);

    // Actual affine UVs, independent analytic samples, holes and negative wrapping.
    auto patterned = atlas;
    for (uint32_t y = 0; y < 256; ++y) for (uint32_t x = 0; x < 256; ++x)
        Texel(patterned,x,320+y,((x/3+y/5)&1) ? 255 : 0);
    auto uv = Square(0,0,32,32);
    for (auto& command : uv) for (auto& vertex : command.shadow.vertices) { vertex.u=256-vertex.x; vertex.v=vertex.y; }
    expected=background;
    for (uint32_t y=0;y<h;++y) for(uint32_t x=0;x<w;++x) if (((255-x)/3+y/5)&1) expected[y*w+x]=Fade(expected[y*w+x],16);
    run.Exact("affine-mirrored-mask-holes",w,h,background,patterned,uv,expected);
    auto clipped=Square(-4,-3,28,29);
    for(auto& command:clipped) for(auto& vertex:command.shadow.vertices) {vertex.u=vertex.x+4;vertex.v=vertex.y+3;}
    expected=background;
    for(uint32_t y=0;y<29;++y) for(uint32_t x=0;x<28;++x) if(((x+4)/3+(y+3)/5)&1) expected[y*w+x]=Fade(expected[y*w+x],16);
    run.Exact("clipped-mask-phase",w,h,background,patterned,clipped,expected);
    auto wrapped=Square(0,0,32,32);
    for(auto& command:wrapped) for(auto& vertex:command.shadow.vertices) {vertex.u=-0.25f;vertex.v=257.25f;}
    expected=background;
    if((255/3+1/5)&1) for(auto& value:expected)value=Fade(value,16);
    run.Exact("negative-eight-bit-wrap",w,h,background,patterned,wrapped,expected);

    // Terrain, shadow, opaque zero sprite, then another shadow: each destination matters.
    PalettePrimitive terrain; terrain.kind=PalettePrimitiveKind::Terrain;
    terrain.terrain.texture_x=320; terrain.terrain.texture_y=320; terrain.terrain.apply_fade=false;
    terrain.terrain.vertices={{{0,0,0,0,0},{32,0,0,0,0},{0,32,0,0,0}}};
    Texel(atlas,320,320,61);
    PalettePrimitive sprite; sprite.sprite={8,8,16,16,300,320,1,1,PaletteBlend::Opaque};
    auto mixed=std::vector<PalettePrimitive>{terrain,square[0],square[1],sprite,second[0],second[1]};
    expected=background;
    for(uint32_t y=0;y<h;++y)for(uint32_t x=0;x<w;++x)if(x+y<31)expected[y*w+x]=61;
    remap(expected,16);
    for(uint32_t y=8;y<24;++y)for(uint32_t x=8;x<24;++x)expected[y*w+x]=0;
    remap(expected,23);
    run.Exact("terrain-shadow-zero-sprite-shadow",w,h,background,atlas,mixed,expected);
    // Wide raw ramps used by independent shadow UV diagnostics must bypass fade.
    auto ramp=atlas;
    for(uint32_t y=0;y<256;++y)for(uint32_t x=0;x<256;++x)Texel(ramp,x,y+320,uint8_t(x^y));
    auto raw=terrain;raw.terrain.texture_x=0;raw.terrain.texture_width=raw.terrain.texture_height=256;
    for(auto& vertex:raw.terrain.vertices){vertex.u=-1.25f;vertex.v=257.25f;}
    expected=background;
    for(uint32_t y=0;y<h;++y)for(uint32_t x=0;x<w;++x)if(x+y<31)expected[y*w+x]=255;
    run.Exact("raw-wide-ramp-no-fade",w,h,background,ramp,{raw},expected);

    for(unsigned bad=0;bad<7;++bad){
        auto invalid=square[0];
        switch(bad){
        case 0:invalid.shadow.shade=15;break;
        case 1:invalid.shadow.shade=32;break;
        case 2:invalid.shadow.mask_width=0;break;
        case 3:invalid.shadow.mask_height=257;break;
        case 4:invalid.shadow.mask_y=atlas.height-255;break;
        case 5:invalid.shadow.vertices[0].u=std::numeric_limits<float>::quiet_NaN();break;
        case 6:invalid.shadow.fade_x=atlas.width-255;break;
        }
        run.Rejected("invalid-shadow-"+std::to_string(bad),atlas,{invalid});
    }
    auto nonbinary=atlas;Texel(nonbinary,0,320,1);
    run.Rejected("nonbinary-mask",nonbinary,{square[0]});
    auto outside=square[0];outside.shadow.mask_width=outside.shadow.mask_height=1;
    for(auto& vertex:outside.shadow.vertices)vertex.u=1;
    run.Rejected("sample-outside-initialized-mask",atlas,{outside},true);
    sprite.sprite={0,0,32,32,300,320,1,1,PaletteBlend::Opaque};
    run.Rejected("mask-error-survives-opaque-sprite",atlas,{outside,sprite},true);
    auto terrain_second=terrain;
    terrain_second.terrain.vertices={{{32,0,0,0,0},{32,32,0,0,0},{0,32,0,0,0}}};
    run.Rejected("mask-error-survives-opaque-terrain",atlas,{outside,terrain,terrain_second},true);
}
}
void RunShadowOperatorTests(Device& device, const std::filesystem::path& output)
{
    Runner run{device,output,device.GetDiagnostics().rasterizer_ordered_views_supported,0,{}};
    std::string error;
    try { Fixtures(run); } catch(const std::exception& failure) {error=failure.what();}
    std::ofstream report(output/"shadow-operator.json");
    report<<"{\"schema\":1,\"passed\":"<<(error.empty()?"true":"false")<<",\"scope\":\"Exact masked destination fade and ordering; no original-raster or world acceptance\",\"indexed_checks\":"<<run.checks
        <<",\"error\":\"";
    for(char c:error)report<<(c=='"'||c=='\\'?'?':c);
    report<<"\",\"cases\":[";
    for(size_t i=0;i<run.records.size();++i){if(i)report<<',';report<<run.records[i];}
    report<<"]}\n";
    Require(bool(report),"Cannot retain shadow operator report");
    if(!error.empty())throw std::runtime_error(error);
}
}
