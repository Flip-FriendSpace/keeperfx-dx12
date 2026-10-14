// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#include "FrozenWorld.h"
#include "SceneIndexedFrame.h"
#include "LegacyWorldOracle.h"
#include <algorithm>
#include <fstream>
#include <stdexcept>

namespace kfx::render::dx12 {
namespace {
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void Bytes(const std::filesystem::path& path, const std::vector<uint8_t>& bytes)
{
    std::ofstream file(path,std::ios::binary);
    file.write(reinterpret_cast<const char*>(bytes.data()),std::streamsize(bytes.size()));
    Require(bool(file),"Cannot retain frozen world bytes");
}
void Bitmap(const std::filesystem::path& path, const Image& image)
{
    std::ofstream file(path,std::ios::binary);
    auto word=[&](uint16_t n){file.put(char(n));file.put(char(n>>8));};
    auto dword=[&](uint32_t n){word(uint16_t(n));word(uint16_t(n>>16));};
    file.put('B');file.put('M');dword(uint32_t(54+image.rgba.size()));
    word(0);word(0);dword(54);dword(40);dword(image.width);dword(image.height);
    word(1);word(32);dword(0);dword(uint32_t(image.rgba.size()));dword(2835);dword(2835);dword(0);dword(0);
    for(uint32_t y=image.height;y-->0;)for(uint32_t x=0;x<image.width;++x){
        const auto p=(size_t(y)*image.width+x)*4;
        file.put(char(image.rgba[p+2]));file.put(char(image.rgba[p+1]));
        file.put(char(image.rgba[p]));file.put(char(image.rgba[p+3]));
    }
    Require(bool(file),"Cannot retain frozen world image");
}
Image Colour(uint32_t width,uint32_t height,const std::vector<uint8_t>& indices,const std::array<uint8_t,768>& palette)
{
    Require(indices.size()==size_t(width)*height,"Frozen world index dimensions differ");
    Image image{width,height,std::vector<uint8_t>(indices.size()*4)};
    for(size_t p=0;p<indices.size();++p){
        for(size_t c=0;c<3;++c)image.rgba[p*4+c]=uint8_t(uint32_t(palette[indices[p]*3+c])*255/63);
        image.rgba[p*4+3]=255;
    }
    return image;
}
uint64_t Differences(const std::vector<uint8_t>& a,const std::vector<uint8_t>& b)
{
    Require(a.size()==b.size(),"Frozen comparison dimensions differ");
    uint64_t count=0;for(size_t p=0;p<a.size();++p)count+=a[p]!=b[p];return count;
}
std::string Json(const std::string& value)
{
    std::string result="\"";
    for(unsigned char c:value){
        if(c=='"'||c=='\\'){result+='\\';result+=char(c);}else result+=c<32?'?':char(c);
    }
    return result+'"';
}
}
FrozenGpuFrame PrepareFrozenWorld(Device& device, const std::filesystem::path& path,
    const std::filesystem::path& output)
{
    const auto capture=LoadSceneCapture(path);
    Require(capture->format_version==8 && capture->trace.backend==0 && ValidateSceneCapture(*capture),
        "Frozen world requires v8 software target/clear proof; older diagnostic backgrounds are not accepted");
    const auto& target=capture->target;
    const uint32_t width=uint32_t(target.window_w),height=uint32_t(target.window_h);
    auto upload=BuildIndexedSceneUpload(*capture);
    const auto before=device.GetDiagnostics();
    auto frame=device.PrepareFrozenIndexedFrame(width,height,target.clear_index,upload.atlas,
        upload.primitives,capture->assets.display_palette,false);
    const auto serial=device.ReadbackFrozenIndices(frame);
    const auto rgba=device.ReadbackFrozenRGBA(frame);
    uint64_t path_checks=0;
    if(before.rasterizer_ordered_views_supported){
        auto ordered=device.PrepareFrozenIndexedFrame(width,height,target.clear_index,upload.atlas,
            upload.primitives,capture->assets.display_palette,true);
        const auto rov=device.ReadbackFrozenIndices(ordered);
        Bytes(output/"frozen-rov.idx",rov);
        Require(rov==serial,"Frozen ROV output differs from serial painter order");
        path_checks=serial.size();
        frame=std::move(ordered);
    }
    // These reads are diagnostic only. No CPU-composed image is uploaded for
    // display; the returned handle retains both GPU index and colour targets.
    Require(rgba.rgba==Colour(width,height,serial,capture->assets.display_palette).rgba,
        "Frozen world GPU palette expansion differs from integer conversion");
    Bytes(output/"frozen-gpu.idx",serial);Bytes(output/"frozen-gpu.rgba",rgba.rgba);
    Bitmap(output/"frozen-gpu.bmp",rgba);
    const std::vector<uint8_t> background(size_t(width)*height,target.clear_index);
    const auto original=ReplayLegacyWorld(*capture,background,false);
    const auto corrected=ReplayLegacyWorld(*capture,background,true);
    Require(corrected.available,"Corrected original world reference could not be evaluated safely");
    Require(original.native_stride && corrected.native_stride,"V8 comparison did not use the native target layout");
    if(original.available){
        Bytes(output/"frozen-original.idx",original.pixels);
        Bitmap(output/"frozen-original.bmp",Colour(width,height,original.pixels,capture->assets.display_palette));
    }
    Bytes(output/"frozen-corrected.idx",corrected.pixels);
    Bitmap(output/"frozen-corrected.bmp",Colour(width,height,corrected.pixels,capture->assets.display_palette));
    Image difference{width,height,std::vector<uint8_t>(serial.size()*4)};
    for(size_t p=0;p<serial.size();++p){
        difference.rgba[p*4]=difference.rgba[p*4+2]=serial[p]==corrected.pixels[p]?0:255;
        difference.rgba[p*4+3]=255;
    }
    Bitmap(output/"frozen-difference.bmp",difference);
    const auto after=device.GetDiagnostics();
    const uint64_t corrected_differences=Differences(serial,corrected.pixels);
    std::ofstream report(output/"frozen-world.json");
    report<<"{\"schema\":2,\"preparationPassed\":true,\"gate2Accepted\":false,\"visualAccepted\":false"
        <<",\"scope\":\"Captured software world viewport before UI, possession swipe and lens composition; resident GPU output with separate original-C diagnostics\""
        <<",\"sourceLimitations\":\"Canonical sprite RLE reconstruction; original packet boundaries unavailable. V8 original-C comparison retains native target pitch and window; outside-viewport contents are not captured, and unsafe source reads make the original comparison unavailable. Corrected oracle changes only the known mirrored row-copy overrun. Hardware terrain/shadow scan conversion remains a separate visual comparison.\""
        <<",\"originalAvailable\":"<<(original.available?"true":"false")<<",\"originalUnavailableReason\":"<<Json(original.unavailable_reason)
        <<",\"originalNativeStride\":true,\"correctedNativeStride\":true"
        <<",\"formatVersion\":"<<capture->format_version<<",\"epoch\":"<<capture->scene.epoch<<",\"sequence\":"<<capture->scene.sequence
        <<",\"targetKind\":"<<unsigned(target.kind)<<",\"clearIndex\":"<<unsigned(target.clear_index)<<",\"lensId\":"<<target.lens_id
        <<",\"targetWidth\":"<<target.width<<",\"targetHeight\":"<<target.height<<",\"targetPitch\":"<<target.pitch
        <<",\"viewport\":["<<target.window_x<<','<<target.window_y<<','<<width<<','<<height<<']'
        <<",\"commands\":"<<upload.primitives.size()<<",\"triangles\":"<<upload.triangles.size()<<",\"shadowTriangles\":"<<upload.shadows.size()
        <<",\"spriteDraws\":"<<capture->trace.draws.size()<<",\"clippedSprites\":"<<upload.clipped_sprites
        <<",\"excludedSpriteCalls\":"<<upload.excluded_sprite_calls<<",\"excludedShadowEntries\":"<<upload.excluded_shadow_entries
        <<",\"unrenderedUiBucketKinds\":[4,5,7,8,10],\"bucketCounts\":[";
    for(size_t i=0;i<upload.bucket_counts.size();++i){if(i)report<<',';report<<upload.bucket_counts[i];}
    report<<"],\"pixels\":"<<serial.size()<<",\"exactRovSerialIndexChecks\":"<<path_checks
        <<",\"exactPaletteByteChecks\":"<<rgba.rgba.size()<<",\"originalDifferingIndices\":"
        <<(original.available?std::to_string(Differences(serial,original.pixels)):"null")
        <<",\"correctedDifferingIndices\":"<<corrected_differences<<",\"originalCorrectionDifferingIndices\":"
        <<(original.available?std::to_string(Differences(original.pixels,corrected.pixels)):"null")
        <<",\"preparations\":"<<after.frozen_preparations-before.frozen_preparations
        <<",\"diagnosticImageReadbacks\":"<<after.frozen_image_readbacks-before.frozen_image_readbacks
        <<",\"originalGuardChanges\":[";
    for(size_t i=0;i<original.guard_changes.size();++i){
        const auto& g=original.guard_changes[i];if(i)report<<',';
        report<<"{\"command\":"<<g.command<<",\"offset\":"<<g.offset<<",\"before\":"<<unsigned(g.before)<<",\"after\":"<<unsigned(g.after)<<'}';
    }
    report<<"],\"originalUnavailableReads\":[";
    for(size_t i=0;i<original.unavailable_reads.size();++i){
        const auto& read=original.unavailable_reads[i];if(i)report<<',';
        report<<"{\"command\":"<<read.command<<",\"sourceOffset\":"<<read.source_offset
            <<",\"destinationOffset\":"<<read.destination_offset<<",\"length\":"<<read.length<<'}';
    }
    report<<"]}\n";Require(bool(report),"Cannot retain frozen world report");
    return frame;
}
}
