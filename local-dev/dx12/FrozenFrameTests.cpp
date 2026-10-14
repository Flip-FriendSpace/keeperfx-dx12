// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#include "FrozenFrameTests.h"
#include <windows.h>
#include <fstream>
#include <stdexcept>

namespace kfx::render::dx12 {
namespace {
void Require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
struct Window {
    HWND handle = CreateWindowExW(0,L"STATIC",L"Frozen resource lifetime check",WS_POPUP,
        0,0,256,8,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    ~Window() { if (handle) DestroyWindow(handle); }
};
template<class F> void Invalid(F&& operation)
{
    bool rejected = false;
    try { operation(); } catch (const std::invalid_argument&) { rejected = true; }
    Require(rejected, "Invalid frozen frame was not rejected");
}
}
void RunFrozenFrameTests(Device& device, const Options& options, const std::filesystem::path& output,
    const std::function<void(uint32_t,uint32_t)>& resize)
{
    constexpr uint32_t width = 256, height = 8;
    Image atlas{256,256,std::vector<uint8_t>(256*256*4)};
    for (uint32_t x = 0; x < 256; ++x) {
        atlas.rgba[x*4] = uint8_t(x); atlas.rgba[x*4+3] = 255;
    }
    std::array<uint8_t,768> palette{};
    for (uint32_t x = 0; x < 256; ++x) {
        palette[x*3] = uint8_t(x%64); palette[x*3+1] = uint8_t((x/4)%64); palette[x*3+2] = uint8_t(63-x%64);
    }
    PalettePrimitive quad;
    quad.sprite = {0,2,width,4,0,0,256,1,PaletteBlend::Opaque};
    const std::vector<PalettePrimitive> commands{quad};
    std::vector<uint8_t> expected(width*height,144);
    for (uint32_t y = 2; y < 6; ++y) for (uint32_t x = 0; x < width; ++x) expected[y*width+x] = uint8_t(x);
    uint64_t index_checks = 0, colour_checks = 0;
    uint32_t presentations = 0, invalid_cases = 0, gpu_status_rejections = 0, culled_clear_cases = 0;
    uint32_t display_palette_cases = 0;
    std::string error;
    try {
        resize(width,height);
        const auto baseline = device.GetDiagnostics();
        for (bool ordered : {false,true}) {
            if (ordered && !baseline.rasterizer_ordered_views_supported) continue;
            auto frame = device.PrepareFrozenIndexedFrame(width,height,144,atlas,commands,palette,ordered);
            Require(frame.Valid() && frame.Width()==width && frame.Height()==height,"Invalid prepared frozen frame");
            auto retained = frame; frame.Reset();
            Require(!frame.Valid() && retained.Valid(),"Copied frozen ownership was not retained");
            auto before = device.GetDiagnostics();
            for (uint32_t i=0;i<7;++i) { Require(device.PresentFrozenFrame(retained),"Frozen presentation failed"); ++presentations; }
            auto after = device.GetDiagnostics();
            Require(after.frozen_preparations==before.frozen_preparations
                && after.frozen_image_readbacks==before.frozen_image_readbacks
                && after.frozen_presentations==before.frozen_presentations+7,
                "Frozen presentation rebuilt or read back image data");
            auto indices = device.ReadbackFrozenIndices(retained); index_checks += indices.size();
            Require(indices==expected,"Frozen index clear/opaque-zero/gradient differs");
            auto rgba = device.ReadbackFrozenRGBA(retained);
            Require(rgba.width==width && rgba.height==height,"Frozen RGBA size mismatch");
            for (size_t p=0;p<expected.size();++p) {
                for (size_t c=0;c<3;++c) {
                    ++colour_checks;
                    Require(rgba.rgba[p*4+c]==uint8_t(uint32_t(palette[expected[p]*3+c])*255/63),
                        "GPU palette expansion differs from exact integer six-bit conversion");
                }
                ++colour_checks; Require(rgba.rgba[p*4+3]==255,"GPU palette expansion changed alpha");
            }
            device.Resize(0,0);
            Require(!device.PresentFrozenFrame(retained),"Suspended frozen presentation did not stop");
            resize(width,height); Require(device.PresentFrozenFrame(retained),"Restored frozen frame failed"); ++presentations;
            resize(width+1,height); Invalid([&]{device.PresentFrozenFrame(retained);}); ++invalid_cases;
            resize(width,height);
            // Release the last caller immediately after queueing a presentation.
            Require(device.PresentFrozenFrame(retained),"Final retained presentation failed"); ++presentations;
            retained.Reset(); device.WaitIdle();
            Require(device.GetDiagnostics().allocation_count==baseline.allocation_count,"Frozen resources did not retire after the presentation fence");

            // These inputs pass CPU bounds/binary-mask checks, but every
            // covered shadow sample lies outside its initialized 1x1 extent.
            auto shadow_atlas = atlas;
            shadow_atlas.rgba[(128*256)*4] = 255;
            PalettePrimitive shadow; shadow.kind = PalettePrimitiveKind::Shadow;
            shadow.shadow.mask_y = 128;
            shadow.shadow.mask_width = shadow.shadow.mask_height = 1;
            shadow.shadow.fade_y = 64; shadow.shadow.shade = 16;
            shadow.shadow.vertices = {{{0,0,1,0},{float(width),0,1,0},{0,float(height),1,0}}};
            auto covering = quad; covering.sprite.y = 0; covering.sprite.height = height;
            for (bool overwrite : {false,true}) {
                std::vector<PalettePrimitive> invalid_commands{shadow};
                if (overwrite) invalid_commands.push_back(covering);
                const auto status_before = device.GetDiagnostics();
                bool rejected = false;
                try {
                    device.PrepareFrozenIndexedFrame(width,height,144,shadow_atlas,invalid_commands,palette,ordered);
                } catch (const std::runtime_error& failure) {
                    if (std::string(failure.what()) !=
                        "Palette compositor encountered an invalid index or shadow sample outside its initialized mask") throw;
                    rejected = true;
                }
                const auto status_after = device.GetDiagnostics();
                Require(rejected,"Frozen GPU status did not reject an invalid shadow sample");
                Require(status_after.frozen_preparations==status_before.frozen_preparations
                    && status_after.frozen_presentations==status_before.frozen_presentations
                    && status_after.frozen_image_readbacks==status_before.frozen_image_readbacks,
                    "Rejected frozen preparation published a frame or read back an image");
                Require(status_after.allocation_count==status_before.allocation_count
                    && status_after.allocation_bytes==status_before.allocation_bytes
                    && status_after.validation_errors==status_before.validation_errors
                    && status_after.validation_warnings==status_before.validation_warnings,
                    "Rejected frozen preparation retained resources or emitted GPU validation diagnostics");
                ++invalid_cases; ++gpu_status_rejections;
            }

            // A reverse-wound shadow emits no vertices. Preparation must still
            // clear the resident index target and expand its palette on GPU.
            shadow.shadow.vertices = {{{0,0,0,0},{0,float(height),0,0},{float(width),0,0,0}}};
            const auto culled_before = device.GetDiagnostics();
            auto culled = device.PrepareFrozenIndexedFrame(width,height,144,shadow_atlas,{shadow},palette,ordered);
            const auto clear_indices = device.ReadbackFrozenIndices(culled);
            index_checks += clear_indices.size();
            Require(clear_indices==std::vector<uint8_t>(width*height,144),"All-culled frozen stream did not preserve the explicit clear");
            const auto clear_rgba = device.ReadbackFrozenRGBA(culled);
            Require(clear_rgba.width==width && clear_rgba.height==height
                && clear_rgba.rgba.size()==size_t(width)*height*4,"All-culled frozen RGBA size mismatch");
            for (size_t p=0;p<clear_indices.size();++p) {
                for (size_t c=0;c<3;++c) {
                    ++colour_checks;
                    Require(clear_rgba.rgba[p*4+c]==uint8_t(uint32_t(palette[144*3+c])*255/63),
                        "All-culled frozen palette expansion differs");
                }
                ++colour_checks; Require(clear_rgba.rgba[p*4+3]==255,"All-culled frozen alpha differs");
            }
            culled.Reset(); device.WaitIdle();
            const auto culled_after = device.GetDiagnostics();
            Require(culled_after.frozen_preparations==culled_before.frozen_preparations+1
                && culled_after.frozen_image_readbacks==culled_before.frozen_image_readbacks+2
                && culled_after.frozen_presentations==culled_before.frozen_presentations,
                "All-culled frozen operation counters differ");
            Require(culled_after.allocation_count==culled_before.allocation_count
                && culled_after.allocation_bytes==culled_before.allocation_bytes
                && culled_after.validation_errors==culled_before.validation_errors
                && culled_after.validation_warnings==culled_before.validation_warnings,
                "All-culled frozen stream retained resources or emitted GPU validation diagnostics");
            ++culled_clear_cases;
        }
        FrozenGpuFrame empty;
        Invalid([&]{device.PresentFrozenFrame(empty);}); Invalid([&]{device.ReadbackFrozenIndices(empty);});
        Invalid([&]{device.ReadbackFrozenRGBA(empty);}); invalid_cases+=3;
        auto malformed_palette=palette;malformed_palette[11]=64;
        Invalid([&]{device.PrepareFrozenIndexedFrame(width,height,144,atlas,commands,malformed_palette,false);}); ++invalid_cases;
        // An actual GPU display palette can contain every eight-bit colour.
        // Preserve it exactly instead of guessing a six-bit inverse palette.
        std::array<uint8_t,1024> display_rgba{};
        for (size_t i=0;i<256;++i) {
            display_rgba[i*4]=uint8_t(i);
            display_rgba[i*4+1]=uint8_t(255-i);
            display_rgba[i*4+2]=uint8_t(i^0x5a);
            display_rgba[i*4+3]=255;
        }
        for (bool ordered : {false,true}) {
            if (ordered && !baseline.rasterizer_ordered_views_supported) continue;
            auto frame=device.PrepareFrozenIndexedFrame(width,height,144,atlas,commands,display_rgba,ordered);
            const auto actual_indices=device.ReadbackFrozenIndices(frame);
            Require(actual_indices==expected,"Eight-bit display palette changed source indices");
            index_checks+=actual_indices.size();
            const auto actual=device.ReadbackFrozenRGBA(frame);
            for (size_t p=0;p<expected.size();++p) for (size_t c=0;c<4;++c) {
                Require(actual.rgba[p*4+c]==display_rgba[size_t(expected[p])*4+c],
                    "Actual eight-bit display palette was quantized or changed opacity");
                ++colour_checks;
            }
            frame.Reset();device.WaitIdle();++display_palette_cases;
            Require(device.GetDiagnostics().allocation_count==baseline.allocation_count,
                "Eight-bit display palette retained resources");
        }
        auto translucent=display_rgba;translucent[3]=0;
        Invalid([&]{device.PrepareFrozenIndexedFrame(width,height,144,atlas,commands,translucent,false);});++invalid_cases;
        // The bounded frozen fixture is not an unbounded cache. Dropping a
        // caller releases its slot once no queued GPU presentation retains it.
        std::vector<FrozenGpuFrame> budget;
        for(int i=0;i<8;++i) budget.push_back(device.PrepareFrozenIndexedFrame(width,height,144,atlas,commands,palette,false));
        Invalid([&]{device.PrepareFrozenIndexedFrame(width,height,144,atlas,commands,palette,false);}); ++invalid_cases;
        budget.pop_back();budget.push_back(device.PrepareFrozenIndexedFrame(width,height,144,atlas,commands,palette,false));
        Window secondary_window;Require(secondary_window.handle!=nullptr,"Cannot create secondary lifetime window");
        auto secondary=std::make_unique<Device>();secondary->Initialize(secondary_window.handle,width,height,options);
        Invalid([&]{secondary->PresentFrozenFrame(budget.front());});
        Invalid([&]{secondary->ReadbackFrozenIndices(budget.front());});invalid_cases+=2;
        auto expired=secondary->PrepareFrozenIndexedFrame(width,height,144,atlas,commands,palette,false);
        // A hidden window may report DXGI_STATUS_OCCLUDED after successfully
        // submitting the copy. The lifetime check does not require visibility.
        secondary->PresentFrozenFrame(expired);
        secondary->WaitIdle();
        const auto secondary_diagnostics=secondary->GetDiagnostics();
        Require(!secondary_diagnostics.validation_errors && !secondary_diagnostics.validation_warnings,
            "Secondary frozen device emitted validation diagnostics");
        secondary.reset();
        Require(!expired.Valid() && expired.Width()==0 && expired.Height()==0,"Frame outlived its GPU device resources");
        Invalid([&]{device.ReadbackFrozenRGBA(expired);});++invalid_cases;expired.Reset();
        budget.clear();device.WaitIdle();
        Require(device.GetDiagnostics().allocation_count==baseline.allocation_count,"Frozen budget tests leaked resources");
    } catch(const std::exception& failure) { error=failure.what(); }
    std::ofstream report(output/"frozen-frame.json");
    report << "{\"schema\":1,\"passed\":" << (error.empty()?"true":"false")
        << ",\"scope\":\"GPU-resident frozen indexed preparation, palette expansion and direct presentation; no runtime asset-cache or live-game acceptance\""
        << ",\"index_checks\":" << index_checks << ",\"colour_byte_checks\":" << colour_checks
        << ",\"presentations\":" << presentations << ",\"invalid_cases\":" << invalid_cases
        << ",\"gpu_status_rejections\":" << gpu_status_rejections << ",\"culled_clear_cases\":" << culled_clear_cases
        << ",\"display_palette_cases\":" << display_palette_cases
        << ",\"gate2Accepted\":false,\"gate3Accepted\":false,\"error\":\"";
    for(char c:error)report << ((c=='"'||c=='\\'||c<32)?'?':c);
    report << "\"}\n";Require(bool(report),"Cannot write frozen-frame report");
    if(!error.empty())throw std::runtime_error(error);
}
}
