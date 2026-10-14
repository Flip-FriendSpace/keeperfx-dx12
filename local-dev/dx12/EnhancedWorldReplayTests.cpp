// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#include "EnhancedWorldReplayTests.h"
#include "EnhancedWorldGLOracle.h"
#include "OutlineGLOracle.h"
#include "EnhancedSpriteTestImages.h"
#include "kfx/renderer/SceneAssets.h"
#include <algorithm>
#include <map>
#include <set>

namespace kfx::render::dx12 {
namespace {
using namespace enhanced_test;
struct Upload {
    EnhancedWorldPass world;
    Image atlas;
    std::vector<OutlinePassDraw> commands;
    uint32_t outlines=0,normals=0,glow_add=0,glow_darken=0,groups=0;
};
Upload Pack(const SceneCapture& capture)
{
    const auto& trace=capture.trace.outline;const auto& source=capture.trace.gl_world;
    Upload result;auto& world=result.world;
    world.native_width=source.native_width;world.native_height=source.native_height;
    world.viewport_x=source.viewport_x;world.viewport_y=source.viewport_y;
    world.viewport_width=uint32_t(source.viewport_width);world.viewport_height=uint32_t(source.viewport_height);
    world.clear_rgba=source.clear_rgba;world.palette_rgba=source.palette_rgba;world.fade=source.fade;
    world.shade_settings=source.shade_settings;world.time=source.time;world.lightmap=source.lightmap;world.tiles=source.tiles;
    std::map<uint64_t,std::pair<uint32_t,uint32_t>> locations;
    uint32_t x=0,y=0,row_height=0;
    const auto allocate=[&](uint64_t id,uint32_t width,uint32_t height){
        if(locations.contains(id))return;
        Require(width&&height&&width<=256&&height<=256,"Invalid enhanced world source extent");
        if(x+width>1024){x=0;y+=row_height;row_height=0;}
        locations.emplace(id,std::pair{x,y});row_height=std::max(row_height,height);x+=width;
    };
    for(const auto& sprite:trace.sprites){
        const auto id=capture.trace.draws.at(sprite.draw).sprite;const auto& pixels=capture.assets.sprites.at(id);
        allocate(id,pixels.width,pixels.height);
    }
    constexpr uint64_t ShadowSource=uint64_t(1)<<32;
    for(const auto& shadow:source.shadows){const auto& mask=source.shadow_sources.at(shadow.source);
        allocate(ShadowSource+shadow.source,mask.width,mask.height);}
    const uint32_t height=std::max<uint32_t>(1,y+row_height);
    Require(height<=16384&&uint64_t(1024)*height*4<=64*1024*1024,"Enhanced world packed atlas exceeds bound");
    result.atlas={1024,height,std::vector<uint8_t>(size_t(1024)*height*4)};
    for(const auto& [id,position]:locations){
        if(id>=ShadowSource){
            const auto& mask=source.shadow_sources.at(size_t(id-ShadowSource));
            for(uint32_t sy=0;sy<mask.height;++sy)for(uint32_t sx=0;sx<mask.width;++sx)
                result.atlas.rgba[(size_t(sy+position.second)*1024+sx+position.first)*4+1]=mask.coverage.at(size_t(sy)*mask.width+sx);
        }else{
            const auto& pixels=capture.assets.sprites.at(size_t(id));
            for(uint32_t sy=0;sy<pixels.height;++sy)for(uint32_t sx=0;sx<pixels.width;++sx){
                const auto from=(size_t(sy)*pixels.width+sx)*2,to=(size_t(sy+position.second)*1024+sx+position.first)*4;
                result.atlas.rgba[to]=pixels.pixels.at(from);result.atlas.rgba[to+1]=pixels.pixels.at(from+1);
            }
        }
    }
    Require(source.terrain.size()==trace.occluders.size(),"Enhanced material/depth records differ");
    for(size_t i=0;i<source.terrain.size();++i){
        EnhancedTerrainTriangle triangle;triangle.tile=source.terrain[i].tile;
        for(size_t j=0;j<3;++j){const auto& p=trace.occluders[i].vertices[j];const auto& v=source.terrain[i].vertices[j];
            triangle.vertices[j]={{p.x,p.y,p.z,p.w},v.u,v.v,v.shade,v.stl_x,v.stl_y};}
        world.terrain.push_back(triangle);
    }
    for(const auto& submitted:source.shadows){
        EnhancedShadowQuad shadow;shadow.z_ndc=submitted.z_ndc;shadow.darken=submitted.darken;
        const auto& mask=source.shadow_sources.at(submitted.source);const auto origin=locations.at(ShadowSource+submitted.source);
        shadow.mask_x=origin.first;shadow.mask_y=origin.second;shadow.source_width=mask.width;shadow.source_height=mask.height;
        for(size_t i=0;i<4;++i){const auto& v=submitted.vertices[i];shadow.vertices[i]={v.x,v.y,v.u,v.v};}
        world.shadows.push_back(shadow);
    }
    std::set<uint32_t> groups;
    for(const auto& command:trace.commands){
        const auto& sprite=trace.sprites.at(command.sprite);const auto& submitted=capture.trace.draws.at(sprite.draw);
        const auto& pixels=capture.assets.sprites.at(submitted.sprite);const auto origin=locations.at(submitted.sprite);
        OutlinePassDraw draw;draw.x=sprite.dst_x;draw.y=sprite.dst_y;draw.width=sprite.dst_w;draw.height=sprite.dst_h;
        draw.phase_x=sprite.phase_x;draw.phase_y=sprite.phase_y;draw.step_x=sprite.step_x;draw.step_y=sprite.step_y;
        draw.mask_x=origin.first;draw.mask_y=origin.second;draw.source_width=pixels.width;draw.source_height=pixels.height;
        draw.content_height=submitted.content_height;draw.flip=(sprite.flags&1)!=0;
        draw.premultiplied=sprite.path==SceneOutlineExecutionPath::Instanced;
        draw.texture_fallback=sprite.path==SceneOutlineExecutionPath::TextureFallback;draw.z_ndc=sprite.z_ndc;
        switch(command.kind){
        case SceneOutlineCommandKind::Outline:
            draw.kind=trace.mode==2?OutlinePassKind::Edge:OutlinePassKind::Silhouette;
            draw.z_ndc=sprite.outline_z_ndc;draw.colour=sprite.owner_rgba;++result.outlines;break;
        case SceneOutlineCommandKind::Sprite:
            if(sprite.additive){draw.kind=OutlinePassKind::GlowAdd;++result.glow_add;}
            else{draw.kind=OutlinePassKind::NormalSprite;draw.source_alpha=sprite.source_alpha;
                draw.lookup_row=trace.colour_bindings.at(command.sprite).lookup_row;++result.normals;}
            break;
        case SceneOutlineCommandKind::GlowDarken:draw.kind=OutlinePassKind::GlowDarken;++result.glow_darken;break;
        default:throw std::runtime_error("Unknown enhanced world sprite phase");
        }
        result.commands.push_back(draw);groups.insert(command.group);
    }
    result.groups=uint32_t(groups.size());
    ValidateEnhancedWorldPass(world,result.atlas.width,result.atlas.height,result.atlas.rgba.size(),trace.colour_rows.size());
    return result;
}
}
EnhancedWorldReplayTestResult RunEnhancedWorldReplayTests(Device& device,
    const std::filesystem::path& scene_path,const std::filesystem::path& output,EnhancedWorldRasterOrigin raster_origin,
    EnhancedFadeAddressing fade_addressing)
{
    using namespace enhanced_test;
    const auto capture=LoadSceneCapture(scene_path);
    Require((capture->format_version==10||capture->format_version==11)&&capture->trace.backend==1&&ValidateSceneCapture(*capture)
        &&capture->trace.gl_world.present&&capture->trace.gl_world.complete,
        "Enhanced world replay requires complete validated GL v10 or v11 capture");
    const bool whole_composition=capture->format_version==11;
    const bool native_origin=whole_composition||raster_origin==EnhancedWorldRasterOrigin::Native;
    const auto& composition=capture->trace.gl_composition;
    auto packed=Pack(*capture);packed.world.fade_addressing=fade_addressing;
    const auto& trace=capture->trace.outline;const auto& world=packed.world;
    EnhancedWorldReplayTestResult result;std::string error;uint64_t alpha=0,different_pixels=0;unsigned maximum_delta=0;
    try{
        const auto before=device.GetDiagnostics();
        result.frame=whole_composition
            ?device.PrepareFrozenEnhancedCompositionFrame(world,packed.atlas,trace.colour_rows,trace.palette_transform,packed.commands,composition)
            :device.PrepareFrozenEnhancedWorldFrame(world,packed.atlas,trace.colour_rows,trace.palette_transform,packed.commands,raster_origin);
        const auto prepared=device.GetDiagnostics();
        Require(prepared.frozen_preparations==before.frozen_preparations+1
            &&prepared.frozen_image_readbacks==before.frozen_image_readbacks,"Enhanced preparation read an image back");
        result.preview=device.ReadbackFrozenRGBA(result.frame);
        const auto after=device.GetDiagnostics();
        Require(after.frozen_image_readbacks==prepared.frozen_image_readbacks+1,
            "Enhanced replay must perform exactly one explicit diagnostic image readback");
        Require(after.validation_errors==before.validation_errors&&after.validation_warnings==before.validation_warnings,
            "Enhanced world replay produced GPU validation messages");
        const auto gl=whole_composition
            ?RenderEnhancedCompositionGLReference(world,packed.atlas,trace.colour_rows,trace.palette_transform,packed.commands,composition)
            :RenderEnhancedWorldGLReference(world,packed.atlas,trace.colour_rows,trace.palette_transform,packed.commands);
        result.compared_pixels=uint64_t(result.preview.width)*result.preview.height;
        result.differing_bytes=Differences(result.preview,gl);
        Image mask{gl.width,gl.height,std::vector<uint8_t>(gl.rgba.size())};
        for(size_t p=0;p<gl.rgba.size();p+=4){bool different=false;
            for(size_t c=0;c<4;++c){const unsigned delta=unsigned(std::abs(int(result.preview.rgba[p+c])-int(gl.rgba[p+c])));
                maximum_delta=std::max(maximum_delta,delta);different|=delta!=0;if(c==3)alpha+=delta!=0;}
            different_pixels+=different;mask.rgba[p]=mask.rgba[p+2]=different?255:0;mask.rgba[p+3]=255;
        }
        const std::string stem=whole_composition?"enhanced-world-native-":"enhanced-world-viewport-";
        Save(output/(stem+"gpu"),result.preview);Save(output/(stem+"gl"),gl);
        Save(output/(stem+"difference"),mask);
    }catch(const std::exception& e){error=e.what();result.frame.Reset();}
    result.exact=error.empty()&&!result.differing_bytes;
    std::ofstream report(output/"enhanced-world-replay.json");
    report<<"{\"schema\":"<<(whole_composition?2:1)<<",\"executionPassed\":"<<(error.empty()?"true":"false")<<",\"passed\":"<<(result.exact?"true":"false")
        <<",\"gate2Accepted\":false,\"reviewRequired\":"<<(!result.exact?"true":"false")
        <<",\"scope\":"<<Json(whole_composition
            ?"Owned GL native world composition: actual screen/source clears, terrain colour and D24, shadows, ordered normal/glow/outline groups, executed depth/flat/swipe quads and effective lens; excludes subsequent ordinary UI, map fades and image presents"
            :"Owned GL pre-overlay world: actual clear, terrain colour and D24, multiplicative shadows, ordered normal/glow/outline groups; excludes subsequent world overlays, swipe, lenses and UI")
        <<",\"sourceLimitations\":\"Coverage outside full logical sources is owned zero; live atlas padding is not reproduced. Hardware raster coverage and RGBA precision differences are retained without tolerance\""
        <<",\"capture\":"<<Json(scene_path.string())<<",\"formatVersion\":"<<capture->format_version<<",\"nativeWidth\":"<<world.native_width
        <<",\"nativeHeight\":"<<world.native_height<<",\"viewport\":["<<world.viewport_x<<','<<world.viewport_y<<','<<world.viewport_width<<','<<world.viewport_height<<']'
        <<",\"rasterOrigin\":\""<<(native_origin?"native":"viewport-local")<<'"'
        <<",\"fadeAddressing\":\""<<(fade_addressing==EnhancedFadeAddressing::TexelLoad?"texel-load":"sampler")<<'"'
        <<",\"workingTargetWidth\":"<<(native_origin?world.native_width:world.viewport_width)
        <<",\"workingTargetHeight\":"<<(native_origin?world.native_height:world.viewport_height)
        <<",\"gpuViewportCrop\":"<<(native_origin&&!whole_composition?"true":"false")
        <<",\"targetKind\":"<<unsigned(capture->trace.gl_world.target_kind)<<",\"residentGpuFrame\":"<<(result.frame.Valid()?"true":"false")
        <<",\"preparationImageReadbacks\":0,\"diagnosticImageReadbacks\":"<<(result.preview.rgba.empty()?0:1)
        <<",\"terrainTriangles\":"<<world.terrain.size()<<",\"tileVersions\":"<<world.tiles.size()<<",\"shadowQuads\":"<<world.shadows.size()
        <<",\"normalDraws\":"<<packed.normals<<",\"outlineDraws\":"<<packed.outlines<<",\"glowAddDraws\":"<<packed.glow_add
        <<",\"glowDarkenDraws\":"<<packed.glow_darken<<",\"spritePhases\":"<<packed.commands.size()<<",\"groups\":"<<packed.groups
        <<",\"comparedPixels\":"<<result.compared_pixels<<",\"differentPixels\":"<<different_pixels<<",\"differingBytes\":"<<result.differing_bytes
        <<",\"alphaDifferingBytes\":"<<alpha<<",\"maximumByteDelta\":"<<maximum_delta<<",\"glDevice\":"<<Json(OutlineGLReferenceDevice())
        <<",\"error\":"<<Json(error);
    if(whole_composition){
        report<<",\"wholeComposition\":true,\"outputWidth\":"<<world.native_width<<",\"outputHeight\":"<<world.native_height
            <<",\"completedPhases\":"<<unsigned(composition.completed_phases)<<",\"overlayPhaseCounts\":["
            <<composition.phase_quad_counts[0]<<','<<composition.phase_quad_counts[1]<<','<<composition.phase_quad_counts[2]<<']'
            <<",\"overlayDraws\":"<<composition.overlays.draws.size()
            <<",\"lensRequestedActive\":"<<(composition.lens.requested_active?"true":"false")
            <<",\"lensCaptureBegan\":"<<(composition.lens.capture_began?"true":"false")
            <<",\"lensOperation\":"<<unsigned(composition.lens.operation)
            <<",\"lensRequestedType\":"<<unsigned(composition.lens.requested_type)
            <<",\"lensFallbackCopy\":"<<(composition.lens.fallback_copy?"true":"false");
    }
    report<<"}\n";
    Require(bool(report),"Cannot retain enhanced world replay report");
    if(!error.empty())throw std::runtime_error(error);
    return result;
}
}
