// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#include "RuntimeResourceTests.h"
#include "kfx/renderer/IndexedWorldFallbackCodec.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <functional>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace kfx::render::dx12 {
namespace {
using Clock = std::chrono::steady_clock;
using Bytes = std::shared_ptr<const std::vector<uint8_t>>;
void Require(bool good, const char* text) { if (!good) throw std::runtime_error(text); }
void Pump()
{
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        Require(message.message != WM_QUIT, "Runtime test window closed");
        TranslateMessage(&message); DispatchMessageW(&message);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
}
IndexedWorldFallbackFrame Fixture()
{
    IndexedWorldFallbackFrame f;
    f.native_width=33; f.native_height=21; f.x=7; f.y=5; f.width=19; f.height=11;
    f.destination_clear_index=233; f.sequence=1; f.custom_draw_attempts_before=7;
    f.custom_draw_attempts_after=8; f.renderer_state_restored=true;
    for (size_t i=0;i<256;++i) {
        f.display_palette_rgba[i*4]=uint8_t(i*73+1);
        f.display_palette_rgba[i*4+1]=uint8_t(i*29+2);
        f.display_palette_rgba[i*4+2]=uint8_t(i*151+3);
        f.display_palette_rgba[i*4+3]=255;
    }
    f.indices.resize(size_t(f.width)*f.height);
    for (int y=0;y<f.height;++y) for (int x=0;x<f.width;++x)
        f.indices[size_t(y)*f.width+x]=(x+y)%5==0?0:(x+y)%7==0?255:uint8_t(x*17+y*31);
    f.indices[0]=0; f.indices[1]=255;
    return f;
}
IndexedWorldFallbackFrame Load(const std::filesystem::path& path)
{
    std::ifstream file(path,std::ios::binary|std::ios::ate);
    Require(bool(file),"Cannot open runtime indexed snapshot");
    const auto n=file.tellg();
    Require(n>=std::streamoff(IndexedWorldFallbackHeaderBytes+IndexedWorldFallbackPaletteBytes)
        &&n<=std::streamoff(IndexedWorldFallbackMaxSnapshotBytes),"Runtime indexed snapshot exceeds bounded length");
    std::vector<uint8_t> bytes(static_cast<size_t>(n)); file.seekg(0);
    file.read(reinterpret_cast<char*>(bytes.data()),std::streamsize(bytes.size()));
    Require(bool(file)&&file.peek()==std::char_traits<char>::eof(),"Runtime snapshot changed during bounded read");
    return DecodeIndexedWorldFallback(bytes);
}
Image Expected(const IndexedWorldFallbackFrame& f)
{
    // An independent direct byte lookup. No GPU asset atlas or frozen command
    // builder supplies the expected native composition.
    Image result{uint32_t(f.native_width),uint32_t(f.native_height),{}};
    result.rgba.resize(size_t(result.width)*result.height*4);
    for (uint32_t y=0;y<result.height;++y) for (uint32_t x=0;x<result.width;++x) {
        uint8_t index=f.destination_clear_index;
        if (x>=uint32_t(f.x)&&y>=uint32_t(f.y)&&x<uint32_t(f.x+f.width)&&y<uint32_t(f.y+f.height))
            index=f.indices[size_t(y-f.y)*f.width+x-f.x];
        std::copy_n(f.display_palette_rgba.data()+size_t(index)*4,4,
            result.rgba.data()+(size_t(y)*result.width+x)*4);
    }
    return result;
}
Bytes Source(const IndexedWorldFallbackFrame& f) { return std::make_shared<const std::vector<uint8_t>>(f.indices); }
Bytes Palette(const IndexedWorldFallbackFrame& f)
{
    return std::make_shared<const std::vector<uint8_t>>(f.display_palette_rgba.begin(),f.display_palette_rgba.end());
}
RuntimeAssetKey Key(uint64_t epoch,uint64_t id,RuntimeAssetKind kind,uint64_t generation=1,uint64_t revision=1)
{ return {epoch,id,generation,revision,kind}; }
RuntimeIndexedFrame Packet(Device& d,const IndexedWorldFallbackFrame& f,uint64_t epoch=1)
{
    RuntimeIndexedFrame p;
    p.native_width=uint32_t(f.native_width);p.native_height=uint32_t(f.native_height);
    p.x=f.x;p.y=f.y;p.width=uint32_t(f.width);p.height=uint32_t(f.height);p.clear_index=f.destination_clear_index;
    p.indices=d.PublishRuntimeAsset(Key(epoch,1,RuntimeAssetKind::Indexed8),{p.width,p.height},Source(f));
    p.palette=d.PublishRuntimeAsset(Key(epoch,2,RuntimeAssetKind::PaletteRGBA8),{256,1},Palette(f));
    return p;
}
void SaveBytes(const std::filesystem::path& path,const std::vector<uint8_t>& bytes)
{
    std::ofstream file(path,std::ios::binary);file.write(reinterpret_cast<const char*>(bytes.data()),std::streamsize(bytes.size()));
    Require(bool(file),"Cannot retain runtime test raw bytes");
}
void SaveBitmap(const std::filesystem::path& path,const Image& image)
{
    std::ofstream f(path,std::ios::binary);
    auto u16=[&](uint16_t v){f.put(char(v));f.put(char(v>>8));};
    auto u32=[&](uint32_t v){u16(uint16_t(v));u16(uint16_t(v>>16));};
    f.put('B');f.put('M');u32(uint32_t(54+image.rgba.size()));u16(0);u16(0);u32(54);u32(40);
    u32(image.width);u32(image.height);u16(1);u16(32);u32(0);u32(uint32_t(image.rgba.size()));
    u32(2835);u32(2835);u32(0);u32(0);
    for(uint32_t y=image.height;y-->0;)for(uint32_t x=0;x<image.width;++x){const auto q=(size_t(y)*image.width+x)*4;
        f.put(char(image.rgba[q+2]));f.put(char(image.rgba[q+1]));f.put(char(image.rgba[q]));f.put(char(image.rgba[q+3]));}
    Require(bool(f),"Cannot retain runtime test bitmap");
}
std::string Quote(const std::string& text)
{
    std::string out="\"";
    for(unsigned char c:text){if(c=='\\'||c=='\"')out+='\\';
        if(c<32){out+="\\u00";out+="0123456789abcdef"[c>>4];out+="0123456789abcdef"[c&15];}
        else out+=char(c);}
    return out+'\"';
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
      <<",\"pressureCollections\":"<<s.pressure_collections<<",\"pressureRecoveries\":"<<s.pressure_recoveries
      <<",\"fenceWaits\":"<<s.fence_waits<<",\"globalIdleCalls\":"<<s.global_idle_calls
      <<",\"imageReadbacks\":"<<s.image_readbacks<<",\"commandBytes\":"<<s.command_bytes
      <<",\"failed\":"<<(s.failed?"true":"false")<<",\"quarantined\":"<<(s.quarantined?"true":"false")<<'}';
}
struct Runner {
    Device& device;
    const std::filesystem::path& output;
    RuntimeResourceTestResult result;
    std::vector<std::string> records;
    RuntimeResourceBudgets budgets;
    uint32_t observed_pending_readers=0;
    RuntimeResourceDiagnostics before_cleanup;
    uint32_t cross_api_preparations=0;
    RuntimeResourceDiagnostics loop_before{},loop_after{};
    bool loop_snapshots_available=false,deterministic_hold_exercised=false;
    std::atomic<bool> reader_hold_watchdog_fired{false};
    Diagnostics validation_baseline{};
    std::string active_case{};
    std::vector<std::string> validation_observations{},retained_validation_messages{};
    Diagnostics ObserveValidation(const std::string& point)
    {
        // Take copies of messages, never reset cumulative validation counters.
        // The retained text is serialized and included again in any thrown
        // failure so consuming the Device message queue cannot hide evidence.
        auto messages=device.TakeValidationMessages();const auto diagnostics=device.GetDiagnostics();
        std::ostringstream item;item<<"{\"case\":"<<Quote(active_case)<<",\"point\":"<<Quote(point)
            <<",\"errors\":"<<diagnostics.validation_errors<<",\"warnings\":"<<diagnostics.validation_warnings<<",\"messages\":[";
        for(size_t i=0;i<messages.size();++i){if(i)item<<',';item<<Quote(messages[i]);retained_validation_messages.push_back(messages[i]);}
        item<<"]}";validation_observations.push_back(item.str());return diagnostics;
    }
    void CheckValidation(const Diagnostics& diagnostics,const std::string& point)
    {
        ++result.checks;
        if(diagnostics.validation_errors!=validation_baseline.validation_errors
            ||diagnostics.validation_warnings!=validation_baseline.validation_warnings)
            throw std::runtime_error("GPU validation first observed at "+active_case+"/"+point
                +"; asynchronous observation does not by itself identify the originating command");
    }
    void Check(bool good,const char* reason) { ++result.checks;Require(good,reason); }
    template<class Exception=std::invalid_argument,class Fn>void Reject(Fn&& fn,const char* reason)
    { bool rejected=false;try{fn();}catch(const Exception&){rejected=true;}Check(rejected,reason); }
    void Case(const std::string& name,const std::function<void()>& body)
    {
        active_case=name;
        const auto before=device.GetRuntimeResourceDiagnostics();
        const auto graphics_before=ObserveValidation("before-case");
        try{CheckValidation(graphics_before,"before-case");body();
            const auto graphics_after=ObserveValidation("after-case");CheckValidation(graphics_after,"after-case");
            ++result.cases;std::ostringstream s;s<<"{\"name\":"<<Quote(name)<<",\"passed\":true,\"before\":";
            Stats(s,before);s<<",\"after\":";Stats(s,device.GetRuntimeResourceDiagnostics());
            s<<",\"validationErrorsBefore\":"<<graphics_before.validation_errors
             <<",\"validationErrorsAfter\":"<<graphics_after.validation_errors
             <<",\"validationWarningsBefore\":"<<graphics_before.validation_warnings
             <<",\"validationWarningsAfter\":"<<graphics_after.validation_warnings<<'}';records.push_back(s.str());}
        catch(const std::exception& e){const auto graphics_after=ObserveValidation("failed-case");
            std::ostringstream s;s<<"{\"name\":"<<Quote(name)<<",\"passed\":false,\"error\":"<<Quote(e.what())
                <<",\"validationErrorsBefore\":"<<graphics_before.validation_errors<<",\"validationErrorsAfter\":"<<graphics_after.validation_errors
                <<",\"validationWarningsBefore\":"<<graphics_before.validation_warnings<<",\"validationWarningsAfter\":"<<graphics_after.validation_warnings<<'}';
            records.push_back(s.str());throw;}
    }
    void Bound()
    {
        const auto s=device.GetRuntimeResourceDiagnostics();
        Check(s.cpu_bytes<=budgets.cpu_bytes&&s.default_bytes<=budgets.default_bytes
            &&s.upload_bytes<=budgets.upload_bytes&&s.readback_bytes<=budgets.readback_bytes,"Actual runtime allocation budget exceeded");
        Check(s.asset_count<=budgets.assets&&s.descriptors<=budgets.descriptors
            &&s.diagnostic_tickets<=budgets.diagnostic_tickets,"Runtime descriptor/asset/ticket count exceeded budget");
        Check(s.pending_upload_bytes<=budgets.cpu_bytes&&s.retired_default_bytes<=budgets.default_bytes,"Retired/pending resources exceed owned budget");
    }
    void Configure(const RuntimeResourceBudgets& value={})
    {
        device.ClearRuntimeResources();budgets=value;device.ConfigureRuntimeResources(value);
    }
    RuntimeFrameTicket Submit(const RuntimeIndexedFrame& p,bool diagnostic)
    {
        const auto deadline=Clock::now()+std::chrono::seconds(20);
        const auto source_key=p.indices.Key(),palette_key=p.palette.Key();
        for(;;){const auto submitted=device.TrySubmitRuntimeIndexedFrame(p,diagnostic);
            Check(p.indices.Valid()&&p.palette.Valid()&&p.indices.Key()==source_key&&p.palette.Key()==palette_key,
                "Submission or pressure consumed caller packet");Bound();
            if(submitted.status==RuntimeSubmitStatus::Submitted){++result.submitted_frames;
                Check(submitted.ticket.Valid()&&submitted.ticket.HasReadback()==diagnostic,"Submitted runtime ticket/readback contract differs");return submitted.ticket;}
            Check(submitted.status==RuntimeSubmitStatus::WouldBlock,"Unexpected runtime suspension");
            Check(!submitted.ticket.Valid(),"Pressure published a partial ticket");
            Require(Clock::now()<deadline,"Runtime submit remained blocked after bounded polling");device.CollectRuntimeResources();Pump();}
    }
    Image Compare(RuntimeFrameTicket& ticket,const IndexedWorldFallbackFrame& f,const std::string& name)
    {
        Image actual;const auto deadline=Clock::now()+std::chrono::seconds(20);
        while(!device.TryReadbackRuntimeFrame(ticket,actual)){Require(Clock::now()<deadline,"Runtime diagnostic readback did not complete");Pump();}
        const auto expected=Expected(f);Check(actual.width==expected.width&&actual.height==expected.height,"Runtime native output extent differs");
        Check(actual.rgba.size()==expected.rgba.size(),"Runtime diagnostic length differs");uint64_t mismatches=0;
        for(size_t i=0;i<actual.rgba.size();++i)mismatches+=actual.rgba[i]!=expected.rgba[i];
        result.rgba_byte_checks+=actual.rgba.size();SaveBytes(output/("runtime-"+name+"-gpu.rgba"),actual.rgba);
        SaveBytes(output/("runtime-"+name+"-expected.rgba"),expected.rgba);SaveBitmap(output/("runtime-"+name+"-gpu.bmp"),actual);
        Check(mismatches==0,"Runtime native output differs from independent byte palette lookup");
        const auto observation=ObserveValidation("completed-readback/"+name);
        CheckValidation(observation,"completed-readback/"+name);
        ticket.Reset();device.CollectRuntimeResources();return actual;
    }
    void Report(bool passed,bool synthetic,const std::filesystem::path& input,const std::string& error)
    {
        std::ofstream f(output/"runtime-resource-tests.json");
        f<<"{\"schema\":1,\"scope\":\"Actual asynchronous Device indexed fallback resource slice; general world resources, live producer/backend integration and Gate3 completion are excluded\""
         <<",\"passed\":"<<(passed?"true":"false")<<",\"gate3Accepted\":false,\"callbackExecuted\":false"
         <<",\"syntheticInput\":"<<(synthetic?"true":"false")<<",\"input\":"<<Quote(input.string())
         <<",\"checks\":"<<result.checks<<",\"rgbaByteChecks\":"<<result.rgba_byte_checks
         <<",\"submittedFrames\":"<<result.submitted_frames<<",\"casesPassed\":"<<result.cases
         <<",\"successfulPresentations\":"<<before_cleanup.presentations
         <<",\"heldDiagnosticSubmissions\":"<<before_cleanup.held_submissions
         <<",\"frameCounterScope\":\"Submitted frames include held diagnostic executions; held work never counts as displayed presentations\""
         <<",\"observedPendingReadersAtReplacement\":"<<observed_pending_readers
         <<",\"threePendingReadersExercised\":"<<(observed_pending_readers==3||deterministic_hold_exercised?"true":"false")
         <<",\"threePendingReadersObservedWithoutHold\":"<<(observed_pending_readers==3?"true":"false")
         <<",\"deterministicDelayedCompletionTested\":"<<(deterministic_hold_exercised?"true":"false")
         <<",\"controlledPendingReaders\":"<<(deterministic_hold_exercised?3:0)
         <<",\"readerHoldWatchdogFired\":"<<(reader_hold_watchdog_fired.load()?"true":"false")
         <<",\"deviceRemovalTested\":false,\"resourceCreationFailureInjected\":false"
         <<",\"fenceSequenceExhaustionTested\":false,\"uploadRingWrapClaimed\":false"
         <<",\"crossApiFrozenPreparationTests\":"<<cross_api_preparations<<",\"error\":"<<Quote(error)<<",\"finalBeforeCleanup\":";Stats(f,before_cleanup);
        f<<",\"afterCleanup\":";Stats(f,device.GetRuntimeResourceDiagnostics());
        f<<",\"unchanged1000Loop\":";
        if(loop_snapshots_available){f<<"{\"frames\":1000,\"before\":";Stats(f,loop_before);f<<",\"after\":";Stats(f,loop_after);f<<'}';}
        else f<<"null";
        f<<",\"validationObservationScope\":\"Cumulative counters and copied messages at case/readback boundaries; first observation is not a causal attribution\","
         <<"\"validationObservations\":[";
        for(size_t i=0;i<validation_observations.size();++i){if(i)f<<',';f<<validation_observations[i];}
        f<<"],\"retainedValidationMessages\":[";
        for(size_t i=0;i<retained_validation_messages.size();++i){if(i)f<<',';f<<Quote(retained_validation_messages[i]);}
        f<<"],\"cases\":[";for(size_t i=0;i<records.size();++i){if(i)f<<',';f<<records[i];}f<<"]}\n";
        Require(bool(f),"Cannot retain runtime resource report");
    }
};
struct HiddenWindow {
    HWND handle=CreateWindowExW(0,L"STATIC",L"Runtime resource foreign owner",WS_POPUP,0,0,33,21,
        nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    HiddenWindow(){Require(handle!=nullptr,"Cannot create foreign-device diagnostic window");}
    ~HiddenWindow(){DestroyWindow(handle);}
};
// The watchdog may only call Release on this stable handle. Stop/join always
// precedes handle reset/destruction, including ordinary exception unwinding.
// Intervention is a test failure; it is never accepted as routine progress.
struct ReaderHoldGuard {
    RuntimeReaderHold hold;
    std::jthread watchdog;
    std::atomic<bool>& fired;
    ReaderHoldGuard(Device& device,std::atomic<bool>& flag):fired(flag)
    {
        const auto limit=Clock::now()+std::chrono::seconds(20);
        while(!device.TryBeginRuntimeReaderHold(hold)){
            Require(!hold.Valid(),"Blocked hold arm consumed empty output");
            Require(Clock::now()<limit,"Reader hold could not arm after bounded polling");
            device.CollectRuntimeResources();Pump();
        }
        Require(hold.Valid()&&!hold.Released(),"Reader hold did not arm");
        watchdog=std::jthread([this](std::stop_token stop){
            const auto deadline=Clock::now()+std::chrono::seconds(15);
            while(!stop.stop_requested()){
                if(Clock::now()>=deadline){fired.store(true);hold.Release();}
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        });
    }
    void StopWatchdog() { watchdog.request_stop();if(watchdog.joinable())watchdog.join(); }
    void Release()
    {
        const bool released=hold.Release();StopWatchdog();
        Require(!fired.load(),"Reader hold watchdog intervened");
        Require(released&&hold.Released(),"Reader hold release did not signal successfully");
    }
    ~ReaderHoldGuard(){StopWatchdog();hold.Release();}
};
}

RuntimeResourceTestResult RunRuntimeResourceTests(Device& device,const Options& options,
    const std::filesystem::path& input,const std::filesystem::path& output)
{
    std::filesystem::create_directories(output);
    const auto captured=input.empty()?Fixture():Load(input);const auto original=Fixture();
    Runner r{device,output,{},{},{},0,{},0};const auto initial=device.GetDiagnostics();
    r.validation_baseline=initial;
    try {
        r.Case(input.empty()?"synthetic-owned-input":"retained-real-kfxidx",[&]{
            r.Configure();device.Resize(uint32_t(captured.native_width),uint32_t(captured.native_height));
            auto p=Packet(device,captured);auto t=r.Submit(p,true);r.Compare(t,captured,"input");
            const auto before=device.GetRuntimeResourceDiagnostics();
            for(unsigned i=0;i<6;++i){auto repeated=r.Submit(p,false);}
            const auto after=device.GetRuntimeResourceDiagnostics();
            r.Check(after.source_upload_bytes==before.source_upload_bytes&&after.palette_upload_bytes==before.palette_upload_bytes,
                "Repeated actual-input frames retransmitted assets");
        });
        r.Configure();device.Resize(33,21);auto p=Packet(device,original);
        r.Case("odd-native-origin-opaque-zero-255-exact8bit",[&]{auto t=r.Submit(p,true);r.result.preview=r.Compare(t,original,"odd");});
        r.Case("publication-immutable-alias-and-conflict",[&]{
            auto mutable_bytes=std::make_shared<std::vector<uint8_t>>(original.indices);const Bytes alias=mutable_bytes;
            auto lease=device.PublishRuntimeAsset(Key(1,3,RuntimeAssetKind::Indexed8),{19,11},alias);
            (*mutable_bytes)[0]=99;mutable_bytes.reset();auto probe=p;probe.indices=lease;
            auto t=r.Submit(probe,true);r.Compare(t,original,"sealed-publication");
            const auto before=device.GetRuntimeResourceDiagnostics();
            auto reused=device.PublishRuntimeAsset(Key(1,3,RuntimeAssetKind::Indexed8),{19,11},Source(original));
            r.Check(reused.Valid()&&device.GetRuntimeResourceDiagnostics().publication_reuses==before.publication_reuses+1,
                "Identical publication did not reuse immutable version");
            r.Reject([&]{device.PublishRuntimeAsset(Key(1,3,RuntimeAssetKind::Indexed8),{19,11},alias);},"Same key with different bytes accepted");
        });
        r.Case("evicted-version-content-conflict",[&]{
            auto disposable=device.PublishRuntimeAsset(Key(1,7,RuntimeAssetKind::Indexed8),{19,11},Source(original));
            disposable.Reset();device.CollectRuntimeResources(true);
            auto changed=original;changed.indices[1]=12;
            r.Reject([&]{device.PublishRuntimeAsset(Key(1,7,RuntimeAssetKind::Indexed8),{19,11},Source(changed));},
                "Evicted same-key version was rebound to different content");
        });
        r.Case("unchanged-1000-no-upload-allocation-idle-readback",[&]{
            for(unsigned i=0;i<6;++i){auto warmup=r.Submit(p,false);}
            const auto before=device.GetRuntimeResourceDiagnostics();
            r.loop_before=before;
            for(unsigned i=0;i<1000;++i){auto t=r.Submit(p,false);r.Check(!t.HasReadback(),"Unrequested output was read back");}
            const auto after=device.GetRuntimeResourceDiagnostics();
            r.loop_after=after;r.loop_snapshots_available=true;
            r.Check(after.submissions==before.submissions+1000,"Unchanged loop did not submit exactly1000 frames");
            r.Check(after.source_upload_bytes==before.source_upload_bytes&&after.palette_upload_bytes==before.palette_upload_bytes
                &&after.aligned_upload_bytes==before.aligned_upload_bytes,"Unchanged loop uploaded asset data");
            r.Check(after.resource_allocations==before.resource_allocations&&after.descriptor_heap_allocations==before.descriptor_heap_allocations,
                "Unchanged loop did not reach allocation/descriptor plateau");
            r.Check(after.global_idle_calls==before.global_idle_calls&&after.image_readbacks==before.image_readbacks
                &&after.fence_waits==before.fence_waits,"Normal submissions waited/idled or read back images");
            auto t=r.Submit(p,true);r.Compare(t,original,"unchanged-final");
        });
        r.Case("source-only-revision-old-admission-rejected",[&]{
            auto changed=original;changed.indices[0]=147;auto old=p;
            p.indices=device.PublishRuntimeAsset(Key(1,1,RuntimeAssetKind::Indexed8,1,2),{19,11},Source(changed));
            const auto before=device.GetRuntimeResourceDiagnostics();auto t=r.Submit(p,true);r.Compare(t,changed,"source-revision");
            const auto after=device.GetRuntimeResourceDiagnostics();
            r.Check(after.source_upload_bytes-before.source_upload_bytes==changed.indices.size()
                &&after.palette_upload_bytes==before.palette_upload_bytes,"Source-only revision retransmitted unrelated asset bytes");
            r.Reject([&]{device.TrySubmitRuntimeIndexedFrame(old);},"Old source revision admitted after replacement");
        });
        r.Case("palette-only-1024-logical-bytes",[&]{
            auto changed=original;changed.indices[0]=147;changed.display_palette_rgba[147*4]=13;changed.display_palette_rgba[147*4+1]=97;
            p.palette=device.PublishRuntimeAsset(Key(1,2,RuntimeAssetKind::PaletteRGBA8,1,2),{256,1},Palette(changed));
            const auto before=device.GetRuntimeResourceDiagnostics();auto t=r.Submit(p,true);r.Compare(t,changed,"palette-revision");
            const auto after=device.GetRuntimeResourceDiagnostics();r.Check(after.palette_upload_bytes-before.palette_upload_bytes==1024
                &&after.source_upload_bytes==before.source_upload_bytes,"Palette-only update retransmitted source indices");
        });
        r.Case("three-retained-versions-generation-epoch",[&]{
            r.Configure();auto f=captured;
            device.Resize(uint32_t(f.native_width),uint32_t(f.native_height));
            const RuntimeAssetDesc desc{uint32_t(f.width),uint32_t(f.height)};
            std::array<RuntimeFrameTicket,3> tickets;
            std::array<IndexedWorldFallbackFrame,3> expected{f,f,f};auto packet=Packet(device,f);
            tickets[0]=r.Submit(packet,true);auto old=packet;
            f.indices[0]=83;expected[1]=f;packet.indices=device.PublishRuntimeAsset(Key(1,1,RuntimeAssetKind::Indexed8,1,2),desc,Source(f));
            tickets[1]=r.Submit(packet,true);
            f.indices[1]=41;f.display_palette_rgba[41*4]=87;expected[2]=f;
            packet.indices=device.PublishRuntimeAsset(Key(1,1,RuntimeAssetKind::Indexed8,2,1),desc,Source(f));
            packet.palette=device.PublishRuntimeAsset(Key(1,2,RuntimeAssetKind::PaletteRGBA8,2,1),{256,1},Palette(f));
            tickets[2]=r.Submit(packet,true);
            for(const auto& t:tickets)r.observed_pending_readers+=!device.IsRuntimeFrameComplete(t);
            r.Reject([&]{device.TrySubmitRuntimeIndexedFrame(old);},"Old generation admitted");
            device.SetRuntimeEpoch(2);
            r.Reject([&]{device.TrySubmitRuntimeIndexedFrame(packet);},"Previous current lease admitted after epoch reset");
            old={};packet={};
            r.Reject([&]{device.PublishRuntimeAsset(Key(1,1,RuntimeAssetKind::Indexed8,3,1),desc,Source(f));},"Old epoch publication admitted");
            device.CollectRuntimeResources(true);for(size_t i=0;i<tickets.size();++i)r.Compare(tickets[i],expected[i],"retained-"+std::to_string(i));
            auto fresh=Packet(device,captured,2);auto t=r.Submit(fresh,true);r.Compare(t,captured,"fresh-epoch");
        });
        device.Resize(33,21);
        r.Case("controlled-three-pending-epoch-replacement-retention",[&]{
            r.Configure();device.Resize(uint32_t(captured.native_width),uint32_t(captured.native_height));
            ReaderHoldGuard gate(device,r.reader_hold_watchdog_fired);
            const auto before=device.GetRuntimeResourceDiagnostics();
            auto f=captured;const RuntimeAssetDesc desc{uint32_t(f.width),uint32_t(f.height)};
            std::array<IndexedWorldFallbackFrame,3> expected{f,f,f};
            std::array<RuntimeFrameTicket,3> tickets;auto packet=Packet(device,f);
            tickets[0]=r.Submit(packet,true);
            f.indices[0]=83;expected[1]=f;
            packet.indices=device.PublishRuntimeAsset(Key(1,1,RuntimeAssetKind::Indexed8,1,2),desc,Source(f));
            tickets[1]=r.Submit(packet,true);
            f.indices[1]=41;f.display_palette_rgba[41*4]=87;expected[2]=f;
            packet.indices=device.PublishRuntimeAsset(Key(1,1,RuntimeAssetKind::Indexed8,2,1),desc,Source(f));
            packet.palette=device.PublishRuntimeAsset(Key(1,2,RuntimeAssetKind::PaletteRGBA8,2,1),{256,1},Palette(f));
            tickets[2]=r.Submit(packet,true);
            for(const auto& t:tickets)r.Check(!device.IsRuntimeFrameComplete(t),"Held reader completed before release");
            const auto held=device.GetRuntimeResourceDiagnostics();
            r.Check(held.reader_hold_active&&held.reader_hold_slots==3
                &&held.held_submissions==before.held_submissions+3&&held.presentations==before.presentations,
                "Controlled readers did not use three real slots without presentation");
            const auto source_key=packet.indices.Key(),palette_key=packet.palette.Key();
            const auto fourth=device.TrySubmitRuntimeIndexedFrame(packet,true);
            r.Check(fourth.status==RuntimeSubmitStatus::WouldBlock&&!fourth.ticket.Valid(),
                "Fourth held reader did not return bounded slot pressure");
            r.Check(packet.indices.Valid()&&packet.palette.Valid()&&packet.indices.Key()==source_key
                &&packet.palette.Key()==palette_key,"Held-slot pressure consumed producer input");
            device.SetRuntimeEpoch(2);auto fresh=Packet(device,captured,2);
            r.Reject([&]{device.TrySubmitRuntimeIndexedFrame(packet);},"Stale held epoch admitted new work");
            packet={};fresh={};device.CollectRuntimeResources(true);
            const auto retired=device.GetRuntimeResourceDiagnostics();
            r.Check(retired.default_bytes==held.default_bytes&&retired.upload_bytes==held.upload_bytes
                &&retired.readback_bytes==held.readback_bytes&&retired.retired_default_bytes>0,
                "Epoch reset/caller release/collection freed an incomplete GPU reader");
            for(const auto& t:tickets)r.Check(!device.IsRuntimeFrameComplete(t),"Reader ceased to be pending while gate remained held");
            r.Check(retired.global_idle_calls==before.global_idle_calls&&retired.fence_waits==before.fence_waits
                &&retired.image_readbacks==before.image_readbacks,"Held replacement inserted idle/wait/readback");
            r.Bound();gate.Release();
            for(size_t i=0;i<tickets.size();++i)r.Compare(tickets[i],expected[i],"controlled-retained-"+std::to_string(i));
            device.CollectRuntimeResources(true);
            const auto reclaimed=device.GetRuntimeResourceDiagnostics();
            r.Check(!reclaimed.reader_hold_active&&!reclaimed.reader_hold_retained
                &&reclaimed.retired_default_bytes==0&&reclaimed.pending_upload_bytes==0
                &&reclaimed.upload_bytes==0&&reclaimed.readback_bytes==0&&reclaimed.diagnostic_tickets==0,
                "Completed held readers were not reclaimed before normal retry");
            RuntimeIndexedFrame resumed;
            resumed.native_width=uint32_t(captured.native_width);resumed.native_height=uint32_t(captured.native_height);
            resumed.x=captured.x;resumed.y=captured.y;resumed.width=uint32_t(captured.width);
            resumed.height=uint32_t(captured.height);resumed.clear_index=captured.destination_clear_index;
            // Epoch2 revision1 was published then evicted above. Re-admission
            // must use new revisions, never silently rebind those retired keys.
            resumed.indices=device.PublishRuntimeAsset(Key(2,1,RuntimeAssetKind::Indexed8,1,2),desc,Source(captured));
            resumed.palette=device.PublishRuntimeAsset(Key(2,2,RuntimeAssetKind::PaletteRGBA8,1,2),{256,1},Palette(captured));
            auto normal=r.Submit(resumed,true);r.Compare(normal,captured,"controlled-resumed");
            const auto completed=device.GetRuntimeResourceDiagnostics();
            r.Check(completed.held_submissions==before.held_submissions+3
                &&completed.submissions==before.submissions+4&&completed.presentations==before.presentations+1,
                "Post-hold retry did not produce exactly one real normal presentation");
            r.Check(completed.global_idle_calls==before.global_idle_calls&&completed.fence_waits==before.fence_waits
                &&completed.retired_default_bytes==0&&completed.pending_upload_bytes==0,
                "Post-hold retry idled/waited or retained stale reader uploads");
            r.Check(!r.reader_hold_watchdog_fired.load(),"Watchdog rescued controlled-reader test");
            r.deterministic_hold_exercised=true;
        });
        device.Resize(33,21);
        r.Case("zero-submission-reader-hold-release-and-clear",[&]{
            r.Configure();
            {
                ReaderHoldGuard gate(device,r.reader_hold_watchdog_fired);
                const auto before=device.GetRuntimeResourceDiagnostics();RuntimeReaderHold nested;
                r.Reject<std::logic_error>([&]{device.TryBeginRuntimeReaderHold(nested);},"Nested reader gate was admitted");
                r.Check(!nested.Valid(),"Rejected nested gate consumed its empty output");
                r.Reject<std::logic_error>([&]{device.WaitIdle();},"WaitIdle entered a held queue");
                r.Reject<std::logic_error>([&]{device.Resize(33,21);},"Resize entered a held queue");
                gate.Release();
                const auto after=device.GetRuntimeResourceDiagnostics();
                r.Check(after.submissions==before.submissions&&after.reader_hold_slots==0,
                    "Zero-submit gate created a phantom frame/slot");
                r.Check(after.reader_hold_releases==before.reader_hold_releases+1,"Zero-submit gate did not release");
            }
            {
                ReaderHoldGuard gate(device,r.reader_hold_watchdog_fired);
                gate.StopWatchdog();device.ClearRuntimeResources();
                r.Check(gate.hold.Released(),"Explicit clear did not release zero-submit gate before draining");
                const auto after=device.GetRuntimeResourceDiagnostics();
                r.Check(!after.reader_hold_active&&!after.reader_hold_retained
                    &&after.default_bytes==0&&after.upload_bytes==0,"Zero-submit clear retained gate/resources");
            }
        });
        r.Case("held-before-execute-rollback-does-not-consume-slot",[&]{
            r.Configure();auto packet=Packet(device,original);
            ReaderHoldGuard gate(device,r.reader_hold_watchdog_fired);
            const auto before=device.GetRuntimeResourceDiagnostics();
            device.SetRuntimeFailureInjection(RuntimeFailurePoint::BeforeExecute);
            r.Reject<std::runtime_error>([&]{device.TrySubmitRuntimeIndexedFrame(packet,true);},
                "Held pre-execution injection did not fail");
            const auto rollback=device.GetRuntimeResourceDiagnostics();
            r.Check(rollback.reader_hold_active&&rollback.reader_hold_slots==0
                &&rollback.submissions==before.submissions&&rollback.held_submissions==before.held_submissions
                &&rollback.default_bytes==before.default_bytes&&rollback.upload_bytes==before.upload_bytes,
                "Pre-execution failure consumed held slot or retained partial allocations");
            std::array<RuntimeFrameTicket,3> tickets;
            for(auto& t:tickets)t=r.Submit(packet,true);
            r.Check(device.GetRuntimeResourceDiagnostics().reader_hold_slots==3,
                "Failed recording prevented three subsequent held submissions");
            for(const auto& t:tickets)r.Check(!device.IsRuntimeFrameComplete(t),"Held retry completed before release");
            gate.Release();
            for(size_t i=0;i<tickets.size();++i)r.Compare(tickets[i],original,"held-rollback-"+std::to_string(i));
            r.Check(!r.reader_hold_watchdog_fired.load(),"Watchdog rescued held rollback test");
        });
        r.Case("invalid-key-shape-palette-packet-no-allocation",[&]{
            r.Configure();auto packet=Packet(device,original);const auto before=device.GetDiagnostics();
            for(unsigned field=0;field<4;++field)for(uint64_t bad:{uint64_t(0),std::numeric_limits<uint64_t>::max()}){
                auto key=Key(1,9,RuntimeAssetKind::Indexed8);if(field==0)key.epoch=bad;if(field==1)key.logical_id=bad;
                if(field==2)key.generation=bad;
                if(field==3)key.revision=bad;
                r.Reject([&]{device.PublishRuntimeAsset(key,{19,11},Source(original));},"Zero/wrapped asset identity admitted");}
            r.Reject([&]{device.SetRuntimeEpoch(1);},"Nonincreasing epoch admitted");
            r.Reject([&]{device.SetRuntimeEpoch(std::numeric_limits<uint64_t>::max());},"Reserved wrap epoch admitted");
            auto nonopaque=original;nonopaque.display_palette_rgba[3]=0;
            r.Reject([&]{device.PublishRuntimeAsset(Key(1,8,RuntimeAssetKind::PaletteRGBA8),{256,1},Palette(nonopaque));},"Nonopaque runtime palette admitted");
            r.Reject([&]{device.PublishRuntimeAsset(Key(1,8,RuntimeAssetKind::Indexed8),{20,11},Source(original));},"Mismatched source length admitted");
            auto bad=packet;bad.x=-1;r.Reject([&]{device.TrySubmitRuntimeIndexedFrame(bad);},"Negative origin admitted");
            bad=packet;bad.x=15;r.Reject([&]{device.TrySubmitRuntimeIndexedFrame(bad);},"Viewport beyond native bounds admitted");
            bad=packet;bad.native_width=16385;r.Reject([&]{device.TrySubmitRuntimeIndexedFrame(bad);},"Oversize target admitted");
            bad=packet;bad.indices.Reset();r.Reject([&]{device.TrySubmitRuntimeIndexedFrame(bad);},"Empty source lease admitted");
            r.Check(device.GetDiagnostics().allocation_bytes==before.allocation_bytes
                &&device.GetDiagnostics().allocation_count==before.allocation_count,"Malformed requests allocated GPU resources");
            auto t=r.Submit(packet,false);Image out;r.Reject([&]{device.TryReadbackRuntimeFrame(t,out);},"Unrequested readback accepted");
        });
        r.Case("foreign-device-leases-and-tickets",[&]{
            r.Configure();HiddenWindow window;Device foreign;foreign.Initialize(window.handle,33,21,options);
            auto own=Packet(device,original),other=Packet(foreign,original);auto mixed=own;mixed.palette=other.palette;
            r.Reject([&]{device.TrySubmitRuntimeIndexedFrame(mixed);},"Foreign palette lease admitted");
            auto t=r.Submit(own,true);Image out;r.Reject([&]{foreign.TryReadbackRuntimeFrame(t,out);},"Foreign ticket readback accepted");
            r.Compare(t,original,"foreign-control");mixed={};other={};own={};foreign.ClearRuntimeResources();
            const auto foreign_diagnostics=foreign.GetDiagnostics();
            r.Check(foreign_diagnostics.validation_errors==0&&foreign_diagnostics.validation_warnings==0,
                "Foreign ownership test emitted GPU validation diagnostics");
        });
        for(unsigned profile=0;profile<3;++profile)r.Case(profile==0?"cpu-publication-budget":profile==1?"aligned-upload-budget-backpressure":"default-allocation-budget-backpressure",[&]{
            RuntimeResourceBudgets limits;if(profile==0)limits.cpu_bytes=1024;if(profile==1)limits.upload_bytes=512;
            if(profile==2)limits.default_bytes=65536;
            r.Configure(limits);
            if(profile==0){auto source=device.PublishRuntimeAsset(Key(1,1,RuntimeAssetKind::Indexed8),{19,11},Source(original));
                r.Reject<std::length_error>([&]{device.PublishRuntimeAsset(Key(1,2,RuntimeAssetKind::PaletteRGBA8),{256,1},Palette(original));},"CPU budget permitted excess publication");
                r.Check(source.Valid()&&device.GetRuntimeResourceDiagnostics().cpu_bytes==original.indices.size(),"Publication pressure damaged admitted source");}
            else{auto packet=Packet(device,original);const auto before=device.GetDiagnostics();const auto s=device.TrySubmitRuntimeIndexedFrame(packet,true);
                r.Check(s.status==RuntimeSubmitStatus::WouldBlock&&!s.ticket.Valid(),"GPU budget did not produce clean backpressure");
                r.Check(packet.indices.Valid()&&packet.palette.Valid(),"Backpressure consumed packet");
                r.Check(device.GetDiagnostics().allocation_count==before.allocation_count,"Oversize reservation allocated before bounds");}
            r.Bound();
        });
        r.Case("diagnostic-ticket-pressure-release-retry",[&]{
            RuntimeResourceBudgets limits;limits.diagnostic_tickets=3;r.Configure(limits);auto packet=Packet(device,original);
            std::array<RuntimeFrameTicket,3> held;for(auto& t:held)t=r.Submit(packet,true);
            const auto denied=device.TrySubmitRuntimeIndexedFrame(packet,true);
            r.Check(denied.status==RuntimeSubmitStatus::WouldBlock&&!denied.ticket.Valid(),"Retained output budget did not block");
            r.Check(packet.indices.Valid()&&packet.palette.Valid(),"Ticket pressure consumed packet");
            r.Compare(held[0],original,"pressure-old");device.CollectRuntimeResources();
            auto retry=r.Submit(packet,true);r.Compare(retry,original,"pressure-retry");
            for(size_t i=1;i<held.size();++i)r.Compare(held[i],original,"pressure-retained-"+std::to_string(i));
        });
        r.Case("completed-descriptor-segment-automatic-pressure-recovery",[&]{
            RuntimeResourceBudgets limits;limits.descriptors=3;r.Configure(limits);auto packet=Packet(device,original);
            auto first=r.Submit(packet,true);r.Compare(first,original,"descriptor-first");
            const auto before=device.GetRuntimeResourceDiagnostics();
            // Compare completed the first reader. Its cached heap may now be
            // reclaimed, but the cap still permits only one three-slot segment.
            auto retry=r.Submit(packet,true);r.Compare(retry,original,"descriptor-completed-retry");
            const auto after=device.GetRuntimeResourceDiagnostics();
            r.Check(after.pressure_collections==before.pressure_collections+1
                &&after.pressure_recoveries==before.pressure_recoveries+1&&after.descriptors==3,
                "Completed descriptor cache did not recover automatically within the three-descriptor cap");
            r.Check(after.source_upload_bytes==before.source_upload_bytes&&after.palette_upload_bytes==before.palette_upload_bytes,
                "Descriptor recovery retransmitted still-leased source/palette assets");
            r.Check(after.global_idle_calls==before.global_idle_calls&&after.fence_waits==before.fence_waits,
                "Completed descriptor recovery inserted an internal idle/wait");r.Bound();
        });
        r.Case("pending-descriptor-segment-pressure-release-exact-retry",[&]{
            RuntimeResourceBudgets limits;limits.descriptors=3;r.Configure(limits);auto packet=Packet(device,original);
            ReaderHoldGuard gate(device,r.reader_hold_watchdog_fired);
            auto pending=r.Submit(packet,true);const auto before=device.GetRuntimeResourceDiagnostics();
            const auto source_key=packet.indices.Key(),palette_key=packet.palette.Key();
            r.Check(before.reader_hold_slots==1&&before.descriptors==3&&!device.IsRuntimeFrameComplete(pending),
                "Descriptor pressure fixture did not retain one actual pending reader");
            const auto denied=device.TrySubmitRuntimeIndexedFrame(packet,true);
            const auto held=device.GetRuntimeResourceDiagnostics();
            r.Check(denied.status==RuntimeSubmitStatus::WouldBlock&&denied.pressure==RuntimePressureReason::Descriptors&&!denied.ticket.Valid(),
                "Pending descriptor segment did not return specific non-consuming descriptor pressure");
            r.Check(held.descriptors==3&&held.reader_hold_slots==1&&!device.IsRuntimeFrameComplete(pending)
                &&held.cpu_bytes==before.cpu_bytes&&held.default_bytes==before.default_bytes
                &&held.upload_bytes==before.upload_bytes&&held.readback_bytes==before.readback_bytes
                &&held.asset_count==before.asset_count&&held.diagnostic_tickets==before.diagnostic_tickets,
                "Descriptor pressure reclaimed pending frame owners");
            r.Check(held.resource_allocations==before.resource_allocations
                &&held.descriptor_heap_allocations==before.descriptor_heap_allocations&&held.submissions==before.submissions,
                "Denied pending descriptor reservation allocated or submitted partial work");
            r.Check(packet.indices.Valid()&&packet.palette.Valid()&&packet.indices.Key()==source_key&&packet.palette.Key()==palette_key,
                "Descriptor pressure consumed caller assets");r.Bound();
            gate.Release();r.Compare(pending,original,"descriptor-held-old");
            auto retry=r.Submit(packet,true);r.Compare(retry,original,"descriptor-held-retry");
            const auto after=device.GetRuntimeResourceDiagnostics();
            r.Check(after.descriptors==3&&after.presentations==before.presentations+1
                &&after.global_idle_calls==before.global_idle_calls&&after.fence_waits==before.fence_waits,
                "Released descriptor pressure did not resume exactly without internal idle/wait");
            r.Check(!r.reader_hold_watchdog_fired.load(),"Watchdog rescued descriptor pressure case");r.Bound();
        });
        r.Case("source-replacement-bounded-reclamation",[&]{
            RuntimeResourceBudgets limits;limits.upload_bytes=3*65536;limits.default_bytes=2*1024*1024;
            r.Configure(limits);auto f=original;auto packet=Packet(device,f);auto first=r.Submit(packet,true);r.Compare(first,f,"reclaim-first");
            const auto before=device.GetRuntimeResourceDiagnostics();
            for(uint64_t revision=2;revision<=65;++revision){f.indices[revision%f.indices.size()]=uint8_t(revision);
                packet.indices=device.PublishRuntimeAsset(Key(1,1,RuntimeAssetKind::Indexed8,1,revision),{19,11},Source(f));
                auto t=r.Submit(packet,revision%16==0||revision==65);if(t.HasReadback())r.Compare(t,f,"reclaim-"+std::to_string(revision));
                device.CollectRuntimeResources(true);r.Bound();}
            const auto after=device.GetRuntimeResourceDiagnostics();r.Check(after.source_upload_bytes-before.source_upload_bytes==64*f.indices.size()
                &&after.palette_upload_bytes==before.palette_upload_bytes,"Repeated source revisions uploaded wrong logical bytes");
            r.Check(after.global_idle_calls==before.global_idle_calls&&after.fence_waits==before.fence_waits,"Reclamation inserted routine idle/wait");
        });
        r.Case("suspended-does-not-consume-frame",[&]{
            r.Configure();auto packet=Packet(device,original);device.Resize(0,0);
            const auto s=device.TrySubmitRuntimeIndexedFrame(packet,true);r.Check(s.status==RuntimeSubmitStatus::Suspended&&!s.ticket.Valid(),"Minimized frame did not suspend");
            r.Check(packet.indices.Valid()&&packet.palette.Valid(),"Suspension consumed input leases");
            device.Resize(33,21);auto t=r.Submit(packet,true);r.Compare(t,original,"restored");
        });
        r.Case("accepted-native-readback-survives-suspend-and-resize",[&]{
            r.Configure();auto packet=Packet(device,original);
            ReaderHoldGuard gate(device,r.reader_hold_watchdog_fired);
            auto ticket=r.Submit(packet,true);
            r.Check(!device.IsRuntimeFrameComplete(ticket),"Resize fixture did not retain an actually pending readback");
            gate.Release();
            device.Resize(0,0);
            r.Check(ticket.Valid()&&ticket.HasReadback(),"Suspension invalidated the accepted readback");
            device.Resize(uint32_t(original.native_width)+14,uint32_t(original.native_height)+8);
            r.Check(ticket.Valid()&&ticket.HasReadback(),"Drawable resize invalidated the accepted native readback");
            // Read the original accepted image, without submitting at the new size.
            // Compare also checks its native dimensions against the source fixture.
            r.Compare(ticket,original,"accepted-readback-after-resize");
            device.Resize(uint32_t(original.native_width),uint32_t(original.native_height));
        });
        r.Case("explicit-drain-invalidates-surviving-handles",[&]{
            r.Configure();auto packet=Packet(device,original);auto ticket=r.Submit(packet,true);
            device.ClearRuntimeResources();
            r.Check(!packet.indices.Valid()&&!packet.palette.Valid()&&!ticket.Valid(),
                "Explicit teardown left old lease/ticket identities valid");
            r.Reject([&]{device.TrySubmitRuntimeIndexedFrame(packet);},"Cleared lease admitted to a new runtime owner");
            const auto after=device.GetRuntimeResourceDiagnostics();
            r.Check(after.cpu_bytes==0&&after.default_bytes==0&&after.upload_bytes==0&&after.readback_bytes==0,
                "Surviving invalid handles retained cleared runtime allocations");
        });
        r.Case("before-execute-rollback-and-retry",[&]{
            r.Configure();auto packet=Packet(device,original);const auto before=device.GetRuntimeResourceDiagnostics();
            device.SetRuntimeFailureInjection(RuntimeFailurePoint::BeforeExecute);
            r.Reject<std::runtime_error>([&]{device.TrySubmitRuntimeIndexedFrame(packet,true);},"Before-execute injection did not fail");
            const auto after=device.GetRuntimeResourceDiagnostics();r.Check(after.submissions==before.submissions&&!after.failed&&!after.quarantined,
                "Pre-execution rollback published work or quarantined safe state");
            r.Check(after.default_bytes==before.default_bytes&&after.upload_bytes==before.upload_bytes&&after.readback_bytes==before.readback_bytes,
                "Pre-execution rollback retained partially allocated frame");
            auto t=r.Submit(packet,true);r.Compare(t,original,"rollback-retry");
        });
        r.Case("after-execute-owner-quarantine-fail-closed",[&]{
            r.Configure();
            Image atlas{256,256,std::vector<uint8_t>(256*256*4)};atlas.rgba[3]=255;
            PalettePrimitive one;one.sprite={0,0,1,1,0,0,1,1,PaletteBlend::Opaque};
            auto frozen=device.PrepareFrozenIndexedFrame(33,21,233,atlas,{one},original.display_palette_rgba,false);
            ++r.cross_api_preparations;
            auto packet=Packet(device,original);device.SetRuntimeFailureInjection(RuntimeFailurePoint::AfterExecute);
            r.Reject<std::runtime_error>([&]{device.TrySubmitRuntimeIndexedFrame(packet,true);},"After-execute injection did not fail");
            const auto before=device.GetRuntimeResourceDiagnostics();r.Check(before.failed&&before.quarantined,"Submitted failure did not quarantine owners");
            r.Reject<std::logic_error>([&]{device.TrySubmitRuntimeIndexedFrame(packet);},"Failed runtime accepted valid new work");
            packet={};device.CollectRuntimeResources(true);const auto after=device.GetRuntimeResourceDiagnostics();
            r.Check(after.quarantined&&after.default_bytes==before.default_bytes&&after.upload_bytes==before.upload_bytes,
                "Submitted owners were released before explicit drain");
            r.Reject<std::logic_error>([&]{device.PresentFrozenFrame(frozen);},"Frozen presentation reused quarantined runtime frame allocator");
            frozen.Reset();
        });
        const auto final=device.GetDiagnostics();r.Check(final.validation_errors==initial.validation_errors
            &&final.validation_warnings==initial.validation_warnings,"Runtime test emitted GPU validation diagnostics");
        r.before_cleanup=device.GetRuntimeResourceDiagnostics();p={};device.ClearRuntimeResources();
        r.Check(device.GetRuntimeResourceDiagnostics().default_bytes==0&&device.GetRuntimeResourceDiagnostics().upload_bytes==0
            &&device.GetRuntimeResourceDiagnostics().readback_bytes==0&&device.GetRuntimeResourceDiagnostics().cpu_bytes==0,
            "Explicit runtime teardown failed to release owned allocations");
        r.CheckValidation(r.ObserveValidation("after-explicit-cleanup"),"after-explicit-cleanup");
        r.Report(true,input.empty(),input,{});
        return r.result;
    } catch(const std::exception& e){
        r.ObserveValidation("suite-failure");r.before_cleanup=device.GetRuntimeResourceDiagnostics();
        std::string failure=e.what();for(const auto& message:r.retained_validation_messages)failure+='\n'+message;
        r.Report(false,input.empty(),input,failure);device.ClearRuntimeResources();
        throw std::runtime_error(failure);
    }
}
}
