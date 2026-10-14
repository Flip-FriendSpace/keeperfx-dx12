// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#include "RuntimeStagingTests.h"
#include "EnhancedSpriteTestImages.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <sstream>
#include <thread>
#include <type_traits>

namespace kfx::render::dx12 {
namespace {
using namespace enhanced_test;
using Clock = std::chrono::steady_clock;
constexpr uint64_t KiB = 1024;
using Colour = std::array<uint8_t,4>;
Colour Ink(uint8_t n, uint64_t revision = 0) { return {uint8_t(n*13+revision),uint8_t(n*29+revision*3),uint8_t(n*53+revision*7),255}; }
void Pump(){MSG m{};while(PeekMessageW(&m,nullptr,0,0,PM_REMOVE)){Require(m.message!=WM_QUIT,"Staging window closed");TranslateMessage(&m);DispatchMessageW(&m);}std::this_thread::sleep_for(std::chrono::milliseconds(1));}
RuntimeAssetLease Publish(Device& d,RuntimeAssetKind kind,uint64_t id,uint64_t revision,uint32_t w,uint32_t h,const std::vector<uint8_t>& bytes,uint64_t epoch=1)
{return d.PublishRuntimeAsset({epoch,id,1,revision,kind},{w,h},std::make_shared<const std::vector<uint8_t>>(bytes));}
std::vector<uint8_t> Palette(uint64_t revision=0){std::vector<uint8_t> p(1024);for(unsigned i=0;i<256;++i){auto c=Ink(uint8_t(i),revision);std::copy(c.begin(),c.end(),p.begin()+i*4);}return p;}
std::vector<uint8_t> Indices(uint32_t w,uint32_t h,uint64_t revision){std::vector<uint8_t> p(size_t(w)*h);for(uint32_t y=0;y<h;++y)for(uint32_t x=0;x<w;++x)p[size_t(y)*w+x]=uint8_t(x*17+y*31+revision);p[0]=0;if(p.size()>1)p[1]=255;return p;}
Image Blank(uint32_t side,Colour c={8,16,24,255}){Image p{side,side,std::vector<uint8_t>(size_t(side)*side*4)};for(size_t i=0;i<p.rgba.size();i+=4)std::copy(c.begin(),c.end(),p.rgba.begin()+i);return p;}
void WorldRevision(Device& d,RuntimeWorldFrame& p,uint64_t revision)
{
    const auto epoch=p.palette.Key().epoch;
    auto tile=Publish(d,RuntimeAssetKind::TileR8,10,revision,32,32,std::vector<uint8_t>(1024,uint8_t(10+revision%10)),epoch);
    for(auto& t:p.terrain)t.tile=tile;
    auto indices=Indices(17,9,revision);std::vector<uint8_t> source(17*9*4);
    for(size_t i=0;i<indices.size();++i){source[i*4]=indices[i];source[i*4+1]=255;}
    auto asset=Publish(d,RuntimeAssetKind::SpriteRGBA8,11,revision,17,9,source,epoch);
    auto row=Publish(d,RuntimeAssetKind::ColourRowRGBA8,12,revision,256,1,Palette(revision),epoch);
    if(p.sprites.empty()){OutlinePassDraw draw;draw.kind=OutlinePassKind::NormalSprite;draw.lookup_row=0;draw.x=7;draw.y=9;draw.width=draw.source_width=17;draw.height=draw.source_height=draw.content_height=9;draw.z_ndc=-0.5f;p.sprites.push_back({draw,asset,row});}
    else{p.sprites[0].source=asset;p.sprites[0].colour_row=row;}
}
RuntimeWorldFrame World(Device& d,uint64_t revision=1,uint64_t epoch=1)
{
    RuntimeWorldFrame p;p.native_width=p.native_height=p.viewport_width=p.viewport_height=64;p.clear_rgba={8.0f/255,16.0f/255,24.0f/255,1};p.shade_settings={0,0,1,1,0,1,1,0,0};
    p.palette=Publish(d,RuntimeAssetKind::PaletteRGBA8,1,1,256,1,Palette(),epoch);
    std::vector<uint8_t> fade(256*64);for(size_t i=0;i<fade.size();++i)fade[i]=uint8_t(i);p.fade=Publish(d,RuntimeAssetKind::FadeR8,2,1,256,64,fade,epoch);
    p.lightmap=Publish(d,RuntimeAssetKind::LightmapR16,3,1,511,511,std::vector<uint8_t>(511*511*2),epoch);
    const EnhancedTerrainVertex a{{-1,1,0,1},0,0,1,-1,-1},b{{1,1,0,1},0,0,1,-1,-1},c{{1,-1,0,1},0,0,1,-1,-1},e{{-1,-1,0,1},0,0,1,-1,-1};
    p.terrain={{{a,b,c},{}},{{a,c,e},{}}};WorldRevision(d,p,revision);return p;
}
Image WorldExpected(uint64_t revision)
{
    auto p=Blank(64,Ink(uint8_t(10+revision%10)));const auto indices=Indices(17,9,revision);
    for(unsigned y=0;y<9;++y)for(unsigned x=0;x<17;++x){const auto c=Ink(indices[y*17+x],revision);std::copy(c.begin(),c.end(),p.rgba.begin()+((y+9)*64+x+7)*4);}
    return p;
}
RuntimeIndexedFrame Indexed(Device& d,uint32_t side=64,uint32_t width=19,uint32_t height=11,uint64_t source_revision=1,uint64_t palette_revision=1,uint64_t epoch=1)
{
    RuntimeIndexedFrame p;p.native_width=p.native_height=side;p.width=width;p.height=height;
    p.x=width==side?0:3;p.y=height==side?0:5;p.clear_index=17;
    p.indices=Publish(d,RuntimeAssetKind::Indexed8,100,source_revision,width,height,Indices(width,height,source_revision),epoch);
    p.palette=Publish(d,RuntimeAssetKind::PaletteRGBA8,101,palette_revision,256,1,Palette(palette_revision),epoch);return p;
}
Image IndexedExpected(const RuntimeIndexedFrame& p,uint64_t source_revision,uint64_t palette_revision)
{
    auto result=Blank(p.native_width,Ink(p.clear_index,palette_revision));const auto indices=Indices(p.width,p.height,source_revision);
    for(uint32_t y=0;y<p.height;++y)for(uint32_t x=0;x<p.width;++x){const auto c=Ink(indices[size_t(y)*p.width+x],palette_revision);std::copy(c.begin(),c.end(),result.rgba.begin()+((size_t(y)+p.y)*p.native_width+x+p.x)*4);}
    return result;
}
void Stats(std::ostream& o,const RuntimeResourceDiagnostics& s)
{
    o<<"{\"stagingAllocations\":"<<s.staging_allocations<<",\"stagingMaps\":"<<s.staging_maps<<",\"stagingReuses\":"<<s.staging_reuses
     <<",\"stagingGrowths\":"<<s.staging_growths<<",\"stagingTrimCount\":"<<s.staging_trim_count<<",\"stagingCapacityBytes\":"<<s.staging_capacity_bytes
     <<",\"stagingAllocationBytes\":"<<s.staging_allocation_bytes<<",\"stagingPools\":"<<s.staging_pools<<",\"resourceAllocations\":"<<s.resource_allocations
     <<",\"descriptorHeapAllocations\":"<<s.descriptor_heap_allocations<<",\"cpuBytes\":"<<s.cpu_bytes<<",\"defaultBytes\":"<<s.default_bytes
     <<",\"uploadBytes\":"<<s.upload_bytes<<",\"readbackBytes\":"<<s.readback_bytes<<",\"uploadHighWater\":"<<s.upload_high_water
     <<",\"descriptors\":"<<s.descriptors<<",\"worldPages\":"<<s.world_pages<<",\"worldRegions\":"<<s.world_regions
     <<",\"worldUploadBytes\":"<<s.world_upload_bytes<<",\"sourceUploadBytes\":"<<s.source_upload_bytes<<",\"paletteUploadBytes\":"<<s.palette_upload_bytes
     <<",\"alignedUploadBytes\":"<<s.aligned_upload_bytes<<",\"geometryUploadBytes\":"<<s.geometry_upload_bytes<<",\"submissions\":"<<s.submissions
     <<",\"presentations\":"<<s.presentations<<",\"heldSubmissions\":"<<s.held_submissions<<",\"pressureCollections\":"<<s.pressure_collections
     <<",\"pressureRecoveries\":"<<s.pressure_recoveries<<",\"wouldBlock\":"<<s.would_block<<",\"globalIdleCalls\":"<<s.global_idle_calls
     <<",\"fenceWaits\":"<<s.fence_waits<<",\"imageReadbacks\":"<<s.image_readbacks<<",\"readerHoldSlots\":"<<s.reader_hold_slots
     <<",\"failed\":"<<(s.failed?"true":"false")<<",\"quarantined\":"<<(s.quarantined?"true":"false")<<'}';
}
struct Runner {
    Device& d;std::filesystem::path output;RuntimeStagingTestResult result;RuntimeResourceBudgets budgets;Diagnostics baseline;uint64_t isolated_submissions=0;
    std::vector<std::string> cases,comparisons,measurements,messages;std::atomic<bool> watchdog{false};std::string active;
    Runner(Device& device,const std::filesystem::path& path):d(device),output(path),baseline(device.GetDiagnostics()){}
    void Check(bool value,const char* why){++result.checks;Require(value,why);}
    template<class E=std::runtime_error,class F>void Reject(F&& fn,const char* why){bool rejected=false;try{fn();}catch(const E&){rejected=true;}Check(rejected,why);}
    void Validation(){auto m=d.TakeValidationMessages();messages.insert(messages.end(),m.begin(),m.end());auto s=d.GetDiagnostics();Check(s.validation_errors==baseline.validation_errors&&s.validation_warnings==baseline.validation_warnings,"Staging GPU validation diagnostics");}
    void Bound(){auto s=d.GetRuntimeResourceDiagnostics();Check(s.cpu_bytes<=budgets.cpu_bytes&&s.default_bytes<=budgets.default_bytes&&s.upload_bytes<=budgets.upload_bytes&&s.readback_bytes<=budgets.readback_bytes&&s.descriptors<=budgets.descriptors&&s.staging_allocation_bytes<=s.upload_bytes&&s.staging_pools<=3,"Staging exceeded actual allocation bounds");}
    void Configure(uint32_t side=64,uint64_t upload=128*1024*KiB){d.ClearRuntimeResources();d.Resize(side,side);budgets={};budgets.upload_bytes=upload;d.ConfigureRuntimeResources(budgets);}
    void Case(const std::string& name,const std::function<void()>& body){active=name;Validation();try{body();Validation();++result.cases;cases.push_back("{\"name\":"+Json(name)+",\"passed\":true}");}catch(const std::exception& e){cases.push_back("{\"name\":"+Json(name)+",\"passed\":false,\"error\":"+Json(e.what())+"}");throw;}}
    void Record(const std::string& name,const RuntimeResourceDiagnostics& a,const RuntimeResourceDiagnostics& b){std::ostringstream o;o<<"{\"name\":"<<Json(name)<<",\"uploadByteCap\":"<<budgets.upload_bytes<<",\"before\":";Stats(o,a);o<<",\"after\":";Stats(o,b);o<<'}';measurements.push_back(o.str());}
    template<class P>RuntimeFrameTicket Submit(const P& p,bool diagnostic=false){const auto until=Clock::now()+std::chrono::seconds(25);for(;;){RuntimeSubmitResult s;if constexpr(std::is_same_v<P,RuntimeWorldFrame>)s=d.TrySubmitRuntimeWorldFrame(p,diagnostic);else s=d.TrySubmitRuntimeIndexedFrame(p,diagnostic);Bound();Check(p.palette.Valid(),"Staging consumed producer lease");if(s.status==RuntimeSubmitStatus::Submitted){++result.submitted_frames;Check(s.ticket.Valid()&&s.ticket.HasReadback()==diagnostic,"Staging ticket mismatch");return s.ticket;}Check(s.status==RuntimeSubmitStatus::WouldBlock&&!s.ticket.Valid(),"Unexpected staging submission status");Require(Clock::now()<until,"Staging submission remained blocked");d.CollectRuntimeResources();Pump();}}
    void Complete(RuntimeFrameTicket& t){const auto until=Clock::now()+std::chrono::seconds(25);while(!d.IsRuntimeFrameComplete(t)){Require(Clock::now()<until,"Staging completion timeout");Pump();}t.Reset();d.CollectRuntimeResources();}
    void Compare(RuntimeFrameTicket& t,const Image& expected,const std::string& name){Image image;const auto until=Clock::now()+std::chrono::seconds(25);while(!d.TryReadbackRuntimeFrame(t,image)){Require(Clock::now()<until,"Staging readback timeout");Pump();}auto diff=Differences(image,expected);auto stem="runtime-staging-"+name;Save(output/(stem+"-gpu"),image);Save(output/(stem+"-expected"),expected);result.rgba_byte_checks+=image.rgba.size();comparisons.push_back("{\"name\":"+Json(name)+",\"gpuStem\":"+Json(stem+"-gpu")+",\"expectedStem\":"+Json(stem+"-expected")+",\"width\":"+std::to_string(image.width)+",\"height\":"+std::to_string(image.height)+",\"rgbaBytes\":"+std::to_string(image.rgba.size())+",\"differingBytes\":"+std::to_string(diff)+"}");Validation();Check(diff==0,"Staging differs from independent analytic output");t.Reset();d.CollectRuntimeResources();}
    void NoWait(const RuntimeResourceDiagnostics& a){auto b=d.GetRuntimeResourceDiagnostics();Check(b.global_idle_calls==a.global_idle_calls&&b.fence_waits==a.fence_waits,"Staging inserted internal idle/wait");}
    void Write(bool passed,const std::string& error,const RuntimeResourceDiagnostics& before){std::ofstream o(output/"runtime-staging-tests.json");o<<"{\"schema\":1,\"scope\":\"Persistent mapped per-frame asset staging shared by indexed/world paths; exact analytic outputs and changed-frame reuse\",\"passed\":"<<(passed?"true":"false")<<",\"gate3Accepted\":false,\"readbackUsedAsInput\":false,\"frozenReferenceUsed\":false,\"liveProducerTested\":false,\"overlaysTested\":false,\"lensesTested\":false,\"dirtyLightmapRectanglesTested\":false,\"processDxgiBudgetTested\":false,\"gpuTimestampsTested\":false,\"defaultTextureRecyclingTested\":false,\"cpuPublicationCopiesEliminated\":false,\"checks\":"<<result.checks<<",\"casesPassed\":"<<result.cases<<",\"successfulSubmissions\":"<<result.submitted_frames<<",\"isolatedDeviceSubmissions\":"<<isolated_submissions<<",\"primaryDeviceSubmissions\":"<<(result.submitted_frames-isolated_submissions)<<",\"submissionCountScope\":\"Suite submissions include isolated devices; Device measurements retain primary cumulative counters\",\"rgbaByteChecks\":"<<result.rgba_byte_checks<<",\"comparisonCount\":"<<comparisons.size()<<",\"rawFiles\":"<<comparisons.size()*2<<",\"bmpFiles\":"<<comparisons.size()*2<<",\"watchdogFired\":"<<(watchdog.load()?"true":"false")<<",\"error\":"<<Json(error)<<",\"activeCase\":"<<Json(active)<<",\"beforeCleanup\":";Stats(o,before);o<<",\"afterCleanup\":";Stats(o,d.GetRuntimeResourceDiagnostics());auto array=[&](const char* name,const std::vector<std::string>& values){o<<",\""<<name<<"\":[";for(size_t i=0;i<values.size();++i){if(i)o<<',';o<<values[i];}o<<']';};array("cases",cases);array("measurements",measurements);array("comparisons",comparisons);o<<",\"validationMessages\":[";for(size_t i=0;i<messages.size();++i){if(i)o<<',';o<<Json(messages[i]);}o<<"]}\n";Require(bool(o),"Cannot retain staging report");}
};
struct Hold {
    RuntimeReaderHold handle;std::jthread timer;std::atomic<bool>& fired;
    Hold(Device& d,std::atomic<bool>& f):fired(f){auto until=Clock::now()+std::chrono::seconds(25);while(!d.TryBeginRuntimeReaderHold(handle)){Require(Clock::now()<until,"Cannot arm staging hold");d.CollectRuntimeResources();Pump();}timer=std::jthread([this](std::stop_token stop){auto until=Clock::now()+std::chrono::seconds(15);while(!stop.stop_requested()){if(Clock::now()>=until){fired.store(true);handle.Release();}std::this_thread::sleep_for(std::chrono::milliseconds(10));}});}
    void Release(){bool ok=handle.Release();timer.request_stop();if(timer.joinable())timer.join();Require(ok&&!fired.load(),"Staging hold release/watchdog failure");}
    ~Hold(){timer.request_stop();if(timer.joinable())timer.join();handle.Release();}
};
void NoStagingAllocation(Runner& r,const RuntimeResourceDiagnostics& a,const RuntimeResourceDiagnostics& b)
{r.Check(b.staging_allocations==a.staging_allocations&&b.staging_maps==a.staging_maps&&b.staging_growths==a.staging_growths&&b.staging_trim_count==a.staging_trim_count,"Warmed staging allocated/mapped/grew/trimmed");}
}

RuntimeStagingTestResult RunRuntimeStagingTests(Device& device,const Options& options,const std::filesystem::path& output)
{
    Runner r(device,output);
    try{
        r.Case("1000-changed-pooled-world-frames-zero-resource-allocation",[&]{
            r.Configure();auto p=World(device);uint64_t revision=1;
            for(unsigned i=0;i<7;++i){if(i)WorldRevision(device,p,++revision);auto t=r.Submit(p);r.Complete(t);}
            auto selected=r.Submit(p,true);r.Compare(selected,WorldExpected(revision),"world-warm");const auto before=device.GetRuntimeResourceDiagnostics();
            r.Check(before.staging_pools==3,"World warmup omitted a staging slot");
            for(unsigned i=0;i<1000;++i){WorldRevision(device,p,++revision);auto t=r.Submit(p);}
            const auto after=device.GetRuntimeResourceDiagnostics();r.Record("world1000",before,after);NoStagingAllocation(r,before,after);
            r.Check(after.resource_allocations==before.resource_allocations&&after.descriptor_heap_allocations==before.descriptor_heap_allocations&&after.world_pages==before.world_pages,"Changed pooled world failed total allocation plateau");
            r.Check(after.staging_reuses==before.staging_reuses+1000&&after.world_upload_bytes==before.world_upload_bytes+2660000&&after.aligned_upload_bytes==before.aligned_upload_bytes+11776000,"World changed upload/reuse counters differ from independent footprint arithmetic");
            r.Check(after.presentations==before.presentations+1000&&after.image_readbacks==before.image_readbacks,"World measured loop did not present1000frames without readback");r.NoWait(before);
            auto final=r.Submit(p,true);r.Compare(final,WorldExpected(revision),"world-final");
            // Each slot must first see the final resolved atlas address retained in its terrain shadow.
            for(unsigned i=0;i<3;++i){auto t=r.Submit(p);r.Complete(t);}
            const auto resident=device.GetRuntimeResourceDiagnostics();for(unsigned i=0;i<6;++i){auto t=r.Submit(p);r.Complete(t);}const auto unchanged=device.GetRuntimeResourceDiagnostics();
            NoStagingAllocation(r,resident,unchanged);r.Check(unchanged.staging_reuses==resident.staging_reuses&&unchanged.world_upload_bytes==resident.world_upload_bytes&&unchanged.aligned_upload_bytes==resident.aligned_upload_bytes,"Unchanged world touched staging counters");r.Record("unchanged-world6",resident,unchanged);
        });
        r.Case("1000-alternating-index-palette-updates-staging-only-plateau",[&]{
            r.Configure();uint64_t sr=1,pr=1;auto p=Indexed(device);
            for(unsigned i=0;i<7;++i){if(i)p=Indexed(device,64,19,11,++sr,++pr);auto t=r.Submit(p);r.Complete(t);}
            auto warm=r.Submit(p,true);r.Compare(warm,IndexedExpected(p,sr,pr),"indexed-warm");const auto before=device.GetRuntimeResourceDiagnostics();r.Check(before.staging_pools==3,"Indexed warmup omitted a staging slot");
            for(unsigned i=0;i<1000;++i){if(i%2){++pr;p.palette=Publish(device,RuntimeAssetKind::PaletteRGBA8,101,pr,256,1,Palette(pr));}else{++sr;p.indices=Publish(device,RuntimeAssetKind::Indexed8,100,sr,19,11,Indices(19,11,sr));}auto t=r.Submit(p);}
            const auto after=device.GetRuntimeResourceDiagnostics();r.Record("indexed1000-default-textures-still-allocate",before,after);NoStagingAllocation(r,before,after);
            r.Check(after.resource_allocations==before.resource_allocations+1000&&after.descriptor_heap_allocations==before.descriptor_heap_allocations,"Indexed DEFAULT allocation scope differs");
            r.Check(after.staging_reuses==before.staging_reuses+1000&&after.source_upload_bytes==before.source_upload_bytes+500*209&&after.palette_upload_bytes==before.palette_upload_bytes+500*1024&&after.aligned_upload_bytes==before.aligned_upload_bytes+1801500,"Indexed legacy span or logical transfer counters differ");
            r.Check(after.presentations==before.presentations+1000&&after.image_readbacks==before.image_readbacks,"Indexed measured loop did not present1000frames without readback");r.NoWait(before);
            auto final=r.Submit(p,true);r.Compare(final,IndexedExpected(p,sr,pr),"indexed-final");
        });
        r.Case("three-held-mixed-world-indexed-footprints-no-slot-alias",[&]{
            r.Configure();auto world=World(device);for(uint64_t revision=1;revision<=7;++revision){if(revision>1)WorldRevision(device,world,revision);auto t=r.Submit(world);r.Complete(t);}
            auto indexed=Indexed(device);Hold gate(device,r.watchdog);WorldRevision(device,world,8);std::array<RuntimeFrameTicket,3> tickets;
            tickets[0]=r.Submit(world,true);tickets[1]=r.Submit(indexed,true);WorldRevision(device,world,9);tickets[2]=r.Submit(world,true);
            std::array<Image,3> expected{WorldExpected(8),IndexedExpected(indexed,1,1),WorldExpected(9)};for(const auto& t:tickets)r.Check(!device.IsRuntimeFrameComplete(t),"Mixed staging reader completed before gate release");
            WorldRevision(device,world,10);const auto before=device.GetRuntimeResourceDiagnostics();auto denied=device.TrySubmitRuntimeWorldFrame(world,true);const auto after=device.GetRuntimeResourceDiagnostics();
            r.Check(denied.status==RuntimeSubmitStatus::WouldBlock&&denied.pressure==RuntimePressureReason::FrameSlot&&!denied.ticket.Valid(),"Fourth mixed reader did not report frame-slot pressure");
            NoStagingAllocation(r,before,after);r.Check(after.staging_reuses==before.staging_reuses&&after.aligned_upload_bytes==before.aligned_upload_bytes&&after.world_upload_bytes==before.world_upload_bytes,"Busy slot modified staging admission counters");
            device.SetRuntimeEpoch(2);world={};indexed={};device.CollectRuntimeResources();const auto retained=device.GetRuntimeResourceDiagnostics();r.Check(retained.staging_allocation_bytes==before.staging_allocation_bytes&&retained.upload_bytes==before.upload_bytes&&retained.reader_hold_slots==3,"Epoch/drop reclaimed held staging owners");
            gate.Release();for(size_t i=0;i<tickets.size();++i)r.Compare(tickets[i],expected[i],"mixed-held-"+std::to_string(i));
            auto resumed=Indexed(device,64,19,11,1,1,2);auto t=r.Submit(resumed,true);r.Compare(t,IndexedExpected(resumed,1,1),"mixed-resumed");r.NoWait(before);r.Record("mixed-held-retention",before,device.GetRuntimeResourceDiagnostics());
        });
        r.Case("fresh-device-growth-and-reuse-rollback-actual-overlap",[&]{
            HWND window=CreateWindowExW(0,L"STATIC",L"Runtime staging rollback",WS_POPUP,0,0,257,257,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);r.Check(window!=nullptr,"Cannot create staging rollback window");
            try{Device fresh;fresh.Initialize(window,257,257,options);Runner local(fresh,output);const auto merge=[&]{r.isolated_submissions+=local.result.submitted_frames;r.result.checks+=local.result.checks;r.result.submitted_frames+=local.result.submitted_frames;r.result.rgba_byte_checks+=local.result.rgba_byte_checks;r.comparisons.insert(r.comparisons.end(),local.comparisons.begin(),local.comparisons.end());r.measurements.insert(r.measurements.end(),local.measurements.begin(),local.measurements.end());r.messages.insert(r.messages.end(),local.messages.begin(),local.messages.end());};try{auto p=Indexed(fresh,257);uint64_t sr=1,pr=1;
                for(unsigned i=0;i<7;++i){if(i)p=Indexed(fresh,257,19,11,++sr,++pr);auto t=local.Submit(p);local.Complete(t);}auto before=fresh.GetRuntimeResourceDiagnostics();local.Check(before.staging_pools==3,"Rollback warmup omitted slot");
                p=Indexed(fresh,257,257,257,++sr,++pr);fresh.SetRuntimeFailureInjection(RuntimeFailurePoint::BeforeExecute);local.Reject([&]{fresh.TrySubmitRuntimeIndexedFrame(p,true);},"Growth rollback not injected");auto rolled=fresh.GetRuntimeResourceDiagnostics();
                local.Check(rolled.staging_capacity_bytes==before.staging_capacity_bytes&&rolled.staging_allocation_bytes==before.staging_allocation_bytes&&rolled.staging_growths==before.staging_growths&&rolled.submissions==before.submissions&&rolled.source_upload_bytes==before.source_upload_bytes,"Growth rollback replaced an old owner or committed uploads");
                local.Check(rolled.staging_allocations==before.staging_allocations+1&&rolled.staging_maps==before.staging_maps+1&&rolled.upload_high_water>=before.upload_bytes+256*KiB,"Rolled-back candidate allocation/map/overlap peak omitted");local.Record("growth-rollback",before,rolled);
                auto retry=local.Submit(p,true);local.Compare(retry,IndexedExpected(p,sr,pr),"growth-retry");auto grown=fresh.GetRuntimeResourceDiagnostics();local.Check(grown.staging_growths==before.staging_growths+1,"Growth retry did not commit exactly one replacement");
                p=Indexed(fresh,257,19,11,++sr,++pr);before=fresh.GetRuntimeResourceDiagnostics();fresh.SetRuntimeFailureInjection(RuntimeFailurePoint::BeforeExecute);local.Reject([&]{fresh.TrySubmitRuntimeIndexedFrame(p,true);},"Reuse rollback not injected");rolled=fresh.GetRuntimeResourceDiagnostics();NoStagingAllocation(local,before,rolled);local.Check(rolled.staging_reuses==before.staging_reuses&&rolled.submissions==before.submissions&&rolled.aligned_upload_bytes==before.aligned_upload_bytes,"Reuse rollback committed a staging transfer");
                auto reuse=local.Submit(p,true);local.Compare(reuse,IndexedExpected(p,sr,pr),"reuse-retry");local.Check(fresh.GetRuntimeResourceDiagnostics().staging_reuses==before.staging_reuses+1,"Reuse retry did not reuse a completed pool");local.NoWait(before);local.Record("reuse-rollback",before,rolled);
                fresh.ClearRuntimeResources();local.Validation();}catch(...){local.Record("isolated-failure",{},fresh.GetRuntimeResourceDiagnostics());merge();throw;}merge();
            }catch(...){DestroyWindow(window);throw;}DestroyWindow(window);
        });
        r.Case("upload-cap-held-pool-protection-pressure-trim-and-reload",[&]{
            r.Configure(257,192*KiB);uint64_t sr=1,pr=1;auto p=Indexed(device,257);
            for(unsigned i=0;i<7;++i){if(i)p=Indexed(device,257,19,11,++sr,++pr);auto t=r.Submit(p);r.Complete(t);}const auto warm=device.GetRuntimeResourceDiagnostics();r.Check(warm.staging_pools==3,"Pressure warmup did not own three pools");
            Hold gate(device,r.watchdog);p=Indexed(device,257,19,11,++sr,++pr);const auto expected=IndexedExpected(p,sr,pr);auto held=r.Submit(p,true);
            p=Indexed(device,257,257,257,++sr,++pr);const auto before=device.GetRuntimeResourceDiagnostics();auto denied=device.TrySubmitRuntimeIndexedFrame(p,true);const auto blocked=device.GetRuntimeResourceDiagnostics();
            r.Check(denied.status==RuntimeSubmitStatus::WouldBlock&&denied.pressure==RuntimePressureReason::UploadBytes&&!denied.ticket.Valid(),"Held staging overlap did not return upload pressure");
            r.Check(!device.IsRuntimeFrameComplete(held)&&blocked.staging_pools==1&&blocked.staging_trim_count==before.staging_trim_count+2&&blocked.staging_allocations==before.staging_allocations&&blocked.staging_maps==before.staging_maps,"Pressure trimmed held owner or allocated before complete reservation");r.Bound();r.Record("held-upload-pressure",before,blocked);
            gate.Release();r.Compare(held,expected,"pressure-held-old");const auto released=device.GetRuntimeResourceDiagnostics();auto retry=r.Submit(p,true);r.Compare(retry,IndexedExpected(p,sr,pr),"pressure-grown-reload");const auto after=device.GetRuntimeResourceDiagnostics();
            r.Check(after.pressure_recoveries>released.pressure_recoveries&&after.staging_trim_count>released.staging_trim_count&&after.staging_growths==released.staging_growths&&after.staging_allocations==released.staging_allocations+1,"Pressure retry did not trim completed pool then allocate a bounded cold pool");r.NoWait(warm);r.Record("released-upload-recovery",released,after);
        });
        const auto before=device.GetRuntimeResourceDiagnostics();device.ClearRuntimeResources();const auto after=device.GetRuntimeResourceDiagnostics();r.Check(!after.upload_bytes&&!after.staging_pools&&!after.staging_capacity_bytes&&!after.staging_allocation_bytes&&!after.default_bytes&&!after.readback_bytes,"Staging teardown retained owners");r.Validation();r.Write(true,"",before);return r.result;
    }catch(const std::exception& e){auto before=device.GetRuntimeResourceDiagnostics();try{device.ClearRuntimeResources();}catch(...){}r.Write(false,e.what(),before);throw;}
}
}
