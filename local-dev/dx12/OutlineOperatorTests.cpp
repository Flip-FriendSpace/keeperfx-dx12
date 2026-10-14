// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#include "OutlineOperatorTests.h"
#include "OutlineGLOracle.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace kfx::render::dx12 {
namespace {
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
std::string Quote(const std::string& text)
{
    std::string result = "\"";
    for (unsigned char c : text) {
        if (c == '\\' || c == '"') { result += '\\'; result += char(c); }
        else if (c < 32) result += ' ';
        else result += char(c);
    }
    return result + '"';
}
Image Solid(uint32_t width, uint32_t height, std::array<uint8_t,4> colour)
{
    Image image{width,height,std::vector<uint8_t>(size_t(width)*height*4)};
    for (size_t i=0;i<image.rgba.size();i+=4) std::copy(colour.begin(),colour.end(),image.rgba.begin()+i);
    return image;
}
void Pixel(Image& image, int x, int y, std::array<uint8_t,4> colour)
{
    if (x<0 || y<0 || x>=int(image.width) || y>=int(image.height)) return;
    std::copy(colour.begin(),colour.end(),image.rgba.begin()+(size_t(y)*image.width+size_t(x))*4);
}
void Retain(const std::filesystem::path& path, const Image& image)
{
    std::ofstream raw(path.string()+".rgba",std::ios::binary);
    raw.write(reinterpret_cast<const char*>(image.rgba.data()),std::streamsize(image.rgba.size()));
    Require(bool(raw),"Cannot retain outline raw RGBA");
    std::ofstream bmp(path.string()+".bmp",std::ios::binary);
    const auto u16=[&](uint16_t n){for(int i=0;i<2;++i)bmp.put(char(n>>(8*i)));};
    const auto u32=[&](uint32_t n){for(int i=0;i<4;++i)bmp.put(char(n>>(8*i)));};
    bmp.write("BM",2);u32(54+uint32_t(image.rgba.size()));u32(0);u32(54);u32(40);
    u32(image.width);u32(image.height);u16(1);u16(32);u32(0);u32(uint32_t(image.rgba.size()));
    u32(0);u32(0);u32(0);u32(0);
    for(uint32_t y=image.height;y--;)for(uint32_t x=0;x<image.width;++x){
        const size_t p=(size_t(y)*image.width+x)*4;
        bmp.put(char(image.rgba[p+2]));bmp.put(char(image.rgba[p+1]));bmp.put(char(image.rgba[p]));bmp.put(char(image.rgba[p+3]));
    }
    Require(bool(bmp),"Cannot retain outline BMP");
}
struct Difference {
    uint64_t bytes=0,pixels=0,rgb_bytes=0,alpha_bytes=0;
    int maximum=0;
    std::array<int64_t,4> signed_sum{};
    std::string Json() const {
        std::ostringstream s;s<<"{\"bytes\":"<<bytes<<",\"pixels\":"<<pixels<<",\"rgb_bytes\":"<<rgb_bytes
            <<",\"alpha_bytes\":"<<alpha_bytes<<",\"maximum_absolute_byte_delta\":"<<maximum<<",\"signed_channel_sum\":[";
        for(size_t i=0;i<4;++i){if(i)s<<',';s<<signed_sum[i];}s<<"]}";return s.str();
    }
};
Difference Compare(const Image& actual,const Image& reference)
{
    Require(actual.width==reference.width && actual.height==reference.height && actual.rgba.size()==reference.rgba.size(),"Outline image dimensions differ");
    Difference result;
    for(size_t p=0;p<actual.rgba.size();p+=4){bool changed=false;
        for(size_t c=0;c<4;++c){const int delta=int(actual.rgba[p+c])-int(reference.rgba[p+c]);
            result.signed_sum[c]+=delta;result.maximum=std::max(result.maximum,std::abs(delta));
            if(delta){++result.bytes;changed=true;if(c==3)++result.alpha_bytes;else ++result.rgb_bytes;}}
        result.pixels+=changed;
    }return result;
}
std::vector<OutlineDepthTriangle> Depth(float z,float w=1)
{
    return {{{{{-1,1,z,w},{1,1,z,w},{1,-1,z,w}}}},{{{{-1,1,z,w},{1,-1,z,w},{-1,-1,z,w}}}}};
}
OutlinePassDraw Draw(OutlinePassKind kind=OutlinePassKind::Silhouette)
{
    OutlinePassDraw draw;draw.kind=kind;draw.x=4;draw.y=4;draw.width=draw.height=6;
    draw.mask_x=3;draw.mask_y=2;draw.source_width=draw.source_height=draw.content_height=6;
    draw.z_ndc=0.5f;draw.colour={1,0,0,1};return draw;
}
Image Mask(bool hole=false)
{
    Image atlas=Solid(16,16,{0,0,0,0});
    for(int y=0;y<6;++y)for(int x=0;x<6;++x)if(!hole || x!=2 || y!=2)Pixel(atlas,x+3,y+2,{0,255,0,255});
    return atlas;
}
bool Covered(const Image& atlas,const OutlinePassDraw& draw,int x,int y)
{
    return x>=0 && y>=0 && x<int(draw.source_width) && y<int(draw.source_height)
        && atlas.rgba[(size_t(draw.mask_y+uint32_t(y))*atlas.width+draw.mask_x+uint32_t(x))*4+1]==255;
}
// Expected sample lists are explicit fixture inputs, independent of shader arithmetic.
void Paint(Image& image,const Image& atlas,const OutlinePassDraw& draw,
    const std::vector<int>& xs,const std::vector<int>& ys,std::array<uint8_t,4> colour)
{
    Require(xs.size()==size_t(draw.width) && ys.size()==size_t(draw.height),"Invalid outline analytic sample list");
    for(int y=0;y<draw.height;++y)for(int x=0;x<draw.width;++x){const int sx=xs[size_t(x)],sy=ys[size_t(y)];
        if(!Covered(atlas,draw,sx,sy))continue;
        if(draw.kind==OutlinePassKind::Edge && Covered(atlas,draw,sx-1,sy) && Covered(atlas,draw,sx+1,sy)
            && Covered(atlas,draw,sx,sy-1) && Covered(atlas,draw,sx,sy+1))continue;
        Pixel(image,draw.x+x,draw.y+y,colour);
    }
}
struct Runner {
    Device& device;std::filesystem::path output;uint64_t strict_bytes=0,blend_difference_bytes=0;
    std::vector<std::string> records;
    Image Render(const Image& background,const Image& atlas,const std::vector<OutlineDepthTriangle>& terrain,const std::vector<OutlinePassDraw>& draws)
    {
        const auto before=device.GetDiagnostics();auto result=device.CompositeOutlines(background,atlas,terrain,draws);const auto after=device.GetDiagnostics();
        Require(before.allocation_count==after.allocation_count && before.allocation_bytes==after.allocation_bytes,"Outline operator retained GPU allocations");
        Require(before.validation_errors==after.validation_errors && before.validation_warnings==after.validation_warnings,"Outline operator emitted validation diagnostics");
        return result;
    }
    void Case(const std::string& name,const Image& background,const Image& atlas,const std::vector<OutlineDepthTriangle>& terrain,
        const std::vector<OutlinePassDraw>& draws,const Image* expected)
    {
        auto actual=Render(background,atlas,terrain,draws);
        auto gl=RenderOutlineGLReference(background,atlas,terrain,draws);
        const auto difference=Compare(actual,gl);
        const auto analytic=expected?Compare(actual,*expected):Difference{};
        records.push_back("{\"name\":"+Quote(name)+",\"comparison\":"+Quote(expected?"exact-analytic-and-production-GL":"recorded-blend-byte-differences")
            +",\"gl_difference\":"+difference.Json()+",\"analytic_difference\":"+(expected?analytic.Json():"null")+"}");
        if(!expected || difference.bytes || analytic.bytes){
            Retain(output/("outline-"+name+"-dx12"),actual);Retain(output/("outline-"+name+"-gl"),gl);
            if(expected)Retain(output/("outline-"+name+"-expected"),*expected);
        }
        if(expected){strict_bytes+=actual.rgba.size()*2;
            if(difference.bytes || analytic.bytes)throw std::runtime_error("Outline exact operator mismatch: "+name);
        }else{
            blend_difference_bytes+=difference.bytes;
            Require(Compare(actual,background).pixels>0 && Compare(gl,background).pixels>0,"Outline blend comparison drew no pixels");
        }
    }
    void Reject(const std::string& name,const Image& background,const Image& atlas,const std::vector<OutlineDepthTriangle>& terrain,const std::vector<OutlinePassDraw>& draws)
    {
        const auto before=device.GetDiagnostics();bool rejected=false;
        try{device.CompositeOutlines(background,atlas,terrain,draws);}catch(const std::invalid_argument&){rejected=true;}
        const auto after=device.GetDiagnostics();
        Require(rejected,"Invalid outline input was not rejected as invalid_argument");
        Require(before.allocation_count==after.allocation_count && before.allocation_bytes==after.allocation_bytes
            && before.validation_errors==after.validation_errors && before.validation_warnings==after.validation_warnings,"Rejected outline input altered GPU resources or diagnostics");
        records.push_back("{\"name\":"+Quote(name)+",\"rejected\":true}");
    }
};
void Fixtures(Runner& run)
{
    const auto background=Solid(32,32,{17,31,47,255});const auto atlas=Mask();const std::vector<int> native={0,1,2,3,4,5};
    auto draw=Draw();Image expected=background;Paint(expected,atlas,draw,native,native,{255,0,0,255});
    run.Case("silhouette-hidden",background,atlas,Depth(0),{draw},&expected);
    run.Case("silhouette-clear-depth",background,atlas,{}, {draw},&background);
    auto equal=draw;equal.z_ndc=0;
    run.Case("greater-strict-equal-rejected",background,atlas,Depth(0),{equal},&background);
    equal.kind=OutlinePassKind::VisibleCoverage;
    run.Case("lequal-equal-visible",background,atlas,Depth(0),{equal},&expected);
    auto far=Depth(0.75f),near=Depth(-0.5f);auto ordered=near;ordered.insert(ordered.end(),far.begin(),far.end());
    run.Case("always-last-far-depth",background,atlas,ordered,{draw},&background);
    ordered=far;ordered.insert(ordered.end(),near.begin(),near.end());
    run.Case("always-last-near-depth",background,atlas,ordered,{draw},&expected);
    auto first=draw;first.z_ndc=0.75f;auto second=draw;second.z_ndc=0.25f;second.colour={0,1,0,1};
    auto green=background;Paint(green,atlas,second,native,native,{0,255,0,255});
    run.Case("hidden-does-not-write-depth",background,atlas,Depth(0),{first,second},&green);
    first.kind=second.kind=OutlinePassKind::VisibleCoverage;first.z_ndc=-0.75f;second.z_ndc=-0.25f;
    run.Case("visible-does-not-write-depth",background,atlas,Depth(0),{first,second},&green);
    auto wdepth=Depth(0);wdepth[0].vertices[0].w=3;wdepth[0].vertices[1].w=511;wdepth[0].vertices[2].w=65537;
    wdepth[1].vertices[0].w=3;wdepth[1].vertices[1].w=65537;wdepth[1].vertices[2].w=2147483648.0f;
    run.Case("varying-camera-w",background,atlas,wdepth,{draw},&expected);
    auto hole=Mask(true);auto edge=Draw(OutlinePassKind::Edge);auto edge_expected=background;
    Paint(edge_expected,hole,edge,native,native,{255,0,0,255});
    run.Case("cardinal-edge-and-hole",background,hole,Depth(0),{edge},&edge_expected);
    edge.height=4;edge.content_height=4;edge_expected=background;
    Paint(edge_expected,atlas,edge,native,{0,1,2,3},{255,0,0,255});
    Require(edge_expected.rgba[(7*32+6)*4]==17,"Content clip fixture accidentally treats bottom as edge");
    run.Case("content-bottom-is-not-logical-edge",background,atlas,Depth(0),{edge},&edge_expected);
    for(bool premultiplied:{false,true})for(bool flip:{false,true}){
        auto mapped=Draw(OutlinePassKind::Edge);mapped.x=-2;mapped.y=3;mapped.width=12;mapped.height=8;
        mapped.content_height=4;mapped.phase_x=mapped.phase_y=65536;mapped.step_x=mapped.step_y=131072;
        mapped.flip=flip;mapped.premultiplied=premultiplied;
        std::vector<int> xs={0,0,0,1,1,2,2,3,3,4,4,5};if(flip)for(int& x:xs)x=5-x;
        auto mapped_expected=background;Paint(mapped_expected,hole,mapped,xs,{0,0,0,1,1,2,2,3},{255,0,0,255});
        run.Case(std::string("clipped-phase-")+(premultiplied?"instanced":"fallback")+(flip?"-flip":""),background,hole,Depth(0),{mapped},&mapped_expected);
    }
    auto overlap=draw;overlap.x=7;overlap.y=6;overlap.colour={0,1,0,1};
    auto forward=expected;Paint(forward,atlas,overlap,native,native,{0,255,0,255});
    auto reverse=background;Paint(reverse,atlas,overlap,native,native,{0,255,0,255});Paint(reverse,atlas,draw,native,native,{255,0,0,255});
    Require(forward.rgba!=reverse.rgba,"Outline overlap fixture is commutative");
    run.Case("overlap-forward",background,atlas,Depth(0),{draw,overlap},&forward);
    run.Case("overlap-reverse",background,atlas,Depth(0),{overlap,draw},&reverse);
    for(bool premultiplied:{false,true}){
        auto blend=draw;blend.premultiplied=premultiplied;blend.colour={0.73f,0.19f,0.43f,0.37f};
        auto blend2=blend;blend2.x=7;blend2.colour={0.11f,0.81f,0.23f,0.61f};
        auto alpha_background=Solid(32,32,{39,81,123,91});
        auto transparent=blend;transparent.colour[3]=0;
        run.Case(premultiplied?"zero-alpha-premultiplied":"zero-alpha-straight",alpha_background,atlas,Depth(0),{transparent},&alpha_background);
        run.Case(premultiplied?"blend-premultiplied":"blend-straight",alpha_background,atlas,Depth(0),{blend,blend2},nullptr);
        run.Case(premultiplied?"blend-premultiplied-reversed":"blend-straight-reversed",alpha_background,atlas,Depth(0),{blend2,blend},nullptr);
    }
    for(const auto& item:std::vector<std::pair<std::string,float>>{{"zero",0},{"negative",-1},{"nan",std::numeric_limits<float>::quiet_NaN()},
        {"over-limit",std::nextafter(2147483648.0f,std::numeric_limits<float>::infinity())}}){
        auto bad=Depth(0);bad[0].vertices[0].w=item.second;run.Reject("invalid-w-"+item.first,background,atlas,bad,{draw});
    }
    for(unsigned i=0;i<10;++i){auto bad=draw;
        switch(i){case 0:bad.width=0;break;case 1:bad.step_x=0;break;case 2:bad.content_height=7;break;
        case 3:bad.source_width=0;break;case 4:bad.mask_x=UINT32_MAX;break;case 5:bad.phase_y=INT32_MIN;break;
        case 6:bad.colour[0]=std::numeric_limits<float>::quiet_NaN();break;case 7:bad.z_ndc=std::numeric_limits<float>::infinity();break;
        case 8:bad.kind=static_cast<OutlinePassKind>(3);break;case 9:bad.source_height=1025;break;}
        run.Reject("invalid-mapping-"+std::to_string(i),background,atlas,Depth(0),{bad});
    }
    auto nonbinary=atlas;Pixel(nonbinary,3,2,{0,127,0,255});run.Reject("nonbinary-coverage",background,nonbinary,Depth(0),{draw});
    auto truncated=atlas;truncated.rgba.pop_back();run.Reject("truncated-atlas",background,truncated,Depth(0),{draw});
    run.Case("repeat-resource-reclamation",background,atlas,Depth(0),{draw},&expected);
}
}
bool RunOutlineOperatorTests(Device& device,const std::filesystem::path& output)
{
    Runner run{device,output,0,0,{}};std::string error;
    try{Fixtures(run);}catch(const std::exception& failure){error=failure.what();}
    std::ofstream report(output/"outline-operator.json");
    report<<"{\"schema\":1,\"passed\":"<<(error.empty() && !run.blend_difference_bytes?"true":"false")
        <<",\"strict_checks_passed\":"<<(error.empty()?"true":"false")
        <<",\"blend_review_required\":"<<(run.blend_difference_bytes?"true":"false")
        <<",\"scope\":\"Exact analytic visibility and opaque production-GL geometry; blend byte differences retained for review, no world or Gate 2 acceptance\""
        <<",\"width\":32,\"height\":32,\"strict_byte_checks\":"<<run.strict_bytes
        <<",\"blend_comparisons_exact\":"<<(run.blend_difference_bytes?"false":"true")
        <<",\"blend_difference_bytes\":"<<run.blend_difference_bytes<<",\"gl_device\":"<<Quote(OutlineGLReferenceDevice())
        <<",\"error\":"<<Quote(error)<<",\"cases\":[";
    for(size_t i=0;i<run.records.size();++i){if(i)report<<',';report<<run.records[i];}report<<"]}\n";
    Require(bool(report),"Cannot retain outline operator report");
    if(!error.empty())throw std::runtime_error(error);
    return run.blend_difference_bytes == 0;
}
}
