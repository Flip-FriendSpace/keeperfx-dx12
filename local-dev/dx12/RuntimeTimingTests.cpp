// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#include "RuntimeTimingTests.h"
#include "EnhancedSpriteTestImages.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <thread>
#include <type_traits>

namespace kfx::render::dx12 {
namespace {
using namespace enhanced_test;
using Clock = std::chrono::steady_clock;

using Colour = std::array<uint8_t,4>;
Colour Ink(uint8_t n, uint64_t revision = 0) { return {uint8_t(n*13+revision),uint8_t(n*29+revision*3),uint8_t(n*53+revision*7),255}; }
void Pump(){MSG m{};while(PeekMessageW(&m,nullptr,0,0,PM_REMOVE)){Require(m.message!=WM_QUIT,"Timing window closed");TranslateMessage(&m);DispatchMessageW(&m);}std::this_thread::sleep_for(std::chrono::milliseconds(1));}
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
void Aggregate(std::ostream& o,const RuntimeTimingAggregate& a){o<<"{\"samples\":"<<a.samples<<",\"totalMs\":"<<a.total_ms<<",\"maximumMs\":"<<a.maximum_ms<<",\"latestMs\":"<<a.latest_ms<<'}';}
void Bucket(std::ostream& o,const RuntimeTimingBucket& b){o<<"{\"cpuSubmitted\":";Aggregate(o,b.cpu_submitted);o<<",\"cpuWouldBlock\":";Aggregate(o,b.cpu_would_block);o<<",\"cpuSuspended\":";Aggregate(o,b.cpu_suspended);o<<",\"cpuFailed\":";Aggregate(o,b.cpu_failed);o<<",\"cpuPresent\":";Aggregate(o,b.cpu_present);o<<",\"gpuUploadRender\":";Aggregate(o,b.gpu_upload_render);o<<'}';}
void Path(std::ostream& o,const RuntimeTimingPath& p){o<<"{\"normal\":";Bucket(o,p.normal);o<<",\"diagnostic\":";Bucket(o,p.diagnostic);o<<",\"held\":";Bucket(o,p.held);o<<'}';}
void Timing(std::ostream& o,const RuntimeTimingDiagnostics& t){o<<std::setprecision(17)<<"{\"frequency\":"<<t.timestamp_frequency<<",\"queryHeaps\":"<<t.query_heaps<<",\"queryEntries\":"<<t.query_entries<<",\"readbackResources\":"<<t.readback_resources<<",\"readbackLogicalBytes\":"<<t.readback_logical_bytes<<",\"readbackAllocationBytes\":"<<t.readback_allocation_bytes<<",\"completed\":"<<t.gpu_completed<<",\"discarded\":"<<t.gpu_discarded<<",\"maps\":"<<t.readback_maps<<",\"pending\":"<<t.gpu_pending<<",\"world\":";Path(o,t.world);o<<",\"indexed\":";Path(o,t.indexed);o<<'}';}
void Resources(std::ostream& o,const RuntimeResourceDiagnostics& t){o<<"{\"submissions\":"<<t.submissions<<",\"presentations\":"<<t.presentations<<",\"heldSubmissions\":"<<t.held_submissions<<",\"resourceAllocations\":"<<t.resource_allocations<<",\"descriptorHeapAllocations\":"<<t.descriptor_heap_allocations<<",\"readbackBytes\":"<<t.readback_bytes<<",\"imageReadbacks\":"<<t.image_readbacks<<",\"globalIdleCalls\":"<<t.global_idle_calls<<",\"fenceWaits\":"<<t.fence_waits<<",\"failed\":"<<(t.failed?"true":"false")<<",\"quarantined\":"<<(t.quarantined?"true":"false")<<'}';}
struct Runner {
    Device& d;std::filesystem::path output;RuntimeTimingTestResult result;Diagnostics baseline;uint64_t isolated=0;
    std::vector<std::string> cases,measurements,comparisons,messages;std::string active;std::atomic<bool> watchdog{false};
    Runner(Device& device,const std::filesystem::path& path):d(device),output(path),baseline(device.GetDiagnostics()){}
    void Check(bool value,const char* why){++result.checks;Require(value,why);}
    template<class F>void Reject(F&& fn,const char* why){bool rejected=false;try{fn();}catch(const std::exception&){rejected=true;}Check(rejected,why);}
    void Validation(){auto m=d.TakeValidationMessages();messages.insert(messages.end(),m.begin(),m.end());auto s=d.GetDiagnostics();Check(s.validation_errors==baseline.validation_errors&&s.validation_warnings==baseline.validation_warnings,"Timing GPU validation diagnostics");}
    void ValidAggregate(const RuntimeTimingAggregate& a){Check(std::isfinite(a.total_ms)&&std::isfinite(a.maximum_ms)&&std::isfinite(a.latest_ms)&&a.total_ms>=0&&a.maximum_ms>=0&&a.latest_ms>=0&&a.maximum_ms>=a.latest_ms&&a.total_ms>=a.maximum_ms,"Invalid timing aggregate duration");if(!a.samples)Check(a.total_ms==0&&a.maximum_ms==0&&a.latest_ms==0,"Empty timing aggregate has duration");}
    void ValidBucket(const RuntimeTimingBucket& b){ValidAggregate(b.cpu_submitted);ValidAggregate(b.cpu_would_block);ValidAggregate(b.cpu_suspended);ValidAggregate(b.cpu_failed);ValidAggregate(b.cpu_present);ValidAggregate(b.gpu_upload_render);}
    RuntimeTimingDiagnostics Read(){auto t=d.GetRuntimeTimingDiagnostics();Check(t.timestamp_frequency>0&&t.query_heaps==1&&t.query_entries==6&&t.readback_resources==3&&t.readback_logical_bytes==48&&t.readback_allocation_bytes>=48&&t.gpu_pending<=3,"Invalid fixed timing instrumentation");Check(t.readback_maps==t.gpu_completed,"Timing readback Map not once per completed sample");ValidBucket(t.world.normal);ValidBucket(t.world.diagnostic);ValidBucket(t.world.held);ValidBucket(t.indexed.normal);ValidBucket(t.indexed.diagnostic);ValidBucket(t.indexed.held);Check(t.gpu_completed==t.world.normal.gpu_upload_render.samples+t.world.diagnostic.gpu_upload_render.samples+t.world.held.gpu_upload_render.samples+t.indexed.normal.gpu_upload_render.samples+t.indexed.diagnostic.gpu_upload_render.samples+t.indexed.held.gpu_upload_render.samples,"GPU timing bucket sum differs");return t;}
    void Fixed(const RuntimeTimingDiagnostics& a,const RuntimeTimingDiagnostics& b){Check(a.timestamp_frequency==b.timestamp_frequency&&a.readback_allocation_bytes==b.readback_allocation_bytes&&a.readback_resources==b.readback_resources&&a.query_heaps==b.query_heaps&&a.query_entries==b.query_entries,"Timing instrumentation allocation changed");}
    void Record(const std::string& name,const RuntimeTimingDiagnostics& a,const RuntimeTimingDiagnostics& b,const RuntimeResourceDiagnostics& ra,const RuntimeResourceDiagnostics& rb){std::ostringstream o;o<<"{\"name\":"<<Json(name)<<",\"before\":";Timing(o,a);o<<",\"after\":";Timing(o,b);o<<",\"resourceBefore\":";Resources(o,ra);o<<",\"resourceAfter\":";Resources(o,rb);o<<'}';measurements.push_back(o.str());}
    void NoWait(const RuntimeResourceDiagnostics& a){auto b=d.GetRuntimeResourceDiagnostics();Check(a.global_idle_calls==b.global_idle_calls&&a.fence_waits==b.fence_waits,"Timing collection inserted wait/idle");}
    void Configure(){d.ClearRuntimeResources();d.Resize(64,64);d.ConfigureRuntimeResources({});}
    void Case(const std::string& name,const std::function<void()>& body){active=name;Validation();try{body();Validation();++result.cases;cases.push_back("{\"name\":"+Json(name)+",\"passed\":true}");}catch(const std::exception& e){cases.push_back("{\"name\":"+Json(name)+",\"passed\":false,\"error\":"+Json(e.what())+"}");throw;}}
    template<class P>RuntimeFrameTicket Submit(const P& p,bool diagnostic=false){auto until=Clock::now()+std::chrono::seconds(25);for(;;){RuntimeSubmitResult s;if constexpr(std::is_same_v<P,RuntimeWorldFrame>)s=d.TrySubmitRuntimeWorldFrame(p,diagnostic);else s=d.TrySubmitRuntimeIndexedFrame(p,diagnostic);if(s.status==RuntimeSubmitStatus::Submitted){++result.submitted_frames;Check(s.ticket.Valid()&&s.ticket.HasReadback()==diagnostic,"Timing ticket mismatch");return s.ticket;}Check(s.status==RuntimeSubmitStatus::WouldBlock&&!s.ticket.Valid(),"Unexpected timing submission status");Require(Clock::now()<until,"Timing submit timeout");d.CollectRuntimeResources();Pump();}}
    void Complete(RuntimeFrameTicket& t){auto until=Clock::now()+std::chrono::seconds(25);while(!d.IsRuntimeFrameComplete(t)){Require(Clock::now()<until,"Timing ticket completion timeout");Pump();}t.Reset();d.CollectRuntimeResources();}
    void Compare(RuntimeFrameTicket& t,const Image& expected,const std::string& name){Image image;auto until=Clock::now()+std::chrono::seconds(25);while(!d.TryReadbackRuntimeFrame(t,image)){Require(Clock::now()<until,"Timing image readback timeout");Pump();}auto diff=Differences(image,expected);auto stem="runtime-timing-"+name;Save(output/(stem+"-gpu"),image);Save(output/(stem+"-expected"),expected);result.rgba_byte_checks+=image.rgba.size();comparisons.push_back("{\"name\":"+Json(name)+",\"gpuStem\":"+Json(stem+"-gpu")+",\"expectedStem\":"+Json(stem+"-expected")+",\"width\":"+std::to_string(image.width)+",\"height\":"+std::to_string(image.height)+",\"rgbaBytes\":"+std::to_string(image.rgba.size())+",\"differingBytes\":"+std::to_string(diff)+"}");Check(diff==0,"Timing fixture differs from analytic pixels");t.Reset();d.CollectRuntimeResources();Validation();}
    void PollStable(const RuntimeTimingDiagnostics& before){auto ra=d.GetRuntimeResourceDiagnostics();for(unsigned i=0;i<16;++i){auto after=Read();Check(after.gpu_completed==before.gpu_completed&&after.readback_maps==before.readback_maps&&after.gpu_pending==before.gpu_pending&&after.gpu_discarded==before.gpu_discarded,"Repeated timing getter consumed duplicate or premature sample");Fixed(before,after);}NoWait(ra);}
    void Merge(const Runner& local){isolated+=local.result.submitted_frames;result.submitted_frames+=local.result.submitted_frames;result.checks+=local.result.checks;result.rgba_byte_checks+=local.result.rgba_byte_checks;measurements.insert(measurements.end(),local.measurements.begin(),local.measurements.end());comparisons.insert(comparisons.end(),local.comparisons.begin(),local.comparisons.end());messages.insert(messages.end(),local.messages.begin(),local.messages.end());Check(!local.watchdog.load(),"Isolated timing watchdog fired");}
    void Write(bool passed,const std::string& error,const RuntimeTimingDiagnostics& before){std::ofstream o(output/"runtime-timing-tests.json");o<<"{\"schema\":1,\"scope\":\"Bounded asynchronous CPU status/Present and fence-gated GPU upload-render instrumentation; durations are not a performance qualification\",\"passed\":"<<(passed?"true":"false")<<",\"gate3Accepted\":false,\"releaseFpsQualified\":false,\"readbackUsedAsInput\":false,\"timingWaitUsed\":false,\"gpuScope\":\"Upload and rendering only; excludes image copies, queue latency and Present\",\"exclusions\":[\"live producer\",\"overlays/lenses\",\"dirty lightmap updates\",\"DXGI process budget\",\"percentile and release performance analysis\"],\"checks\":"<<result.checks<<",\"casesPassed\":"<<result.cases<<",\"successfulSubmissions\":"<<result.submitted_frames<<",\"primaryDeviceSubmissions\":"<<result.submitted_frames-isolated<<",\"isolatedDeviceSubmissions\":"<<isolated<<",\"rgbaByteChecks\":"<<result.rgba_byte_checks<<",\"comparisonCount\":"<<comparisons.size()<<",\"rawFiles\":"<<comparisons.size()*2<<",\"bmpFiles\":"<<comparisons.size()*2<<",\"watchdogFired\":"<<(watchdog.load()?"true":"false")<<",\"error\":"<<Json(error)<<",\"activeCase\":"<<Json(active)<<",\"beforeCleanup\":";Timing(o,before);o<<",\"afterCleanup\":";Timing(o,d.GetRuntimeTimingDiagnostics());auto array=[&](const char* name,const std::vector<std::string>& values){o<<",\""<<name<<"\":[";for(size_t i=0;i<values.size();++i){if(i)o<<',';o<<values[i];}o<<']';};array("cases",cases);array("measurements",measurements);array("comparisons",comparisons);o<<",\"validationMessages\":[";for(size_t i=0;i<messages.size();++i){if(i)o<<',';o<<Json(messages[i]);}o<<"]}\n";Require(bool(o),"Cannot retain timing report");}
};
struct Hold {
    RuntimeReaderHold handle;std::jthread timer;std::atomic<bool>& fired;
    Hold(Device& d,std::atomic<bool>& f):fired(f){auto until=Clock::now()+std::chrono::seconds(25);while(!d.TryBeginRuntimeReaderHold(handle)){Require(Clock::now()<until,"Cannot arm timing hold");d.CollectRuntimeResources();Pump();}timer=std::jthread([this](std::stop_token stop){auto until=Clock::now()+std::chrono::seconds(15);while(!stop.stop_requested()){if(Clock::now()>=until){fired.store(true);handle.Release();}std::this_thread::sleep_for(std::chrono::milliseconds(10));}});}
    void Release(){bool ok=handle.Release();timer.request_stop();if(timer.joinable())timer.join();Require(ok&&!fired.load(),"Timing hold release/watchdog failure");}
    ~Hold(){timer.request_stop();if(timer.joinable())timer.join();handle.Release();}
};
}
RuntimeTimingTestResult RunRuntimeTimingTests(Device& device,const Options& options,const std::filesystem::path& output)
{
    Runner r(device,output);
    try{
        r.Case("mixed-path-exact-pixels-and-once-only-collection",[&]{
            r.Configure();auto world=World(device);auto indexed=Indexed(device);auto a=r.Read();auto ra=device.GetRuntimeResourceDiagnostics();
            auto w=r.Submit(world,true);r.Compare(w,WorldExpected(1),"mixed-world");auto i=r.Submit(indexed,true);r.Compare(i,IndexedExpected(indexed,1,1),"mixed-indexed");
            auto b=r.Read();r.Check(b.gpu_completed==a.gpu_completed+2&&b.gpu_pending==0&&b.readback_maps==a.readback_maps+2,"Mixed diagnostic timing sample count");
            r.Check(b.world.diagnostic.cpu_submitted.samples==a.world.diagnostic.cpu_submitted.samples+1&&b.indexed.diagnostic.cpu_submitted.samples==a.indexed.diagnostic.cpu_submitted.samples+1&&b.world.diagnostic.cpu_present.samples==a.world.diagnostic.cpu_present.samples+1&&b.indexed.diagnostic.cpu_present.samples==a.indexed.diagnostic.cpu_present.samples+1&&b.world.diagnostic.gpu_upload_render.samples==a.world.diagnostic.gpu_upload_render.samples+1&&b.indexed.diagnostic.gpu_upload_render.samples==a.indexed.diagnostic.gpu_upload_render.samples+1,"Mixed diagnostic status/Present/GPU buckets");
            r.Fixed(a,b);r.PollStable(b);r.NoWait(ra);r.Record("mixed-exact",a,b,ra,device.GetRuntimeResourceDiagnostics());
        });
        r.Case("1000-mixed-ordinary-frames-fixed-instrumentation",[&]{
            r.Configure();auto world=World(device);auto indexed=Indexed(device);
            for(unsigned n=0;n<6;++n){auto w=r.Submit(world);r.Complete(w);auto i=r.Submit(indexed);r.Complete(i);}auto a=r.Read();auto ra=device.GetRuntimeResourceDiagnostics();
            for(unsigned n=0;n<1000;++n){auto t=n%2?r.Submit(indexed):r.Submit(world);r.Complete(t);}auto b=r.Read();auto rb=device.GetRuntimeResourceDiagnostics();
            r.Check(b.gpu_completed==a.gpu_completed+1000&&b.readback_maps==a.readback_maps+1000&&b.gpu_pending==0&&b.gpu_discarded==a.gpu_discarded,"1000-frame sample accounting");
            r.Check(b.world.normal.cpu_submitted.samples==a.world.normal.cpu_submitted.samples+500&&b.indexed.normal.cpu_submitted.samples==a.indexed.normal.cpu_submitted.samples+500&&b.world.normal.gpu_upload_render.samples==a.world.normal.gpu_upload_render.samples+500&&b.indexed.normal.gpu_upload_render.samples==a.indexed.normal.gpu_upload_render.samples+500&&b.world.normal.cpu_present.samples==a.world.normal.cpu_present.samples+500&&b.indexed.normal.cpu_present.samples==a.indexed.normal.cpu_present.samples+500,"1000-frame path/status/Present accounting");
            r.Check(rb.submissions==ra.submissions+1000&&rb.presentations==ra.presentations+1000&&rb.image_readbacks==ra.image_readbacks&&rb.resource_allocations==ra.resource_allocations&&rb.descriptor_heap_allocations==ra.descriptor_heap_allocations,"Timing loop allocated runtime resources or image readbacks");
            r.Fixed(a,b);r.PollStable(b);r.NoWait(ra);r.Record("ordinary1000",a,b,ra,rb);
        });
        r.Case("three-held-slots-no-premature-timestamp-read",[&]{
            r.Configure();auto world=World(device);auto indexed=Indexed(device);Hold gate(device,r.watchdog);auto a=r.Read();auto ra=device.GetRuntimeResourceDiagnostics();
            std::array<RuntimeFrameTicket,3> t{r.Submit(world,true),r.Submit(indexed,true),r.Submit(world,true)};for(const auto& ticket:t)r.Check(!device.IsRuntimeFrameComplete(ticket),"Timing held ticket completed prematurely");
            auto blocked=device.TrySubmitRuntimeIndexedFrame(indexed,true);r.Check(blocked.status==RuntimeSubmitStatus::WouldBlock&&!blocked.ticket.Valid()&&blocked.pressure==RuntimePressureReason::FrameSlot,"Timing fourth held frame not blocked");auto held=r.Read();
            r.Check(held.gpu_completed==a.gpu_completed&&held.readback_maps==a.readback_maps&&held.gpu_pending==3,"Held timestamp read occurred before fence");r.Check(held.world.held.cpu_submitted.samples==a.world.held.cpu_submitted.samples+2&&held.indexed.held.cpu_submitted.samples==a.indexed.held.cpu_submitted.samples+1&&held.indexed.held.cpu_would_block.samples==a.indexed.held.cpu_would_block.samples+1&&held.world.held.cpu_present.samples==a.world.held.cpu_present.samples&&held.indexed.held.cpu_present.samples==a.indexed.held.cpu_present.samples,"Held status counts/Present separation");
            r.PollStable(held);r.Record("held-before-release",a,held,ra,device.GetRuntimeResourceDiagnostics());gate.Release();
            r.Compare(t[0],WorldExpected(1),"held-world0");r.Compare(t[1],IndexedExpected(indexed,1,1),"held-indexed1");r.Compare(t[2],WorldExpected(1),"held-world2");auto b=r.Read();
            r.Check(b.gpu_completed==a.gpu_completed+3&&b.gpu_pending==0&&b.world.held.gpu_upload_render.samples==a.world.held.gpu_upload_render.samples+2&&b.indexed.held.gpu_upload_render.samples==a.indexed.held.gpu_upload_render.samples+1,"Released held samples missing or misclassified");r.PollStable(b);r.NoWait(ra);r.Record("held-released",held,b,ra,device.GetRuntimeResourceDiagnostics());
        });
        r.Case("suspended-and-before-execute-cpu-status-no-gpu-sample",[&]{
            r.Configure();auto world=World(device);auto indexed=Indexed(device);auto a=r.Read();auto ra=device.GetRuntimeResourceDiagnostics();device.Resize(0,0);
            auto ws=device.TrySubmitRuntimeWorldFrame(world);auto is=device.TrySubmitRuntimeIndexedFrame(indexed);r.Check(ws.status==RuntimeSubmitStatus::Suspended&&is.status==RuntimeSubmitStatus::Suspended&&!ws.ticket.Valid()&&!is.ticket.Valid(),"Timing suspended status mismatch");device.Resize(64,64);
            device.SetRuntimeFailureInjection(RuntimeFailurePoint::BeforeExecute);r.Reject([&]{device.TrySubmitRuntimeWorldFrame(world,true);},"Timing world preExecute failure absent");device.SetRuntimeFailureInjection(RuntimeFailurePoint::BeforeExecute);r.Reject([&]{device.TrySubmitRuntimeIndexedFrame(indexed,true);},"Timing indexed preExecute failure absent");auto b=r.Read();
            r.Check(b.world.normal.cpu_suspended.samples==a.world.normal.cpu_suspended.samples+1&&b.indexed.normal.cpu_suspended.samples==a.indexed.normal.cpu_suspended.samples+1&&b.world.diagnostic.cpu_failed.samples==a.world.diagnostic.cpu_failed.samples+1&&b.indexed.diagnostic.cpu_failed.samples==a.indexed.diagnostic.cpu_failed.samples+1,"Suspended/exception CPU timing counts");r.Check(b.gpu_completed==a.gpu_completed&&b.gpu_pending==0&&b.readback_maps==a.readback_maps&&b.gpu_discarded==a.gpu_discarded,"BeforeExecute published GPU sample");r.Check(b.world.diagnostic.cpu_present.samples==a.world.diagnostic.cpu_present.samples&&b.indexed.diagnostic.cpu_present.samples==a.indexed.diagnostic.cpu_present.samples,"BeforeExecute recorded Present");r.PollStable(b);r.Record("suspended-before-execute",a,b,ra,device.GetRuntimeResourceDiagnostics());
            auto w=r.Submit(world,true);r.Compare(w,WorldExpected(1),"before-retry-world");auto i=r.Submit(indexed,true);r.Compare(i,IndexedExpected(indexed,1,1),"before-retry-indexed");
        });
        for(bool world_path:{true,false})r.Case(world_path?"isolated-world-old-fence-quarantine":"isolated-indexed-old-fence-quarantine",[&]{
            HWND window=CreateWindowExW(0,L"STATIC",L"Runtime timing quarantine",WS_POPUP,0,0,64,64,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);r.Check(window!=nullptr,"Cannot create timing quarantine window");
            try{Device fresh;fresh.Initialize(window,64,64,options);Runner local(fresh,output);try{auto world=World(fresh);auto indexed=Indexed(fresh);const std::string label=world_path?"world":"indexed";
                for(unsigned n=0;n<6;++n){auto t=world_path?local.Submit(world):local.Submit(indexed);local.Complete(t);}auto a=local.Read();auto ra=fresh.GetRuntimeResourceDiagnostics();local.Check(a.gpu_completed==6&&a.gpu_pending==0,"Fresh old-fence warmup incomplete");
                Hold gate(fresh,local.watchdog);fresh.SetRuntimeFailureInjection(RuntimeFailurePoint::AfterExecute);local.Reject([&]{if(world_path)fresh.TrySubmitRuntimeWorldFrame(world,true);else fresh.TrySubmitRuntimeIndexedFrame(indexed,true);},"Timing postExecute injection absent");auto held=local.Read();auto failed=fresh.GetRuntimeResourceDiagnostics();local.Check(failed.failed&&failed.quarantined&&held.gpu_pending==1&&held.gpu_completed==a.gpu_completed&&held.readback_maps==a.readback_maps,"Old completed fence authorized quarantined timestamp");const auto& ab=world_path?a.world.held:a.indexed.held;const auto& hb=world_path?held.world.held:held.indexed.held;local.Check(hb.cpu_failed.samples==ab.cpu_failed.samples+1&&hb.cpu_present.samples==ab.cpu_present.samples,"Quarantined CPU failure/Present timing");local.PollStable(held);local.Record(label+"-quarantine-held",a,held,ra,failed);
                gate.Release();local.PollStable(held);local.NoWait(ra);fresh.ClearRuntimeResources();auto cleared=local.Read();local.Check(cleared.gpu_pending==0&&cleared.gpu_completed==a.gpu_completed&&cleared.readback_maps==a.readback_maps&&cleared.gpu_discarded==a.gpu_discarded+1,"Drain did not discard unfenced quarantined sample");local.Fixed(a,cleared);local.PollStable(cleared);local.Record(label+"-quarantine-cleared",held,cleared,failed,fresh.GetRuntimeResourceDiagnostics());local.Validation();
                world=World(fresh);indexed=Indexed(fresh);const auto recovery_resources=fresh.GetRuntimeResourceDiagnostics();Hold recovery_gate(fresh,local.watchdog);auto recovered_ticket=world_path?local.Submit(world,true):local.Submit(indexed,true);auto recovery_pending=local.Read();local.Check(recovery_pending.gpu_pending==1&&recovery_pending.gpu_completed==cleared.gpu_completed&&recovery_pending.readback_maps==cleared.readback_maps,"Recovered slot read timestamp before release");local.PollStable(recovery_pending);recovery_gate.Release();local.Compare(recovered_ticket,world_path?WorldExpected(1):IndexedExpected(indexed,1,1),label+"-quarantine-recovery");auto recovered=local.Read();local.Check(recovered.gpu_pending==0&&recovered.gpu_completed==cleared.gpu_completed+1&&recovered.readback_maps==cleared.readback_maps+1&&recovered.gpu_discarded==cleared.gpu_discarded,"Discarded timing slot failed exact once-only reuse");const auto& cb=world_path?cleared.world.held:cleared.indexed.held;const auto& rb=world_path?recovered.world.held:recovered.indexed.held;local.Check(rb.cpu_submitted.samples==cb.cpu_submitted.samples+1&&rb.gpu_upload_render.samples==cb.gpu_upload_render.samples+1&&rb.cpu_present.samples==cb.cpu_present.samples,"Recovered held slot misclassified");local.Fixed(cleared,recovered);local.PollStable(recovered);local.NoWait(recovery_resources);local.Record(label+"-quarantine-reused",cleared,recovered,recovery_resources,fresh.GetRuntimeResourceDiagnostics());local.Validation();fresh.ClearRuntimeResources();
            }catch(...){local.Record("isolated-failure",{},fresh.GetRuntimeTimingDiagnostics(),{},fresh.GetRuntimeResourceDiagnostics());r.Merge(local);throw;}r.Merge(local);}catch(...){DestroyWindow(window);throw;}DestroyWindow(window);
        });
        auto before=r.Read();device.ClearRuntimeResources();auto after=r.Read();r.Fixed(before,after);r.Check(after.gpu_pending==0,"Timing teardown retained pending sample");r.Validation();r.Write(true,"",before);return r.result;
    }catch(const std::exception& e){auto before=device.GetRuntimeTimingDiagnostics();try{device.ClearRuntimeResources();}catch(...){}r.Write(false,e.what(),before);throw;}
}
}
