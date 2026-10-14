// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#include "EnhancedSpriteOperatorTests.h"
#include "EnhancedSpriteTestImages.h"
#include "OutlineGLOracle.h"
#include <algorithm>
#include <limits>

namespace kfx::render::dx12 {
namespace {
using namespace enhanced_test;
constexpr std::array<float,9> Identity{1,0,0,0,1,0,0,0,1};
struct Runner {
    Device& device;
    std::filesystem::path output;
    Image background{256,8,std::vector<uint8_t>(256*8*4)};
    Image atlas{256,8,std::vector<uint8_t>(256*8*4)};
    std::vector<SpriteColourRow> rows{3};
    std::vector<std::string> records;
    uint64_t checks=0, differences=0;
    uint32_t invalid=0;
    Runner(Device& d,const std::filesystem::path& p):device(d),output(p)
    {
        for(size_t p=0;p<256*8;++p){
            background.rgba[p*4]=80;background.rgba[p*4+1]=110;
            background.rgba[p*4+2]=140;background.rgba[p*4+3]=73;
            atlas.rgba[p*4]=uint8_t(p);atlas.rgba[p*4+1]=p%7?255:0;
        }
        for(size_t i=0;i<256;++i){
            rows[0][i*4]=uint8_t(i);rows[0][i*4+1]=uint8_t(255-i);
            rows[0][i*4+2]=uint8_t(i^91);rows[0][i*4+3]=255;
        }
        for(size_t i=0;i<256;++i){
            for(size_t c=0;c<4;++c)rows[1][i*4+c]=rows[0][(255-i)*4+c];
        }
    }
    Image Compare(const std::string& name,const std::vector<OutlinePassDraw>& draws,
        const Image* expected=nullptr,bool exact_gl=false,const std::array<float,9>& matrix=Identity,
        const std::vector<OutlineDepthTriangle>& depth={},
        const std::vector<std::array<uint32_t,6>>& controls={})
    {
        const auto before=device.GetDiagnostics();
        auto gpu=device.CompositeEnhancedSprites(background,atlas,rows,matrix,depth,draws);
        const auto after=device.GetDiagnostics();
        Require(after.allocation_count==before.allocation_count && after.allocation_bytes==before.allocation_bytes
            && after.validation_errors==before.validation_errors && after.validation_warnings==before.validation_warnings,
            "Enhanced sprite call retained resources or produced validation messages");
        const auto gl=RenderEnhancedSpritesGLReference(background,atlas,rows,matrix,depth,draws);
        const auto count=Differences(gpu,gl);differences+=count;checks+=gpu.rgba.size();
        const auto analytic=expected?Differences(gpu,*expected)+Differences(gl,*expected):0;
        uint64_t different_pixels=0,rgb_differences=0,alpha_differences=0,large_pixels=0,control_failures=0;
        unsigned maximum_delta=0;
        for(size_t p=0;p<gpu.rgba.size();p+=4){
            unsigned pixel_delta=0;
            for(size_t c=0;c<4;++c){
                const unsigned delta=unsigned(std::abs(int(gpu.rgba[p+c])-int(gl.rgba[p+c])));
                pixel_delta=std::max(pixel_delta,delta);
                if(c==3)alpha_differences+=delta!=0;else rgb_differences+=delta!=0;
            }
            different_pixels+=pixel_delta!=0;large_pixels+=pixel_delta>1;
            maximum_delta=std::max(maximum_delta,pixel_delta);
        }
        for(const auto& control:controls){
            Require(control[0]<gpu.width && control[1]<gpu.height,"Invalid enhanced control coordinate");
            const auto p=(size_t(control[1])*gpu.width+control[0])*4;
            for(size_t c=0;c<4;++c){
                control_failures+=gpu.rgba[p+c]!=control[c+2];
                control_failures+=gl.rgba[p+c]!=control[c+2];
            }
            checks+=8;
        }
        if(expected)checks+=gpu.rgba.size()*2;
        records.push_back("{\"name\":"+Json(name)+",\"bytes\":"+std::to_string(gpu.rgba.size())
            +",\"glDifferingBytes\":"+std::to_string(count)+",\"analyticDifferingBytes\":"+std::to_string(analytic)
            +",\"differentPixels\":"+std::to_string(different_pixels)+",\"rgbDifferingBytes\":"+std::to_string(rgb_differences)
            +",\"alphaDifferingBytes\":"+std::to_string(alpha_differences)+",\"maximumByteDelta\":"+std::to_string(maximum_delta)
            +",\"pixelsWithDeltaAboveOne\":"+std::to_string(large_pixels)+",\"strictControlPixels\":"+std::to_string(controls.size())
            +",\"controlFailures\":"+std::to_string(control_failures)
            +",\"strictGeometry\":"+(exact_gl?"true":"false")+"}");
        if(count||analytic||control_failures||!exact_gl){
            Save(output/("enhanced-"+name+"-gpu"),gpu);Save(output/("enhanced-"+name+"-gl"),gl);
            if(expected)Save(output/("enhanced-"+name+"-expected"),*expected);
        }
        Require(!analytic,"Enhanced sprite analytic comparison failed");
        Require(!control_failures,"Enhanced sprite exact control pixel comparison failed");
        Require(!exact_gl||!count,"Opaque enhanced sprite production-GL comparison failed");
        return gpu;
    }
};
OutlinePassDraw Draw(unsigned path,OutlinePassKind kind=OutlinePassKind::NormalSprite)
{
    OutlinePassDraw d;d.kind=kind;d.width=256;d.height=8;
    d.source_width=256;d.source_height=d.content_height=8;
    d.premultiplied=path==0;d.texture_fallback=path==2;
    if(kind==OutlinePassKind::NormalSprite)d.lookup_row=0;
    return d;
}
Image Opaque(const Runner& r,const OutlinePassDraw& d)
{
    auto out=r.background;
    for(int y=0;y<int(out.height);++y)for(int x=0;x<int(out.width);++x){
        if(x<d.x||x>=d.x+d.width||y<d.y||y>=d.y+d.height)continue;
        int sx=std::min(std::max(((x-d.x+1)*65536-d.phase_x-1),0)/d.step_x,int(d.source_width)-1);
        const int sy=std::min(std::max(((y-d.y+1)*65536-d.phase_y-1),0)/d.step_y,int(d.content_height)-1);
        if(d.flip)sx=int(d.source_width)-1-sx;
        const auto sample=(size_t(sy+d.mask_y)*r.atlas.width+sx+d.mask_x)*4;
        if(!r.atlas.rgba[sample+1])continue;
        const auto index=r.atlas.rgba[sample];
        const auto pixel=(size_t(y)*out.width+x)*4;
        for(size_t c=0;c<3;++c)out.rgba[pixel+c]=r.rows[d.lookup_row][index*4+c];
        out.rgba[pixel+3]=255;
    }
    return out;
}
void NativeEdgeFixtures(Runner& r)
{
    // The captured edge crosses pixel centres at slope 1/9 and continues past
    // the native viewport. Retain the original float NDC values, not rounded
    // screen coordinates. The two bucket depths straddle the sprite depth.
    auto old_background=std::move(r.background),old_atlas=std::move(r.atlas);
    r.background={3087,2160,std::vector<uint8_t>(size_t(3087)*2160*4)};
    for(size_t p=0;p<r.background.rgba.size();p+=4){
        r.background.rgba[p]=31;r.background.rgba[p+1]=47;
        r.background.rgba[p+2]=61;r.background.rgba[p+3]=255;
    }
    r.atlas={48,64,std::vector<uint8_t>(48*64*4)};
    for(unsigned y=0;y<64;++y)for(unsigned x=0;x<48;++x){
        const auto p=(y*48+x)*4;
        r.atlas.rgba[p]=uint8_t(x*17+y*13);r.atlas.rgba[p+1]=(x+y)%5?255:0;
    }
    for(unsigned path=0;path<3;++path){
        auto mapping=Draw(path);mapping.x=2945;mapping.y=1350;mapping.width=93;mapping.height=128;
        mapping.source_width=48;mapping.source_height=mapping.content_height=64;
        mapping.phase_x=66131;mapping.phase_y=4361;mapping.step_x=132263;mapping.step_y=133098;
        mapping.flip=path==1;
        const auto expected=Opaque(r,mapping);
        r.Compare("native-phase-no-depth-path"+std::to_string(path),{mapping},&expected,true);
    }
    std::vector<OutlineDepthTriangle> depth(2);
    depth[0].vertices={{{1.0038871765136719f,-0.3305555582046509f,-0.8877227306365967f,1},
        {0.951409101486206f,-0.3222222328186035f,-0.8877227306365967f,1},
        {0.9572399854660034f,-0.2749999761581421f,-0.8877227306365967f,1}}};
    depth[1].vertices={{{0.951409101486206f,-0.3222222328186035f,-0.8901635408401489f,1},
        {1.0038871765136719f,-0.3305555582046509f,-0.8901635408401489f,1},
        {1.00453519821167f,-0.3712962865829468f,-0.8901635408401489f,1}}};
    r.atlas={1,1,{11,255,0,0}}; // Orange family, strength 2: exact RGB delta48,32,0.
    auto glow=Draw(0,OutlinePassKind::GlowAdd);
    glow.x=2945;glow.y=1350;glow.width=93;glow.height=128;
    glow.source_width=glow.source_height=glow.content_height=1;
    glow.z_ndc=-0.8879668116569519f;
    for(bool translated:{false,true}){
        const int shift=translated?256:0;
        auto triangles=depth;auto draw=glow;draw.x-=shift;
        if(translated)for(auto& triangle:triangles)for(auto& v:triangle.vertices)
            v.x-=2.0f*float(shift)/float(r.background.width);
        const std::string name=translated?"native-edge-inside":"native-edge-clipped";
        const std::vector<std::array<uint32_t,6>> controls={
            {uint32_t(3035-shift),1405,79,79,61,255}, // Far triangle: glow passes.
            {uint32_t(3035-shift),1435,31,47,61,255}, // Near triangle: glow rejected.
            {uint32_t(2980-shift),1405,79,79,61,255}}; // Clear depth: glow passes.
        r.Compare(name+"-glow",{draw},nullptr,false,Identity,triangles,controls);
        // Read-only comparison probes expose the final D24 relation at every
        // pixel: red=clear, green=far triangle, blue=near triangle. They do not
        // substitute computed depth or reference pixels into the GPU render.
        auto probe=draw;probe.kind=OutlinePassKind::VisibleCoverage;probe.z_ndc=0.5f;probe.colour={1,0,0,1};
        auto any_terrain=probe;any_terrain.kind=OutlinePassKind::Silhouette;any_terrain.colour={0,1,0,1};
        auto near_terrain=any_terrain;near_terrain.z_ndc=draw.z_ndc;near_terrain.colour={0,0,1,1};
        r.Compare(name+"-depth-classes",{probe,any_terrain,near_terrain},nullptr,false,Identity,triangles,
            {{uint32_t(3035-shift),1405,0,255,0,255},{uint32_t(3035-shift),1435,0,0,255,255},
             {uint32_t(2980-shift),1405,255,0,0,255}});
    }
    // Small rectangles strictly inside the near/far triangles admit a complete
    // analytic image comparison, including exact LEQUAL at the near bucket.
    for(unsigned mode=0;mode<3;++mode){
        auto draw=glow;draw.x=3034;draw.y=mode?1434:1404;draw.width=draw.height=3;
        if(mode==2)draw.z_ndc=depth[1].vertices[0].z;
        auto expected=r.background;
        if(mode!=1)for(int y=draw.y;y<draw.y+3;++y)for(int x=draw.x;x<draw.x+3;++x){
            const auto p=(size_t(y)*expected.width+x)*4;expected.rgba[p]=79;expected.rgba[p+1]=79;
        }
        r.Compare("native-edge-strict-control-"+std::to_string(mode),{draw},&expected,true,Identity,depth);
    }
    r.background=std::move(old_background);r.atlas=std::move(old_atlas);
}
void Fixtures(Runner& r)
{
    std::vector<OutlineDepthTriangle> depth(2);
    depth[0].vertices={{{-1,1,0,1},{1,1,0,1},{-1,-1,0,1}}};
    depth[1].vertices={{{1,1,0,1},{1,-1,0,1},{-1,-1,0,1}}};
    for(unsigned path=0;path<3;++path){
        const auto suffix="-path"+std::to_string(path);
        auto d=Draw(path);
        // The first covered index-zero texel distinguishes opaque zero from a hole.
        r.atlas.rgba[256*4+1]=255;
        auto expected=Opaque(r,d);r.Compare("opaque"+suffix,{d},&expected,true);
        d.lookup_row=1;expected=Opaque(r,d);r.Compare("effective-remap"+suffix,{d},&expected,true);
        d.x=-3;d.width=263;d.phase_x=18000;d.step_x=80000;d.flip=true;
        d.content_height=5;d.phase_y=41000;d.step_y=97000;
        expected=Opaque(r,d);r.Compare("clipped-phase-content-flip"+suffix,{d},&expected,true);
        d=Draw(path);d.lookup_row=2;
        expected=path==2?Opaque(r,d):r.background;
        r.Compare("zero-initialized-clut"+suffix,{d},&expected,true);
        for(float alpha:{0.0f,0.75f,1.0f/3.0f,2.0f/3.0f}){
            d=Draw(path);d.source_alpha=alpha;
            const auto name="opacity-"+std::to_string(alpha)+suffix;
            r.Compare(name,{d},alpha==0?&r.background:nullptr,alpha==0);
        }
        d=Draw(path);d.source_alpha=0.75f;r.rows[2]=r.rows[0];
        for(size_t i=0;i<256;++i)r.rows[2][i*4+3]=uint8_t(i);
        d.lookup_row=2;r.Compare("lookup-alpha"+suffix,{d});r.rows[2].fill(0);

        auto add=Draw(path,OutlinePassKind::GlowAdd),dark=Draw(path,OutlinePassKind::GlowDarken);
        // Analytic integer RGB deltas for all 256 raw indices, including holes,
        // every family/strength, black, invalid codes and zero-strength codes.
        auto glow_expected=r.background;
        constexpr int steps[8][3]={{16,16,16},{24,16,0},{24,4,4},{8,8,24},{8,24,8},{12,0,12},{8,8,8},{24,12,4}};
        for(size_t p=0;p<256*8;++p){
            const int px=r.atlas.rgba[p*4];if(px<1||px>64)continue;
            const int family=(px-1)/8,strength=(px-1)%8;if(!strength)continue;
            for(size_t c=0;c<3;++c)glow_expected.rgba[p*4+c]=uint8_t(std::clamp(int(glow_expected.rgba[p*4+c])
                +(family==6?-1:1)*steps[family][c]*strength,0,255));
            if(path!=0&&family!=6)glow_expected.rgba[p*4+3]=255;
        }
        r.Compare("all-glow-codes"+suffix,{add,dark},&glow_expected,true);
        const std::array<float,9> matrix{0.25f,0.5f,0.125f,-0.5f,1,0,0.125f,0.25f,0.5f};
        r.Compare("glow-asymmetric-matrix"+suffix,{add,dark},nullptr,false,matrix);
        auto normal=Draw(path);normal.source_alpha=0.75f;
        const auto forward=r.Compare("normal-glow-order"+suffix,{normal,add,dark,normal});
        const auto reverse=r.Compare("glow-normal-order"+suffix,{add,dark,normal,normal});
        Require(forward.rgba!=reverse.rgba,"Enhanced overlap order fixture is commutative");
        // Constant black/white samples force saturation before subtraction.
        auto white=add;white.source_width=white.source_height=white.content_height=1;white.mask_x=8;
        auto black=dark;black.source_width=black.source_height=black.content_height=1;black.mask_x=56;
        r.Compare("saturate-then-subtract"+suffix,{white,white,white,black});
        r.Compare("subtract-then-saturate"+suffix,{black,white,white,white});
        auto saved_atlas=r.atlas;
        for(size_t i=1;i<r.atlas.rgba.size();i+=4)r.atlas.rgba[i]=255;
        auto front_glow=add;front_glow.z_ndc=-0.5f;
        auto equal_normal=Draw(path);expected=Opaque(r,equal_normal);
        r.Compare("glow-does-not-write-depth"+suffix,{front_glow,equal_normal},&expected,true,Identity,depth);
        auto hidden_outline=Draw(path,OutlinePassKind::Silhouette);
        hidden_outline.z_ndc=0.5f;hidden_outline.colour={0,1,0,1};
        auto hidden_normal=Draw(path);hidden_normal.z_ndc=0.25f;
        for(size_t p=0;p<expected.rgba.size();p+=4){
            expected.rgba[p]=expected.rgba[p+2]=0;expected.rgba[p+1]=expected.rgba[p+3]=255;
        }
        r.Compare("outline-does-not-write-depth"+suffix,{hidden_outline,hidden_normal},&expected,true,Identity,depth);
        r.atlas=std::move(saved_atlas);
    }
    // Full-screen depth plane plus painter overwrite: colour stays declared.
    auto visible=Draw(0);visible.z_ndc=0;
    auto silhouette=Draw(0,OutlinePassKind::Silhouette);silhouette.z_ndc=0;silhouette.colour={1,0,1,1};
    auto expected=Opaque(r,visible);
    r.Compare("equal-depth-outline-rejected-normal-visible",{silhouette,visible},&expected,true,Identity,depth);
    visible.z_ndc=0.5f;r.Compare("normal-behind-depth",{visible},&r.background,true,Identity,depth);
    auto foreground=Draw(0);foreground.z_ndc=-0.5f;
    auto later=Draw(0);later.z_ndc=0;later.lookup_row=1;expected=Opaque(r,later);
    r.Compare("normal-does-not-write-depth",{foreground,later},&expected,true,Identity,depth);
    silhouette.z_ndc=0.5f;silhouette.colour={0.25f,0.5f,0.75f,0.5f};
    auto edge=silhouette;edge.kind=OutlinePassKind::Edge;
    auto add=Draw(0,OutlinePassKind::GlowAdd),dark=Draw(0,OutlinePassKind::GlowDarken);
    r.Compare("two-outline-normal-glow-groups",{silhouette,foreground,add,dark,edge,later},nullptr,false,Identity,depth);
    for(unsigned bad=0;bad<12;++bad){
        auto draw=Draw(0);auto atlas=r.atlas;auto matrix=Identity;
        switch(bad){
        case 0:draw.lookup_row=NoSpriteColourRow;break;
        case 1:draw.lookup_row=uint32_t(r.rows.size());break;
        case 2:draw.texture_fallback=true;break;
        case 3:draw.source_alpha=std::numeric_limits<float>::quiet_NaN();break;
        case 4:draw.kind=OutlinePassKind::GlowAdd;break; // Unsupported lookup binding on glow.
        case 5:draw.kind=static_cast<OutlinePassKind>(255);break;
        case 6:matrix[3]=std::numeric_limits<float>::infinity();break;
        case 7:matrix[4]=1025;break;
        case 8:draw.step_x=0;break;
        case 9:draw.source_width=257;break;
        case 10:atlas.rgba[1]=1;break;
        case 11:draw.kind=OutlinePassKind::GlowDarken;draw.lookup_row=NoSpriteColourRow;draw.source_alpha=0.5f;break;
        }
        const auto before=r.device.GetDiagnostics();bool rejected=false;
        try{r.device.CompositeEnhancedSprites(r.background,atlas,r.rows,matrix,{}, {draw});}
        catch(const std::invalid_argument&){rejected=true;}
        const auto after=r.device.GetDiagnostics();
        Require(rejected && after.allocation_count==before.allocation_count && after.allocation_bytes==before.allocation_bytes
            && after.validation_errors==before.validation_errors && after.validation_warnings==before.validation_warnings,
            "Malformed enhanced sprite input was not cleanly rejected");
        ++r.invalid;
    }
    bool rejected=false;
    try{r.device.CompositeOutlines(r.background,r.atlas,{}, {Draw(0)});}catch(const std::invalid_argument&){rejected=true;}
    Require(rejected,"Old outline API accepted new sprite-art operator");++r.invalid;
    NativeEdgeFixtures(r);
}
}
bool RunEnhancedSpriteOperatorTests(Device& device,const std::filesystem::path& output)
{
    Runner run(device,output);std::string error;
    try{Fixtures(run);}catch(const std::exception& failure){error=failure.what();}
    std::ofstream report(output/"enhanced-sprite-operator.json");
    report<<"{\"schema\":1,\"strictPassed\":"<<(error.empty()?"true":"false")
        <<",\"passed\":"<<(error.empty()&&!run.differences?"true":"false")
        <<",\"scope\":\"Normal sprite artwork, effective GL colour rows, glow and ordered outline phases on declared RGBA background; terrain depth only\""
        <<",\"gate2Accepted\":false,\"comparedBytes\":"<<run.checks<<",\"glDifferingBytes\":"<<run.differences
        <<",\"reviewRequired\":"<<(run.differences?"true":"false")<<",\"invalidCases\":"<<run.invalid
        <<",\"glDevice\":"<<Json(OutlineGLReferenceDevice())<<",\"error\":"<<Json(error)<<",\"cases\":[";
    for(size_t i=0;i<run.records.size();++i){if(i)report<<',';report<<run.records[i];}report<<"]}\n";
    Require(bool(report),"Cannot retain enhanced sprite operator report");
    if(!error.empty())throw std::runtime_error(error);
    return !run.differences;
}
}
