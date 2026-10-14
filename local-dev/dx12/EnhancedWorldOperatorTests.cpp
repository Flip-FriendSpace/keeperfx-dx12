// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#include "EnhancedWorldOperatorTests.h"
#include "EnhancedWorldGLOracle.h"
#include "EnhancedSpriteTestImages.h"
#include <algorithm>
#include <limits>

namespace kfx::render::dx12 {
namespace {
using namespace enhanced_test;
constexpr std::array<float,9> Identity{1,0,0,0,1,0,0,0,1};
EnhancedWorldPass Base()
{
    EnhancedWorldPass p;p.native_width=80;p.native_height=64;
    p.viewport_x=7;p.viewport_y=9;p.viewport_width=64;p.viewport_height=48;
    p.clear_rgba={31.0f/255,47.0f/255,61.0f/255,1};
    p.lightmap.assign(511*511,8192);p.tiles.resize(2);
    for(unsigned i=0;i<256;++i){
        p.palette_rgba[i*4]=uint8_t(i);p.palette_rgba[i*4+1]=uint8_t(255-i);
        p.palette_rgba[i*4+2]=uint8_t(i^91);p.palette_rgba[i*4+3]=255;
        for(unsigned row=0;row<64;++row)p.fade[row*256+i]=uint8_t(row==32?i:(i+row)%256);
    }
    p.palette_rgba[200*4+1]=56;p.palette_rgba[200*4+2]=148;
    for(unsigned y=0;y<32;++y)for(unsigned x=0;x<32;++x)p.tiles[0][y*32+x]=uint8_t(x*7+y*13);
    p.tiles[1].fill(200);
    const EnhancedTerrainVertex a{{-1,1,0,1},-16,-32,1,-1,-1},b{{1,1,0,1},48,-32,1,-1,-1},
        c{{1,-1,0,1},48,16,1,-1,-1},d{{-1,-1,0,1},-16,16,1,-1,-1};
    p.terrain={{{a,b,c},0},{{a,c,d},0}};
    return p;
}
Image Clear(const EnhancedWorldPass& p)
{
    Image image{p.viewport_width,p.viewport_height,std::vector<uint8_t>(size_t(p.viewport_width)*p.viewport_height*4)};
    for(size_t i=0;i<image.rgba.size();++i)image.rgba[i]=uint8_t(std::lround(p.clear_rgba[i%4]*255));
    return image;
}
Image Flat(const EnhancedWorldPass& p,uint8_t index)
{
    auto image=Clear(p);
    for(size_t i=0;i<image.rgba.size();i+=4)for(size_t c=0;c<4;++c)image.rgba[i+c]=c==3?255:p.palette_rgba[index*4+c];
    return image;
}
struct Runner {
    Device& device;std::filesystem::path output;uint64_t pixels=0,differences=0;unsigned invalid=0;
    EnhancedFadeAddressing fade_addressing;
    std::vector<std::string> records;
    Runner(Device& d,const std::filesystem::path& p,EnhancedFadeAddressing mode):device(d),output(p),fade_addressing(mode){}
    std::array<Image,2> Compare(const std::string& name,const EnhancedWorldPass& world,const Image& atlas,
        const std::vector<OutlinePassDraw>& draws={},const Image* expected=nullptr,
        EnhancedWorldRasterOrigin origin=EnhancedWorldRasterOrigin::ViewportLocal,bool boundary_probe=false)
    {
        const auto before=device.GetDiagnostics();
        auto submitted=world;submitted.fade_addressing=fade_addressing;
        auto resident=device.PrepareFrozenEnhancedWorldFrame(submitted,atlas,{world.palette_rgba},Identity,draws,origin);
        const auto prepared=device.GetDiagnostics();
        Require(resident.Valid()&&resident.Width()==world.viewport_width&&resident.Height()==world.viewport_height
            &&prepared.frozen_preparations==before.frozen_preparations+1
            &&prepared.frozen_image_readbacks==before.frozen_image_readbacks,"Enhanced preparation did not retain an unread GPU frame");
        bool no_indices=false;
        try{device.ReadbackFrozenIndices(resident);}catch(const std::invalid_argument&){no_indices=true;}
        Require(no_indices&&device.GetDiagnostics().frozen_image_readbacks==before.frozen_image_readbacks,
            "Colour-only enhanced frame exposed an index surface");
        auto gpu=device.ReadbackFrozenRGBA(resident);resident.Reset();device.WaitIdle();
        const auto after=device.GetDiagnostics();
        Require(after.allocation_count==before.allocation_count&&after.allocation_bytes==before.allocation_bytes
            &&after.validation_errors==before.validation_errors&&after.validation_warnings==before.validation_warnings,
            "Enhanced world retained resources or produced validation messages");
        const auto gl=RenderEnhancedWorldGLReference(world,atlas,{world.palette_rgba},Identity,draws);
        const auto difference=Differences(gpu,gl);
        const auto gpu_analytic=expected?Differences(gpu,*expected):0,gl_analytic=expected?Differences(gl,*expected):0;
        const auto analytic=gpu_analytic+gl_analytic;
        pixels+=uint64_t(gpu.width)*gpu.height;differences+=difference;
        unsigned maximum_delta=0;uint64_t alpha=0;
        Image mask{gpu.width,gpu.height,std::vector<uint8_t>(gpu.rgba.size())};
        for(size_t p=0;p<gpu.rgba.size();p+=4){bool changed=false;
            for(size_t c=0;c<4;++c){const unsigned delta=unsigned(std::abs(int(gpu.rgba[p+c])-int(gl.rgba[p+c])));
                maximum_delta=std::max(maximum_delta,delta);changed|=delta!=0;if(c==3)alpha+=delta!=0;}
            mask.rgba[p]=mask.rgba[p+2]=changed?255:0;mask.rgba[p+3]=255;
        }
        records.push_back("{\"name\":"+Json(name)+",\"pixels\":"+std::to_string(uint64_t(gpu.width)*gpu.height)
            +",\"differingBytes\":"+std::to_string(difference)+",\"alphaDifferingBytes\":"+std::to_string(alpha)
            +",\"maximumByteDelta\":"+std::to_string(maximum_delta)+",\"analyticDifferingBytes\":"+std::to_string(analytic)
            +",\"gpuAnalyticDifferingBytes\":"+std::to_string(gpu_analytic)+",\"glAnalyticDifferingBytes\":"+std::to_string(gl_analytic)
            +",\"fadeBoundaryProbe\":"+(boundary_probe?"true":"false")+"}");
        Save(output/("world-"+name+"-gpu"),gpu);Save(output/("world-"+name+"-gl"),gl);
        Save(output/("world-"+name+"-difference"),mask);if(expected)Save(output/("world-"+name+"-expected"),*expected);
        if(boundary_probe){
            if(fade_addressing==EnhancedFadeAddressing::TexelLoad)
                Require(!gpu_analytic,"Explicit fade addressing failed the independent floor-row expectation");
        }else Require(!analytic,"Enhanced world analytic comparison failed");
        return {std::move(gpu),gl};
    }
};
void Fixtures(Runner& r)
{
    Image atlas{8,8,std::vector<uint8_t>(8*8*4)};
    for(size_t p=0;p<64;++p){atlas.rgba[p*4]=17;atlas.rgba[p*4+1]=((p%8)+(p/8))%3?255:0;}
    auto p=Base();auto clear=p;clear.terrain.clear();const auto blank=Clear(clear);
    r.Compare("clear-native-origin",clear,atlas,{},&blank);
    auto expected=Clear(p);
    for(uint32_t y=0;y<expected.height;++y)for(uint32_t x=0;x<expected.width;++x){
        const unsigned index=p.tiles[0][((y+32)&31)*32+((x+16)&31)];
        const auto pixel=(size_t(y)*expected.width+x)*4;
        for(size_t c=0;c<3;++c)expected.rgba[pixel+c]=p.palette_rgba[index*4+c];
        expected.rgba[pixel+3]=255;
    }
    r.Compare("nearest-negative-uv-wrap",p,atlas,{},&expected);
    const auto nearest_expected=expected;
    for(auto& triangle:p.terrain)triangle.tile=1;
    expected=Flat(p,200);
    auto zero=p;zero.tiles[1].fill(0);const auto zero_expected=Flat(p,0);
    r.Compare("opaque-index-zero",zero,atlas,{},&zero_expected);
    r.Compare("constant-palette-row32",p,atlas,{},&expected);
    // Constant binary-exact inputs isolate sampler addressing from varying
    // interpolation. A 1/1024-texel offset lies inside a 1/256 sampler snap.
    for(int side=-1;side<=1;++side){
        auto probe=p;
        const float shade=(20.5f+float(side)/1024.0f)/32.0f;
        for(auto& triangle:probe.terrain)for(auto& vertex:triangle.vertices)vertex.shade=shade;
        const unsigned row=side<0?20:21;
        const auto analytic=Flat(probe,probe.fade[row*256+200]);
        const std::string name=side<0?"fade-boundary-below":side>0?"fade-boundary-above":"fade-boundary-exact";
        r.Compare(name,probe,atlas,{},&analytic,EnhancedWorldRasterOrigin::ViewportLocal,true);
    }
    auto painter=p;auto near=p.terrain;for(auto& t:near){t.tile=0;for(auto& v:t.vertices)v.position.z=-0.75f;}
    painter.terrain.insert(painter.terrain.begin(),near.begin(),near.end());
    r.Compare("terrain-always-overwrites-nearer",painter,atlas,{},&expected);
    auto gamma=p;gamma.shade_settings={0,0,1,2,0,0,1,0.4f,0};
    for(auto& t:gamma.terrain)for(auto& v:t.vertices)v.shade=0.5f;
    auto gamma_expected=expected;for(size_t i=0;i<gamma_expected.rgba.size();i+=4)
        for(size_t c=0;c<3;++c)gamma_expected.rgba[i+c]=uint8_t(std::lround(expected.rgba[i+c]*0.25f));
    r.Compare("linear-gamma",gamma,atlas,{},&gamma_expected);
    for(unsigned mode=0;mode<3;++mode)for(unsigned filter=0;filter<2;++filter){
        auto q=Base();q.shade_settings[5]=float(mode);q.shade_settings[8]=float(filter);
        for(auto& t:q.terrain)for(auto& v:t.vertices){v.shade=0.4f;v.position.w=v.position.x<0?2:7;}
        r.Compare("perspective-darkness"+std::to_string(mode)+"-filter"+std::to_string(filter),q,atlas);
    }
    auto fog=p;fog.native_width=fog.native_height=512;fog.viewport_x=301;fog.viewport_y=407;
    fog.shade_settings[5]=2;fog.shade_settings[7]=1;fog.time=0.875f;
    for(auto& t:fog.terrain)for(auto& v:t.vertices)v.shade=0.4f;
    const auto fog_images=r.Compare("active-fog-native-origin",fog,atlas);
    for(const auto& image:fog_images){
        uint64_t changed=0;
        const std::array<unsigned,3> lower{15,25,15},upper{213,42,142};
        for(size_t pixel=0;pixel<image.rgba.size();pixel+=4){
            for(size_t c=0;c<3;++c){
                Require(image.rgba[pixel+c]>=lower[c]&&image.rgba[pixel+c]<=upper[c],
                    "Active fog colour escaped its analytic blend endpoints");
                changed+=image.rgba[pixel+c]!=upper[c];
            }
            Require(image.rgba[pixel+3]==255,"Active fog changed opaque alpha");
        }
        Require(changed!=0,"Active fog fixture did not exercise fog blending");
    }
    fog.native_height+=100;fog.viewport_y+=100;
    const auto translated_fog=r.Compare("active-fog-equivalent-global-origin",fog,atlas);
    for(size_t api=0;api<2;++api)
        Require(!Differences(fog_images[api],translated_fog[api]),"Equivalent global fog coordinates changed output");
    const auto native_fog=r.Compare("native-target-fog-crop",fog,atlas,{},nullptr,EnhancedWorldRasterOrigin::Native);
    for(size_t api=0;api<2;++api)
        Require(!Differences(translated_fog[api],native_fog[api]),"Native target changed the controlled global fog image");
    auto modern=Base();modern.shade_settings[4]=1;
    for(unsigned y=0;y<511;++y)for(unsigned x=0;x<511;++x)modern.lightmap[y*511+x]=uint16_t(2048+(x*97+y*313)%24576);
    for(auto& t:modern.terrain)for(auto& v:t.vertices){v.stl_x=v.position.x<0?8:19;v.stl_y=v.position.y>0?7:23;}
    r.Compare("lightmap-ratio-bilinear",modern,atlas);
    auto fallback=modern;for(auto& t:fallback.terrain)for(auto& v:t.vertices)v.stl_x=v.stl_y=-1;
    r.Compare("lightmap-missing-coordinates",fallback,atlas,{},&nearest_expected);
    auto zero_light=modern;std::fill(zero_light.lightmap.begin(),zero_light.lightmap.end(),0);
    r.Compare("lightmap-zero-retains-face-shade",zero_light,atlas,{},&nearest_expected);
    auto missing=p;for(auto& t:missing.terrain)t.tile=MissingEnhancedTile;
    auto checker=Clear(missing);for(uint32_t y=0;y<checker.height;++y)for(uint32_t x=0;x<checker.width;++x){
        const bool magenta=(((x+uint32_t(missing.viewport_x))/8+(missing.native_height-uint32_t(missing.viewport_y)-1-y)/8)&1)==0;
        const auto pixel=(size_t(y)*checker.width+x)*4;checker.rgba[pixel]=checker.rgba[pixel+2]=magenta?255:0;
        checker.rgba[pixel+1]=0;checker.rgba[pixel+3]=255;
    }
    r.Compare("missing-atlas-global-checkerboard",missing,atlas,{},&checker);
    r.Compare("native-target-checkerboard-crop",missing,atlas,{},&checker,EnhancedWorldRasterOrigin::Native);
    EnhancedShadowQuad shadow;shadow.vertices={{{8,8,0,0},{24,8,7.0f/256,0},{24,24,7.0f/256,7.0f/256},{8,24,0,7.0f/256}}};
    shadow.z_ndc=0;shadow.darken=0.5f;shadow.source_width=shadow.source_height=8;
    auto shadows=p;shadows.shadows={shadow};
    auto shadow_expected=Flat(p,200);
    for(unsigned y=8;y<24;++y)for(unsigned x=8;x<24;++x){
        const unsigned sx=((x-8)*2+1)*7/32,sy=((y-8)*2+1)*7/32;
        if(atlas.rgba[(sy*8+sx)*4+1])for(size_t c=0;c<3;++c)shadow_expected.rgba[(size_t(y)*64+x)*4+c]/=2;
    }
    r.Compare("shadow-binary-holes-equal-depth",shadows,atlas,{},&shadow_expected);
    auto double_shadow=shadow_expected;
    for(unsigned y=8;y<24;++y)for(unsigned x=8;x<24;++x){
        const unsigned sx=((x-8)*2+1)*7/32,sy=((y-8)*2+1)*7/32;
        if(atlas.rgba[(sy*8+sx)*4+1])for(size_t c=0;c<3;++c)double_shadow.rgba[(size_t(y)*64+x)*4+c]/=2;
    }
    shadows.shadows.push_back(shadow);r.Compare("shadow-overlap-multiply",shadows,atlas,{},&double_shadow);
    shadows.shadows[0].z_ndc=shadows.shadows[1].z_ndc=0.1f;expected=Flat(p,200);
    r.Compare("shadow-behind-terrain",shadows,atlas,{},&expected);
    shadows.shadows={shadow};
    OutlinePassDraw sprite;sprite.kind=OutlinePassKind::NormalSprite;sprite.x=8;sprite.y=8;sprite.width=sprite.height=16;
    sprite.source_width=sprite.source_height=sprite.content_height=8;sprite.step_x=sprite.step_y=131072;
    sprite.lookup_row=0;sprite.z_ndc=0;sprite.source_alpha=1;
    auto sprite_expected=shadow_expected;
    for(unsigned y=8;y<24;++y)for(unsigned x=8;x<24;++x){
        const unsigned sx=(x-8)/2,sy=(y-8)/2;
        if(atlas.rgba[(sy*8+sx)*4+1])for(size_t c=0;c<4;++c)
            sprite_expected.rgba[(size_t(y)*64+x)*4+c]=p.palette_rgba[17*4+c];
    }
    r.Compare("shadow-before-normal-art",shadows,atlas,{sprite},&sprite_expected);
    auto outline=sprite;outline.kind=OutlinePassKind::Edge;outline.lookup_row=NoSpriteColourRow;
    outline.z_ndc=0.2f;outline.colour={1,0.25f,0.5f,0.5f};
    auto glow=sprite;glow.kind=OutlinePassKind::GlowAdd;glow.lookup_row=NoSpriteColourRow;
    r.Compare("terrain-shadow-outline-normal-glow",shadows,atlas,{outline,sprite,glow});
    for(unsigned bad=0;bad<11;++bad){
        auto q=p;
        switch(bad){
        case 0:q.terrain[0].vertices[0].position.w=0;break;
        case 1:q.terrain[0].vertices[0].u=std::numeric_limits<float>::quiet_NaN();break;
        case 2:q.lightmap.pop_back();break;
        case 3:q.terrain[0].tile=uint32_t(q.tiles.size());break;
        case 4:q.shade_settings[5]=3;break;
        case 5:q.viewport_x=-1;break;
        case 6:q.viewport_width=q.native_width;break;
        case 7:q.shadows={shadow};q.shadows[0].source_width=9;break;
        case 8:q.clear_rgba[2]=std::numeric_limits<float>::infinity();break;
        case 9:break; // Invalid raster-origin enum, with otherwise valid inputs.
        case 10:q.fade_addressing=static_cast<EnhancedFadeAddressing>(2);break;
        }
        const auto before=r.device.GetDiagnostics();bool rejected=false;
        try{r.device.PrepareFrozenEnhancedWorldFrame(q,atlas,{p.palette_rgba},Identity,{},
            bad==9?static_cast<EnhancedWorldRasterOrigin>(3):EnhancedWorldRasterOrigin::ViewportLocal);}
        catch(const std::invalid_argument&){rejected=true;}
        const auto after=r.device.GetDiagnostics();
        Require(rejected&&before.allocation_count==after.allocation_count&&before.allocation_bytes==after.allocation_bytes
            &&before.frozen_preparations==after.frozen_preparations&&before.validation_errors==after.validation_errors
            &&before.validation_warnings==after.validation_warnings,"Malformed enhanced world was not cleanly rejected");++r.invalid;
    }
}
}
bool RunEnhancedWorldOperatorTests(Device& device,const std::filesystem::path& output,EnhancedFadeAddressing fade_addressing)
{
    Runner run{device,output,fade_addressing};std::string error;
    try{Fixtures(run);}catch(const std::exception& e){error=e.what();}
    std::ofstream report(output/"enhanced-world-operator.json");
    report<<"{\"schema\":1,\"executionPassed\":"<<(error.empty()?"true":"false")
        <<",\"passed\":"<<(error.empty()&&!run.differences?"true":"false")<<",\"gate2Accepted\":false"
        <<",\"fadeAddressing\":\""<<(fade_addressing==EnhancedFadeAddressing::TexelLoad?"texel-load":"sampler")<<'"'
        <<",\"comparedPixels\":"<<run.pixels<<",\"differingBytes\":"<<run.differences<<",\"invalidCases\":"<<run.invalid
        <<",\"error\":"<<Json(error)<<",\"cases\":[";
    for(size_t i=0;i<run.records.size();++i){if(i)report<<',';report<<run.records[i];}report<<"]}\n";
    Require(bool(report),"Cannot retain enhanced world operator report");
    if(!error.empty())throw std::runtime_error(error);
    return !run.differences;
}
}
