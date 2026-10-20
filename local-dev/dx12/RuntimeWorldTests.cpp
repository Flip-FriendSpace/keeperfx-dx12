// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
// KeeperFX renderer performance changes: 2026-10-19.
#include "RuntimeWorldTests.h"
#include "kfx/renderer/d3d12/OwnedWorldBridge.h"
#include "EnhancedWorldGLOracle.h"
#include "EnhancedSpriteTestImages.h"
#include "kfx/renderer/SceneAssets.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <sstream>
#include <thread>

namespace kfx::render::dx12 {
namespace {
using namespace enhanced_test;
std::string Quote(const std::string& text){std::string result="\"";for(unsigned char c:text){if(c=='\\'||c=='"')result+='\\';if(c<32){result+="\\u00";result+="0123456789abcdef"[c>>4];result+="0123456789abcdef"[c&15];}else result+=char(c);}return result+'"';}
using Clock=std::chrono::steady_clock;
using Bytes=std::shared_ptr<const std::vector<uint8_t>>;
constexpr std::array<float,9> Identity{1,0,0,0,1,0,0,0,1};
struct Source { uint32_t width=0,height=0;std::vector<uint8_t> rgba; };
struct Fixture {
    EnhancedWorldPass world;
    std::array<float,9> matrix=Identity;
    std::vector<Source> sources;
    std::vector<uint32_t> shadow_sources,sprite_sources,sprite_rows;
    std::vector<OutlinePassDraw> draws;
    std::vector<SpriteColourRow> rows;
};
void Pump(){MSG m{};while(PeekMessageW(&m,nullptr,0,0,PM_REMOVE)){Require(m.message!=WM_QUIT,"Runtime world window closed");TranslateMessage(&m);DispatchMessageW(&m);}std::this_thread::sleep_for(std::chrono::milliseconds(1));}
Bytes Owned(const auto& container){return std::make_shared<const std::vector<uint8_t>>(container.begin(),container.end());}
std::vector<uint8_t> LightBytes(const Fixture& f){std::vector<uint8_t> b;b.reserve(f.world.lightmap.size()*2);for(uint16_t n:f.world.lightmap){b.push_back(uint8_t(n));b.push_back(uint8_t(n>>8));}return b;}
RuntimeAssetKey Key(RuntimeAssetKind kind,uint64_t id,uint64_t epoch=1,uint64_t revision=1){return {epoch,id,1,revision,kind};}
RuntimeAssetLease Publish(Device& d,RuntimeAssetKind kind,uint64_t id,uint32_t w,uint32_t h,const auto& bytes,uint64_t epoch=1,uint64_t revision=1){return d.PublishRuntimeAsset(Key(kind,id,epoch,revision),{w,h},Owned(bytes));}
RuntimeWorldFrame Packet(Device& d,const Fixture& f,uint64_t epoch=1,uint64_t revision=1){
    const auto& w=f.world;RuntimeWorldFrame p;
    p.native_width=w.native_width;p.native_height=w.native_height;p.viewport_x=w.viewport_x;p.viewport_y=w.viewport_y;p.viewport_width=w.viewport_width;p.viewport_height=w.viewport_height;
    p.clear_rgba=w.clear_rgba;p.shade_settings=w.shade_settings;p.palette_transform=f.matrix;p.time=w.time;p.fade_addressing=w.fade_addressing;
    p.palette=Publish(d,RuntimeAssetKind::PaletteRGBA8,1,256,1,w.palette_rgba,epoch,revision);
    p.fade=Publish(d,RuntimeAssetKind::FadeR8,2,256,64,w.fade,epoch,revision);
    p.lightmap=Publish(d,RuntimeAssetKind::LightmapR16,3,511,511,LightBytes(f),epoch,revision);
    std::vector<RuntimeAssetLease> tiles,sources,rows;
    for(size_t i=0;i<w.tiles.size();++i)tiles.push_back(Publish(d,RuntimeAssetKind::TileR8,100+i,32,32,w.tiles[i],epoch,revision));
    for(size_t i=0;i<f.sources.size();++i){const auto& s=f.sources[i];sources.push_back(Publish(d,RuntimeAssetKind::SpriteRGBA8,100000+i,s.width,s.height,s.rgba,epoch,revision));}
    for(size_t i=0;i<f.rows.size();++i)rows.push_back(Publish(d,RuntimeAssetKind::ColourRowRGBA8,200000+i,256,1,f.rows[i],epoch,revision));
    for(const auto& t:w.terrain)p.terrain.push_back({t.vertices,t.tile==MissingEnhancedTile?RuntimeAssetLease{}:tiles.at(t.tile)});
    for(size_t i=0;i<w.shadows.size();++i)p.shadows.push_back({w.shadows[i],sources.at(f.shadow_sources.at(i))});
    for(size_t i=0;i<f.draws.size();++i){auto draw=f.draws[i];const bool normal=draw.kind==OutlinePassKind::NormalSprite;draw.lookup_row=normal?0:NoSpriteColourRow;p.sprites.push_back({draw,sources.at(f.sprite_sources.at(i)),normal?rows.at(f.sprite_rows.at(i)):RuntimeAssetLease{}});}
    return p;
}
void SourceRevision(Device& d,RuntimeWorldFrame& p,const Fixture& f,uint32_t source,uint64_t revision,uint64_t generation=1){
    const auto& s=f.sources.at(source);auto lease=d.PublishRuntimeAsset({p.palette.Key().epoch,100000+source,generation,revision,RuntimeAssetKind::SpriteRGBA8},{s.width,s.height},Owned(s.rgba));
    for(size_t i=0;i<p.sprites.size();++i)if(f.sprite_sources[i]==source)p.sprites[i].source=lease;
    for(size_t i=0;i<p.shadows.size();++i)if(f.shadow_sources[i]==source)p.shadows[i].source=lease;
}
Fixture Synthetic(){
    Fixture f;auto& p=f.world;p.native_width=80;p.native_height=64;p.viewport_x=7;p.viewport_y=9;p.viewport_width=64;p.viewport_height=48;
    p.clear_rgba={12.0f/255,24.0f/255,36.0f/255,1};p.shade_settings={0,0,1,1,0,1,1,0,0};p.lightmap.assign(511*511,8192);p.tiles.resize(1);p.tiles[0].fill(10);
    for(unsigned i=0;i<256;++i){p.palette_rgba[i*4]=uint8_t(i);p.palette_rgba[i*4+1]=uint8_t(i*3);p.palette_rgba[i*4+2]=uint8_t(i*7);p.palette_rgba[i*4+3]=255;for(unsigned row=0;row<64;++row)p.fade[row*256+i]=uint8_t(i);}
    for(auto [index,colour]:std::array<std::pair<unsigned,std::array<uint8_t,3>>,2>{{{10,{40,80,120}},{11,{60,100,140}}}})for(unsigned c=0;c<3;++c)p.palette_rgba[index*4+c]=colour[c];
    const EnhancedTerrainVertex a{{-1,1,0,1},0,0,1,-1,-1},b{{1,1,0,1},32,0,1,-1,-1},c{{1,-1,0,1},32,32,1,-1,-1},e{{-1,-1,0,1},0,32,1,-1,-1};p.terrain={{{a,b,c},0},{{a,c,e},0}};
    f.sources={{4,4,std::vector<uint8_t>(64)},{2,2,std::vector<uint8_t>(16)},{4,4,std::vector<uint8_t>(64)}};
    for(size_t s=0;s<f.sources.size();++s)for(size_t i=0;i<f.sources[s].rgba.size();i+=4){f.sources[s].rgba[i]=s==2?21:20;f.sources[s].rgba[i+1]=255;}
    f.sources[0].rgba[0]=0;f.sources[0].rgba[5]=0;
    SpriteColourRow row{};for(unsigned i=0;i<256;++i){row[i*4]=uint8_t(i*5);row[i*4+1]=uint8_t(i*7);row[i*4+2]=uint8_t(i*11);row[i*4+3]=255;}f.rows.push_back(row);
    EnhancedShadowQuad shadow;shadow.vertices={{{28,16,0,0},{36,16,2.0f/256,0},{36,24,2.0f/256,2.0f/256},{28,24,0,2.0f/256}}};shadow.z_ndc=-0.25f;shadow.darken=0.5f;shadow.source_width=shadow.source_height=2;p.shadows.push_back(shadow);f.shadow_sources={1};
    OutlinePassDraw draw;draw.kind=OutlinePassKind::NormalSprite;draw.x=4;draw.y=4;draw.width=draw.height=4;draw.source_width=draw.source_height=draw.content_height=4;draw.z_ndc=-0.5f;draw.lookup_row=0;f.draws.push_back(draw);draw.x=6;f.draws.push_back(draw);f.sprite_sources={0,2};f.sprite_rows={0,0};return f;
}
Fixture Captured(const std::filesystem::path& path){
    const auto c=LoadSceneCapture(path);Require((c->format_version==10||c->format_version==11)&&c->trace.backend==1&&ValidateSceneCapture(*c)&&c->trace.gl_world.present&&c->trace.gl_world.complete,"Runtime world fixture requires owned complete GL v10/v11 world");
    Fixture f;const auto& w=c->trace.gl_world;const auto& outline=c->trace.outline;auto& p=f.world;
    p.native_width=w.native_width;p.native_height=w.native_height;p.viewport_x=w.viewport_x;p.viewport_y=w.viewport_y;p.viewport_width=uint32_t(w.viewport_width);p.viewport_height=uint32_t(w.viewport_height);p.clear_rgba=w.clear_rgba;p.palette_rgba=w.palette_rgba;p.fade=w.fade;p.shade_settings=w.shade_settings;p.time=w.time;p.lightmap=w.lightmap;p.tiles=w.tiles;f.matrix=outline.palette_transform;f.rows=outline.colour_rows;
    std::map<uint32_t,uint32_t> sprites,masks;
    const auto sprite_source=[&](uint32_t index){auto [it,inserted]=sprites.emplace(index,uint32_t(f.sources.size()));if(inserted){const auto& s=c->assets.sprites.at(index);Source out{s.width,s.height,std::vector<uint8_t>(size_t(s.width)*s.height*4)};for(size_t i=0;i<size_t(s.width)*s.height;++i){out.rgba[i*4]=s.pixels.at(i*2);out.rgba[i*4+1]=s.pixels.at(i*2+1);}f.sources.push_back(std::move(out));}return it->second;};
    for(size_t i=0;i<w.terrain.size();++i){EnhancedTerrainTriangle t;t.tile=w.terrain[i].tile;for(size_t j=0;j<3;++j){const auto& a=outline.occluders.at(i).vertices[j];const auto& b=w.terrain[i].vertices[j];t.vertices[j]={{a.x,a.y,a.z,a.w},b.u,b.v,b.shade,b.stl_x,b.stl_y};}p.terrain.push_back(t);}
    for(const auto& s:w.shadows){auto [it,inserted]=masks.emplace(s.source,uint32_t(f.sources.size()));const auto& mask=w.shadow_sources.at(s.source);if(inserted){Source out{mask.width,mask.height,std::vector<uint8_t>(size_t(mask.width)*mask.height*4)};for(size_t i=0;i<mask.coverage.size();++i)out.rgba[i*4+1]=mask.coverage[i];f.sources.push_back(std::move(out));}EnhancedShadowQuad q;q.z_ndc=s.z_ndc;q.darken=s.darken;q.source_width=mask.width;q.source_height=mask.height;for(size_t i=0;i<4;++i){const auto& v=s.vertices[i];q.vertices[i]={v.x,v.y,v.u,v.v};}p.shadows.push_back(q);f.shadow_sources.push_back(it->second);}
    for(const auto& command:outline.commands){const auto& s=outline.sprites.at(command.sprite);const auto& owned=c->trace.draws.at(s.draw);const auto source=sprite_source(owned.sprite);const auto& pixels=f.sources.at(source);OutlinePassDraw d;d.x=s.dst_x;d.y=s.dst_y;d.width=s.dst_w;d.height=s.dst_h;d.phase_x=s.phase_x;d.phase_y=s.phase_y;d.step_x=s.step_x;d.step_y=s.step_y;d.source_width=pixels.width;d.source_height=pixels.height;d.content_height=owned.content_height;d.flip=(s.flags&1)!=0;d.premultiplied=s.path==SceneOutlineExecutionPath::Instanced;d.texture_fallback=s.path==SceneOutlineExecutionPath::TextureFallback;d.z_ndc=s.z_ndc;uint32_t row=0;
        switch(command.kind){case SceneOutlineCommandKind::Outline:d.kind=outline.mode==2?OutlinePassKind::Edge:OutlinePassKind::Silhouette;d.z_ndc=s.outline_z_ndc;d.colour=s.owner_rgba;break;case SceneOutlineCommandKind::Sprite:if(s.additive)d.kind=OutlinePassKind::GlowAdd;else{d.kind=OutlinePassKind::NormalSprite;d.source_alpha=s.source_alpha;row=outline.colour_bindings.at(command.sprite).lookup_row;d.lookup_row=row;}break;case SceneOutlineCommandKind::GlowDarken:d.kind=OutlinePassKind::GlowDarken;break;default:throw std::invalid_argument("Unknown owned sprite phase");}
        f.draws.push_back(d);f.sprite_sources.push_back(source);f.sprite_rows.push_back(row);
    }return f;
}
struct ReferenceInputs { EnhancedWorldPass world;Image atlas;std::vector<OutlinePassDraw> draws; };
ReferenceInputs ReferencePack(const Fixture& f){
    ReferenceInputs r{f.world,{},f.draws};std::vector<std::array<uint32_t,2>> origins;uint32_t x=0,y=0,h=0;
    for(const auto& s:f.sources){if(x+s.width>1024){x=0;y+=h;h=0;}origins.push_back({x,y});x+=s.width;h=std::max(h,s.height);}
    r.atlas={1024,std::max(1u,y+h),std::vector<uint8_t>(size_t(1024)*std::max(1u,y+h)*4)};
    Require(r.atlas.rgba.size()<=64*1024*1024,"Reference atlas exceeds diagnostic cap");
    for(size_t i=0;i<f.sources.size();++i){const auto& s=f.sources[i];for(uint32_t row=0;row<s.height;++row)std::copy_n(s.rgba.data()+size_t(row)*s.width*4,size_t(s.width)*4,r.atlas.rgba.data()+(size_t(origins[i][1]+row)*1024+origins[i][0])*4);}
    for(size_t i=0;i<r.world.shadows.size();++i){r.world.shadows[i].mask_x=origins.at(f.shadow_sources[i])[0];r.world.shadows[i].mask_y=origins.at(f.shadow_sources[i])[1];}
    for(size_t i=0;i<r.draws.size();++i){r.draws[i].mask_x=origins.at(f.sprite_sources[i])[0];r.draws[i].mask_y=origins.at(f.sprite_sources[i])[1];}
    return r;
}
Image Native(const Fixture& f,const Image& crop){
    Image image{f.world.native_width,f.world.native_height,std::vector<uint8_t>(size_t(f.world.native_width)*f.world.native_height*4)};
    for(size_t i=0;i<image.rgba.size();++i)image.rgba[i]=uint8_t(std::lround(f.world.clear_rgba[i%4]*255));
    Require(crop.width==f.world.viewport_width&&crop.height==f.world.viewport_height,"Reference viewport extent differs");
    for(uint32_t y=0;y<crop.height;++y)std::copy_n(crop.rgba.data()+size_t(y)*crop.width*4,size_t(crop.width)*4,image.rgba.data()+(size_t(y+f.world.viewport_y)*image.width+f.world.viewport_x)*4);
    return image;
}
Image Analytic(const Fixture& f){
    Image crop{64,48,std::vector<uint8_t>(64*48*4)};for(size_t i=0;i<crop.rgba.size();i+=4)std::copy_n(f.world.palette_rgba.data()+f.world.tiles[0][0]*4,4,crop.rgba.data()+i);
    for(uint32_t y=16;y<24;++y)for(uint32_t x=28;x<36;++x)for(size_t c=0;c<3;++c)crop.rgba[(size_t(y)*64+x)*4+c]/=2;
    for(size_t i=0;i<f.draws.size();++i){const auto& d=f.draws[i];const auto& s=f.sources[f.sprite_sources[i]];const auto& row=f.rows[f.sprite_rows[i]];for(int y=0;y<4;++y)for(int x=0;x<4;++x){const auto at=(size_t(y)*s.width+size_t(x))*4;if(s.rgba[at+1])std::copy_n(row.data()+s.rgba[at]*4,4,crop.rgba.data()+(size_t(d.y+y)*64+d.x+x)*4);}}
    return Native(f,crop);
}
void Stats(std::ostream& o,const RuntimeResourceDiagnostics& s)
{
    o << "{\"epoch\":"<<s.epoch<<",\"publications\":"<<s.publications<<",\"publicationReuses\":"<<s.publication_reuses
      <<",\"submissions\":"<<s.submissions<<",\"wouldBlock\":"<<s.would_block<<",\"suspended\":"<<s.suspended
      <<",\"presentations\":"<<s.presentations<<",\"heldSubmissions\":"<<s.held_submissions
      <<",\"readerHoldArms\":"<<s.reader_hold_arms<<",\"readerHoldReleases\":"<<s.reader_hold_releases
      <<",\"readerHoldSlots\":"<<s.reader_hold_slots<<",\"readerHoldActive\":"<<(s.reader_hold_active?"true":"false")
      <<",\"readerHoldRetained\":"<<(s.reader_hold_retained?"true":"false")
      <<",\"evictions\":"<<s.evictions<<",\"sourceUploadBytes\":"<<s.source_upload_bytes
      <<",\"paletteUploadBytes\":"<<s.palette_upload_bytes<<",\"alignedUploadBytes\":"<<s.aligned_upload_bytes
      <<",\"resourceAllocations\":"<<s.resource_allocations<<",\"descriptorHeapAllocations\":"<<s.descriptor_heap_allocations
      <<",\"cpuBytes\":"<<s.cpu_bytes<<",\"defaultBytes\":"<<s.default_bytes<<",\"uploadBytes\":"<<s.upload_bytes
      <<",\"readbackBytes\":"<<s.readback_bytes<<",\"retiredDefaultBytes\":"<<s.retired_default_bytes
      <<",\"pendingUploadBytes\":"<<s.pending_upload_bytes<<",\"cpuHighWater\":"<<s.cpu_high_water
      <<",\"defaultHighWater\":"<<s.default_high_water<<",\"uploadHighWater\":"<<s.upload_high_water
      <<",\"readbackHighWater\":"<<s.readback_high_water<<",\"retiredHighWater\":"<<s.retired_high_water
      <<",\"pendingHighWater\":"<<s.pending_high_water<<",\"assetCount\":"<<s.asset_count
      <<",\"descriptors\":"<<s.descriptors<<",\"descriptorHighWater\":"<<s.descriptor_high_water
      <<",\"diagnosticTickets\":"<<s.diagnostic_tickets<<",\"slotPressure\":"<<s.slot_pressure
      <<",\"fenceWaits\":"<<s.fence_waits<<",\"globalIdleCalls\":"<<s.global_idle_calls
      <<",\"imageReadbacks\":"<<s.image_readbacks<<",\"commandBytes\":"<<s.command_bytes
      <<",\"worldSubmissions\":"<<s.world_submissions<<",\"worldUploadBytes\":"<<s.world_upload_bytes<<",\"geometryUploadBytes\":"<<s.geometry_upload_bytes<<",\"wholeImages\":"<<s.whole_images<<",\"wholeImageIdle\":"<<s.whole_image_idle<<",\"wholeImageReuses\":"<<s.whole_image_reuses<<",\"worldPages\":"<<s.world_pages<<",\"worldShaderClears\":"<<s.world_shader_clears
      <<",\"stagingPools\":"<<s.staging_pools<<",\"stagingCapacityBytes\":"<<s.staging_capacity_bytes
      <<",\"stagingAllocationBytes\":"<<s.staging_allocation_bytes<<",\"stagingAllocations\":"<<s.staging_allocations<<",\"stagingMaps\":"<<s.staging_maps
      <<",\"terrainComparedBytes\":"<<s.terrain_compared_bytes<<",\"terrainUploadBytes\":"<<s.terrain_upload_bytes
      <<",\"terrainReusedBytes\":"<<s.terrain_reused_bytes<<",\"terrainDirtyRanges\":"<<s.terrain_dirty_ranges
      <<",\"terrainCpuBytes\":"<<s.terrain_cpu_bytes<<",\"terrainCpuHighWater\":"<<s.terrain_cpu_high_water
      <<",\"terrainDefaultBytes\":"<<s.terrain_default_bytes<<",\"terrainCapacityBytes\":"<<s.terrain_capacity_bytes
      <<",\"failed\":"<<(s.failed?"true":"false")<<",\"quarantined\":"<<(s.quarantined?"true":"false")<<'}';
}

struct Runner {
    Device& device;std::filesystem::path output;RuntimeWorldTestResult result{};
    RuntimeResourceBudgets budgets{};std::vector<std::string> records,comparisons,references,messages,isolated_devices,terrain_measurements;
    RuntimeResourceDiagnostics loop_before{},loop_after{};bool loop_done=false,controlled=false;
    std::atomic<bool> watchdog_fired{false};uint64_t reference_preparations=0,reference_readbacks=0;
    std::string active;Diagnostics initial{};
    Runner(Device& d,const std::filesystem::path& p):device(d),output(p),initial(d.GetDiagnostics()){}
    void Check(bool value,const char* reason){++result.checks;Require(value,reason);}
    template<class Exception=std::invalid_argument,class Fn>void Reject(Fn&& fn,const char* reason){bool rejected=false;try{fn();}catch(const Exception&){rejected=true;}Check(rejected,reason);}
    void Validation(const std::string& point){const auto fresh=device.TakeValidationMessages();messages.insert(messages.end(),fresh.begin(),fresh.end());const auto s=device.GetDiagnostics();if(s.validation_errors!=initial.validation_errors||s.validation_warnings!=initial.validation_warnings){std::string error="Runtime world validation first observed at "+active+"/"+point;for(const auto& m:messages)error+='\n'+m;throw std::runtime_error(error);}}
    void Bound(){const auto s=device.GetRuntimeResourceDiagnostics();Check(s.terrain_cpu_bytes<=budgets.geometry_cpu_bytes&&s.cpu_bytes<=budgets.cpu_bytes&&s.default_bytes<=budgets.default_bytes&&s.upload_bytes<=budgets.upload_bytes&&s.readback_bytes<=budgets.readback_bytes&&s.descriptors<=budgets.descriptors&&s.asset_count<=budgets.assets&&s.diagnostic_tickets<=budgets.diagnostic_tickets,"Runtime world actual owned allocation budget exceeded");}
    void Configure(const RuntimeResourceBudgets& b={}){device.ClearRuntimeResources();budgets=b;device.ConfigureRuntimeResources(b);}
    void Case(const std::string& name,const std::function<void()>& body){active=name;const auto before=device.GetRuntimeResourceDiagnostics();Validation("before");try{body();Validation("after");++result.cases;std::ostringstream o;o<<"{\"name\":"<<Quote(name)<<",\"passed\":true,\"before\":";Stats(o,before);o<<",\"after\":";Stats(o,device.GetRuntimeResourceDiagnostics());o<<'}';records.push_back(o.str());}catch(const std::exception& e){records.push_back("{\"name\":"+Quote(name)+",\"passed\":false,\"error\":"+Quote(e.what())+"}");throw;}}
    RuntimeFrameTicket Submit(const RuntimeWorldFrame& p,bool diagnostic){const auto deadline=Clock::now()+std::chrono::seconds(25);const auto key=p.palette.Key();for(;;){const auto s=device.TrySubmitRuntimeWorldFrame(p,diagnostic);Bound();Check(p.palette.Valid()&&p.palette.Key()==key,"World submit consumed producer packet");if(s.status==RuntimeSubmitStatus::Submitted){++result.submitted_frames;Check(s.ticket.Valid()&&s.ticket.HasReadback()==diagnostic,"World ticket contract differs");return s.ticket;}Check(s.status==RuntimeSubmitStatus::WouldBlock&&!s.ticket.Valid(),"Unexpected world suspension or partial pressure ticket");Require(Clock::now()<deadline,"World submit remained blocked after bounded polling");device.CollectRuntimeResources();Pump();}}
    void Complete(RuntimeFrameTicket& t){const auto deadline=Clock::now()+std::chrono::seconds(25);while(!device.IsRuntimeFrameComplete(t)){Require(Clock::now()<deadline,"World fence remained pending");Pump();}t.Reset();device.CollectRuntimeResources();}
    Image Compare(RuntimeFrameTicket& ticket,const Image& wanted,const std::string& name){Image image;const auto deadline=Clock::now()+std::chrono::seconds(25);while(!device.TryReadbackRuntimeFrame(ticket,image)){Require(Clock::now()<deadline,"World diagnostic readback remained pending");Pump();}const auto differing=Differences(image,wanted);Save(output/("runtime-world-"+name+"-gpu"),image);Save(output/("runtime-world-"+name+"-expected"),wanted);result.rgba_byte_checks+=image.rgba.size();comparisons.push_back("{\"name\":"+Quote(name)+",\"rgbaBytes\":"+std::to_string(image.rgba.size())+",\"differingBytes\":"+std::to_string(differing)+"}");Validation("completed-readback/"+name);Check(differing==0,"Runtime world output differs from independent expected data");ticket.Reset();device.CollectRuntimeResources();return image;}
    Image Reference(const Fixture& f,const std::string& name,const Image* analytic=nullptr){
        const auto packed=ReferencePack(f);const auto before=device.GetDiagnostics();auto frame=device.PrepareFrozenEnhancedWorldFrame(packed.world,packed.atlas,f.rows,f.matrix,packed.draws,EnhancedWorldRasterOrigin::Native);const auto gpu=Native(f,device.ReadbackFrozenRGBA(frame));frame.Reset();device.WaitIdle();const auto after=device.GetDiagnostics();reference_preparations+=after.frozen_preparations-before.frozen_preparations;reference_readbacks+=after.frozen_image_readbacks-before.frozen_image_readbacks;
        const auto gl=Native(f,RenderEnhancedWorldGLReference(packed.world,packed.atlas,f.rows,f.matrix,packed.draws));Save(output/("runtime-world-"+name+"-frozen-reference"),gpu);Save(output/("runtime-world-"+name+"-gl-reference"),gl);
        const auto gd=Differences(gpu,gl);uint64_t ad=0;if(analytic){ad=Differences(gpu,*analytic)+Differences(gl,*analytic);Save(output/("runtime-world-"+name+"-analytic"),*analytic);Check(ad==0,"Synthetic frozen/GL control differs from independent analytic output");}
        references.push_back("{\"name\":"+Quote(name)+",\"gpuGlDifferingBytes\":"+std::to_string(gd)+",\"analyticProvided\":"+(analytic?"true":"false")+",\"analyticDifferingBytes\":"+std::to_string(ad)+"}");Validation("reference-complete");return gpu;
    }
    void Write(const std::filesystem::path& capture,bool pass,const std::string& error,const RuntimeResourceDiagnostics& before_cleanup){
        std::ofstream o(output/"runtime-world-tests.json");o<<"{\"schema\":1,\"scope\":\"Real asynchronous terrain/shadow/ordered sprite, world overlay, standard lens, image/UI/cursor runtime resources; retained fixtures only; map-fade/PiP/live producer integration and full Gate3 pending\",\"passed\":"<<(pass?"true":"false")<<",\"gate3Accepted\":false,\"capture\":"<<Quote(capture.string())<<",\"casesPassed\":"<<result.cases<<",\"checks\":"<<result.checks<<",\"rgbaByteChecks\":"<<result.rgba_byte_checks<<",\"successfulSubmissions\":"<<result.submitted_frames<<",\"referencePreparations\":"<<reference_preparations<<",\"referenceImageReadbacks\":"<<reference_readbacks<<",\"controlledPendingReaders\":"<<(controlled?3:0)<<",\"watchdogFired\":"<<(watchdog_fired.load()?"true":"false")<<",\"assetCopiesPerFrame\":false,\"readbackUsedAsInput\":false,\"overlaysTested\":true,\"lensesTested\":true,\"liveProducerTested\":false,\"dirtyLightmapRectanglesTested\":false,\"uploadRingReuseTested\":false,\"pageRegionReclamationTested\":false,\"processDxgiBudgetTested\":false,\"gpuTimestampsTested\":false,\"lightmapRevisionLogicalBytes\":522242,\"error\":"<<Quote(error)<<",\"beforeCleanup\":";Stats(o,before_cleanup);o<<",\"afterCleanup\":";Stats(o,device.GetRuntimeResourceDiagnostics());o<<",\"unchanged1000Loop\":";if(loop_done){o<<"{\"frames\":1000,\"before\":";Stats(o,loop_before);o<<",\"after\":";Stats(o,loop_after);o<<'}';}else o<<"null";
        const auto array=[&](const char* name,const std::vector<std::string>& items){o<<",\""<<name<<"\":[";for(size_t i=0;i<items.size();++i){if(i)o<<',';o<<items[i];}o<<']';};array("cases",records);array("comparisons",comparisons);array("references",references);array("isolatedDeviceCases",isolated_devices);array("terrainMeasurements",terrain_measurements);o<<",\"validationMessages\":[";for(size_t i=0;i<messages.size();++i){if(i)o<<',';o<<Quote(messages[i]);}o<<"]}\n";Require(bool(o),"Cannot retain runtime world report");
    }
};
struct Hold {
    RuntimeReaderHold handle;std::jthread watchdog;std::atomic<bool>& fired;
    Hold(Device& d,std::atomic<bool>& flag):fired(flag){const auto deadline=Clock::now()+std::chrono::seconds(25);while(!d.TryBeginRuntimeReaderHold(handle)){Require(Clock::now()<deadline,"Cannot arm world reader hold");d.CollectRuntimeResources();Pump();}watchdog=std::jthread([this](std::stop_token stop){const auto end=Clock::now()+std::chrono::seconds(15);while(!stop.stop_requested()){if(Clock::now()>=end){fired.store(true);handle.Release();}std::this_thread::sleep_for(std::chrono::milliseconds(10));}});}
    void Stop(){watchdog.request_stop();if(watchdog.joinable())watchdog.join();}
    void Release(){const bool ok=handle.Release();Stop();Require(ok&&handle.Released()&&!fired.load(),"World hold release failed or watchdog intervened");}
    ~Hold(){Stop();handle.Release();}
};
}
RuntimeWorldTestResult RunRuntimeWorldTests(Device& device,const Options& options,const std::filesystem::path& capture,const std::filesystem::path& output){
    (void)options;Runner r(device,output);const auto synthetic=Synthetic();
    try{
        const auto analytic=Analytic(synthetic);const auto reference=r.Reference(synthetic,"synthetic-core",&analytic);
        if(!capture.empty())r.Case("retained-capture-adapted-once-world-only",[&]{const auto owned=Captured(capture);const auto wanted=r.Reference(owned,"captured-core");r.Configure();device.Resize(owned.world.native_width,owned.world.native_height);auto p=Packet(device,owned);auto t=r.Submit(p,true);r.result.preview=r.Compare(t,wanted,"captured");const auto before=device.GetRuntimeResourceDiagnostics();for(unsigned i=0;i<6;++i){auto q=r.Submit(p,false);}const auto after=device.GetRuntimeResourceDiagnostics();r.Check(after.world_upload_bytes==before.world_upload_bytes,"Retained world repeated asset uploads");});
        r.Case("mixed32-64-128-256-terrain-native-size-wrap-and-fine-texels",[&]{
            r.Configure();device.Resize(64,64);
            RuntimeWorldFrame q;q.native_width=q.native_height=q.viewport_width=q.viewport_height=64;
            q.shade_settings={1,0,1,1,0,0,1,0,0};
            std::vector<uint8_t> palette(1024);for(unsigned i=0;i<256;++i){palette[i*4]=palette[i*4+1]=palette[i*4+2]=uint8_t(i);palette[i*4+3]=255;}
            q.palette=Publish(device,RuntimeAssetKind::PaletteRGBA8,1,256,1,palette);
            q.fade=Publish(device,RuntimeAssetKind::FadeR8,2,256,64,std::vector<uint8_t>(16384));
            q.lightmap=Publish(device,RuntimeAssetKind::LightmapR16,3,511,511,std::vector<uint8_t>(511*511*2));
            for(uint32_t size:{32u,64u,128u,256u}) {
                std::vector<uint8_t> pixels(size*size);for(uint32_t y=0;y<size;++y)for(uint32_t x=0;x<size;++x)pixels[y*size+x]=uint8_t(x*3+y*5);
                auto tile=Publish(device,RuntimeAssetKind::TileR8,90000+size,size,size,pixels);
                for(unsigned mode=0;mode<3;++mode){
                    const float start=mode==1?-32:mode==2?30:0,span=mode==2?4:32;
                    const auto v=[&](float x,float y,float u,float w){EnhancedTerrainVertex a;a.position={x,y,0,1};a.u=u;a.v=w;return a;};
                    const auto a=v(-1,1,start,start),b=v(1,1,start+span,start),c=v(1,-1,start+span,start+span),d=v(-1,-1,start,start+span);
                    q.terrain={{{a,b,c},tile},{{a,c,d},tile}};
                    Image expected{64,64,std::vector<uint8_t>(64*64*4)};
                    for(unsigned y=0;y<64;++y)for(unsigned x=0;x<64;++x){
                        const int tx=int(std::floor((start+(float(x)+0.5f)*span/64)*float(size)/32))&int(size-1);
                        const int ty=int(std::floor((start+(float(y)+0.5f)*span/64)*float(size)/32))&int(size-1);
                        const uint8_t index=pixels[size_t(ty)*size+size_t(tx)];const size_t at=(size_t(y)*64+x)*4;
                        expected.rgba[at]=expected.rgba[at+1]=expected.rgba[at+2]=index;expected.rgba[at+3]=255;
                    }
                    auto ticket=r.Submit(q,true);r.Compare(ticket,expected,"terrain-resolution-"+std::to_string(size)+"-wrap-"+std::to_string(mode));
                }
            }
        });
        r.active="synthetic-setup";
        r.Configure();r.Validation("after-configure");
        device.Resize(80,64);r.Validation("after-resize80x64");
        auto fixture=synthetic;auto p=Packet(device,fixture);r.Validation("after-cpu-publication");
        r.Case("native-origin-terrain-shadow-painter-order-analytic",[&]{auto t=r.Submit(p,true);r.result.preview=r.Compare(t,reference,"analytic-native");});
        r.Case("compact-sprites-exact-rgba-pixels-all-passes",[&]{
            auto compact=p;std::vector<RuntimeAssetLease> sources;
            const auto before=device.GetRuntimeResourceDiagnostics();size_t payload=0;
            for(size_t i=0;i<fixture.sources.size();++i) {
                const auto& src=fixture.sources[i];std::vector<uint8_t> rg;
                for(size_t at=0;at<src.rgba.size();at+=4){rg.push_back(src.rgba[at]);rg.push_back(src.rgba[at+1]);}
                payload+=rg.size();sources.push_back(Publish(device,RuntimeAssetKind::SpriteRG8,800000+i,src.width,src.height,rg));
            }
            r.Check(device.GetRuntimeResourceDiagnostics().cpu_bytes==before.cpu_bytes+payload,"Compact sprite CPU storage is not two bytes per texel");
            for(size_t i=0;i<compact.shadows.size();++i)compact.shadows[i].source=sources[fixture.shadow_sources[i]];
            for(size_t i=0;i<compact.sprites.size();++i)compact.sprites[i].source=sources[fixture.sprite_sources[i]];
            auto first=r.Submit(compact,true);r.Compare(first,analytic,"compact-normal-shadow");
            r.Check(device.GetRuntimeResourceDiagnostics().world_upload_bytes==before.world_upload_bytes+payload,"Compact sprite uploads are not two bytes per texel");
            auto effect=fixture.sources[0].rgba;effect[0]=50;
            std::vector<uint8_t> effect_rg;for(size_t i=0;i<effect.size();i+=4){effect_rg.push_back(effect[i]);effect_rg.push_back(effect[i+1]);}
            auto old_effect=Publish(device,RuntimeAssetKind::SpriteRGBA8,810000,4,4,effect);
            auto new_effect=Publish(device,RuntimeAssetKind::SpriteRG8,810000,4,4,effect_rg);
            for(unsigned pass=0;pass<=5;++pass)for(bool flip:{false,true}) {
                auto original=p,packed=compact;
                original.sprites[0].source=old_effect;packed.sprites[0].source=new_effect;
                for(auto* packet:{&original,&packed})for(auto& sprite:packet->sprites) {
                    sprite.draw.kind=OutlinePassKind(pass);sprite.draw.flip=flip;
                    sprite.draw.colour={0.2f,0.4f,0.6f,0.8f};
                    const bool normal=sprite.draw.kind==OutlinePassKind::NormalSprite;
                    sprite.draw.lookup_row=normal?0:NoSpriteColourRow;sprite.draw.source_alpha=normal?0.6f:1;
                    if(!normal)sprite.colour_row.Reset();
                }
                auto old=r.Submit(original,true);Image expected;
                const auto deadline=Clock::now()+std::chrono::seconds(25);
                while(!device.TryReadbackRuntimeFrame(old,expected)){Require(Clock::now()<deadline,"Compact reference timeout");Pump();}
                old.Reset();device.CollectRuntimeResources();
                auto now=r.Submit(packed,true);r.Compare(now,expected,"compact-pass-"+std::to_string(pass)+"-flip-"+std::to_string(flip));
            }
        });
        r.Case("immutable-sprite-publication-alias",[&]{auto bytes=std::make_shared<std::vector<uint8_t>>(fixture.sources[0].rgba);auto lease=device.PublishRuntimeAsset(Key(RuntimeAssetKind::SpriteRGBA8,900000),{4,4},bytes);(*bytes)[0]=22;bytes.reset();auto probe=p;probe.sprites[0].source=lease;auto t=r.Submit(probe,true);r.Compare(t,analytic,"sealed-source");});
        r.Case("unchanged-1000-no-asset-upload-allocation-idle-readback",[&]{for(unsigned i=0;i<6;++i){auto t=r.Submit(p,false);}r.loop_before=device.GetRuntimeResourceDiagnostics();const auto graphics=device.GetDiagnostics();for(unsigned i=0;i<1000;++i){auto t=r.Submit(p,false);}r.loop_after=device.GetRuntimeResourceDiagnostics();r.loop_done=true;const auto& a=r.loop_before;const auto& b=r.loop_after;
            r.Check(b.world_upload_bytes==a.world_upload_bytes&&b.source_upload_bytes==a.source_upload_bytes&&b.palette_upload_bytes==a.palette_upload_bytes&&b.aligned_upload_bytes==a.aligned_upload_bytes,"Unchanged world retransmitted assets");
            r.Check(b.terrain_upload_bytes==a.terrain_upload_bytes&&b.terrain_dirty_ranges==a.terrain_dirty_ranges
                &&b.terrain_reused_bytes==a.terrain_reused_bytes+1000*p.terrain.size()*120,"Unchanged1000 terrain copies did not plateau");
            r.Check(b.resource_allocations==a.resource_allocations&&b.descriptor_heap_allocations==a.descriptor_heap_allocations&&b.world_pages==a.world_pages,"World warmup allocation did not plateau");
            r.Check(b.global_idle_calls==a.global_idle_calls&&b.fence_waits==a.fence_waits&&b.image_readbacks==a.image_readbacks&&b.publications==a.publications,"World loop internally waited/read back/copied assets");
            r.Check(b.presentations==a.presentations+1000&&b.world_submissions==a.world_submissions+1000&&device.GetDiagnostics().frozen_preparations==graphics.frozen_preparations,"World loop did not execute1000 actual runtime presentations");auto t=r.Submit(p,true);r.Compare(t,analytic,"unchanged-final");});
        r.Case("warmed-three-slot-clear-rgb-alpha-exact-no-resident-reallocation",[&]{
            const std::array<std::array<float,4>,3> clears{{{64.0f/255,128.0f/255,192.0f/255,0},{192.0f/255,64.0f/255,128.0f/255,128.0f/255},synthetic.world.clear_rgba}};
            const auto before=device.GetRuntimeResourceDiagnostics();const auto graphics=device.GetDiagnostics();
            // Three consecutive presents per state traverse the warmed slots;
            // each slot sees transparent, fractional and opaque clears. The
            // viewport remains the unchanged opaque analytic world.
            for(unsigned i=0;i<9;++i){auto frame=fixture;frame.world.clear_rgba=clears[i/3];p.clear_rgba=frame.world.clear_rgba;auto t=r.Submit(p,true);r.Compare(t,Analytic(frame),"clear-rgb-alpha-"+std::to_string(i));}
            p.clear_rgba=fixture.world.clear_rgba;const auto after=device.GetRuntimeResourceDiagnostics();
            r.Check(after.resource_allocations==before.resource_allocations+9&&after.image_readbacks==before.image_readbacks+9,"Clear changes allocated resident resources beyond exactly9 requested readbacks");
            r.Check(after.default_bytes==before.default_bytes&&after.upload_bytes==before.upload_bytes&&after.readback_bytes==before.readback_bytes&&after.descriptors==before.descriptors&&after.descriptor_heap_allocations==before.descriptor_heap_allocations&&after.world_pages==before.world_pages,"Clear changes failed resident allocation plateau");
            r.Check(after.world_upload_bytes==before.world_upload_bytes&&after.source_upload_bytes==before.source_upload_bytes&&after.palette_upload_bytes==before.palette_upload_bytes&&after.aligned_upload_bytes==before.aligned_upload_bytes&&after.publications==before.publications,"Clear changes uploaded or recopied assets");
            r.Check(after.global_idle_calls==before.global_idle_calls&&after.fence_waits==before.fence_waits&&device.GetDiagnostics().frozen_preparations==graphics.frozen_preparations,"Changing clear inserted synchronous waits or frozen preparation");
            r.Check(after.presentations==before.presentations+9&&after.world_shader_clears>before.world_shader_clears,"Changed clears did not exercise actual fallback clears and presentations");
        });
        r.Case("tile-only-revision-1024-bytes-old-admission-rejected",[&]{auto old=p;fixture.world.tiles[0].fill(11);auto next=Publish(device,RuntimeAssetKind::TileR8,100,32,32,fixture.world.tiles[0],1,2);for(auto& t:p.terrain)t.tile=next;const auto before=device.GetRuntimeResourceDiagnostics();auto ticket=r.Submit(p,true);r.Compare(ticket,Analytic(fixture),"tile-revision");r.Check(device.GetRuntimeResourceDiagnostics().world_upload_bytes==before.world_upload_bytes+1024,"Tile replacement uploaded unrelated assets");r.Reject([&]{device.TrySubmitRuntimeWorldFrame(old);},"Stale tile packet admitted after version replacement");});
        r.Case("sprite-only-revision-64-bytes",[&]{fixture.sources[0].rgba[0]=22;SourceRevision(device,p,fixture,0,2);const auto before=device.GetRuntimeResourceDiagnostics();auto t=r.Submit(p,true);r.Compare(t,Analytic(fixture),"sprite-revision");r.Check(device.GetRuntimeResourceDiagnostics().world_upload_bytes==before.world_upload_bytes+64,"Sprite replacement uploaded unrelated assets");});
        r.Case("colour-row-only-revision-1024-bytes",[&]{fixture.rows[0][22*4]=18;fixture.rows[0][22*4+1]=34;auto row=Publish(device,RuntimeAssetKind::ColourRowRGBA8,200000,256,1,fixture.rows[0],1,2);for(auto& s:p.sprites)s.colour_row=row;const auto before=device.GetRuntimeResourceDiagnostics();auto t=r.Submit(p,true);r.Compare(t,Analytic(fixture),"row-revision");r.Check(device.GetRuntimeResourceDiagnostics().world_upload_bytes==before.world_upload_bytes+1024,"CLUT replacement retransmitted sprite/terrain assets");});
        r.Case("lightmap-only-version-real-shading",[&]{auto f=synthetic;f.world.shade_settings={0,0,1,1,1,0,1,0,0};for(auto& tri:f.world.terrain)for(auto& v:tri.vertices){v.shade=0.25f;v.stl_x=v.position.x<0?0:2;v.stl_y=v.position.y>0?0:2;}const auto baseline=r.Reference(f,"lightmap-before");auto changed=f;changed.world.lightmap[512]=16384;const auto wanted=r.Reference(changed,"lightmap-after");r.Check(Differences(baseline,wanted)>0,"Lightmap revision has no visible contribution");r.Configure();auto packet=Packet(device,f);auto t=r.Submit(packet,true);r.Compare(t,baseline,"lightmap-before");packet.lightmap=Publish(device,RuntimeAssetKind::LightmapR16,3,511,511,LightBytes(changed),1,2);const auto before=device.GetRuntimeResourceDiagnostics();auto next=r.Submit(packet,true);r.Compare(next,wanted,"lightmap-revision");const auto after=device.GetRuntimeResourceDiagnostics();r.Check(after.world_upload_bytes==before.world_upload_bytes+2&&after.lightmap_upload_bytes==before.lightmap_upload_bytes+2,"Sparse R16 lightmap revision did not upload exactly one changed texel");r.Check(after.lightmap_images==before.lightmap_images&&after.lightmap_image_reuses==before.lightmap_image_reuses+1&&after.lightmap_cold_uploads==before.lightmap_cold_uploads,"Completed lightmap image was not reused");});
        r.Case("native-runtime-overlays-order-depth-clut-fade-and-reuse", [&] {
            r.Configure();
            auto fixture = synthetic;
            fixture.world.fade[63 * 256 + 1] = 2;
            fixture.world.fade[63 * 256 + 2] = 3;
            fixture.world.fade[63 * 256 + 3] = 1;
            auto packet = Packet(device, fixture);
            auto expected = Analytic(fixture);
            const std::vector<uint8_t> indices{0,1,2,3};
            auto source = Publish(device, RuntimeAssetKind::Indexed8, 300001, 2, 2, indices);
            SpriteColourRow colours{};
            const std::array<std::array<uint8_t,4>,4> inks{{{0,0,0,0},{200,20,40,0},{30,210,50,255},{60,80,220,255}}};
            for (unsigned i = 0; i < 4; ++i) std::copy(inks[i].begin(), inks[i].end(), colours.begin() + i * 4);
            auto row = Publish(device, RuntimeAssetKind::ColourRowRGBA8, 300002, 256, 1, colours);
            const auto pixel = [&](unsigned x, unsigned y, std::array<uint8_t,4> colour) {
                std::copy(colour.begin(), colour.end(), expected.rgba.begin() + (size_t(y) * expected.width + x) * 4);
            };
            const auto solid = [&](WorldOverlayPhase phase, float x, float y, float z, std::array<float,4> colour) {
                RuntimeWorldOverlay overlay;
                overlay.draw.phase = phase; overlay.draw.mode = WorldOverlayMode::Solid;
                overlay.draw.depth_test = phase == WorldOverlayPhase::DepthWorld;
                overlay.draw.x0 = x; overlay.draw.y0 = y; overlay.draw.x1 = x + 4; overlay.draw.y1 = y + 4;
                overlay.draw.z_ndc = z; overlay.draw.colour = colour;
                packet.overlays.push_back(overlay);
            };
            solid(WorldOverlayPhase::DepthWorld, 8, 40, 0.5f, {1,0,0,1}); // Hidden by terrain D24.
            solid(WorldOverlayPhase::DepthWorld, 14, 40, -0.75f, {1,0,0,1});
            for (unsigned y = 40; y < 44; ++y) for (unsigned x = 14; x < 18; ++x) pixel(x,y,{255,0,0,255});
            solid(WorldOverlayPhase::FlatWorld, 0, 0, 0.5f, {0,1,0,1}); // Native border, outside world viewport.
            for (unsigned y = 0; y < 4; ++y) for (unsigned x = 0; x < 4; ++x) pixel(x,y,{0,255,0,255});
            for (unsigned mode = 0; mode < 4; ++mode) {
                RuntimeWorldOverlay overlay; auto& q = overlay.draw;
                q.phase = WorldOverlayPhase::FlatWorld; q.depth_test = false;
                q.mode = mode == 0 ? WorldOverlayMode::Sprite : mode == 1 ? WorldOverlayMode::Fade
                    : mode == 2 ? WorldOverlayMode::Clut : WorldOverlayMode::Colored;
                q.x0 = float(20 + mode * 6); q.y0 = 2; q.x1 = q.x0 + 4; q.y1 = 6;
                q.u1 = q.v1 = 1; q.source = 0; overlay.source = source;
                if (mode != 3) { q.colour_row = 0; overlay.colour_row = row; }
                else q.colour = {1,0,1,1};
                if (mode == 1) q.fade_row = 63;
                if (mode == 0) { q.scissor_enabled = true; q.scissor_x = 21; q.scissor_y = 3; q.scissor_w = q.scissor_h = 2; }
                packet.overlays.push_back(overlay);
                for (unsigned y = 0; y < 4; ++y) for (unsigned x = 0; x < 4; ++x) {
                    const auto index = indices[(y / 2) * 2 + x / 2];
                    if (!index || (mode == 2 && !inks[index][3])) continue;
                    if (mode == 0 && (x < 1 || x >= 3 || y < 1 || y >= 3)) continue;
                    const unsigned lookup = mode == 1 ? (index == 3 ? 1 : index + 1) : index;
                    auto colour = mode == 3 ? std::array<uint8_t,4>{255,0,255,255} : inks[lookup];
                    colour[3] = 255; // Sprite/Fade ignore palette alpha; opaque Clut entries remain.
                    pixel(20 + mode * 6 + x,2 + y,colour);
                }
            }
            solid(WorldOverlayPhase::Swipe, 44, 2, 0, {128.0f/255,64.0f/255,32.0f/255,0.5f});
            for (unsigned y = 2; y < 6; ++y) for (unsigned x = 44; x < 48; ++x) pixel(x,y,{70,44,34,191});
            solid(WorldOverlayPhase::Swipe, 22, 3, 0, {0,1,0,1});
            for (unsigned y = 3; y < 7; ++y) for (unsigned x = 22; x < 26; ++x) pixel(x,y,{0,255,0,255});
            auto invalid = packet; invalid.overlays.back().draw.phase = WorldOverlayPhase::DepthWorld;
            r.Reject([&] { device.TrySubmitRuntimeWorldFrame(invalid); }, "Out-of-order runtime overlay phase accepted");
            invalid = packet; invalid.overlays[3].draw.source = 1;
            r.Reject([&] { device.TrySubmitRuntimeWorldFrame(invalid); }, "Physical overlay source index accepted");
            for (unsigned i = 0; i < 3; ++i) { auto ticket = r.Submit(packet,true); r.Compare(ticket,expected,"overlays-warm-" + std::to_string(i)); }
            const auto before = device.GetRuntimeResourceDiagnostics();
            for (unsigned i = 0; i < 6; ++i) { auto ticket = r.Submit(packet,false); r.Complete(ticket); }
            const auto after = device.GetRuntimeResourceDiagnostics();
            r.Check(after.resource_allocations == before.resource_allocations
                && after.descriptor_heap_allocations == before.descriptor_heap_allocations
                && after.world_upload_bytes == before.world_upload_bytes
                && after.global_idle_calls == before.global_idle_calls && after.fence_waits == before.fence_waits,
                "Unchanged runtime overlays reallocated, reuploaded or waited");
            auto ticket = r.Submit(packet,true); r.Compare(ticket,expected,"overlays-reused");
        });
        r.Case("native-runtime-lens-resident-source-operators-and-reuse", [&] {
            r.Configure();
            auto fixture = synthetic;
            auto packet = Packet(device, fixture);
            auto scene = Analytic(fixture);
            RuntimeWorldOverlay marker;
            marker.draw.phase = WorldOverlayPhase::FlatWorld; marker.draw.depth_test = false;
            marker.draw.mode = WorldOverlayMode::Solid;
            marker.draw.x0 = 7; marker.draw.y0 = 0; marker.draw.x1 = 11; marker.draw.y1 = 4;
            marker.draw.colour = {1,0,0,1}; packet.overlays.push_back(marker);
            for (unsigned y = 0; y < 4; ++y) for (unsigned x = 7; x < 11; ++x) {
                const auto offset = (size_t(y) * scene.width + x) * 4;
                scene.rgba[offset] = 255; scene.rgba[offset+1] = scene.rgba[offset+2] = 0;
            }
            std::vector<uint8_t> remap{0,0,0,0,255,255,0,0,0,0,255,255,5,0,15,0};
            auto remap_lease = Publish(device,RuntimeAssetKind::RemapRG16UInt,400001,2,2,remap);
            std::vector<uint8_t> density(256 * 256), rgb6(512 * 512,10), fade(256 * 64);
            for (unsigned y = 0; y < 256; ++y) for (unsigned x = 0; x < 256; ++x) density[y*256+x] = uint8_t(x+3*y);
            for (unsigned y = 0; y < 64; ++y) for (unsigned x = 0; x < 256; ++x) fade[y*256+x] = uint8_t(y%2 ? 10 : 11);
            const std::vector<uint8_t> overlay{255,0,200,40};
            auto density_lease = Publish(device,RuntimeAssetKind::Indexed8,400002,256,256,density);
            auto rgb6_lease = Publish(device,RuntimeAssetKind::Indexed8,400003,512,512,rgb6);
            auto fade_lease = Publish(device,RuntimeAssetKind::FadeR8,400004,256,64,fade);
            auto overlay_lease = Publish(device,RuntimeAssetKind::Indexed8,400005,2,2,overlay);
            const auto configure = [&](LensPassKind kind) {
                RuntimeWorldLens lens;
                lens.kind = kind; lens.source_x = 7; lens.destination_x = 7; lens.destination_y = 9;
                lens.viewport_width = 64; lens.viewport_height = 48;
                lens.clear_rgba = {4.0f/255,8.0f/255,16.0f/255,1};
                if (kind == LensPassKind::Remap) lens.remap = remap_lease;
                if (kind == LensPassKind::Mist || kind == LensPassKind::Overlay) {
                    lens.palette = packet.palette; lens.rgb6 = rgb6_lease;
                }
                if (kind == LensPassKind::Mist) {
                    lens.fade = fade_lease; lens.density = density_lease;
                    lens.mist_offsets = {1.75f,2.25f,3.5f,4.125f}; lens.mist_lightness = 8;
                }
                if (kind == LensPassKind::Overlay) { lens.overlay = overlay_lease; lens.overlay_alpha = 128; }
                packet.lens = lens;
            };
            const auto expected = [&] {
                const auto& lens = *packet.lens;
                Image image{80,64,std::vector<uint8_t>(80*64*4)};
                for (size_t i = 0; i < image.rgba.size(); ++i) image.rgba[i] = uint8_t(std::lround(lens.clear_rgba[i%4]*255));
                for (unsigned y = 0; y < 48; ++y) for (unsigned x = 0; x < 64; ++x) {
                    unsigned sx = x, sy = y;
                    if (lens.kind == LensPassKind::Remap) {
                        const auto at = (std::min(y,1u)*2+std::min(x,1u))*4;
                        sx = unsigned(remap[at]) | unsigned(remap[at+1]) << 8;
                        sy = unsigned(remap[at+2]) | unsigned(remap[at+3]) << 8;
                    }
                    const auto source = (size_t(std::min(sy,63u))*80+std::min(sx+7,79u))*4;
                    std::array<uint8_t,4> colour{};
                    std::copy_n(scene.rgba.begin()+source,4,colour.begin()); colour[3] = 255;
                    int index = -1;
                    if (lens.kind == LensPassKind::Mist) {
                        const unsigned vx = x*10, vy = y*10;
                        const auto first = density[((2+vy)&255)*256+((1+vx)&255)];
                        const auto second = density[((4+65536-vx)&255)*256+((3+65536-vy)&255)];
                        const unsigned row = 8 + std::min((unsigned(first)+second)/8,32u);
                        index = row%2 ? 10 : 11;
                    } else if (lens.kind == LensPassKind::Overlay) {
                        // Production truncates the 16.16 scale before multiplying each pixel coordinate.
                        const unsigned ox = (x*((2u<<16)/64u))>>16, oy = (y*((2u<<16)/48u))>>16;
                        const auto value = overlay[oy*2+ox];
                        if (value != 255) index = (unsigned(value)+10)/2;
                    }
                    if (index >= 0) std::copy_n(fixture.world.palette_rgba.begin()+index*4,4,colour.begin());
                    std::copy(colour.begin(),colour.end(),image.rgba.begin()+((size_t(y)+9)*80+x+7)*4);
                }
                return image;
            };
            configure(LensPassKind::Copy);
            auto invalid = packet; invalid.lens->density = density_lease;
            r.Reject([&] { device.TrySubmitRuntimeWorldFrame(invalid); }, "Unused lens lease accepted");
            for (auto kind : {LensPassKind::Copy,LensPassKind::Remap,LensPassKind::Mist,LensPassKind::Overlay}) {
                configure(kind);
                auto ticket = r.Submit(packet,true);
                r.Compare(ticket,expected(),"lens-kind-"+std::to_string(unsigned(kind)));
            }
            // Warm every presentation slot with the same complete lens resources.
            for (unsigned i = 0; i < 3; ++i) { auto ticket = r.Submit(packet,false); r.Complete(ticket); }
            const auto before = device.GetRuntimeResourceDiagnostics();
            for (unsigned i = 0; i < 6; ++i) {
                packet.lens->clear_rgba = i%2 ? std::array<float,4>{4.0f/255,8.0f/255,16.0f/255,1}
                    : std::array<float,4>{8.0f/255,16.0f/255,32.0f/255,0.5f};
                auto ticket = r.Submit(packet,false); r.Complete(ticket);
            }
            const auto after = device.GetRuntimeResourceDiagnostics();
            r.Check(after.resource_allocations == before.resource_allocations
                && after.descriptor_heap_allocations == before.descriptor_heap_allocations
                && after.world_upload_bytes == before.world_upload_bytes
                && after.global_idle_calls == before.global_idle_calls && after.fence_waits == before.fence_waits,
                "Resident lens composition reallocated, reuploaded or waited");
            device.SetRuntimeFailureInjection(RuntimeFailurePoint::BeforeExecute);
            r.Reject<std::runtime_error>([&] { device.TrySubmitRuntimeWorldFrame(packet,true); }, "Lens rollback injection absent");
            r.Check(device.GetRuntimeResourceDiagnostics().submissions == after.submissions, "Lens rollback consumed submission");
            auto ticket = r.Submit(packet,true); r.Compare(ticket,expected(),"lens-reused-after-rollback");
            packet.lens.reset();
            auto unfiltered = r.Submit(packet,true); r.Compare(unfiltered,scene,"lens-disabled-resident-source");
        });
        r.Case("map-fade-immutable-captures-warp-palette-and-post-order",[&] {
            r.Configure();auto packet=Packet(device,synthetic);packet.terrain.clear();packet.shadows.clear();packet.sprites.clear();
            packet.viewport_x=packet.viewport_y=0;packet.viewport_width=80;packet.viewport_height=64;
            const auto fill=[](Image& image,int x0,int y0,int x1,int y1,std::array<uint8_t,4> c){for(int y=y0;y<y1;++y)for(int x=x0;x<x1;++x)std::copy(c.begin(),c.end(),image.rgba.begin()+(size_t(y)*image.width+x)*4);};
            const auto solid=[](int x0,int y0,int x1,int y1,std::array<uint8_t,4> c){WorldOverlayQuad q;q.phase=WorldOverlayPhase::FlatWorld;q.mode=WorldOverlayMode::Solid;q.depth_test=false;q.x0=float(x0);q.y0=float(y0);q.x1=float(x1);q.y1=float(y1);for(size_t i=0;i<4;++i)q.colour[i]=float(c[i])/255;return q;};
            Image world{80,64,std::vector<uint8_t>(80*64*4)},paper=world;
            const std::array<std::array<uint8_t,4>,4> colours{{{85,0,170,255},{0,255,85,255},{170,85,255,255},{255,170,0,255}}};
            for(unsigned i=0;i<4;++i){const int x=int(i%2)*40,y=int(i/2)*32;packet.overlays.push_back({solid(x,y,x+40,y+32,colours[i]),{}, {}});fill(world,x,y,x+40,y+32,colours[i]);}
            std::vector<uint8_t> rgb6(512*512),fade(256*64),ghost(256*256),indices(7*5);
            for(unsigned key=0;key<rgb6.size();++key)rgb6[key]=uint8_t(((key>>12)*3+((key>>6)&63)*5+(key&63)*7)&255);
            for(unsigned row=0;row<64;++row)for(unsigned i=0;i<256;++i)fade[row*256+i]=uint8_t(i+row*3);
            for(unsigned row=0;row<256;++row)for(unsigned col=0;col<256;++col)ghost[row*256+col]=uint8_t(row*3+col*5);
            for(unsigned i=0;i<indices.size();++i)indices[i]=uint8_t(i*7);
            auto pal=synthetic.world.palette_rgba;
            RuntimeWorldMapFade m;m.transition_id=1;m.capture_pending=true;m.palette=packet.palette;
            m.fade=Publish(device,RuntimeAssetKind::FadeR8,700010,256,64,fade);
            m.rgb6=Publish(device,RuntimeAssetKind::Indexed8,700011,512,512,rgb6);
            m.ghost_table=Publish(device,RuntimeAssetKind::Indexed8,700012,256,256,ghost);packet.map_fade=m;
            RuntimePostDraw image;image.phase=RuntimePostPhase::ImagePresents;image.kind=RuntimePostKind::ImageOpaque;
            image.draw.phase=WorldOverlayPhase::FlatWorld;image.draw.depth_test=false;image.draw.x1=80;image.draw.y1=64;
            image.draw.u1=image.draw.v1=1;image.draw.source=image.draw.colour_row=0;image.colour_row=packet.palette;
            image.source=Publish(device,RuntimeAssetKind::Indexed8,700013,7,5,indices);packet.post_composition.push_back(image);
            for(unsigned y=0;y<64;++y)for(unsigned x=0;x<80;++x){const unsigned i=indices[size_t((y+.5f)/64*5)*7+size_t((x+.5f)/80*7)];std::copy_n(pal.begin()+i*4,4,paper.rgba.begin()+(y*80+x)*4);}
            const auto add=[&](RuntimePostPhase phase,int x,int y,int w,int h,std::array<uint8_t,4> c){RuntimePostDraw d;d.phase=phase;d.draw=solid(x,y,x+w,y+h,c);packet.post_composition.push_back(d);};
            add(RuntimePostPhase::GameUI,30,20,20,15,{85,170,0,255});fill(paper,30,20,50,35,{85,170,0,255});
            add(RuntimePostPhase::ParchmentWorld,31,21,18,13,{255,0,85,255});packet.post_composition.back().draw.depth_test=true;packet.post_composition.back().draw.phase=WorldOverlayPhase::DepthWorld;
            fill(paper,31,21,49,34,{255,0,85,255});
            add(RuntimePostPhase::ParchmentUI,32,22,16,11,{0,85,255,255});fill(paper,32,22,48,33,{0,85,255,255});
            RuntimePostDraw tile;tile.phase=RuntimePostPhase::ZoomBoxes;tile.kind=RuntimePostKind::QuadOpaque;tile.draw=image.draw;tile.draw.x0=2;tile.draw.y0=2;tile.draw.x1=9;tile.draw.y1=9;
            tile.draw.scissor_enabled=true;tile.draw.scissor_x=3;tile.draw.scissor_y=3;tile.draw.scissor_w=5;tile.draw.scissor_h=5;
            tile.source=Publish(device,RuntimeAssetKind::Indexed8,700014,1,1,std::array<uint8_t,1>{0});
            auto tilepal=pal;tilepal[0]=17;tilepal[1]=29;tilepal[2]=43;
            tile.colour_row=Publish(device,RuntimeAssetKind::ColourRowRGBA8,700015,256,1,tilepal);packet.post_composition.push_back(tile);
            add(RuntimePostPhase::FrontOverlay,4,4,3,3,{85,255,0,255});add(RuntimePostPhase::Text,5,5,2,2,{255,85,0,255});add(RuntimePostPhase::Cursor,6,6,1,1,{0,170,255,255});
            const auto expected=[&](float step,float warp){
                Image result=world;
                const auto sample=[&](const Image& source,float strength,unsigned x,unsigned y){const float fx=(x+.5f)/80,fy=(y+.5f)/64;
                    // Define the exact collapse independently of CPU/GPU reciprocal rounding;
                    // this singular point is not a claim of prior GL byte-for-byte behaviour.
                    const float px=strength==40?0.5f:std::clamp(fx+strength*(4-8*fx)/320,0.0f,1.0f);
                    const float py=strength==40?0.5f:std::clamp(fy+strength*4*(1-2*fy)/320,0.0f,1.0f);
                    // GL samples (x,1-y) in bottom-up capture storage.
                    const auto at=((63u-std::min(unsigned((1-py)*64),63u))*80+std::min(unsigned(px*80),79u))*4;
                    const auto six=[&](unsigned c){return unsigned(float(source.rgba[at+c])/255*63+.5f);};return rgb6[(six(0)<<12)|(six(1)<<6)|six(2)];};
                const unsigned row=unsigned(step+.5f);
                for(unsigned y=0;y<64;++y)for(unsigned x=0;x<80;++x){const unsigned pi=fade[row*256+sample(paper,(32-step)*warp,x,y)],wi=fade[(32-row)*256+sample(world,step*warp,x,y)];
                    const unsigned index=ghost[wi*256+pi];std::copy_n(pal.begin()+index*4,4,result.rgba.begin()+(y*80+x)*4);}
                fill(result,3,3,8,8,{17,29,43,255});fill(result,4,4,7,7,{85,255,0,255});fill(result,5,5,7,7,{255,85,0,255});fill(result,6,6,7,7,{0,170,255,255});return result;
            };
            // First candidate must roll back every capture allocation before execute.
            const auto before=device.GetRuntimeResourceDiagnostics();device.SetRuntimeFailureInjection(RuntimeFailurePoint::BeforeExecute);
            r.Reject<std::runtime_error>([&]{device.TrySubmitRuntimeWorldFrame(packet,true);},"Map-fade pre-execute injection absent");
            const auto rollback=device.GetRuntimeResourceDiagnostics();r.Check(rollback.default_bytes==before.default_bytes&&rollback.submissions==before.submissions,"Map-fade rollback retained capture resources");
            auto t=r.Submit(packet,true);r.Compare(t,expected(0,1),"mapfade-capture-step0");
            packet.map_fade->capture_pending=false;packet.post_composition.erase(std::remove_if(packet.post_composition.begin(),packet.post_composition.end(),[](const auto& d){return d.phase>=RuntimePostPhase::ParchmentWorld&&d.phase<=RuntimePostPhase::ParchmentFront;}),packet.post_composition.end());
            packet.overlays.clear();packet.clear_rgba={1,0,0,1}; // Must not replace the retained world.
            for(const auto step:std::array<float,3>{16,32,11.25f}){packet.map_fade->step=step;packet.map_fade->warp_scale=step==11.25f?2.5f:1;auto ticket=r.Submit(packet,true);r.Compare(ticket,expected(step,packet.map_fade->warp_scale),"mapfade-step-"+std::to_string(step));}
            for(size_t i=0;i<256;++i){pal[i*4]=uint8_t(255-pal[i*4]);pal[i*4+2]=uint8_t(255-pal[i*4+2]);}
            packet.map_fade->palette=Publish(device,RuntimeAssetKind::PaletteRGBA8,700016,256,1,pal);
            {Hold hold(device,r.watchdog_fired);std::array<RuntimeFrameTicket,3> tickets;std::array<Image,3> wanted;
                for(size_t i=0;i<3;++i){packet.map_fade->step=float(i)*16;wanted[i]=expected(packet.map_fade->step,2.5f);tickets[i]=r.Submit(packet,true);}
                for(const auto& ticket:tickets)r.Check(!device.IsRuntimeFrameComplete(ticket),"Map-fade held reader completed early");
                r.Check(device.TrySubmitRuntimeWorldFrame(packet).status==RuntimeSubmitStatus::WouldBlock,"Map-fade held readers did not apply backpressure");hold.Release();for(size_t i=0;i<3;++i)r.Compare(tickets[i],wanted[i],"mapfade-held-palette-"+std::to_string(i));}
            for(unsigned i=0;i<3;++i){auto ticket=r.Submit(packet,false);r.Complete(ticket);}const auto warm=device.GetRuntimeResourceDiagnostics();
            for(unsigned i=0;i<6;++i){packet.map_fade->step=float(i)*5;auto ticket=r.Submit(packet,false);r.Complete(ticket);}const auto reused=device.GetRuntimeResourceDiagnostics();
            r.Check(reused.resource_allocations==warm.resource_allocations&&reused.world_upload_bytes==warm.world_upload_bytes&&reused.fence_waits==warm.fence_waits&&reused.global_idle_calls==warm.global_idle_calls,"Map-fade retained steps allocated/uploaded/waited");
            auto invalid=packet;invalid.map_fade->transition_id=2;r.Reject([&]{device.TrySubmitRuntimeWorldFrame(invalid);},"Map-fade accepted missing transition capture");
            packet.map_fade->transition_id=2;packet.map_fade->capture_pending=true;auto next=r.Submit(packet,false);r.Complete(next);
            r.Check(device.GetRuntimeResourceDiagnostics().resource_allocations==reused.resource_allocations+2,"New map-fade transition did not allocate exactly two capture images");
        });
        r.Case("whole-image-revision-reuse-and-held-readers",[&] {
            r.Configure();auto packet=Packet(device,synthetic);
            packet.terrain.clear();packet.shadows.clear();packet.sprites.clear();
            RuntimePostDraw image;image.phase=RuntimePostPhase::ImagePresents;image.kind=RuntimePostKind::ImageComposite;
            image.draw.phase=WorldOverlayPhase::FlatWorld;image.draw.depth_test=false;
            image.draw.x1=80;image.draw.y1=64;image.draw.u1=image.draw.v1=1;image.draw.source=image.draw.colour_row=0;
            image.colour_row=packet.palette;packet.post_composition.push_back(image);
            uint64_t revision=0;
            const auto change=[&](uint8_t index) {
                packet.post_composition[0].source=Publish(device,RuntimeAssetKind::Indexed8,800001,3,5,std::vector<uint8_t>(15,index),1,++revision);
            };
            const auto wanted=[&](uint8_t index) {
                Image out{80,64,std::vector<uint8_t>(80*64*4)};
                for(size_t i=0;i<out.rgba.size();i+=4)std::copy_n(synthetic.world.palette_rgba.begin()+index*4,4,out.rgba.begin()+i);
                return out;
            };
            for(unsigned i=0;i<3;++i){change(10);auto ticket=r.Submit(packet,false);r.Complete(ticket);}
            const auto warm=device.GetRuntimeResourceDiagnostics();
            for(unsigned i=0;i<12;++i){change(i%2?11:10);auto ticket=r.Submit(packet,false);r.Complete(ticket);}
            const auto reused=device.GetRuntimeResourceDiagnostics();
            r.Check(reused.resource_allocations==warm.resource_allocations&&reused.default_bytes==warm.default_bytes
                &&reused.whole_image_reuses==warm.whole_image_reuses+12,"Changed indexed images did not reuse warmed DEFAULT storage");
            auto final=r.Submit(packet,true);r.Compare(final,wanted(11),"whole-image-reused");
            Hold hold(device,r.watchdog_fired);std::array<RuntimeFrameTicket,3> tickets;
            const auto before_hold=device.GetRuntimeResourceDiagnostics();
            for(unsigned i=0;i<3;++i){change(uint8_t(10+i));tickets[i]=r.Submit(packet,true);}
            const auto held=device.GetRuntimeResourceDiagnostics();
            r.Check(held.whole_images==before_hold.whole_images+2&&held.whole_image_reuses==before_hold.whole_image_reuses+1,
                "Whole-image pool overwrote an in-flight image or missed its one idle image");
            packet.post_composition[0].source.Reset();device.CollectRuntimeResources(false);
            r.Check(device.GetRuntimeResourceDiagnostics().whole_images==held.whole_images,"Held whole-image storage was retired prematurely");
            hold.Release();for(unsigned i=0;i<3;++i)r.Compare(tickets[i],wanted(uint8_t(10+i)),"whole-image-held-"+std::to_string(i));
            change(11);const auto released=device.GetRuntimeResourceDiagnostics();
            auto retry=r.Submit(packet,false);r.Complete(retry);
            const auto after=device.GetRuntimeResourceDiagnostics();
            r.Check(after.resource_allocations==released.resource_allocations&&after.whole_image_reuses==released.whole_image_reuses+1,
                "Retired whole-image pool failed to reuse storage after reader release");
        });
        r.Case("owned-full-frame-images-ui-and-cursor-order",[&] {
            r.Configure(); OwnedRenderAssets catalog(1); OwnedFrameMailbox mailbox;
            const auto publish=[&](uint64_t id,OwnedAssetFormat format,uint32_t width,uint32_t height,const auto& bytes) {
                auto value=catalog.Publish({1,70,id,1,1},{{format,width,height},bytes,size_t(width)*(format==OwnedAssetFormat::RGBA8?4:1)});
                r.Check(bool(value),"Full-frame fixture publication failed");return value.lease;
            };
            std::array<uint8_t,1024> palette{};
            const std::array<std::array<uint8_t,4>,4> colours{{{200,10,20,255},{20,180,30,255},{30,40,190,255},{140,150,20,255}}};
            for(unsigned i=0;i<256;++i)std::copy(colours[i%4].begin(),colours[i%4].end(),palette.begin()+i*4);
            const auto row=publish(1,OwnedAssetFormat::RGBA8,256,1,palette);
            const auto source=publish(2,OwnedAssetFormat::R8,2,2,std::array<uint8_t,4>{0,1,2,3});
            const auto coverage=publish(3,OwnedAssetFormat::R8,2,2,std::array<uint8_t,4>{255,127,128,0});
            const auto hand=publish(4,OwnedAssetFormat::RGBA8,2,1,std::array<uint8_t,8>{2,255,0,0,2,255,0,0});
            OwnedFrameInfo info;info.epoch=info.sequence=1;info.native_width=80;info.native_height=64;
            info.viewport_x=7;info.viewport_y=9;info.viewport_width=64;info.viewport_height=48;info.clear_rgba={1,0,1,1};
            auto builder=mailbox.Begin(info);r.Check(bool(builder),"Full-frame builder unavailable");
            Image expected{80,64,std::vector<uint8_t>(80*64*4)};
            for(size_t i=3;i<expected.rgba.size();i+=4)expected.rgba[i]=255;
            const auto pixel=[&](unsigned x,unsigned y,const std::array<uint8_t,4>& colour){std::copy(colour.begin(),colour.end(),expected.rgba.begin()+(size_t(y)*80+x)*4);};
            const auto image=[&](OwnedImageMode mode,int x,int y,bool masked) {
                OwnedFrameImage q;q.mode=mode;q.phase=mode==OwnedImageMode::Transparent?OwnedFramePhase::ImagePresentOverlay:OwnedFramePhase::ImagePresents;
                q.x=x;q.y=y;q.width=q.height=16;q.source=source;q.palette=row;if(masked)q.coverage=coverage;
                r.Check(builder.AddImage(q),"Full-frame image rejected");
                for(unsigned yy=0;yy<16;++yy)for(unsigned xx=0;xx<16;++xx){const unsigned index=(yy/8)*2+xx/8;
                    if(mode==OwnedImageMode::Transparent&&(masked?(index==1||index==3):index==0))continue;
                    pixel(x+xx,y+yy,colours[index]);}
            };
            image(OwnedImageMode::Opaque,4,4,false);image(OwnedImageMode::Composite,24,4,false);
            const auto quad=[&](OwnedFramePhase phase,int x,int y,int width,int height,const std::array<uint8_t,4>& colour) {
                OwnedFrameQuad q;q.phase=phase;q.mode=OwnedQuadMode::Solid;q.x0=float(x);q.y0=float(y);q.x1=float(x+width);q.y1=float(y+height);
                for(unsigned c=0;c<4;++c)q.colour[c]=float(colour[c])/255;
                r.Check(builder.AddQuad(q),"Full-frame UI quad rejected");
                for(int yy=y;yy<y+height;++yy)for(int xx=x;xx<x+width;++xx)pixel(xx,yy,colour);
            };
            quad(OwnedFramePhase::GameUI,48,4,16,16,colours[0]);
            quad(OwnedFramePhase::FrontOverlay,52,8,12,12,colours[1]);
            quad(OwnedFramePhase::Text,56,12,8,8,colours[2]);
            image(OwnedImageMode::Transparent,4,28,false);image(OwnedImageMode::Transparent,24,28,true);
            OwnedWorldSprite cursor;cursor.phase=OwnedFramePhase::Cursor;cursor.x=50;cursor.y=40;cursor.width=2;cursor.height=1;
            cursor.source_width=2;cursor.source_height=cursor.content_height=1;cursor.source=hand;cursor.colour_row=row;cursor.colour={1,1,1,1};
            r.Check(builder.AddSprite(cursor),"Full-frame hand cursor rejected");pixel(50,40,colours[2]);pixel(51,40,colours[2]);
            quad(OwnedFramePhase::Cursor,50,40,1,1,colours[3]);
            for(unsigned i=0;i<unsigned(OwnedFramePhase::Count);++i)builder.FinishPhase(OwnedFramePhase(i));
            auto packet=builder.Seal();r.Check(packet&&packet->Complete(),"Full-frame fixture did not seal complete");
            OwnedWorldBridge bridge(device);
            const auto submit=[&](const std::string& name) {
                r.Check(bridge.PrepareFrame(packet)==OwnedWorldPrepareStatus::Ready,"Full-frame bridge preparation failed");
                r.Check(bridge.PreparedWorldFragment()==nullptr&&bridge.PreparedFrame()!=nullptr,"Full-frame/world-fragment APIs mixed");
                const auto deadline=Clock::now()+std::chrono::seconds(25);
                for(;;) {
                    auto result=bridge.TrySubmitPreparedFrame(true);r.Bound();
                    if(result.status==RuntimeSubmitStatus::Submitted){++r.result.submitted_frames;r.Compare(result.ticket,expected,name);break;}
                    r.Check(result.status==RuntimeSubmitStatus::WouldBlock&&bridge.Stats().pending,"Full-frame retry consumed pending packet");
                    Require(Clock::now()<deadline,"Full-frame submit remained blocked");device.CollectRuntimeResources();Pump();
                }
                r.Check(!bridge.Stats().pending,"Full-frame success retained pending packet");
            };
            for(unsigned i=0;i<3;++i)submit("full-composition-warm-"+std::to_string(i));
            const auto before=device.GetRuntimeResourceDiagnostics();submit("full-composition-reused");
            const auto after=device.GetRuntimeResourceDiagnostics();
            r.Check(after.publications==before.publications&&after.world_upload_bytes==before.world_upload_bytes,"Unchanged full-frame composition republished or uploaded sources");
            OwnedWorldBridge too_small(device,{1,0x200000000ull});
            r.Check(too_small.PrepareFrame(packet)==OwnedWorldPrepareStatus::InvalidInput&&!too_small.Stats().pending,"Oversized full-frame gather did not fail permanently");
        });
        r.Case("owned-heart-fly-in-sprite-visible-range",[&] {
            r.Configure();device.Resize(96,256);OwnedRenderAssets catalog(1);OwnedFrameMailbox mailbox;
            std::vector<uint8_t> pixels(58*107*4);std::array<uint8_t,1024> palette{};
            for(unsigned i=0;i<256;++i){palette[i*4]=uint8_t(i*3);palette[i*4+1]=uint8_t(i*2);palette[i*4+2]=uint8_t(i);palette[i*4+3]=255;}
            for(unsigned y=0;y<107;++y)for(unsigned x=0;x<58;++x){const size_t at=(y*58+x)*4;pixels[at]=uint8_t((x*3+y*5)%64+1);pixels[at+1]=(x+y)%5?255:0;}
            const auto publish=[&](uint64_t id,uint32_t w,uint32_t h,const auto& bytes) {
                auto result=catalog.Publish({1,72,id,1,1},{{OwnedAssetFormat::RGBA8,w,h},bytes,size_t(w)*4});
                r.Check(bool(result),"Heart sprite fixture publication failed");return result.lease;
            };
            const auto source=publish(1,58,107,pixels),row=publish(2,256,1,palette),offscreen_source=publish(3,58,107,pixels);
            const auto fade=catalog.Publish({1,72,4,1,1},{{OwnedAssetFormat::R8,256,64},std::vector<uint8_t>(256*64),256});
            const auto lightmap=catalog.Publish({1,72,5,1,1},{{OwnedAssetFormat::R16LE,511,511},std::vector<uint8_t>(511*511*2),511*2});
            r.Check(bool(fade)&&bool(lightmap),"Heart sprite globals publication failed");
            OwnedFrameInfo info;info.epoch=1;info.native_width=96;info.native_height=256;
            info.viewport_x=7;info.viewport_y=9;info.viewport_width=80;info.viewport_height=240;info.clear_rgba={16/255.0f,32/255.0f,48/255.0f,1};
            // Three globals plus the visible sprite and its row fit. The unique
            // measured offscreen glow must not acquire a sixth residency slot.
            OwnedWorldBridge bridge(device,{5,0x400000000ull});
            for(bool flip:{false,true}) {
                ++info.sequence;auto builder=mailbox.Begin(info);OwnedWorldSprite sprite;
                r.Check(builder.SetWorldAssets(row,fade.lease,lightmap.lease),"Heart sprite globals rejected");
                sprite.pass=OwnedSpritePass::GlowAdd;sprite.x=-16612;sprite.y=-2705;sprite.width=9448;sprite.height=17431;
                sprite.phase_x=29544;sprite.phase_y=65511;sprite.step_x=10675921;sprite.step_y=10675906;
                sprite.source_width=58;sprite.source_height=sprite.content_height=107;sprite.source=offscreen_source;
                r.Check(builder.AddSprite(sprite),"Measured offscreen heart glow rejected by owned producer");
                sprite.pass=OwnedSpritePass::Normal;sprite.source=source;sprite.colour_row=row;sprite.x=-161;sprite.flip=flip;
                r.Check(builder.AddSprite(sprite),"Partially visible tall heart sprite rejected");
                auto cursor=sprite;cursor.phase=OwnedFramePhase::Cursor;cursor.x=90;cursor.y=2;cursor.width=30;cursor.height=8;
                cursor.phase_x=cursor.phase_y=0;cursor.step_x=cursor.step_y=65536;
                r.Check(builder.AddSprite(cursor),"Native-edge cursor rejected");
                for(unsigned i=0;i<unsigned(OwnedFramePhase::Count);++i)builder.FinishPhase(OwnedFramePhase(i));
                auto packet=builder.Seal();r.Check(packet&&packet->Complete(),"Heart sprite packet incomplete");
                Image expected{96,256,std::vector<uint8_t>(96*256*4)};
                for(size_t at=0;at<expected.rgba.size();at+=4){expected.rgba[at]=16;expected.rgba[at+1]=32;expected.rgba[at+2]=48;expected.rgba[at+3]=255;}
                const auto paint=[&](const OwnedWorldSprite& d,int ox,int oy,int w,int h) {
                    for(int y=0;y<h;++y)for(int x=0;x<w;++x) {
                        const int64_t qx=int64_t(x)-d.x,qy=int64_t(y)-d.y;
                        if(qx<0||qy<0||qx>=d.width||qy>=d.height)continue;
                        int sx=int(std::min<int64_t>(57,std::max<int64_t>((qx+1)*65536-d.phase_x-1,0)/d.step_x));
                        const int sy=int(std::min<int64_t>(106,std::max<int64_t>((qy+1)*65536-d.phase_y-1,0)/d.step_y));
                        if(d.flip)sx=57-sx;
                        const size_t at=(sy*58+sx)*4;if(!pixels[at+1])continue;
                        std::copy_n(palette.begin()+pixels[at]*4,4,expected.rgba.begin()+((y+oy)*96+x+ox)*4);
                    }
                };
                paint(sprite,7,9,80,240);paint(cursor,0,0,96,256);
                r.Check(bridge.PrepareFrame(packet)==OwnedWorldPrepareStatus::Ready,"Heart clipping bridge admission failed");
                const auto* prepared=bridge.PreparedFrame();
                r.Check(prepared&&prepared->sprites.size()==1&&prepared->cursor_sprites.size()==1
                    &&prepared->palette.Valid()&&prepared->fade.Valid()&&prepared->lightmap.Valid(),"World/native viewport culling or globals mismatch");
                const auto deadline=Clock::now()+std::chrono::seconds(25);
                for(;;){auto result=bridge.TrySubmitPreparedFrame(true);r.Bound();
                    if(result.status==RuntimeSubmitStatus::Submitted){++r.result.submitted_frames;r.Compare(result.ticket,expected,flip?"heart-large-visible-flip":"heart-large-visible");break;}
                    r.Check(result.status==RuntimeSubmitStatus::WouldBlock&&bridge.Stats().pending,"Heart sprite retry lost packet");
                    Require(Clock::now()<deadline,"Heart sprite submit remained blocked");device.CollectRuntimeResources();Pump();
                }
            }
            device.Resize(80,64);
        });
        r.Case("owned-landview-zoom-and-palette-replay",[&] {
            r.Configure();OwnedRenderAssets catalog(1);OwnedFrameMailbox mailbox;OwnedFramePaletteReplay replay(catalog);
            const auto publish=[&](uint64_t id,uint64_t revision,OwnedAssetFormat format,uint32_t w,uint32_t h,const auto& bytes) {
                auto value=catalog.Publish({1,71,id,1,revision},{{format,w,h},bytes,size_t(w)*(format==OwnedAssetFormat::RGBA8?4:1)});
                r.Check(bool(value),"Zoom fixture source publication failed");return value.lease;
            };
            std::array<uint8_t,1024> palette{};
            std::array<std::array<uint8_t,4>,4> colours{{{200,10,20,255},{20,180,30,255},{30,40,190,255},{140,150,20,255}}};
            const auto fill_palette=[&] {for(unsigned i=0;i<256;++i)std::copy(colours[i%4].begin(),colours[i%4].end(),palette.begin()+i*4);};
            fill_palette();auto row=publish(1,1,OwnedAssetFormat::RGBA8,256,1,palette);
            std::vector<uint8_t> map{0,1,2,3};auto source=publish(2,1,OwnedAssetFormat::R8,2,2,map);
            std::fill(map.begin(),map.end(),255);map.clear(); // The sealed source must outlive mutable producer storage.
            const auto ignored=publish(3,1,OwnedAssetFormat::R8,2,2,std::array<uint8_t,4>{3,3,3,3});
            const auto coverage=publish(4,1,OwnedAssetFormat::R8,2,2,std::array<uint8_t,4>{255,127,128,0});
            OwnedFrameInfo info;info.epoch=info.sequence=1;info.native_width=80;info.native_height=64;
            info.viewport_width=80;info.viewport_height=64;info.clear_rgba={1,0,1,1};
            auto builder=mailbox.Begin(info);OwnedFrameImage zoom;zoom.width=80;zoom.height=64;
            zoom.source=ignored;zoom.palette=row;zoom.uses_display_palette=true;
            r.Check(builder.AddImage(zoom),"Zoom ignored background rejected");
            zoom.mode=OwnedImageMode::LandviewZoom;zoom.source=source;zoom.zoom={0.25f,-0.125f,30.75f,24.25f,0.125f};
            r.Check(builder.AddImage(zoom),"Zoom mapping rejected");
            OwnedFrameImage overlay;overlay.mode=OwnedImageMode::Transparent;overlay.phase=OwnedFramePhase::ImagePresentOverlay;
            overlay.x=24;overlay.y=28;overlay.width=overlay.height=16;overlay.source=source;overlay.palette=row;
            overlay.coverage=coverage;overlay.uses_display_palette=true;r.Check(builder.AddImage(overlay),"Zoom overlay rejected");
            for(auto phase:{OwnedFramePhase::GameUI,OwnedFramePhase::Text,OwnedFramePhase::Cursor}) {
                OwnedFrameQuad q;q.phase=phase;q.mode=OwnedQuadMode::Solid;q.x0=26;q.y0=30;q.x1=30;q.y1=34;
                q.colour=phase==OwnedFramePhase::Cursor?std::array<float,4>{1,1,1,1}:std::array<float,4>{0,1,0,1};
                r.Check(builder.AddQuad(q),"Zoom later UI rejected");
            }
            for(unsigned i=0;i<unsigned(OwnedFramePhase::Count);++i)builder.FinishPhase(OwnedFramePhase(i));
            auto packet=builder.Seal();r.Check(packet&&packet->Complete(),"Zoom packet incomplete");
            // Exactly source+palette+coverage. Gathering the ignored base would exceed this bound.
            OwnedWorldBridge bridge(device,{3,0x300000000ull});
            const auto submit=[&](const std::string& name) {
                Image expected{80,64,std::vector<uint8_t>(80*64*4)};
                const auto pixel=[&](unsigned x,unsigned y,const std::array<uint8_t,4>& colour){std::copy(colour.begin(),colour.end(),expected.rgba.begin()+(size_t(y)*80+x)*4);};
                const auto mapping=packet->Images()[1].zoom;
                for(unsigned y=0;y<64;++y)for(unsigned x=0;x<80;++x) {
                    // Production GL zoom formula, evaluated independently of the DX12 shader.
                    const float sx=std::floor(mapping[0]+(float(x)+0.5f-mapping[2])*mapping[4]);
                    const float sy=std::floor(mapping[1]+(float(y)+0.5f-mapping[3])*mapping[4]);
                    pixel(x,y,sx<0||sy<0||sx>=2||sy>=2?std::array<uint8_t,4>{0,0,0,255}:colours[unsigned(sy)*2+unsigned(sx)]);
                }
                for(unsigned y=30;y<34;++y)for(unsigned x=26;x<30;++x)pixel(x,y,{0,255,0,255});
                for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x) {
                    const unsigned index=(y/8)*2+x/8;if(index==0||index==2)pixel(24+x,28+y,colours[index]);
                }
                for(unsigned y=30;y<34;++y)for(unsigned x=26;x<30;++x)pixel(x,y,{255,255,255,255});
                r.Check(bridge.PrepareFrame(packet)==OwnedWorldPrepareStatus::Ready,"Zoom bridge admission or base suppression failed");
                const auto* frame=bridge.PreparedFrame();
                r.Check(frame&&frame->post_composition.size()==5&&frame->post_composition.front().kind==RuntimePostKind::ImageZoom,"Zoom composition order changed");
                const auto deadline=Clock::now()+std::chrono::seconds(25);
                for(;;) {
                    auto result=bridge.TrySubmitPreparedFrame(true);r.Bound();
                    if(result.status==RuntimeSubmitStatus::Submitted){++r.result.submitted_frames;r.Compare(result.ticket,expected,name);break;}
                    r.Check(result.status==RuntimeSubmitStatus::WouldBlock&&bridge.Stats().pending,"Zoom retry lost immutable packet");
                    Require(Clock::now()<deadline,"Zoom submission remained blocked");device.CollectRuntimeResources();Pump();
                }
            };
            submit("landview-zoom-fractional");const auto original=packet;
            colours[0]={11,22,33,255};colours[1]={44,55,66,255};colours[2]={77,88,99,255};fill_palette();
            row=publish(1,2,OwnedAssetFormat::RGBA8,256,1,palette);info.sequence=2;builder=mailbox.Begin(info);
            r.Check(builder.ReplayDisplayPalette(*original,row,replay)==OwnedFrameStatus::Ready,"Zoom palette replay failed");packet=builder.Seal();
            r.Check(original->Images()[1].source.Bytes()[0]==0&&packet->Images()[1].zoom==zoom.zoom,"Zoom replay lost historical source or mapping");
            submit("landview-zoom-display-replay");
            packet.reset();info.sequence=3;builder=mailbox.Begin(info);
            r.Check(builder.ReplayDisplayPalette(*original,row,replay)==OwnedFrameStatus::Ready,"Zero-scale zoom replay failed");
            zoom.palette=row;zoom.zoom={0,0,30.75f,24.25f,0};r.Check(builder.AddImage(zoom),"Zero-scale zoom rejected");packet=builder.Seal();
            submit("landview-zoom-zero-scale");
        });
        r.Case("owned-world-bridge-historical-mixed-pressure-and-reuse",[&] {
            RuntimeResourceBudgets budgets;
            budgets.cpu_bytes=1024+16384+522242+2*1024; // Globals plus exactly two tile payloads.
            r.Configure(budgets);
            OwnedRenderAssets catalog(1);
            const auto publish=[&](uint64_t domain,uint64_t id,uint64_t revision,OwnedAssetFormat format,
                uint32_t width,uint32_t height,const auto& bytes) {
                const uint32_t pixel=format==OwnedAssetFormat::RGBA8?4:format==OwnedAssetFormat::R16LE?2:1;
                auto result=catalog.Publish({1,domain,id,1,revision},{{format,width,height},bytes,size_t(width)*pixel});
                r.Check(bool(result),"Bridge fixture owned source publication failed");return result.lease;
            };
            const auto palette=publish(1,1,1,OwnedAssetFormat::RGBA8,256,1,synthetic.world.palette_rgba);
            const auto fade=publish(1,2,1,OwnedAssetFormat::R8,256,64,synthetic.world.fade);
            const auto light_bytes=LightBytes(synthetic);
            const auto lightmap=publish(1,3,1,OwnedAssetFormat::R16LE,511,511,light_bytes);
            std::array<uint8_t,1024> tile_bytes{};tile_bytes.fill(10);
            const auto old_tile=publish(2,1,1,OwnedAssetFormat::R8,32,32,tile_bytes);
            tile_bytes.fill(11);
            const auto new_tile=publish(2,1,2,OwnedAssetFormat::R8,32,32,tile_bytes);
            OwnedFrameLimits limits;limits.terrain=4;limits.sprites=limits.shadows=limits.images=1;limits.quads=1;
            OwnedFrameMailbox mailbox(limits);
            OwnedFrameInfo info;info.epoch=1;info.native_width=80;info.native_height=64;
            info.viewport_x=7;info.viewport_y=9;info.viewport_width=64;info.viewport_height=48;
            info.clear_rgba=synthetic.world.clear_rgba;info.shade_settings=synthetic.world.shade_settings;
            const auto finish=[&](OwnedFrameBuilder& builder) {
                for(auto phase:{OwnedFramePhase::Clear,OwnedFramePhase::BeginWorldCapture,OwnedFramePhase::World,
                    OwnedFramePhase::WorldSprites,OwnedFramePhase::WorldFlatOverlay,OwnedFramePhase::Swipe,
                    OwnedFramePhase::ResolveWorldCapture,OwnedFramePhase::LensPaletteUIExclusion})builder.FinishPhase(phase);
                return builder.Seal();
            };
            const auto packet=[&](const OwnedAssetLease& left,const OwnedAssetLease& right) {
                ++info.sequence;auto builder=mailbox.Begin(info);r.Check(bool(builder),"Bridge fixture mailbox full");
                r.Check(builder.SetWorldAssets(palette,fade,lightmap),"Bridge globals rejected");
                for(unsigned side=0;side<2;++side) {
                    const float x0=side?0.0f:-1.0f,x1=side?1.0f:0.0f;
                    const auto vertex=[](float x,float y){WorldVertex v{};v.x=x;v.y=y;v.camera_z=1;v.shade=1;v.stl_x=v.stl_y=-1;return v;};
                    const auto a=vertex(x0,1),b=vertex(x1,1),c=vertex(x1,-1),d=vertex(x0,-1);
                    const auto tile=side?right:left;
                    r.Check(builder.AddTerrain({{a,b,c},tile})&&builder.AddTerrain({{a,c,d},tile}),"Bridge terrain rejected");
                }
                return finish(builder);
            };
            auto old_packet=packet(old_tile,old_tile),new_packet=packet(new_tile,new_tile),mixed_packet=packet(old_tile,new_tile);
            r.Check(old_packet&&!old_packet->Complete()&&old_tile.Valid(),"World fragment or historical owned lease contract changed");
            const auto expected=[&](unsigned left,unsigned right) {
                Image image{80,64,std::vector<uint8_t>(80*64*4)};
                for(unsigned y=0;y<64;++y)for(unsigned x=0;x<80;++x) {
                    const auto offset=(size_t(y)*80+x)*4;
                    if(x>=7&&x<71&&y>=9&&y<57) {
                        const auto index=x<39?left:right;
                        std::copy_n(synthetic.world.palette_rgba.begin()+index*4,4,image.rgba.begin()+offset);
                    }else for(unsigned channel=0;channel<4;++channel)
                        image.rgba[offset+channel]=uint8_t(std::lround(info.clear_rgba[channel]*255));
                }return image;
            };
            OwnedWorldBridge bridge(device,{5,0x100000000ull});
            // A held unrelated CPU lease forces real partial publication admission.
            auto blocker=Publish(device,RuntimeAssetKind::Indexed8,900001,256,256,std::vector<uint8_t>(65536));
            const auto before=device.GetRuntimeResourceDiagnostics();
            r.Check(bridge.PrepareWorldFragment(old_packet)==OwnedWorldPrepareStatus::WouldBlock,"Bridge did not expose publication pressure");
            const auto partial=device.GetRuntimeResourceDiagnostics();
            r.Check(partial.publications==before.publications+2&&partial.submissions==before.submissions
                &&bridge.Stats().pending&&!bridge.Stats().prepared,"Partial bridge publication was consumed or not retained");
            r.Check(bridge.PrepareWorldFragment(new_packet)==OwnedWorldPrepareStatus::PendingPacket,"Bridge replaced pending producer packet");
            bridge.Abandon();r.Check(!bridge.Stats().pending&&bridge.Stats().selected_slots==0,"Bridge abandon retained selected packet");
            r.Check(bridge.PrepareWorldFragment(old_packet)==OwnedWorldPrepareStatus::WouldBlock
                &&device.GetRuntimeResourceDiagnostics().publications==partial.publications,"Bridge duplicated partial successful publications");
            blocker.Reset();
            r.Check(bridge.PrepareWorldFragment(old_packet)==OwnedWorldPrepareStatus::Ready,"Bridge publication did not recover after releasing unrelated owner");
            const auto original_key=bridge.PreparedWorldFragment()->terrain.front().tile.Key();
            const auto submit=[&](const OwnedFrameLease& owned,const Image& wanted,const char* name) {
                r.Check(bridge.PrepareWorldFragment(owned)==OwnedWorldPrepareStatus::Ready,"Bridge preparation failed");
                const auto deadline=Clock::now()+std::chrono::seconds(25);
                for(;;) {
                    auto result=bridge.TrySubmitPreparedWorldFragment(true);r.Bound();
                    if(result.status==RuntimeSubmitStatus::Submitted) {
                        ++r.result.submitted_frames;r.Compare(result.ticket,wanted,name);break;
                    }
                    r.Check(result.status==RuntimeSubmitStatus::WouldBlock&&bridge.Stats().pending,"Bridge submission consumed blocked packet");
                    Require(Clock::now()<deadline,"Bridge submission remained blocked");Pump();
                }
                r.Check(!bridge.Stats().pending,"Bridge successful submit retained pending packet");
            };
            submit(old_packet,expected(10,10),"bridge-old");
            const auto cached=device.GetRuntimeResourceDiagnostics();
            submit(old_packet,expected(10,10),"bridge-repeated");
            const auto repeated=device.GetRuntimeResourceDiagnostics();
            r.Check(repeated.publications==cached.publications&&repeated.world_upload_bytes==cached.world_upload_bytes,
                "Unchanged owned packet repeated publication or upload");
            submit(new_packet,expected(11,11),"bridge-new");
            r.Check(bridge.PrepareWorldFragment(old_packet)==OwnedWorldPrepareStatus::Ready,"Historical owned packet did not restore");
            const auto restored_key=bridge.PreparedWorldFragment()->terrain.front().tile.Key();
            r.Check(restored_key.logical_id==original_key.logical_id&&restored_key.generation==original_key.generation
                &&restored_key.revision>original_key.revision&&old_tile.Key().content_revision==1,"Historical content key was mutated instead of advancing residency");
            submit(old_packet,expected(10,10),"bridge-restored");
            r.Check(bridge.PrepareWorldFragment(mixed_packet)==OwnedWorldPrepareStatus::Ready,"Mixed historical/current lineage rejected");
            const auto* mixed=bridge.PreparedWorldFragment();
            r.Check(mixed->terrain[0].tile.Valid()&&mixed->terrain[2].tile.Valid()
                &&mixed->terrain[0].tile.Key().logical_id!=mixed->terrain[2].tile.Key().logical_id,"Mixed lineage versions invalidated one another");
            submit(mixed_packet,expected(10,11),"bridge-mixed");
            // All five slots have been used; changing the source mix must recycle
            // both old Tile slots into Indexed8 and ColourRowRGBA8 slots.
            new_packet.reset();mixed_packet.reset();++info.sequence;auto builder=mailbox.Begin(info);
            r.Check(bool(builder)&&builder.SetWorldAssets(palette,fade,lightmap),"Bridge overlay fixture failed");
            OwnedFrameQuad quad;quad.phase=OwnedFramePhase::WorldFlatOverlay;quad.mode=OwnedQuadMode::Sprite;
            quad.x0=7;quad.y0=9;quad.x1=71;quad.y1=57;quad.u1=quad.v1=1;quad.source=old_tile;quad.colour_row=palette;
            r.Check(builder.AddQuad(quad),"Bridge overlay quad rejected");auto overlay_packet=finish(builder);
            submit(overlay_packet,expected(10,10),"bridge-recycled-kinds");
            r.Check(bridge.Stats().occupied_slots==5&&old_tile.Valid()&&old_tile.Bytes().front()==10,
                "Bridge slot recycling exceeded cap or mutated owned history");
            bridge.Abandon();bridge.TrimUnselected();
            // Distinct animation identities must remain cached while unused
            // mapping slots exist, rather than repeatedly replacing one slot.
            overlay_packet.reset();r.Configure();
            tile_bytes.fill(11);const auto alternate_tile=publish(2,2,1,OwnedAssetFormat::R8,32,32,tile_bytes);
            const auto alternate_packet=packet(alternate_tile,alternate_tile);
            OwnedWorldBridge animation_cache(device,{8,0x300000000ull});
            const auto animate=[&](const OwnedFrameLease& owned,unsigned index,const std::string& name) {
                r.Check(animation_cache.PrepareWorldFragment(owned)==OwnedWorldPrepareStatus::Ready,"Animation cache prepare failed");
                auto ticket=r.Submit(*animation_cache.PreparedWorldFragment(),true);animation_cache.Abandon();
                r.Compare(ticket,expected(index,index),name);
            };
            animate(old_packet,10,"animation-cache-a");animate(alternate_packet,11,"animation-cache-b");
            const auto warmed=device.GetRuntimeResourceDiagnostics();
            for(unsigned i=0;i<4;++i)animate(i%2?alternate_packet:old_packet,i%2?11:10,"animation-cache-return-"+std::to_string(i));
            const auto reused=device.GetRuntimeResourceDiagnostics();
            r.Check(animation_cache.Stats().occupied_slots==5&&reused.publications==warmed.publications
                &&reused.world_upload_bytes==warmed.world_upload_bytes,"Alternating animation identities evicted despite free cache slots");
        });
        r.Case("retained-terrain-visible-edits-held-versions-growth-rollback-prefix",[&]{
            auto base=synthetic;base.world.shadows.clear();base.draws.clear();base.sources.clear();base.rows.clear();
            auto moved=base;moved.world.terrain[0].vertices[1].position.x=0.0f;
            auto third=base;third.world.terrain[0].vertices[1].position.x=-0.5f;
            auto short_frame=base;short_frame.world.terrain.resize(1);
            const auto base_image=r.Reference(base,"terrain-base"),moved_image=r.Reference(moved,"terrain-moved"),
                third_image=r.Reference(third,"terrain-third"),short_image=r.Reference(short_frame,"terrain-short");
            r.Check(Differences(base_image,moved_image)>0&&Differences(moved_image,third_image)>0,"Terrain edit fixture is not visibly distinct");
            r.Configure();auto packet=Packet(device,base);
            const auto record=[&](const char* name,const RuntimeResourceDiagnostics& before,const RuntimeResourceDiagnostics& after){
                std::ostringstream o;o<<"{\"name\":"<<Quote(name)<<",\"before\":";Stats(o,before);o<<",\"after\":";Stats(o,after);o<<'}';r.terrain_measurements.push_back(o.str());};
            const auto set=[&](const Fixture& f){for(size_t i=0;i<f.world.terrain.size();++i)packet.terrain[i].vertices=f.world.terrain[i].vertices;};
            for(unsigned i=0;i<6;++i){auto t=r.Submit(packet,false);r.Complete(t);}
            auto before=device.GetRuntimeResourceDiagnostics();set(moved);
            for(unsigned i=0;i<3;++i){auto t=r.Submit(packet,true);r.Compare(t,moved_image,"terrain-one-edit-"+std::to_string(i));}
            auto after=device.GetRuntimeResourceDiagnostics();
            r.Check(after.terrain_upload_bytes==before.terrain_upload_bytes+360&&after.terrain_dirty_ranges==before.terrain_dirty_ranges+3
                &&after.terrain_reused_bytes==before.terrain_reused_bytes+360,"One visibly changed triangle did not copy exactly120 bytes per slot");record("three-slot-one-edit",before,after);
            before=after;{Hold hold(device,r.watchdog_fired);std::array<RuntimeFrameTicket,3> tickets;
                set(base);tickets[0]=r.Submit(packet,true);set(moved);tickets[1]=r.Submit(packet,true);set(third);tickets[2]=r.Submit(packet,true);
                for(const auto& t:tickets)r.Check(!device.IsRuntimeFrameComplete(t),"Terrain held geometry completed prematurely");
                const auto held=device.GetRuntimeResourceDiagnostics();auto denied=device.TrySubmitRuntimeWorldFrame(packet);
                r.Check(denied.status==RuntimeSubmitStatus::WouldBlock&&denied.pressure==RuntimePressureReason::FrameSlot,"Held geometry did not retain all three slots");
                device.CollectRuntimeResources(true);const auto retained=device.GetRuntimeResourceDiagnostics();
                r.Check(retained.terrain_cpu_bytes==held.terrain_cpu_bytes&&retained.terrain_default_bytes==held.terrain_default_bytes,"Collection reclaimed pending terrain owners");
                hold.Release();r.Compare(tickets[0],base_image,"terrain-held-base");r.Compare(tickets[1],moved_image,"terrain-held-moved");r.Compare(tickets[2],third_image,"terrain-held-third");
            }record("held-three-versions",before,device.GetRuntimeResourceDiagnostics());
            set(base);for(unsigned i=0;i<6;++i){auto t=r.Submit(packet,false);r.Complete(t);}
            const auto pair=packet.terrain;packet.terrain.clear();for(unsigned i=0;i<300;++i)packet.terrain.insert(packet.terrain.end(),pair.begin(),pair.end());
            before=device.GetRuntimeResourceDiagnostics();
            {Hold hold(device,r.watchdog_fired);device.SetRuntimeFailureInjection(RuntimeFailurePoint::BeforeExecute);
                r.Reject<std::runtime_error>([&]{device.TrySubmitRuntimeWorldFrame(packet,true);},"Terrain growth rollback injection not observed");
                const auto rolled=device.GetRuntimeResourceDiagnostics();
                r.Check(rolled.terrain_cpu_bytes==before.terrain_cpu_bytes&&rolled.terrain_default_bytes==before.terrain_default_bytes
                    &&rolled.terrain_capacity_bytes==before.terrain_capacity_bytes&&rolled.default_bytes==before.default_bytes
                    &&rolled.terrain_upload_bytes==before.terrain_upload_bytes&&rolled.terrain_compared_bytes==before.terrain_compared_bytes
                    &&rolled.terrain_dirty_ranges==before.terrain_dirty_ranges&&rolled.submissions==before.submissions,"Terrain rollback changed ownership or committed baseline counters");
                r.Check(rolled.terrain_cpu_high_water>=before.terrain_cpu_bytes+72000,"Terrain rollback omitted old+new CPU shadow peak");record("growth-rollback",before,rolled);
                auto retry=r.Submit(packet,true);const auto submitted=device.GetRuntimeResourceDiagnostics();
                r.Check(submitted.terrain_upload_bytes==before.terrain_upload_bytes+72000,"Growth retry did not upload complete new terrain capacity prefix");
                hold.Release();r.Compare(retry,base_image,"terrain-growth-retry");
            }
            for(unsigned i=0;i<6;++i){auto t=r.Submit(packet,false);r.Complete(t);}
            packet.terrain.resize(1);before=device.GetRuntimeResourceDiagnostics();
            for(unsigned i=0;i<3;++i){auto t=r.Submit(packet,true);r.Compare(t,short_image,"terrain-shrink-"+std::to_string(i));}
            after=device.GetRuntimeResourceDiagnostics();r.Check(after.terrain_upload_bytes==before.terrain_upload_bytes,"Unchanged terrain prefix uploaded during shrink");
            packet.terrain=pair;before=after;
            for(unsigned i=0;i<3;++i){auto t=r.Submit(packet,true);r.Compare(t,base_image,"terrain-regrow-"+std::to_string(i));}
            after=device.GetRuntimeResourceDiagnostics();r.Check(after.terrain_upload_bytes==before.terrain_upload_bytes+360,"Regrow reused bytes outside each slot's valid prefix");record("shrink-regrow",before,after);
            r.Check(after.global_idle_calls==before.global_idle_calls&&after.fence_waits==before.fence_waits,"Terrain retention introduced a runtime wait");
        });
        r.Case("terrain-cpu-budget-exact-capacity-epoch-trim",[&]{
            RuntimeResourceBudgets budget;budget.geometry_cpu_bytes=239;r.Configure(budget);auto packet=Packet(device,synthetic);
            const auto before=device.GetRuntimeResourceDiagnostics();const auto allocation=device.GetDiagnostics().allocation_count;
            auto denied=device.TrySubmitRuntimeWorldFrame(packet);
            r.Check(denied.status==RuntimeSubmitStatus::WouldBlock&&denied.pressure==RuntimePressureReason::GeometryCpuBytes
                &&packet.palette.Valid()&&device.GetDiagnostics().allocation_count==allocation,"CPU shadow denial allocated GPU owners or consumed packet");
            r.Check(device.GetRuntimeResourceDiagnostics().terrain_cpu_bytes==0,"Denied terrain retained CPU shadow");
            budget.geometry_cpu_bytes=720;r.Configure(budget);packet=Packet(device,synthetic);
            for(unsigned i=0;i<6;++i){auto t=r.Submit(packet,false);r.Complete(t);}
            const auto warmed=device.GetRuntimeResourceDiagnostics();r.Check(warmed.terrain_cpu_bytes==720&&warmed.terrain_capacity_bytes==720,"Tight CPU cap did not admit three exact240-byte shadows");
            device.SetRuntimeEpoch(2);packet=Packet(device,synthetic,2);
            for(unsigned i=0;i<3;++i){auto t=r.Submit(packet,true);r.Compare(t,analytic,"terrain-epoch-"+std::to_string(i));}
            const auto epoch=device.GetRuntimeResourceDiagnostics();r.Check(epoch.terrain_upload_bytes==warmed.terrain_upload_bytes+720,"Epoch reused previous scene geometry baseline");
            packet={};device.CollectRuntimeResources(true);const auto trimmed=device.GetRuntimeResourceDiagnostics();
            r.Check(trimmed.terrain_cpu_bytes==0&&trimmed.terrain_default_bytes==0&&trimmed.terrain_capacity_bytes==0,"Completed terrain trim retained CPU/DEFAULT owners");
            std::ostringstream o;o<<"{\"name\":\"cpu-capacity-epoch-trim\",\"before\":";Stats(o,before);o<<",\"warmed\":";Stats(o,warmed);o<<",\"epoch\":";Stats(o,epoch);o<<",\"trimmed\":";Stats(o,trimmed);o<<'}';r.terrain_measurements.push_back(o.str());
        });
        r.Case("three-held-world-versions-epoch-drop-collect-release-resume",[&]{r.Configure();auto f=synthetic;auto packet=Packet(device,f);Hold hold(device,r.watchdog_fired);std::array<Image,3> expected{Analytic(f),{},{} };std::array<RuntimeFrameTicket,3> tickets;tickets[0]=r.Submit(packet,true);f.world.tiles[0].fill(11);auto tile=Publish(device,RuntimeAssetKind::TileR8,100,32,32,f.world.tiles[0],1,2);for(auto& t:packet.terrain)t.tile=tile;tile.Reset();expected[1]=Analytic(f);tickets[1]=r.Submit(packet,true);f.sources[0].rgba[0]=22;SourceRevision(device,packet,f,0,1,2);expected[2]=Analytic(f);tickets[2]=r.Submit(packet,true);for(const auto& t:tickets)r.Check(!device.IsRuntimeFrameComplete(t),"Held world reader completed before release");const auto held=device.GetRuntimeResourceDiagnostics();r.Check(held.reader_hold_slots==3&&held.reader_hold_active,"World did not occupy3 actual held slots");auto denied=device.TrySubmitRuntimeWorldFrame(packet,true);r.Check(denied.status==RuntimeSubmitStatus::WouldBlock&&!denied.ticket.Valid()&&packet.palette.Valid(),"Fourth held world reader did not return non-consuming pressure");device.SetRuntimeEpoch(2);r.Reject([&]{device.TrySubmitRuntimeWorldFrame(packet);},"Old world epoch admitted");auto fresh=Packet(device,synthetic,2);packet={};fresh={};device.CollectRuntimeResources(true);const auto retained=device.GetRuntimeResourceDiagnostics();r.Check(retained.default_bytes==held.default_bytes&&retained.upload_bytes==held.upload_bytes&&retained.readback_bytes==held.readback_bytes&&retained.reader_hold_slots==3,"Dropping world producer leases reclaimed pending reader owners");hold.Release();for(size_t i=0;i<3;++i)r.Compare(tickets[i],expected[i],"held-"+std::to_string(i));r.controlled=true;device.CollectRuntimeResources(true);const auto before_resume=device.GetRuntimeResourceDiagnostics();auto resume=Packet(device,synthetic,2,2);auto t=r.Submit(resume,true);r.Compare(t,analytic,"held-resumed");const auto resumed=device.GetRuntimeResourceDiagnostics();r.Check(resumed.presentations==before_resume.presentations+1&&resumed.global_idle_calls==before_resume.global_idle_calls&&resumed.fence_waits==before_resume.fence_waits&&!resumed.reader_hold_retained,"World did not resume a normal presentation without drain/wait");});

        r.Case("missing-tile-native-checker-analytic",[&]{auto f=synthetic;for(auto& t:f.world.terrain)t.tile=MissingEnhancedTile;Image crop{64,48,std::vector<uint8_t>(64*48*4)};for(uint32_t y=0;y<48;++y)for(uint32_t x=0;x<64;++x){const bool lit=((int((float(x)+7.5f)/8)+int((64.0f-9.0f-float(y)-0.5f)/8))&1)==0;const auto at=(size_t(y)*64+x)*4;crop.rgba[at]=crop.rgba[at+2]=lit?255:0;crop.rgba[at+3]=255;}auto wanted=Native(f,crop);f.draws.clear();f.sprite_sources.clear();f.sprite_rows.clear();f.world.shadows.clear();f.shadow_sources.clear();const auto ref=r.Reference(f,"missing-checker",&wanted);r.Configure();auto packet=Packet(device,f);auto t=r.Submit(packet,true);r.Compare(t,ref,"missing-checker");});
        r.Case("active-fog-native-origin-qualified-core",[&]{auto f=synthetic;f.world.native_width=f.world.native_height=512;f.world.viewport_x=301;f.world.viewport_y=407;f.world.shade_settings[5]=2;f.world.shade_settings[7]=1;f.world.time=0.875f;for(auto& t:f.world.terrain)for(auto& v:t.vertices)v.shade=0.4f;const auto expected=r.Reference(f,"active-fog");r.Configure();device.Resize(512,512);auto packet=Packet(device,f);auto t=r.Submit(packet,true);r.Compare(t,expected,"active-fog");device.Resize(80,64);});
        for(unsigned profile=0;profile<3;++profile)r.Case(profile==0?"world-upload-budget-pressure":profile==1?"world-default-page-budget-pressure":"world-descriptor-budget-pressure",[&]{RuntimeResourceBudgets b;if(profile==0)b.upload_bytes=512;if(profile==1)b.default_bytes=1024*1024;if(profile==2)b.descriptors=3;r.Configure(b);auto packet=Packet(device,synthetic);const auto before=device.GetDiagnostics();auto denied=device.TrySubmitRuntimeWorldFrame(packet,true);r.Check(denied.status==RuntimeSubmitStatus::WouldBlock&&!denied.ticket.Valid()&&packet.palette.Valid()&&packet.sprites[0].source.Valid(),"World bounded pressure consumed assets or submitted partial work");r.Check(device.GetDiagnostics().allocation_count==before.allocation_count,"Denied world reservation allocated GPU resources");r.Bound();});
        r.Case("asset-shape-kind-viewport-no-allocation",[&]{r.Configure();auto packet=Packet(device,synthetic);const auto before=device.GetDiagnostics();auto invalid=packet;invalid.viewport_x=-1;r.Reject([&]{device.TrySubmitRuntimeWorldFrame(invalid);},"Negative world viewport accepted");invalid=packet;invalid.sprites[0].source=packet.palette;r.Reject([&]{device.TrySubmitRuntimeWorldFrame(invalid);},"Wrong source kind accepted");invalid=packet;invalid.sprites[0].draw.source_width=257;r.Reject([&]{device.TrySubmitRuntimeWorldFrame(invalid);},"Unowned sprite extent accepted");invalid=packet;invalid.sprites[0].draw.lookup_row=1;r.Reject([&]{device.TrySubmitRuntimeWorldFrame(invalid);},"Runtime lease-local CLUT row other than0 accepted");r.Reject([&]{Publish(device,RuntimeAssetKind::TileR8,999,31,32,std::vector<uint8_t>(31*32));},"Malformed tile dimension accepted");auto nonbinary=synthetic.sources[0].rgba;nonbinary[1]=1;r.Reject([&]{Publish(device,RuntimeAssetKind::SpriteRGBA8,998,4,4,nonbinary);},"Nonbinary runtime coverage accepted");RuntimeResourceBudgets too_many;too_many.descriptors=131073;r.Reject([&]{device.ClearRuntimeResources();device.ConfigureRuntimeResources(too_many);},"Runtime descriptor cap beyond131072 accepted");r.Check(device.GetDiagnostics().allocation_count==before.allocation_count,"Rejected world input allocated resources");});
        r.Case("world-before-execute-held-rollback-three-slot-retry",[&]{r.Configure();auto packet=Packet(device,synthetic);Hold hold(device,r.watchdog_fired);const auto before=device.GetRuntimeResourceDiagnostics();device.SetRuntimeFailureInjection(RuntimeFailurePoint::BeforeExecute);r.Reject<std::runtime_error>([&]{device.TrySubmitRuntimeWorldFrame(packet,true);},"World pre-execute injection not observed");const auto rollback=device.GetRuntimeResourceDiagnostics();r.Check(rollback.reader_hold_slots==0&&rollback.world_pages==before.world_pages&&rollback.default_bytes==before.default_bytes&&rollback.upload_bytes==before.upload_bytes&&rollback.readback_bytes==before.readback_bytes,"World rollback consumed slot/page or leaked allocation");std::array<RuntimeFrameTicket,3> tickets;for(auto& t:tickets)t=r.Submit(packet,true);for(const auto& t:tickets)r.Check(!device.IsRuntimeFrameComplete(t),"Rollback retry did not retain3 pending world readers");hold.Release();for(size_t i=0;i<tickets.size();++i)r.Compare(tickets[i],analytic,"rollback-"+std::to_string(i));});
        r.Case("large-sprite-revisions-page-capacity-bounded-reclamation",[&]{RuntimeResourceBudgets b;b.default_bytes=16*1024*1024;b.upload_bytes=2*1024*1024;r.Configure(b);auto f=synthetic;const auto original=f.sources[0];auto& source=f.sources[0];source.width=source.height=256;source.rgba.assign(256*256*4,0);for(uint32_t y=0;y<4;++y)std::copy_n(original.rgba.data()+size_t(y)*4*4,16,source.rgba.data()+size_t(y)*256*4);auto packet=Packet(device,f);auto first=r.Submit(packet,true);r.Compare(first,Analytic(f),"churn-first");const auto before=device.GetRuntimeResourceDiagnostics();for(uint64_t revision=2;revision<=81;++revision){f.sources[0].rgba[0]=uint8_t(revision);SourceRevision(device,packet,f,0,revision);auto t=r.Submit(packet,revision%16==0||revision==81);if(t.HasReadback())r.Compare(t,Analytic(f),"churn-"+std::to_string(revision));else r.Complete(t);device.CollectRuntimeResources(true);r.Bound();}const auto after=device.GetRuntimeResourceDiagnostics();r.Check(after.world_upload_bytes==before.world_upload_bytes+80*256*256*4,"Page churn retransmitted unrelated asset bytes");r.Check(after.global_idle_calls==before.global_idle_calls&&after.fence_waits==before.fence_waits,"Page reclamation inserted routine Idle/wait");});
        r.Case("fresh-device-replacement-rollback-highwater-overlap",[&]{
            HWND window=CreateWindowExW(0,L"STATIC",L"Runtime world isolated peak accounting",WS_POPUP,0,0,1024,1024,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);Require(window!=nullptr,"Cannot create isolated peak device");
            try{Device fresh;fresh.Initialize(window,80,64,options);Runner local(fresh,output);auto packet=Packet(fresh,synthetic);
                // The diagnostic hold explicitly chooses slot0. No average of unlike slot pools is used.
                {Hold warm(fresh,local.watchdog_fired);auto t=local.Submit(packet,false);warm.Release();local.Complete(t);}
                fresh.Resize(1024,1024);auto expanded=synthetic;expanded.world.native_width=expanded.world.native_height=1024;
                packet.native_width=packet.native_height=1024;const auto pair=packet.terrain;
                const auto before=fresh.GetRuntimeResourceDiagnostics();
                while(uint64_t(packet.terrain.size())*120<=before.staging_capacity_bytes)
                    packet.terrain.insert(packet.terrain.end(),pair.begin(),pair.end());
                const uint64_t terrain_bytes=uint64_t(packet.terrain.size())*120;
                const auto record=[&](const char* phase,const RuntimeResourceDiagnostics& value){std::ostringstream o;
                    o<<"{\"name\":\"fresh-device-replacement-rollback\",\"phase\":"<<Quote(phase)<<",\"terrainLogicalBytes\":"<<terrain_bytes<<",\"stats\":";Stats(o,value);o<<'}';r.isolated_devices.push_back(o.str());};
                record("before",before);
                local.Check(before.staging_pools==1&&before.terrain_capacity_bytes>0,"Isolated peak fixture did not own exactly one known slot");
                Hold hold(fresh,local.watchdog_fired);fresh.SetRuntimeFailureInjection(RuntimeFailurePoint::BeforeExecute);
                local.Reject<std::runtime_error>([&]{fresh.TrySubmitRuntimeWorldFrame(packet,true);},"Isolated replacement rollback injection not observed");
                const auto rollback=fresh.GetRuntimeResourceDiagnostics();record("rollback",rollback);
                local.Check(rollback.submissions==before.submissions&&rollback.default_bytes==before.default_bytes&&rollback.upload_bytes==before.upload_bytes
                    &&rollback.readback_bytes==before.readback_bytes&&rollback.terrain_cpu_bytes==before.terrain_cpu_bytes,"Replacement rollback changed admitted ownership");
                local.Check(rollback.staging_allocations==before.staging_allocations+1&&rollback.staging_maps==before.staging_maps+1,
                    "Measured terrain growth did not create exactly one replacement staging allocation");
                auto retry=local.Submit(packet,true);const auto accepted=fresh.GetRuntimeResourceDiagnostics();record("retry-submitted",accepted);
                hold.Release();local.Compare(retry,Analytic(expanded),"highwater-retry");
                const auto after_retry=fresh.GetRuntimeResourceDiagnostics();record("retry-completed",after_retry);
                // Only slot0 has ever owned these buffers. These are actual D3D12MA allocation sizes.
                local.Check(after_retry.staging_pools==1&&after_retry.staging_capacity_bytes>before.staging_capacity_bytes
                    &&after_retry.staging_allocation_bytes>=terrain_bytes,"Retry did not replace the measured sole staging pool");
                local.Check(rollback.upload_high_water>=before.upload_bytes+after_retry.staging_allocation_bytes,
                    "Old UPLOAD owners plus actual replacement staging allocation omitted from rollback peak");
                local.Check(after_retry.terrain_default_bytes>=terrain_bytes
                    &&rollback.default_high_water>=before.default_bytes+2ull*1024*1024*4+after_retry.terrain_default_bytes,
                    "Old DEFAULT owners plus new surfaces and actual terrain allocation omitted from rollback peak");
                local.Check(rollback.readback_high_water>=1024ull*1024*4,"Rolled-back selected readback peak omitted");
                r.result.checks+=local.result.checks;r.result.rgba_byte_checks+=local.result.rgba_byte_checks;
                r.comparisons.insert(r.comparisons.end(),local.comparisons.begin(),local.comparisons.end());
                fresh.ClearRuntimeResources();local.Validation("isolated-cleanup");
            }catch(...){DestroyWindow(window);throw;}DestroyWindow(window);
        });
        r.Case("waitable-presentation-permit-lifecycle",[&]{
            HWND window=CreateWindowExW(0,L"STATIC",L"Waitable presentation lifecycle",WS_POPUP,0,0,80,64,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
            Require(window!=nullptr,"Cannot create waitable presentation window");
            try {
                Options paced=options;paced.waitable_presentation=true;paced.presentation_diagnostics=true;paced.vsync=true;
                Device fresh;fresh.Initialize(window,80,64,paced);Runner local(fresh,output);auto packet=Packet(fresh,synthetic);
                const auto deadline=Clock::now()+std::chrono::seconds(3);
                while(!fresh.TryAcquireRuntimePresentation()){Require(Clock::now()<deadline,"Initial presentation permit did not become ready");Pump();}
                const auto acquired=fresh.GetPresentationDiagnostics();
                local.Check(acquired.waitable&&acquired.permit_held&&acquired.admission_grants==1,"Initial waitable permit not retained");
                for(unsigned i=0;i<32;++i)local.Check(fresh.TryAcquireRuntimePresentation(),"Repeated producer entry lost its permit");
                const auto repeated=fresh.GetPresentationDiagnostics();
                local.Check(repeated.admission_polls==acquired.admission_polls&&repeated.admission_grants==1,"Repeated producer entry consumed more readiness events");
                fresh.Resize(0,0);local.Check(!fresh.TryAcquireRuntimePresentation(),"Suspended surface admitted production");
                local.Check(fresh.GetPresentationDiagnostics().permit_held,"Suspension discarded unspent presentation readiness");
                fresh.Resize(96,72);fresh.SetRuntimePresentation(true,0,0,96,72);
                local.Check(fresh.TryAcquireRuntimePresentation()&&fresh.GetPresentationDiagnostics().admission_grants==1,"Resize consumed another event for an unspent permit");
                fresh.SetRuntimeEpoch(2);packet=Packet(fresh,synthetic,2);
                local.Check(fresh.TryAcquireRuntimePresentation()&&fresh.GetPresentationDiagnostics().admission_grants==1,"Epoch cancellation discarded unspent permit");
                fresh.SetRuntimeFailureInjection(RuntimeFailurePoint::BeforeExecute);
                local.Reject<std::runtime_error>([&]{fresh.TrySubmitRuntimeWorldFrame(packet,true);},"Waitable rollback injection absent");
                local.Check(fresh.GetPresentationDiagnostics().permit_held,"Pre-execute rollback consumed presentation permit");
                auto ticket=local.Submit(packet,true);
                local.Check(!fresh.GetPresentationDiagnostics().permit_held,"Actual Present failed to consume its permit");
                local.Compare(ticket,Analytic(synthetic),"waitable-resize-rollback");
                fresh.SetRuntimePresentation(false,0,0,96,72);
                const auto before_off=fresh.GetPresentationDiagnostics();
                local.Check(fresh.TryAcquireRuntimePresentation(),"VSync off did not bypass presentation admission");
                local.Check(fresh.GetPresentationDiagnostics().admission_polls==before_off.admission_polls,"VSync off polled the presentation event");
                ticket=local.Submit(packet,true);local.Compare(ticket,Analytic(synthetic),"waitable-vsync-off");
                fresh.ClearRuntimeResources();local.Validation("waitable-cleanup");
                r.result.checks+=local.result.checks;r.result.rgba_byte_checks+=local.result.rgba_byte_checks;r.result.submitted_frames+=local.result.submitted_frames;
                r.comparisons.insert(r.comparisons.end(),local.comparisons.begin(),local.comparisons.end());
            } catch(...) {DestroyWindow(window);throw;}
            DestroyWindow(window);
        });
        r.Case("cpu-pressure-evicts-one-keeps-warm-frame-resources",[&]{
            RuntimeResourceBudgets limits;limits.cpu_bytes=541842+4*65536;
            r.Configure(limits);device.Resize(80,64);auto packet=Packet(device,synthetic);
            for(unsigned i=0;i<6;++i){auto t=r.Submit(packet,true);r.Compare(t,Analytic(synthetic),"pressure-warm");}
            const std::vector<uint8_t> rg(65536,0);
            for(unsigned i=0;i<4;++i)Publish(device,RuntimeAssetKind::SpriteRG8,700000+i,256,128,rg);
            const auto before=device.GetRuntimeResourceDiagnostics();
            auto held=Publish(device,RuntimeAssetKind::SpriteRG8,700004,256,128,rg);
            const auto after=device.GetRuntimeResourceDiagnostics();
            r.Check(after.evictions==before.evictions+1 && after.asset_count==before.asset_count,"CPU pressure flushed more than one required entry");
            r.Check(after.default_bytes==before.default_bytes && after.upload_bytes==before.upload_bytes
                && after.terrain_default_bytes==before.terrain_default_bytes,"CPU pressure destroyed warm GPU resources");
            auto t=r.Submit(packet,false);
            const auto rendered=device.GetRuntimeResourceDiagnostics();
            r.Check(rendered.resource_allocations==before.resource_allocations
                && rendered.world_upload_bytes==before.world_upload_bytes,"CPU pressure forced unrelated GPU reallocation/upload");
            t.Reset();auto pixels=r.Submit(packet,true);r.Compare(pixels,Analytic(synthetic),"pressure-preserved-pixels");
            r.Check(held.Valid(),"CPU pressure damaged external sprite lease");r.Bound();
        });
        r.Case("foreign-world-lease-owner-rejected",[&]{r.Configure();auto packet=Packet(device,synthetic);HWND window=CreateWindowExW(0,L"STATIC",L"Foreign runtime world owner",WS_POPUP,0,0,80,64,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);Require(window!=nullptr,"Cannot create foreign owner window");try{Device foreign;foreign.Initialize(window,80,64,options);auto lease=Publish(foreign,RuntimeAssetKind::PaletteRGBA8,1,256,1,synthetic.world.palette_rgba);auto invalid=packet;invalid.palette=lease;r.Reject([&]{device.TrySubmitRuntimeWorldFrame(invalid);},"World accepted foreign Device palette");foreign.ClearRuntimeResources();}catch(...){DestroyWindow(window);throw;}DestroyWindow(window);});
        r.Case("world-after-execute-quarantine-fail-closed",[&]{r.Configure();auto packet=Packet(device,synthetic);device.SetRuntimeFailureInjection(RuntimeFailurePoint::AfterExecute);r.Reject<std::runtime_error>([&]{device.TrySubmitRuntimeWorldFrame(packet,true);},"World post-execute injection not observed");const auto failed=device.GetRuntimeResourceDiagnostics();r.Check(failed.failed&&failed.quarantined&&failed.default_bytes>0&&failed.upload_bytes>0,"Failed world execution did not retain submitted owners");r.Reject<std::logic_error>([&]{device.TrySubmitRuntimeWorldFrame(packet);},"World accepted frame after quarantine");});
        const auto before=device.GetRuntimeResourceDiagnostics();device.ClearRuntimeResources();const auto after=device.GetRuntimeResourceDiagnostics();r.Check(after.cpu_bytes==0&&after.default_bytes==0&&after.upload_bytes==0&&after.readback_bytes==0&&after.world_pages==0&&after.descriptors==0&&!after.failed&&!after.quarantined,"World cleanup leaked owners");r.Validation("cleanup");r.Write(capture,true,"",before);return std::move(r.result);
    }catch(const std::exception& e){const auto before=device.GetRuntimeResourceDiagnostics();try{device.ClearRuntimeResources();}catch(...){}r.Write(capture,false,e.what(),before);throw;}
}
}
