// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#include "WorldOverlayOperatorTests.h"
#include "WorldOverlayGLOracle.h"
#include "EnhancedSpriteTestImages.h"
#include <limits>

namespace kfx::render::dx12 {
namespace {
using namespace enhanced_test;
WorldOverlayPass Base()
{
    WorldOverlayPass p;p.native_width=32;p.native_height=24;
    IndexedOverlaySource source;source.width=source.height=8;source.indices.resize(64);source.initialized_rows.assign(8,1);
    for(unsigned y=0;y<8;++y)for(unsigned x=0;x<8;++x)source.indices[y*8+x]=(x+y*3)%5==0?0:uint8_t(1+(x+y*7)%7);
    p.sources.push_back(std::move(source));p.colour_rows.resize(1);
    for(unsigned i=0;i<256;++i){auto& row=p.colour_rows[0];row[i*4]=uint8_t(i*37+19);
        row[i*4+1]=uint8_t(i*67+41);row[i*4+2]=uint8_t(i*103+71);row[i*4+3]=0;}
    return p;
}
WorldOverlayQuad Quad(WorldOverlayMode mode=WorldOverlayMode::Sprite)
{
    WorldOverlayQuad q;q.mode=mode;q.x0=4;q.y0=4;q.x1=28;q.y1=20;
    if(mode!=WorldOverlayMode::Solid){q.source=0;q.u0=q.v0=(1.0f/32)/8;q.u1=q.v1=1-q.u0;}
    if(mode!=WorldOverlayMode::Solid&&mode!=WorldOverlayMode::Colored)q.colour_row=0;
    return q;
}
Image Background(const WorldOverlayPass& p)
{
    Image image{p.native_width,p.native_height,std::vector<uint8_t>(size_t(p.native_width)*p.native_height*4)};
    for(unsigned y=0;y<p.native_height;++y)for(unsigned x=0;x<p.native_width;++x){const size_t i=(size_t(y)*p.native_width+x)*4;
        image.rgba[i]=uint8_t(17+x*3+y);image.rgba[i+1]=uint8_t(31+y*7);image.rgba[i+2]=uint8_t(81+x);image.rgba[i+3]=uint8_t(x*13+y*11);}
    return image;
}
void Fill(Image& image,std::array<uint8_t,4> colour)
{
    for(size_t i=0;i<image.rgba.size();i+=4)std::copy(colour.begin(),colour.end(),image.rgba.begin()+i);
}
std::vector<OutlineDepthTriangle> Plane(float depth,float w=1)
{
    const OutlineDepthVertex a{-1,1,depth,w},b{1,1,depth,w},c{1,-1,depth,w},d{-1,-1,depth,w};
    return {{{a,b,c}},{{a,c,d}}};
}
// The fixtures seed constant full-screen planes, so the independent analytic
// depth oracle needs no triangle rasterizer. Overlay rectangles use pixel
// centres and exact owned resource bytes; blending is computed in double.
Image Analytic(const WorldOverlayPass& p,const Image& background,float terrain_depth=1)
{
    Image result=background;
    for(const auto& q:p.draws)for(unsigned y=0;y<p.native_height;++y)for(unsigned x=0;x<p.native_width;++x){
        const double px=x+0.5,py=y+0.5;
        if(px<q.x0||px>=q.x1||py<q.y0||py>=q.y1||q.z_ndc<-1||q.z_ndc>1)continue;
        if(q.depth_test&&q.z_ndc>terrain_depth)continue;
        if(q.scissor_enabled&&(int64_t(x)<q.scissor_x||int64_t(y)<q.scissor_y
            ||int64_t(x)>=int64_t(q.scissor_x)+q.scissor_w||int64_t(y)>=int64_t(q.scissor_y)+q.scissor_h))continue;
        unsigned index=0;
        if(q.mode!=WorldOverlayMode::Solid){
            const auto& source=p.sources[q.source];
            const double u=double(q.u0)+(px-q.x0)/(double(q.x1)-q.x0)*(double(q.u1)-q.u0);
            const double v=double(q.v0)+(py-q.y0)/(double(q.y1)-q.y0)*(double(q.v1)-q.v0);
            const auto sx=unsigned(std::clamp(std::floor(u*source.width),0.0,double(source.width-1)));
            const auto sy=unsigned(std::clamp(std::floor(v*source.height),0.0,double(source.height-1)));
            index=source.indices[size_t(sy)*source.width+sx];
            if(index==0)continue;
        }
        std::array<double,4> src{};for(unsigned c=0;c<4;++c)src[c]=q.colour[c];
        if(q.mode==WorldOverlayMode::Fade)index=p.fade[q.fade_row*256+index];
        if(q.mode==WorldOverlayMode::Sprite||q.mode==WorldOverlayMode::Fade||q.mode==WorldOverlayMode::Clut){
            const auto& row=p.colour_rows[q.colour_row];
            for(unsigned c=0;c<3;++c)src[c]*=double(row[index*4+c])/255;
            if(q.mode==WorldOverlayMode::Clut)src[3]*=double(row[index*4+3])/255;
        }
        const size_t i=(size_t(y)*p.native_width+x)*4;
        for(unsigned c=0;c<4;++c)
            result.rgba[i+c]=uint8_t(std::clamp(std::lround(src[c]*src[3]*255+result.rgba[i+c]*(1-src[3])),0l,255l));
    }
    return result;
}
struct Runner {
    Device& device;std::filesystem::path output;std::vector<std::string> records;
    uint64_t comparisons=0,gpu_gl=0,gpu_analytic=0,gl_analytic=0,strict_analytic=0;
    uint64_t classified_gpu_analytic=0,classified_gl_analytic=0,classified_gpu_gl=0;
    unsigned cases=0,invalid=0,classified_boundary_pixels=0;bool checks=true;
    std::string boundary_samples="[]";
    Runner(Device& d,const std::filesystem::path& path):device(d),output(path){}
    std::array<Image,2> Compare(const std::string& name,const WorldOverlayPass& p,const std::vector<OutlineDepthTriangle>& terrain={},
        float final_depth=1,const Image* supplied_background=nullptr,bool boundary_diagnostic=false)
    {
        ValidateWorldOverlayPass(p);const auto background=supplied_background?*supplied_background:Background(p);
        const auto expected=Analytic(p,background,final_depth);const auto before=device.GetDiagnostics();
        const auto gpu=device.CompositeWorldOverlays(p,background,terrain);device.WaitIdle();const auto after=device.GetDiagnostics();
        const bool clean=after.allocation_count==before.allocation_count&&after.allocation_bytes==before.allocation_bytes
            &&after.validation_errors==before.validation_errors&&after.validation_warnings==before.validation_warnings;
        checks&=clean;
        const auto gl=RenderWorldOverlayGLReference(p,background,terrain);
        const auto dg=Differences(gpu,gl),da=Differences(gpu,expected),db=Differences(gl,expected);
        gpu_gl+=dg;gpu_analytic+=da;gl_analytic+=db;comparisons+=uint64_t(gpu.width)*gpu.height*3;++cases;
        if(!boundary_diagnostic)strict_analytic+=da+db;
        Image mask{gpu.width,gpu.height,std::vector<uint8_t>(gpu.rgba.size())};uint64_t alpha=0;unsigned maximum=0;
        for(size_t i=0;i<gpu.rgba.size();i+=4){
            for(unsigned c=0;c<4;++c){mask.rgba[i]|=gpu.rgba[i+c]!=expected.rgba[i+c]?255:0;
                mask.rgba[i+1]|=gl.rgba[i+c]!=expected.rgba[i+c]?255:0;mask.rgba[i+2]|=gpu.rgba[i+c]!=gl.rgba[i+c]?255:0;
                maximum=std::max(maximum,unsigned(std::abs(int(gpu.rgba[i+c])-int(gl.rgba[i+c]))));
                if(c==3)alpha+=gpu.rgba[i+c]!=gl.rgba[i+c];}
            mask.rgba[i+3]=255;
        }
        const auto stem=output/("world-overlay-"+name);
        Save(stem.string()+"-gpu",gpu);Save(stem.string()+"-gl",gl);Save(stem.string()+"-analytic",expected);
        Save(stem.string()+"-difference",mask);Save(stem.string()+"-background",background);
        records.push_back("{\"name\":"+Json(name)+",\"analyticPolicy\":"+Json(boundary_diagnostic?"namedSourceBoundaryDiagnostic":"strictExact")
            +",\"width\":"+std::to_string(gpu.width)+",\"height\":"+std::to_string(gpu.height)
            +",\"draws\":"+std::to_string(p.draws.size())+",\"gpuGlDifferingBytes\":"+std::to_string(dg)
            +",\"gpuAnalyticDifferingBytes\":"+std::to_string(da)+",\"glAnalyticDifferingBytes\":"+std::to_string(db)
            +",\"gpuGlAlphaDifferingBytes\":"+std::to_string(alpha)+",\"maximumGpuGlByteDelta\":"+std::to_string(maximum)
            +",\"resourcesReclaimed\":"+(clean?"true":"false")+"}");
        return {gpu,gl};
    }
    void Reject(const std::string& name,const WorldOverlayPass& p,const Image& background,const std::vector<OutlineDepthTriangle>& terrain={})
    {
        const auto before=device.GetDiagnostics();bool gpu=false,gl=false;
        try{device.CompositeWorldOverlays(p,background,terrain);}catch(const std::invalid_argument&){gpu=true;}
        try{RenderWorldOverlayGLReference(p,background,terrain);}catch(const std::invalid_argument&){gl=true;}
        device.WaitIdle();const auto after=device.GetDiagnostics();
        const bool clean=after.allocation_count==before.allocation_count&&after.allocation_bytes==before.allocation_bytes
            &&after.validation_errors==before.validation_errors&&after.validation_warnings==before.validation_warnings;
        checks&=gpu&&gl&&clean;++invalid;
        records.push_back("{\"name\":"+Json(name)+",\"invalidInput\":true,\"gpuRejected\":"+(gpu?"true":"false")
            +",\"glRejected\":"+(gl?"true":"false")+",\"resourcesUnchanged\":"+(clean?"true":"false")+"}");
    }
};
void BoundaryFixture(Runner& r)
{
    auto p=Base();p.draws={Quad()};auto& q=p.draws[0];q.x0=7;q.x1=25;q.y0=5;q.y1=19;
    std::swap(q.u0,q.u1);std::swap(q.v0,q.v1);
    q.u0=q.u0+(3.0f/21)*(q.u1-q.u0);q.colour={1,0,1,1};
    const auto background=Background(p);
    const auto original=r.Compare("flipped-inset-clipped-normalized-uv",p,{},1,&background,true);
    // Independent coordinate labels are source inputs, never CPU/GPU reference
    // output. Each covered texel reveals its actual integer U/V in R/G.
    auto probe=p;probe.draws[0].colour={1,1,1,1};
    for(unsigned y=0;y<8;++y)for(unsigned x=0;x<8;++x){
        const unsigned index=1+y*8+x;probe.sources[0].indices[y*8+x]=uint8_t(index);
        auto& row=probe.colour_rows[0];row[index*4]=uint8_t(x+1);row[index*4+1]=uint8_t(y+1);
        row[index*4+2]=213;row[index*4+3]=255;
    }
    const auto sampled=r.Compare("flipped-boundary-owned-uv-probe",probe,{},1,&background,true);
    for(int direction:{-1,1}){
        auto control=probe;
        const float shift=float(direction)/(64*8); // Exactly +/-1/64 of an original source texel.
        control.draws[0].u0+=shift;control.draws[0].u1+=shift;
        r.Compare(direction<0?"flipped-boundary-shift-minus-1of64-texel":"flipped-boundary-shift-plus-1of64-texel",control);
    }
    const auto ideal=Analytic(p,background),ideal_probe=Analytic(probe,background);
    std::array<Image,2> explained{background,background};std::string coordinates="[";unsigned count=0;
    std::array<unsigned,2> boundary_counts{};
    for(unsigned backend=0;backend<2;++backend)
    for(unsigned y=0;y<p.native_height;++y)for(unsigned x=0;x<p.native_width;++x){
        const size_t i=(size_t(y)*p.native_width+x)*4;
        const double px=x+0.5,py=y+0.5;
        if(px<q.x0||px>=q.x1||py<q.y0||py>=q.y1){
            for(unsigned c=0;c<4;++c)Require(sampled[backend].rgba[i+c]==background.rgba[i+c],"Boundary probe changed outside geometry");
            continue;
        }
        Require(sampled[backend].rgba[i]>=1&&sampled[backend].rgba[i]<=8&&sampled[backend].rgba[i+1]>=1&&sampled[backend].rgba[i+1]<=8
            &&sampled[backend].rgba[i+2]==213&&sampled[backend].rgba[i+3]==255,"Boundary probe produced an invalid coordinate label");
        const unsigned sx=sampled[backend].rgba[i]-1,sy=sampled[backend].rgba[i+1]-1;
        const double u=(double(q.u0)+(px-q.x0)/(double(q.x1)-q.x0)*(double(q.u1)-q.u0))*8;
        const double v=(double(q.v0)+(py-q.y0)/(double(q.y1)-q.y0)*(double(q.v1)-q.v0))*8;
        const unsigned ix=unsigned(std::floor(u)),iy=unsigned(std::floor(v));
        if(sx!=ix||sy!=iy){
            // Exact named fixture evidence: Radeon/GL round the lower triangle
            // onto boundary 4; WARP rounds both triangles. Independent labels
            // must reconstruct each backend, and shifted controls stay exact.
            // No epsilon or generic adjacent-texel allowance is used.
            Require(x==14&&y>=5&&y<=18&&u==4.0-std::ldexp(1.0,-23)&&ix==3&&sx==4&&sy==iy,
                "Unclassified world overlay source boundary difference");
            ++boundary_counts[backend];
            if(count++)coordinates+=',';
            coordinates+="{\"backend\":"+Json(backend?"gl":"gpu")+",\"x\":"+std::to_string(x)+",\"y\":"+std::to_string(y)
                +",\"idealSourceU\":3.99999988079071044921875,\"idealTexelU\":3,\"sampledTexelU\":4,\"sampledTexelV\":"+std::to_string(sy)+"}";
        }
        if(x==14&&y>=11&&y<=18)Require(sx==4,"Named lower triangle boundary samples changed");
        const unsigned index=p.sources[0].indices[sy*8+sx];
        if(index){explained[backend].rgba[i]=p.colour_rows[0][index*4];explained[backend].rgba[i+1]=0;
            explained[backend].rgba[i+2]=p.colour_rows[0][index*4+2];explained[backend].rgba[i+3]=255;}
    }
    for(const auto n:boundary_counts)Require(n==8||n==14,"Named source boundary sample set changed");
    Require(Differences(original[0],explained[0])==0&&Differences(original[1],explained[1])==0,
        "Sample coordinates do not explain every original world overlay pixel");
    for(unsigned backend=0;backend<2;++backend)
        Require(Differences(original[backend],ideal)==(boundary_counts[backend]==8?22:40)
            &&Differences(sampled[backend],ideal_probe)==boundary_counts[backend],
            "Named boundary diagnostic raw mismatch counts changed");
    r.classified_gpu_analytic=boundary_counts[0]==8?30:54;
    r.classified_gl_analytic=boundary_counts[1]==8?30:54;
    r.classified_gpu_gl=boundary_counts[0]==boundary_counts[1]?0:24;
    r.classified_boundary_pixels=count;
    r.boundary_samples=coordinates+']';
    Save(r.output/"world-overlay-flipped-boundary-gpu-sample-explained",explained[0]);
    Save(r.output/"world-overlay-flipped-boundary-gl-sample-explained",explained[1]);
}
void Fixtures(Runner& r)
{
    auto p=Base();r.Compare("empty-preserves-native-rgba",p);
    p.draws={Quad()};r.Compare("sprite-zero-discard-ignores-palette-alpha",p);
    p.draws={Quad(WorldOverlayMode::Colored)};p.draws[0].colour={1,0,1,1};r.Compare("colored-nonzero-source-mask",p);
    p.draws={Quad(WorldOverlayMode::Solid)};p.draws[0].colour={0,1,0,1};r.Compare("solid-no-texture",p);
    p.draws={Quad(WorldOverlayMode::Fade)};p.draws[0].fade_row=63;p.fade.resize(16384);
    for(unsigned row=0;row<64;++row)for(unsigned i=0;i<256;++i)p.fade[row*256+i]=uint8_t(i+row*3);
    p.fade[63*256]=200;p.fade[63*256+1]=0;r.Compare("fade-row63-discard-before-map-zero-opaque",p);
    p=Base();p.draws={Quad(WorldOverlayMode::Clut)};
    for(unsigned i=0;i<256;++i)p.colour_rows[0][i*4+3]=i%2?255:0;
    p.colour_rows[0][3]=255;r.Compare("clut-source-zero-discard-and-row-alpha",p);
    p=Base();p.draws={Quad(WorldOverlayMode::Solid)};p.draws[0].colour={1,1,1,0.5f};
    auto background=Background(p);Fill(background,{1,1,1,0});r.Compare("straight-half-alpha-rgb-and-alpha",p,{},1,&background);
    auto second=p.draws[0];second.colour={0,0,0,0.5f};second.x0=12;second.y0=8;p.draws.push_back(second);
    r.Compare("straight-alpha-ordered-overlap",p,{},1,&background);
    p=Base();p.draws={Quad(WorldOverlayMode::Clut)};p.sources[0].indices.assign(64,1);
    p.colour_rows[0][4]=255;p.colour_rows[0][5]=0;p.colour_rows[0][6]=255;p.colour_rows[0][7]=128;
    background=Background(p);Fill(background,{0,255,0,128});r.Compare("clut-alpha128-straight-channel-factors",p,{},1,&background);
    p.draws[0].colour[3]=0.5f;Fill(background,{0,255,0,64});r.Compare("clut-row-alpha-times-vertex-alpha",p,{},1,&background);
    p=Base();p.draws={Quad(WorldOverlayMode::Solid)};p.draws[0].colour={1,0,0,1};
    p.draws[0].scissor_enabled=true;p.draws[0].scissor_x=11;p.draws[0].scissor_y=7;p.draws[0].scissor_w=7;p.draws[0].scissor_h=5;
    r.Compare("native-offset-scissor",p);
    p.draws[0].scissor_w=0;r.Compare("empty-scissor-keeps-background",p);
    p=Base();auto near=Quad(WorldOverlayMode::Solid);near.z_ndc=-0.5f;near.colour={1,0,0,1};
    auto equal=near;equal.z_ndc=0;equal.x0=12;equal.colour={0,1,0,1};p.draws={near,equal};
    r.Compare("lequal-equality-overlay-depth-readonly",p,Plane(0),0);
    p.draws={equal};p.draws[0].z_ndc=0.25f;r.Compare("behind-terrain-rejected",p,Plane(0),0);
    auto flat=p.draws[0];flat.phase=WorldOverlayPhase::FlatWorld;flat.depth_test=false;flat.x1=20;flat.colour={0,0,1,1};
    auto swipe=flat;swipe.phase=WorldOverlayPhase::Swipe;swipe.x0=16;swipe.x1=26;swipe.colour={1,0,1,1};
    p.draws={p.draws[0],flat,swipe};r.Compare("depth-flat-swipe-phase-order",p,Plane(0),0);
    auto depth=Plane(-0.5f,7);const auto far=Plane(0.25f,3);depth.insert(depth.end(),far.begin(),far.end());
    p.draws={equal};r.Compare("terrain-last-painter-depth-not-nearest",p,depth,0.25f);
    // Original atlas dimensions and placement, including proved uploaded rows
    // and untouched unknown rows. No UV rebasing to a compact sprite texture.
    p=Base();p.sources.clear();IndexedOverlaySource large;large.width=4096;large.height=2048;
    large.indices.assign(size_t(large.width)*large.height,0);large.initialized_rows.assign(large.height,0);
    for(unsigned y=1700;y<1708;++y){large.initialized_rows[y]=1;
        for(unsigned x=3077;x<3085;++x)large.indices[size_t(y)*large.width+x]=uint8_t(1+(x+y)%7);}
    p.sources.push_back(std::move(large));auto q=Quad();q.u0=(3077+1.0f/32)/4096;q.u1=(3085-1.0f/32)/4096;
    q.v0=(1700+1.0f/32)/2048;q.v1=(1708-1.0f/32)/2048;p.draws={q};
    r.Compare("original-atlas-origin-uploaded-row-proof",p);
    BoundaryFixture(r);
}
void Invalid(Runner& r)
{
    auto base=Base();base.draws={Quad()};const auto background=Background(base);
    auto reject=[&](const char* name,const WorldOverlayPass& p){r.Reject(name,p,background);};
    auto p=base;p.draws[0].mode=WorldOverlayMode(255);reject("invalid-mode",p);
    p=base;p.draws[0].phase=WorldOverlayPhase::Swipe;p.draws.push_back(Quad());reject("invalid-phase-order",p);
    p=base;p.draws[0].depth_test=false;reject("invalid-world-depth-state",p);
    p=base;p.sources[0].indices.pop_back();reject("invalid-source-byte-count",p);
    p=base;p.sources[0].initialized_rows.pop_back();reject("invalid-row-proof-size",p);
    p=base;p.sources[0].initialized_rows[3]=2;reject("invalid-nonbinary-row-proof",p);
    p=base;p.sources[0].initialized_rows[3]=0;reject("invalid-unknown-row-nonzero-bytes",p);
    p=base;p.sources[0].initialized_rows[3]=0;std::fill_n(p.sources[0].indices.begin()+24,8,0);reject("invalid-sampled-unknown-row",p);
    p=base;p.draws[0].colour_row=1;reject("invalid-colour-row-reference",p);
    p=base;p.draws[0].source=1;reject("invalid-source-reference",p);
    p=base;p.draws[0].u0=std::numeric_limits<float>::quiet_NaN();reject("invalid-uv-nan",p);
    p=base;p.draws[0].v1=1.01f;reject("invalid-uv-range",p);
    p=base;p.draws[0].scissor_x=1;reject("invalid-unused-scissor",p);
    p=base;p.draws[0].scissor_enabled=true;p.draws[0].scissor_w=-1;reject("invalid-scissor-extent",p);
    p=base;p.draws[0].x1=p.draws[0].x0;reject("invalid-empty-geometry",p);
    p=base;p.draws[0].z_ndc=std::numeric_limits<float>::infinity();reject("invalid-depth-infinity",p);
    p=base;p.draws[0].colour[3]=1.1f;reject("invalid-vertex-alpha",p);
    p=base;p.draws[0].mode=WorldOverlayMode::Fade;reject("invalid-missing-fade",p);
    p=base;p.fade.resize(16384);reject("invalid-unused-fade",p);
    auto bad=background;bad.rgba.pop_back();r.Reject("invalid-background-bytes",base,bad);
    auto terrain=Plane(0);terrain[0].vertices[1].w=0;r.Reject("invalid-terrain-w",base,background,terrain);
}
}
bool RunWorldOverlayOperatorTests(Device& device,const std::filesystem::path& output)
{
    std::filesystem::create_directories(output);Runner r(device,output);std::string error;
    try{Invalid(r);Fixtures(r);}catch(const std::exception& e){error=e.what();}
    const bool raw_analytic_exact=!r.gpu_analytic&&!r.gl_analytic;
    const bool strict_checks=r.checks&&r.gpu_gl==r.classified_gpu_gl&&!r.strict_analytic&&r.classified_boundary_pixels>=16
        &&r.gpu_analytic==r.classified_gpu_analytic&&r.gl_analytic==r.classified_gl_analytic;
    const bool passed=error.empty()&&strict_checks;
    std::ofstream report(output/"world-overlay-operators.json");
    report<<"{\"schema\":3,\"scope\":\"owned pre-lens UI quad operators with D24 painter terrain depth\",\"gate2Accepted\":false,\"passed\":"
        <<(passed?"true":"false")<<",\"executionPassed\":"<<(error.empty()?"true":"false")
        <<",\"strictChecksPassed\":"<<(strict_checks?"true":"false")<<",\"rawAnalyticExact\":"<<(raw_analytic_exact?"true":"false")
        <<",\"boundaryPolicy\":\"Only named flipped source-boundary fixture and coordinate probe; backend-specific exact coordinates independently reconstruct all pixels; shifted controls strict\""
        <<",\"classifiedBoundaryPixels\":"<<r.classified_boundary_pixels<<",\"classifiedGpuAnalyticBytes\":"<<r.classified_gpu_analytic
        <<",\"classifiedGlAnalyticBytes\":"<<r.classified_gl_analytic<<",\"classifiedGpuGlBytes\":"<<r.classified_gpu_gl<<",\"boundarySamples\":"<<r.boundary_samples
        <<",\"strictAnalyticDifferingBytes\":"<<r.strict_analytic<<",\"toleranceBytes\":0,\"gpuAdapter\":"<<Json(device.GetDiagnostics().adapter)
        <<",\"glDevice\":"<<Json(WorldOverlayGLReferenceDevice())<<",\"casesCompleted\":"<<r.cases<<",\"invalidInputCases\":"<<r.invalid
        <<",\"pairwisePixelComparisons\":"<<r.comparisons<<",\"pairwiseByteComparisons\":"<<r.comparisons*4
        <<",\"gpuGlDifferingBytes\":"<<r.gpu_gl<<",\"gpuAnalyticDifferingBytes\":"<<r.gpu_analytic
        <<",\"glAnalyticDifferingBytes\":"<<r.gl_analytic<<",\"error\":"<<Json(error)<<",\"cases\":[";
    for(size_t i=0;i<r.records.size();++i){if(i)report<<',';report<<r.records[i];}report<<"]}";report.close();
    Require(bool(report),"Cannot retain world overlay operator report");Require(error.empty(),error.c_str());
    Require(strict_checks,"World overlay strict control, classification, input, lifetime or validation checks failed; see world-overlay-operators.json");
    return raw_analytic_exact&&!r.gpu_gl;
}
}
