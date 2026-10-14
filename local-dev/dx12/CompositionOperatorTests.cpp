// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#include "CompositionOperatorTests.h"
#include "EnhancedWorldGLOracle.h"
#include "EnhancedSpriteTestImages.h"
#include <algorithm>
#include <cmath>
#include <functional>

namespace kfx::render::dx12 {
namespace {
using namespace enhanced_test;
constexpr uint32_t Width=64,Height=48;
constexpr std::array<float,9> Identity{1,0,0,0,1,0,0,0,1};
using Colour=std::array<uint8_t,4>;
constexpr Colour WorldClear{16,32,48,255},ScreenClear{4,8,12,255},TerrainColour{40,80,120,255};
std::array<float,4> FloatColour(const Colour& colour)
{
    std::array<float,4> result{};for(size_t i=0;i<4;++i)result[i]=float(colour[i])/255;return result;
}
void Rectangle(Image& image,unsigned x0,unsigned y0,unsigned x1,unsigned y1,const Colour& colour)
{
    for(unsigned y=y0;y<y1;++y)for(unsigned x=x0;x<x1;++x)
        std::copy(colour.begin(),colour.end(),image.rgba.begin()+(size_t(y)*image.width+x)*4);
}
Image Filled(const Colour& colour)
{
    Image result{Width,Height,std::vector<uint8_t>(Width*Height*4)};
    Rectangle(result,0,0,Width,Height,colour);return result;
}
EnhancedWorldPass World()
{
    EnhancedWorldPass world;world.native_width=Width;world.native_height=Height;
    world.viewport_x=8;world.viewport_y=6;world.viewport_width=48;world.viewport_height=36;
    world.clear_rgba=FloatColour(WorldClear);world.lightmap.assign(511*511,8192);
    world.tiles.resize(1);world.tiles[0].fill(7);
    for(unsigned i=0;i<256;++i){
        for(unsigned c=0;c<4;++c)world.palette_rgba[i*4+c]=TerrainColour[c];
        for(unsigned row=0;row<64;++row)world.fade[row*256+i]=uint8_t(i);
    }
    const EnhancedTerrainVertex a{{-1,1,0,1},0,0,1,-1,-1},b{{1,1,0,1},32,0,1,-1,-1},
        c{{1,-1,0,1},32,32,1,-1,-1},d{{-1,-1,0,1},0,32,1,-1,-1};
    world.terrain={{{a,b,c},0},{{a,c,d},0}};return world;
}
SceneGLCompositionTrace Composition()
{
    SceneGLCompositionTrace result;result.present=result.complete=true;result.completed_phases=15;
    result.screen_clear_rgba=FloatColour(WorldClear);
    result.overlays.native_width=Width;result.overlays.native_height=Height;return result;
}
void Add(SceneGLCompositionTrace& c,SceneGLOverlayPhase phase,float z,
    unsigned x0,unsigned y0,unsigned x1,unsigned y1,const Colour& colour)
{
    SceneGLOverlayQuad q;q.phase=phase;q.mode=SceneGLOverlayMode::Solid;
    q.depth_test=phase==SceneGLOverlayPhase::DepthWorld;q.z_ndc=z;q.colour=FloatColour(colour);
    q.x0=float(x0);q.y0=float(y0);q.x1=float(x1);q.y1=float(y1);
    c.overlays.draws.push_back(q);++c.phase_quad_counts[size_t(phase)];
}
void Overlays(SceneGLCompositionTrace& c)
{
    // Behind terrain must fail. A nearer overlay must not write depth, so the
    // second qualifying overlay still passes the unchanged terrain depth.
    Add(c,SceneGLOverlayPhase::DepthWorld,0.25f,10,8,50,38,{0,0,255,255});
    Add(c,SceneGLOverlayPhase::DepthWorld,-0.5f,12,10,28,24,{255,0,0,255});
    Add(c,SceneGLOverlayPhase::DepthWorld,-0.25f,16,14,24,20,{0,255,0,255});
    Add(c,SceneGLOverlayPhase::DepthWorld,0,10,28,18,34,{255,0,255,255});
    Add(c,SceneGLOverlayPhase::FlatWorld,0.75f,32,12,40,20,{0,0,255,255});
    Add(c,SceneGLOverlayPhase::Swipe,0.75f,35,15,44,25,{255,255,0,255});
}
Image Source(bool overlays)
{
    auto source=Filled(WorldClear);Rectangle(source,8,6,56,42,TerrainColour);
    if(overlays){
        Rectangle(source,12,10,28,24,{255,0,0,255});Rectangle(source,16,14,24,20,{0,255,0,255});
        Rectangle(source,10,28,18,34,{255,0,255,255});Rectangle(source,32,12,40,20,{0,0,255,255});
        Rectangle(source,35,15,44,25,{255,255,0,255});
    }
    return source;
}
unsigned Key(const uint8_t* rgb)
{
    const auto channel=[](unsigned value){return unsigned(std::floor(double(value)*63/255+0.5));};
    return (channel(rgb[0])<<12)|(channel(rgb[1])<<6)|channel(rgb[2]);
}
void Lens(SceneGLCompositionTrace& c,SceneGLLensOperation operation,const Image& source)
{
    auto& lens=c.lens;lens.requested_active=lens.capture_began=true;lens.operation=operation;
    lens.requested_type=operation==SceneGLLensOperation::Mist?1:operation==SceneGLLensOperation::Remap?2:
        operation==SceneGLLensOperation::Overlay?4:0;
    c.screen_clear_rgba=FloatColour(ScreenClear);
    if(operation==SceneGLLensOperation::NotDrawn){lens.requested_type=1;return;}
    auto& p=lens.pass;p.kind=SceneGLLensPassKind(unsigned(operation)-1);
    p.source_width=p.target_width=Width;p.source_height=p.target_height=Height;
    p.source_x=p.destination_x=8;p.destination_y=10;p.viewport_width=40;p.viewport_height=24;
    if(operation==SceneGLLensOperation::Remap){
        p.remap_width=4;p.remap_height=3;
        for(unsigned y=0;y<3;++y)for(unsigned x=0;x<4;++x)
            p.remap.push_back({std::array<uint16_t,4>{0,15,35,65535}[x],std::array<uint16_t,3>{0,16,25}[y]});
        lens.resources[4]={2,7};
    }
    if(operation==SceneGLLensOperation::Mist||operation==SceneGLLensOperation::Overlay){
        p.palette_rgba.resize(1024);p.rgb6_lookup.assign(512*512,0);
        for(unsigned i=0;i<256;++i){p.palette_rgba[i*4]=uint8_t(i);p.palette_rgba[i*4+1]=uint8_t(i*3);
            p.palette_rgba[i*4+2]=uint8_t(i*5);p.palette_rgba[i*4+3]=255;}
        // Distinct source colours select distinct table entries. This verifies
        // the lens consumes rendered terrain/overlay RGB, not a flat stand-in.
        for(size_t i=0;i<source.rgba.size();i+=4){const auto key=Key(source.rgba.data()+i);p.rgb6_lookup[key]=uint8_t(1+key%53);}
        lens.resources[0]={3,11};lens.resources[2]={5,13};
    }
    if(operation==SceneGLLensOperation::Mist){
        p.density.assign(256*256,8);p.mist_lightness=3;p.mist_offsets={5.25f,9.5f,11.75f,17};
        p.fade.resize(16384);for(unsigned row=0;row<64;++row)for(unsigned i=0;i<256;++i)p.fade[row*256+i]=uint8_t(i+row);
        lens.resources[1]={4,12};lens.resources[3]={6,14};
    }
    if(operation==SceneGLLensOperation::Overlay){
        p.overlay_width=3;p.overlay_height=2;p.overlay_alpha=128;p.overlay={255,17,33,49,65,81};lens.resources[5]={7,15};
    }
}
Image Expected(const SceneGLCompositionTrace& composition,const Image& source)
{
    const auto& lens=composition.lens;
    if(!lens.capture_began)return source;
    auto output=Filled(ScreenClear);
    if(lens.operation==SceneGLLensOperation::NotDrawn)return output;
    const auto& p=lens.pass;
    for(unsigned y=0;y<p.viewport_height;++y)for(unsigned x=0;x<p.viewport_width;++x){
        unsigned sx=x,sy=y;
        if(lens.operation==SceneGLLensOperation::Remap){
            const auto value=p.remap[std::min(y,p.remap_height-1)*p.remap_width+std::min(x,p.remap_width-1)];sx=value[0];sy=value[1];
        }
        sx=std::min(unsigned(p.source_x)+sx,Width-1);sy=std::min(sy,Height-1);
        const auto from=(size_t(sy)*Width+sx)*4,to=(size_t(unsigned(p.destination_y)+y)*Width+unsigned(p.destination_x)+x)*4;
        std::copy_n(source.rgba.data()+from,3,output.rgba.data()+to);output.rgba[to+3]=255;
        if(lens.operation==SceneGLLensOperation::Mist){
            const unsigned index=p.fade[5*256+p.rgb6_lookup[Key(source.rgba.data()+from)]];
            std::copy_n(p.palette_rgba.data()+index*4,3,output.rgba.data()+to);
        }else if(lens.operation==SceneGLLensOperation::Overlay){
            const auto ox=std::min((x*((p.overlay_width<<16)/p.viewport_width))>>16,p.overlay_width-1);
            const auto oy=std::min((y*((p.overlay_height<<16)/p.viewport_height))>>16,p.overlay_height-1);
            const unsigned ov=p.overlay[oy*p.overlay_width+ox];
            if(ov!=255){const unsigned index=(ov+p.rgb6_lookup[Key(source.rgba.data()+from)])>>1;
                std::copy_n(p.palette_rgba.data()+index*4,3,output.rgba.data()+to);}
        }
    }
    return output;
}
struct Runner {
    Device& device;std::filesystem::path output;std::vector<std::string> records;
    uint64_t pixels=0,byte_differences=0,analytic_differences=0;unsigned rejected=0;
    void Compare(const std::string& name,const EnhancedWorldPass& world,const SceneGLCompositionTrace& composition,const Image& expected)
    {
        const Image atlas{1,1,{0,0,0,0}};const auto before=device.GetDiagnostics();
        auto frame=device.PrepareFrozenEnhancedCompositionFrame(world,atlas,{},Identity,{},composition);
        auto alias=frame;frame.Reset();const auto prepared=device.GetDiagnostics();
        Require(!frame.Valid()&&alias.Valid()&&alias.Width()==Width&&alias.Height()==Height
            &&prepared.frozen_preparations==before.frozen_preparations+1
            &&prepared.frozen_image_readbacks==before.frozen_image_readbacks,"Composition preparation did not retain an unread native frame");
        bool no_indices=false;try{device.ReadbackFrozenIndices(alias);}catch(const std::invalid_argument&){no_indices=true;}
        Require(no_indices&&device.GetDiagnostics().frozen_image_readbacks==before.frozen_image_readbacks,
            "Composition colour frame exposed indexed data");
        const auto gpu=device.ReadbackFrozenRGBA(alias);alias.Reset();device.WaitIdle();const auto after=device.GetDiagnostics();
        Require(after.frozen_image_readbacks==before.frozen_image_readbacks+1
            &&after.frozen_presentations==before.frozen_presentations&&after.allocation_count==before.allocation_count
            &&after.allocation_bytes==before.allocation_bytes&&after.validation_errors==before.validation_errors
            &&after.validation_warnings==before.validation_warnings,"Composition preparation leaked resources or emitted validation diagnostics");
        const auto gl=RenderEnhancedCompositionGLReference(world,atlas,{},Identity,{},composition);
        const auto difference=Differences(gpu,gl),analytic=Differences(gpu,expected)+Differences(gl,expected);
        pixels+=uint64_t(Width)*Height;byte_differences+=difference;analytic_differences+=analytic;
        Image mask{Width,Height,std::vector<uint8_t>(Width*Height*4)};unsigned maximum=0;
        for(size_t i=0;i<mask.rgba.size();i+=4){bool changed=false;
            for(size_t channel=0;channel<4;++channel){const auto delta=unsigned(std::abs(int(gpu.rgba[i+channel])-int(gl.rgba[i+channel])));
                changed|=delta!=0;maximum=std::max(maximum,delta);}
            mask.rgba[i]=mask.rgba[i+2]=changed?255:0;mask.rgba[i+3]=255;}
        const auto stem="composition-operator-"+name;
        Save(output/(stem+"-gpu"),gpu);Save(output/(stem+"-gl"),gl);Save(output/(stem+"-expected"),expected);Save(output/(stem+"-difference"),mask);
        records.push_back("{\"name\":"+Json(name)+",\"pixels\":3072,\"differingBytes\":"+std::to_string(difference)
            +",\"analyticDifferingBytes\":"+std::to_string(analytic)+",\"maximumByteDelta\":"+std::to_string(maximum)
            +",\"preparations\":1,\"preparationImageReadbacks\":0,\"diagnosticImageReadbacks\":1,\"lifetimePassed\":true}");
        Require(!difference&&!analytic,"Resident composition analytic or independent GL comparison failed");
    }
    void Reject(const EnhancedWorldPass& world,const SceneGLCompositionTrace& composition)
    {
        const auto before=device.GetDiagnostics();bool rejected_input=false;
        try{device.PrepareFrozenEnhancedCompositionFrame(world,{1,1,{0,0,0,0}},{},Identity,{},composition);}
        catch(const std::invalid_argument&){rejected_input=true;}
        const auto after=device.GetDiagnostics();Require(rejected_input&&after.frozen_preparations==before.frozen_preparations
            &&after.frozen_image_readbacks==before.frozen_image_readbacks&&after.allocation_count==before.allocation_count
            &&after.allocation_bytes==before.allocation_bytes&&after.validation_errors==before.validation_errors
            &&after.validation_warnings==before.validation_warnings,"Invalid composition allocated, published or escaped rejection");++rejected;
    }
};
void Fixtures(Runner& r)
{
    const auto world=World();const auto source=Source(false);auto c=Composition();
    r.Compare("inactive-world-native",world,c,source);
    c.lens.requested_active=true;c.lens.requested_type=1;c.lens.operation=SceneGLLensOperation::NotDrawn;
    r.Compare("uncaptured-not-drawn",world,c,source);
    c=Composition();Lens(c,SceneGLLensOperation::NotDrawn,source);
    r.Compare("captured-not-drawn-clears-screen",world,c,Expected(c,source));
    c=Composition();Overlays(c);const auto overlaid=Source(true);
    r.Compare("depth-flat-swipe-order-no-lens",world,c,overlaid);
    for(const auto& [operation,name]:std::array<std::pair<SceneGLLensOperation,const char*>,4>{{
        {SceneGLLensOperation::Copy,"copy-offset-retains-overlays"},{SceneGLLensOperation::Remap,"remap-resident-source"},
        {SceneGLLensOperation::Mist,"mist-resident-source"},{SceneGLLensOperation::Overlay,"overlay-resident-source"}}}){
        auto test=c;Lens(test,operation,overlaid);r.Compare(name,world,test,Expected(test,overlaid));
    }
    auto fallback=c;Lens(fallback,SceneGLLensOperation::Copy,overlaid);fallback.lens.requested_type=1;fallback.lens.fallback_copy=true;
    r.Compare("effective-copy-fallback",world,fallback,Expected(fallback,overlaid));
    auto invalid=c;invalid.completed_phases=7;r.Reject(world,invalid);
    invalid=c;++invalid.phase_quad_counts[0];r.Reject(world,invalid);
    invalid=c;invalid.overlays.native_width=63;r.Reject(world,invalid);
    invalid=c;invalid.lens.requested_active=true;r.Reject(world,invalid);
    invalid=c;Lens(invalid,SceneGLLensOperation::Mist,overlaid);invalid.lens.resources[2]={};r.Reject(world,invalid);
    invalid=c;Lens(invalid,SceneGLLensOperation::Copy,overlaid);invalid.lens.pass.source_width=63;r.Reject(world,invalid);
}
}
bool RunCompositionOperatorTests(Device& device,const std::filesystem::path& output)
{
    Runner runner{device,output,{},0,0,0,0};std::string error;
    try{Fixtures(runner);}catch(const std::exception& e){error=e.what();}
    const bool passed=error.empty()&&!runner.byte_differences&&!runner.analytic_differences;
    std::ofstream report(output/"composition-operators.json");
    report<<"{\"schema\":1,\"executionPassed\":"<<(error.empty()?"true":"false")<<",\"passed\":"<<(passed?"true":"false")
        <<",\"gate2Accepted\":false,\"reviewRequired\":"<<(passed?"false":"true")<<",\"nativeWidth\":64,\"nativeHeight\":48"
        <<",\"scope\":\"Resident world, retained D24 overlays and actual lens composition; opaque analytic controls and independent production GL\""
        <<",\"caseCount\":"<<runner.records.size()<<",\"rejectedInputs\":"<<runner.rejected<<",\"comparedPixels\":"<<runner.pixels
        <<",\"differingBytes\":"<<runner.byte_differences<<",\"analyticDifferingBytes\":"<<runner.analytic_differences
        <<",\"residentPresentations\":0,\"error\":"<<Json(error)<<",\"cases\":[";
    for(size_t i=0;i<runner.records.size();++i){if(i)report<<',';report<<runner.records[i];}report<<"]}\n";
    Require(bool(report),"Cannot retain composition operator report");if(!error.empty())throw std::runtime_error(error);return passed;
}
}
