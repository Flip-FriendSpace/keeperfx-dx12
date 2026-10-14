// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#include "LensOperatorTests.h"
#include "LensGLOracle.h"
#include "EnhancedSpriteTestImages.h"
#include <algorithm>
#include <limits>

namespace kfx::render::dx12 {
namespace {
using namespace enhanced_test;
Image Pattern(uint32_t width, uint32_t height, unsigned seed)
{
    Image image{width,height,std::vector<uint8_t>(size_t(width)*height*4)};
    for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x){
        const size_t p=(size_t(y)*width+x)*4;
        image.rgba[p]=uint8_t(seed+x*37+y*11);
        image.rgba[p+1]=uint8_t(seed*3+x*7+y*43);
        image.rgba[p+2]=uint8_t(seed*5+x*19+y*29);
        image.rgba[p+3]=uint8_t(seed+x*13+y*17);
    }
    return image;
}
LensPass Base()
{
    LensPass p;p.source_width=19;p.source_height=13;p.source_x=3;
    p.target_width=31;p.target_height=23;p.destination_x=7;p.destination_y=6;
    p.viewport_width=17;p.viewport_height=11;return p;
}
void LookupResources(LensPass& p)
{
    p.palette_rgba.resize(1024);p.rgb6_lookup.resize(512*512);
    for(unsigned i=0;i<256;++i){
        p.palette_rgba[i*4]=uint8_t(i*73+19);p.palette_rgba[i*4+1]=uint8_t(i*29+41);
        p.palette_rgba[i*4+2]=uint8_t(i*113+7);p.palette_rgba[i*4+3]=uint8_t(i*17);
    }
    for(unsigned key=0;key<p.rgb6_lookup.size();++key)
        p.rgb6_lookup[key]=uint8_t((key>>12)*17+((key>>6)&63)*31+(key&63)*7+23);
}
LensPass Mist()
{
    auto p=Base();p.kind=LensPassKind::Mist;LookupResources(p);
    p.mist_offsets={255.75f,249.25f,2.875f,253.5f};p.mist_lightness=37;
    p.density.resize(256*256);p.fade.resize(256*64);
    for(unsigned y=0;y<256;++y)for(unsigned x=0;x<256;++x)
        p.density[y*256+x]=uint8_t(x*47+y*89+(x^y)*11);
    for(unsigned row=0;row<64;++row)for(unsigned i=0;i<256;++i)
        p.fade[row*256+i]=uint8_t(i*13+row*7+11);
    return p;
}
LensPass Overlay(unsigned alpha)
{
    auto p=Base();p.kind=LensPassKind::Overlay;LookupResources(p);
    p.overlay_width=7;p.overlay_height=5;p.overlay_alpha=uint16_t(alpha);p.overlay.resize(35);
    for(unsigned i=0;i<35;++i)p.overlay[i]=i%5==0?255:uint8_t(i*31+9);
    return p;
}
unsigned Key(const uint8_t* rgb)
{
    // Exact integer rounding of an uploaded UNORM byte to six bits. There is
    // no half-way tie: 255 is odd. Palette duplicate ties are owned by the LUT.
    return (((unsigned(rgb[0])*63+127)/255)<<12)
        |(((unsigned(rgb[1])*63+127)/255)<<6)|((unsigned(rgb[2])*63+127)/255);
}
struct Analytic {
    Image image;
    uint64_t clamped=0, transparent=0, row63=0, density_saturated=0;
};
Analytic Reference(const LensPass& p,const Image& source,const Image& background)
{
    Analytic result{background};
    for(uint32_t y=0;y<p.viewport_height;++y)for(uint32_t x=0;x<p.viewport_width;++x){
        uint32_t sx=x,sy=y;
        if(p.kind==LensPassKind::Remap){
            const auto& r=p.remap[size_t(std::min(y,p.remap_height-1))*p.remap_width+std::min(x,p.remap_width-1)];
            sx=r[0];sy=r[1];
        }
        sx+=uint32_t(p.source_x);
        result.clamped+=sx>=source.width||sy>=source.height;
        sx=std::min(sx,source.width-1);sy=std::min(sy,source.height-1);
        const auto* rgb=&source.rgba[(size_t(sy)*source.width+sx)*4];
        const size_t dst=((size_t(p.destination_y)+y)*background.width+uint32_t(p.destination_x)+x)*4;
        const uint8_t* output=rgb;
        if(p.kind==LensPassKind::Mist){
            const auto vx=unsigned(uint64_t(x)*((uint64_t(640)<<16)/p.viewport_width)/65536);
            const auto vy=unsigned(uint64_t(y)*((uint64_t(480)<<16)/p.viewport_height)/65536);
            const unsigned ax=(unsigned(p.mist_offsets[0])+vx)%256,ay=(unsigned(p.mist_offsets[1])+vy)%256;
            const unsigned bx=(unsigned(p.mist_offsets[2])+65536-vy)%256,by=(unsigned(p.mist_offsets[3])+65536-vx)%256;
            const unsigned raw=(unsigned(p.density[ay*256+ax])+p.density[by*256+bx])/8;
            result.density_saturated+=raw>32;
            const unsigned row=std::min(unsigned(p.mist_lightness)+std::min(raw,32u),63u);
            result.row63+=row==63;
            output=&p.palette_rgba[p.fade[row*256+p.rgb6_lookup[Key(rgb)]]*4];
        }else if(p.kind==LensPassKind::Overlay){
            const unsigned ox=unsigned(uint64_t(x)*((uint64_t(p.overlay_width)<<16)/p.viewport_width)/65536);
            const unsigned oy=unsigned(uint64_t(y)*((uint64_t(p.overlay_height)<<16)/p.viewport_height)/65536);
            const unsigned index=p.overlay[size_t(std::min(oy,p.overlay_height-1))*p.overlay_width+std::min(ox,p.overlay_width-1)];
            if(index==255)++result.transparent;
            else output=&p.palette_rgba[((index*p.overlay_alpha+unsigned(p.rgb6_lookup[Key(rgb)])*(256-p.overlay_alpha))/256)*4];
        }
        for(unsigned c=0;c<3;++c)result.image.rgba[dst+c]=output[c];
        result.image.rgba[dst+3]=255;
    }
    return result;
}
bool Outside(const LensPass& p,unsigned x,unsigned y)
{
    return x<unsigned(p.destination_x)||y<unsigned(p.destination_y)
        ||x>=unsigned(p.destination_x)+p.viewport_width||y>=unsigned(p.destination_y)+p.viewport_height;
}
struct Runner {
    Device& device;std::filesystem::path output;std::vector<std::string> records;
    uint64_t compared_pixels=0,gpu_gl=0,gpu_analytic=0,gl_analytic=0;
    unsigned cases=0,invalid=0;bool checks=true;
    Runner(Device& d,const std::filesystem::path& path):device(d),output(path){}
    void Compare(const std::string& name,const LensPass& p,const Image* source_override=nullptr)
    {
        ValidateLensPass(p);
        const auto source=source_override?*source_override:Pattern(p.source_width,p.source_height,7);
        const auto background=Pattern(p.target_width,p.target_height,101);
        const auto expected=Reference(p,source,background);
        if(p.kind==LensPassKind::Mist)Require(expected.row63&&expected.density_saturated,"Mist fixture misses saturation or row 63");
        if(p.kind==LensPassKind::Overlay&&name.find("ownership")==std::string::npos)
            Require(expected.transparent,"Overlay fixture misses transparent index 255");
        const auto before=device.GetDiagnostics();
        const auto gpu=device.CompositeLens(p,source,background);device.WaitIdle();
        const auto after=device.GetDiagnostics();
        const bool resources=after.allocation_count==before.allocation_count&&after.allocation_bytes==before.allocation_bytes
            &&after.validation_errors==before.validation_errors&&after.validation_warnings==before.validation_warnings;
        checks&=resources;
        const auto gl=RenderLensGLReference(p,source,background);
        const auto dg=Differences(gpu,gl),da=Differences(gpu,expected.image),db=Differences(gl,expected.image);
        gpu_gl+=dg;gpu_analytic+=da;gl_analytic+=db;compared_pixels+=uint64_t(gpu.width)*gpu.height*3;++cases;
        uint64_t outside=0,alpha=0;unsigned max_delta=0;
        Image difference{gpu.width,gpu.height,std::vector<uint8_t>(gpu.rgba.size())};
        for(unsigned y=0;y<gpu.height;++y)for(unsigned x=0;x<gpu.width;++x){
            const size_t i=(size_t(y)*gpu.width+x)*4;
            for(unsigned c=0;c<4;++c){
                difference.rgba[i]|=gpu.rgba[i+c]!=expected.image.rgba[i+c]?255:0;
                difference.rgba[i+1]|=gl.rgba[i+c]!=expected.image.rgba[i+c]?255:0;
                difference.rgba[i+2]|=gpu.rgba[i+c]!=gl.rgba[i+c]?255:0;
                max_delta=std::max(max_delta,unsigned(std::abs(int(gpu.rgba[i+c])-int(gl.rgba[i+c]))));
                if(Outside(p,x,y))outside+=(gpu.rgba[i+c]!=background.rgba[i+c])+(gl.rgba[i+c]!=background.rgba[i+c]);
                if(c==3)alpha+=(gpu.rgba[i+c]!=expected.image.rgba[i+c])+(gl.rgba[i+c]!=expected.image.rgba[i+c]);
            }
            difference.rgba[i+3]=255;
        }
        checks&=!outside&&!alpha;
        const auto stem=output/("lens-"+name);
        Save(stem.string()+"-gpu",gpu);Save(stem.string()+"-gl",gl);Save(stem.string()+"-analytic",expected.image);
        Save(stem.string()+"-difference",difference);Save(stem.string()+"-source",source);Save(stem.string()+"-background",background);
        records.push_back("{\"name\":"+Json(name)+",\"scope\":\"standardOperatorFullNativeTarget\",\"width\":"+std::to_string(gpu.width)
            +",\"height\":"+std::to_string(gpu.height)+",\"destination\":["+std::to_string(p.destination_x)+","+std::to_string(p.destination_y)
            +","+std::to_string(p.viewport_width)+","+std::to_string(p.viewport_height)+"],\"gpuGlDifferingBytes\":"+std::to_string(dg)
            +",\"gpuAnalyticDifferingBytes\":"+std::to_string(da)+",\"glAnalyticDifferingBytes\":"+std::to_string(db)
            +",\"maximumGpuGlByteDelta\":"+std::to_string(max_delta)+",\"outsideDifferingBytes\":"+std::to_string(outside)
            +",\"alphaDifferingBytes\":"+std::to_string(alpha)+",\"sourceClampedPixels\":"+std::to_string(expected.clamped)
            +",\"transparentOverlayPixels\":"+std::to_string(expected.transparent)+",\"mistRow63Pixels\":"+std::to_string(expected.row63)
            +",\"mistDensitySaturatedPixels\":"+std::to_string(expected.density_saturated)+",\"resourcesReclaimed\":"+(resources?"true":"false")+"}");
    }
    void Reject(const std::string& name,const LensPass& p,Image source,Image background)
    {
        const auto before=device.GetDiagnostics();bool gpu=false,gl=false;
        try{device.CompositeLens(p,source,background);}catch(const std::invalid_argument&){gpu=true;}
        try{RenderLensGLReference(p,source,background);}catch(const std::invalid_argument&){gl=true;}
        catch(const std::runtime_error& e){
            // This precise pre-context image check predates the pass validator.
            // Do not turn a GL setup, compilation or rendering error into a pass.
            if(std::string(e.what())!="Lens oracle native image extent or byte count mismatch")throw;
            gl=true;
        }
        device.WaitIdle();const auto after=device.GetDiagnostics();
        const bool clean=after.allocation_count==before.allocation_count&&after.allocation_bytes==before.allocation_bytes
            &&after.validation_errors==before.validation_errors&&after.validation_warnings==before.validation_warnings;
        checks&=gpu&&gl&&clean;++invalid;
        records.push_back("{\"name\":"+Json(name)+",\"invalidInput\":true,\"gpuRejected\":"+(gpu?"true":"false")
            +",\"glRejected\":"+(gl?"true":"false")+",\"resourcesUnchanged\":"+(clean?"true":"false")+"}");
    }
};
void Fixtures(Runner& r)
{
    auto p=Base();r.Compare("copy-source-row-zero-offset-target",p);
    p.source_x=18;p.viewport_height=15;r.Compare("copy-source-xy-clamp",p);
    p=Base();p.kind=LensPassKind::Remap;p.remap_width=3;p.remap_height=2;
    p.remap={{{0,0}},{{1,12}},{{65535,1}},{{18,65535}},{{7,4}},{{65535,65535}}};
    r.Compare("unsigned-remap-table-and-source-clamp",p);
    p=Mist();r.Compare("mist-virtual-integer-two-wrapped-layers",p);
    p.mist_lightness=63;r.Compare("mist-lightness-row63",p);
    for(unsigned alpha:{0u,1u,128u,255u,256u})r.Compare("overlay-index-alpha-"+std::to_string(alpha),Overlay(alpha));
    p=Overlay(128);p.overlay.assign(p.overlay.size(),10);
    // Two palette entries share a colour. The supplied RGB6 table owns the
    // selected index; inferring an index from the palette would lose this tie.
    for(unsigned c=0;c<3;++c)p.palette_rgba[201*4+c]=p.palette_rgba[5*4+c];
    auto source=Pattern(p.source_width,p.source_height,7);
    for(size_t i=0;i<source.rgba.size();i+=4)for(unsigned c=0;c<3;++c)source.rgba[i+c]=p.palette_rgba[5*4+c];
    const auto background=Pattern(p.target_width,p.target_height,101);
    const size_t first=(size_t(p.destination_y)*p.target_width+unsigned(p.destination_x))*4;
    p.rgb6_lookup[Key(source.rgba.data())]=201;
    const auto high=Reference(p,source,background);
    Require(high.image.rgba[first]==p.palette_rgba[105*4],"Owned high-index duplicate fixture is ineffective");
    r.Compare("rgb6-ownership-duplicate-tie-high",p,&source);
    p.rgb6_lookup[Key(source.rgba.data())]=5;
    const auto low=Reference(p,source,background);
    Require(low.image.rgba[first]==p.palette_rgba[7*4]&&low.image.rgba[first]!=high.image.rgba[first],
        "Owned low-index duplicate fixture is ineffective");
    r.Compare("rgb6-ownership-duplicate-tie-low",p,&source);
}
void Invalid(Runner& r)
{
    const auto source=Pattern(19,13,7),background=Pattern(31,23,101);
    auto reject=[&](const char* name,const LensPass& p){r.Reject(name,p,source,background);};
    auto p=Base();p.kind=LensPassKind(255);reject("invalid-kind",p);
    p=Base();p.source_y=1;reject("invalid-nonzero-source-y",p);
    p=Base();p.source_x=-1;reject("invalid-source-x",p);
    p=Base();p.destination_y=-1;reject("invalid-negative-destination",p);
    p=Base();p.viewport_width=std::numeric_limits<uint32_t>::max();reject("invalid-destination-overflow",p);
    p=Base();p.target_width=16385;reject("invalid-target-extent",p);
    p=Base();p.palette_rgba.resize(1024);reject("invalid-unused-resource",p);
    p=Base();p.kind=LensPassKind::Remap;p.remap_width=p.remap_height=1;reject("invalid-remap-size",p);
    p=Mist();p.density.pop_back();reject("invalid-density-size",p);
    p=Mist();p.fade.pop_back();reject("invalid-fade-size",p);
    p=Mist();p.mist_offsets[2]=std::numeric_limits<float>::quiet_NaN();reject("invalid-mist-nan",p);
    p=Mist();p.mist_offsets[0]=256;reject("invalid-mist-offset",p);
    p=Mist();p.mist_lightness=64;reject("invalid-mist-lightness",p);
    p=Overlay(257);reject("invalid-overlay-alpha",p);
    p=Overlay(128);p.overlay.pop_back();reject("invalid-overlay-size",p);
    p=Overlay(128);p.rgb6_lookup.pop_back();reject("invalid-rgb6-size",p);
    p=Overlay(128);p.palette_rgba.pop_back();reject("invalid-palette-size",p);
    auto bad_source=source;bad_source.rgba.pop_back();r.Reject("invalid-source-bytes",Base(),bad_source,background);
    auto bad_background=background;bad_background.rgba.pop_back();r.Reject("invalid-target-bytes",Base(),source,bad_background);
    bad_source=source;bad_source.width=18;r.Reject("invalid-source-dimensions",Base(),bad_source,background);
}
}
bool RunLensOperatorTests(Device& device,const std::filesystem::path& output)
{
    std::filesystem::create_directories(output);Runner r(device,output);std::string error;
    try{Invalid(r);Fixtures(r);}catch(const std::exception& e){error=e.what();}
    const bool passed=error.empty()&&r.checks&&!r.gpu_gl&&!r.gpu_analytic&&!r.gl_analytic;
    std::ofstream report(output/"lens-operators.json");
    report<<"{\"schema\":1,\"scope\":\"owned standard GL lens operators; no complete possession-frame claim\",\"gate2Accepted\":false,\"passed\":"
        <<(passed?"true":"false")<<",\"toleranceBytes\":0,\"gpuAdapter\":"<<Json(device.GetDiagnostics().adapter)
        <<",\"glDevice\":"<<Json(LensGLReferenceDevice())<<",\"fullNativeTargetCases\":"<<r.cases
        <<",\"invalidInputCases\":"<<r.invalid<<",\"pairwisePixelComparisons\":"<<r.compared_pixels
        <<",\"pairwiseByteComparisons\":"<<r.compared_pixels*4<<",\"gpuGlDifferingBytes\":"<<r.gpu_gl
        <<",\"gpuAnalyticDifferingBytes\":"<<r.gpu_analytic<<",\"glAnalyticDifferingBytes\":"<<r.gl_analytic
        <<",\"error\":"<<Json(error)<<",\"cases\":[";
    for(size_t i=0;i<r.records.size();++i){if(i)report<<',';report<<r.records[i];}report<<"]}";report.close();
    Require(bool(report),"Cannot retain lens operator report");
    Require(error.empty(),error.c_str());
    Require(r.checks&&!r.gpu_analytic&&!r.gl_analytic,"Lens analytic, invalid-input, alpha, bounds or resource checks failed; see lens-operators.json");
    return !r.gpu_gl;
}
}
