// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#include "EnhancedSpriteReplayTests.h"
#include "EnhancedSpriteTestImages.h"
#include "OutlineGLOracle.h"
#include "kfx/renderer/SceneAssets.h"
#include <algorithm>
#include <map>
#include <set>

namespace kfx::render::dx12 {
namespace {
using namespace enhanced_test;
struct Packed {
    Image atlas;
    std::vector<OutlineDepthTriangle> terrain;
    std::vector<OutlinePassDraw> commands;
    std::array<uint32_t,6> resolutions{};
    std::array<uint32_t,3> paths{};
    uint32_t outlines=0,normals=0,glow_add=0,glow_darken=0,border_sources=0,groups=0;
};
Packed Pack(const SceneCapture& capture)
{
    const auto& trace=capture.trace.outline;
    Packed result;
    std::map<uint32_t,std::pair<uint32_t,uint32_t>> locations;
    uint32_t x=0,y=0,row_height=0;
    for(const auto& sprite:trace.sprites){
        const auto source_id=capture.trace.draws.at(sprite.draw).sprite;
        if(locations.contains(source_id))continue;
        const auto& source=capture.assets.sprites.at(source_id);
        Require(source.width && source.height && source.width<=256 && source.height<=256,"Unsupported enhanced source extent");
        if(x+source.width>1024){x=0;y+=row_height;row_height=0;}
        locations.emplace(source_id,std::pair{x,y});
        row_height=std::max<uint32_t>(row_height,source.height);x+=source.width;
    }
    const uint32_t height=std::max<uint32_t>(1,y+row_height);
    Require(height<=16384 && uint64_t(1024)*height*4+uint64_t(trace.colour_rows.size())*1024<=64*1024*1024,
        "Enhanced source and colour rows exceed 64 MiB");
    result.atlas={1024,height,std::vector<uint8_t>(size_t(1024)*height*4)};
    for(const auto& [id,position]:locations){
        const auto& source=capture.assets.sprites.at(id);
        for(uint32_t sy=0;sy<source.height;++sy)for(uint32_t sx=0;sx<source.width;++sx){
            const auto from=(size_t(sy)*source.width+sx)*2;
            const auto to=(size_t(sy+position.second)*1024+sx+position.first)*4;
            result.atlas.rgba[to]=source.pixels.at(from);result.atlas.rgba[to+1]=source.pixels.at(from+1);
        }
    }
    for(const auto& triangle:trace.occluders){
        OutlineDepthTriangle converted;
        for(size_t i=0;i<3;++i){const auto& v=triangle.vertices[i];converted.vertices[i]={v.x,v.y,v.z,v.w};}
        result.terrain.push_back(converted);
    }
    for(size_t i=0;i<trace.sprites.size();++i){
        const auto& sprite=trace.sprites[i];
        ++result.paths.at(size_t(sprite.path));
        ++result.resolutions.at(size_t(trace.colour_bindings.at(i).resolution));
        result.border_sources+=sprite.source_border_covered;
    }
    std::set<uint32_t> groups;
    for(const auto& command:trace.commands){
        const auto& sprite=trace.sprites.at(command.sprite);
        const auto& submitted=capture.trace.draws.at(sprite.draw);
        const auto& source=capture.assets.sprites.at(submitted.sprite);
        const auto origin=locations.at(submitted.sprite);
        OutlinePassDraw draw;
        draw.x=sprite.dst_x;draw.y=sprite.dst_y;draw.width=sprite.dst_w;draw.height=sprite.dst_h;
        draw.phase_x=sprite.phase_x;draw.phase_y=sprite.phase_y;draw.step_x=sprite.step_x;draw.step_y=sprite.step_y;
        draw.mask_x=origin.first;draw.mask_y=origin.second;draw.source_width=source.width;draw.source_height=source.height;
        draw.content_height=submitted.content_height;draw.flip=(sprite.flags&1)!=0;
        draw.premultiplied=sprite.path==SceneOutlineExecutionPath::Instanced;
        draw.texture_fallback=sprite.path==SceneOutlineExecutionPath::TextureFallback;
        draw.z_ndc=sprite.z_ndc;
        switch(command.kind){
        case SceneOutlineCommandKind::Outline:
            draw.kind=trace.mode==2?OutlinePassKind::Edge:OutlinePassKind::Silhouette;
            draw.z_ndc=sprite.outline_z_ndc;draw.colour=sprite.owner_rgba;++result.outlines;break;
        case SceneOutlineCommandKind::Sprite:
            if(sprite.additive){draw.kind=OutlinePassKind::GlowAdd;++result.glow_add;}
            else{
                draw.kind=OutlinePassKind::NormalSprite;draw.source_alpha=sprite.source_alpha;
                // The row already includes actual remap/cache fallback. Never
                // apply submitted.remap again or derive it from the palette.
                draw.lookup_row=trace.colour_bindings.at(command.sprite).lookup_row;++result.normals;
            }
            break;
        case SceneOutlineCommandKind::GlowDarken:draw.kind=OutlinePassKind::GlowDarken;++result.glow_darken;break;
        default:throw std::runtime_error("Unknown enhanced sprite command");
        }
        result.commands.push_back(draw);groups.insert(command.group);
    }
    result.groups=uint32_t(groups.size());
    Require(result.commands.size()==trace.commands.size(),"Enhanced sprite phases were omitted");
    return result;
}
}
EnhancedSpriteReplayTestResult RunEnhancedSpriteReplayTests(Device& device,
    const std::filesystem::path& scene_path,const std::filesystem::path& output)
{
    using namespace enhanced_test;
    const auto capture=LoadSceneCapture(scene_path);
    Require(capture->format_version==9 && capture->trace.backend==1 && ValidateSceneCapture(*capture)
        && capture->trace.outline.present && capture->trace.outline.complete,
        "Enhanced sprite replay requires validated v9 actual colour bindings");
    const auto packed=Pack(*capture);const auto& trace=capture->trace.outline;
    const auto width=uint32_t(capture->scene.camera.viewport_w),height=uint32_t(capture->scene.camera.viewport_h);
    Require(width && height && width<=4096 && height<=4096,"Enhanced replay viewport exceeds bound");
    EnhancedSpriteReplayTestResult result;
    std::vector<std::string> records;std::string error;
    try{
        for(bool patterned:{false,true}){
            const std::string name=patterned?"patterned":"flat";
            Image background{width,height,std::vector<uint8_t>(size_t(width)*height*4)};
            for(uint32_t y=0;y<height;++y)for(uint32_t x=0;x<width;++x){
                const auto p=(size_t(y)*width+x)*4;
                background.rgba[p]=patterned?uint8_t(x*13+y*7):31;
                background.rgba[p+1]=patterned?uint8_t(x*3+y*17):47;
                background.rgba[p+2]=patterned?uint8_t(x*19+y*11):61;
                background.rgba[p+3]=patterned?uint8_t(x+y):255;
            }
            const auto before=device.GetDiagnostics();
            auto gpu=device.CompositeEnhancedSprites(background,packed.atlas,trace.colour_rows,trace.palette_transform,packed.terrain,packed.commands);
            const auto after=device.GetDiagnostics();
            Require(before.allocation_count==after.allocation_count && before.allocation_bytes==after.allocation_bytes
                && before.validation_errors==after.validation_errors && before.validation_warnings==after.validation_warnings,
                "Enhanced replay retained resources or produced validation messages");
            const auto gl=RenderEnhancedSpritesGLReference(background,packed.atlas,trace.colour_rows,trace.palette_transform,packed.terrain,packed.commands);
            const auto differences=Differences(gpu,gl);result.differing_bytes+=differences;result.compared_pixels+=uint64_t(width)*height;
            uint64_t rgb_differences=0,alpha_differences=0;unsigned max_delta=0;
            Image mask{width,height,std::vector<uint8_t>(gpu.rgba.size())};
            for(size_t p=0;p<gpu.rgba.size();p+=4){
                bool different=false;
                for(size_t c=0;c<4;++c){
                    const unsigned delta=unsigned(std::abs(int(gpu.rgba[p+c])-int(gl.rgba[p+c])));
                    max_delta=std::max(max_delta,delta);different|=delta!=0;
                    if(c==3)alpha_differences+=delta!=0;else rgb_differences+=delta!=0;
                }
                mask.rgba[p]=mask.rgba[p+2]=different?255:0;mask.rgba[p+3]=255;
            }
            Save(output/("enhanced-replay-"+name+"-gpu"),gpu);Save(output/("enhanced-replay-"+name+"-gl"),gl);
            Save(output/("enhanced-replay-"+name+"-difference"),mask);
            records.push_back("{\"background\":"+Json(name)+",\"comparedPixels\":"+std::to_string(uint64_t(width)*height)
                +",\"differingRgbBytes\":"+std::to_string(rgb_differences)+",\"differingAlphaBytes\":"+std::to_string(alpha_differences)
                +",\"maximumByteDelta\":"+std::to_string(max_delta)+"}");
            if(!patterned)result.preview=std::move(gpu);
        }
    }catch(const std::exception& failure){error=failure.what();}
    result.exact=error.empty()&&!result.differing_bytes;
    std::ofstream report(output/"enhanced-sprite-replay.json");
    report<<"{\"schema\":1,\"executionPassed\":"<<(error.empty()?"true":"false")<<",\"passed\":"<<(result.exact?"true":"false")
        <<",\"gate2Accepted\":false,\"visualAccepted\":false,\"reviewRequired\":"<<(!result.exact?"true":"false")
        <<",\"scope\":\"Actual v9 normal art, effective lookup bytes, all glow phases and ordered owner outlines over declared RGBA backgrounds; actual terrain depth, no terrain colour or creature shadows\""
        <<",\"sourceLimitations\":\"Owned source coverage is zero outside logical dimensions; live uninitialized atlas padding is not reproduced\""
        <<",\"capture\":"<<Json(scene_path.string())<<",\"formatVersion\":9,\"width\":"<<width<<",\"height\":"<<height
        <<",\"occluders\":"<<packed.terrain.size()<<",\"sprites\":"<<trace.sprites.size()<<",\"commands\":"<<packed.commands.size()
        <<",\"normalDraws\":"<<packed.normals<<",\"outlineDraws\":"<<packed.outlines<<",\"glowAddDraws\":"<<packed.glow_add
        <<",\"glowDarkenDraws\":"<<packed.glow_darken<<",\"omittedGlowCommands\":0,\"flushGroups\":"<<packed.groups
        <<",\"sourceBorderCovered\":"<<packed.border_sources<<",\"colourRows\":"<<trace.colour_rows.size()
        <<",\"resolutionCounts\":[";
    for(size_t i=0;i<packed.resolutions.size();++i){if(i)report<<',';report<<packed.resolutions[i];}
    report<<"],\"pathCounts\":[";for(size_t i=0;i<packed.paths.size();++i){if(i)report<<',';report<<packed.paths[i];}
    report<<"],\"comparedPixels\":"<<result.compared_pixels<<",\"differingBytes\":"<<result.differing_bytes
        <<",\"glDevice\":"<<Json(OutlineGLReferenceDevice())<<",\"error\":"<<Json(error)<<",\"cases\":[";
    for(size_t i=0;i<records.size();++i){if(i)report<<',';report<<records[i];}report<<"]}\n";
    Require(bool(report),"Cannot retain enhanced sprite replay report");
    if(!error.empty())throw std::runtime_error(error);
    return result;
}
}
