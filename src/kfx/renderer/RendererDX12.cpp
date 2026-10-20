// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
// KeeperFX renderer performance changes: 2026-10-19.
#include "pre_inc.h"
#include "RendererDX12.h"
#if defined(KFX_ENABLE_DX12)
#include "OwnedGameWorld.h"
#include "EmissivePresentation.h"
#include "DX12LiveBenchmark.h"
#include "OwnedWorldViewRenderer.h"
#include "OwnedWorldOverlayCapture.h"
#include "OwnedFrameUI.h"
#include "RendererManager.h"
#include "RendererFrameCounter.h"
#include "RendererThread.h"
#include "RenderTaskProducerRegistry.h"
#include "SceneReferenceImage.h"
#include "WorldScene.h"
#include "WorldSceneCapture.h"
#include "TerrainColumnPlan.h"
#include "FrontTerrainPlan.h"
#include "d3d12/Dx12Device.h"
#include "d3d12/OwnedWorldBridge.h"
#include "kfx/platform/IPlatform.h"
#include "kfx/platform/IWindowSystem.h"
#include "bflib_mouse.h"
#include "bflib_video.h"
#include "globals.h"
#include "local_camera.h"
#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include "post_inc.h"

using namespace kfx::render;
namespace dx12 = kfx::render::dx12;

struct RendererDX12::Impl {
    dx12::Device device;
    DX12LiveBenchmark benchmark;
    uint64_t diagnostic_fail_after = 0;
    uint64_t diagnostic_remove_after = 0;
    OwnedRenderAssets assets{1};
    OwnedFramePaletteReplay palette_replay{assets};
    OwnedFrameMailbox mailbox;
    OwnedGameWorld producer{assets};
    OwnedWorldViewRenderer world{producer};
    OwnedFrameUI ui{assets,producer.SpriteSources()};
    dx12::OwnedWorldBridge bridge{device};
    OwnedFrameBuilder builder;
    OwnedFrameLease pending,front;
    std::unique_ptr<OwnedWorldOverlayCapture> swipe;
    uint64_t epoch=1,sequence=0,scene_epoch=0,submitted=0,rejected=0;
    uint64_t palette_replayed=0;
    uint64_t world_epoch=0;
    int drawable_w=0,drawable_h=0;
    uint8_t clear_index=0;
    bool ready=false,failed=false,world_bound=false,parchment_capture=false,submitted_world=false;
    bool indexed_requested=false,indexed_submitted=false,pending_world=false;
    bool pending_palette_replay=false;
    bool finishing=false;
    bool production_admitted=false; // A write window may outlive several cached palette presentations.
    std::shared_ptr<const WorldScene> finishing_scene;
    std::optional<std::chrono::steady_clock::time_point> blocked_since;
    std::optional<std::chrono::steady_clock::time_point> presentation_blocked_since;
    std::array<uint8_t,1024> palette{};
    struct Screenshot {
        std::string path;
        int format=1;
        std::shared_ptr<const WorldScene> scene;
        dx12::RuntimeFrameTicket ticket;
        SceneReferencePresentation presentation;
    };
    std::optional<Screenshot> screenshot;
    // One diagnostic gate and one watchdog. Only RuntimeReaderHold::Release,
    // which signals a CPU fence, crosses threads; no Device calls do.
    struct ReaderHoldControl {
        dx12::RuntimeReaderHold hold;
        std::mutex mutex;
        std::condition_variable wake;
        bool cancelled = false;
        std::thread worker;
        std::atomic<uint64_t> automatic_releases{0};
        std::atomic<bool> release_failed{false};
        ~ReaderHoldControl() { Stop(); }
        bool Stop() noexcept {
            const bool released = hold.Release();
            {
                std::lock_guard lock(mutex);
                cancelled = true;
            }
            wake.notify_one();
            if (worker.joinable()) worker.join();
            // The worker can no longer inspect the handle. Device retains its
            // own gate owner until the queued wait has retired.
            hold.Reset();
            if(released)release_failed.store(false);
            return released;
        }
        void StartWatchdog() {
            cancelled = false;
            release_failed.store(false);
            worker = std::thread([this] {
                std::unique_lock lock(mutex);
                if (wake.wait_for(lock, std::chrono::seconds(5), [this] { return cancelled; })) return;
                lock.unlock();
                if (hold.Release()) ++automatic_releases;
                else release_failed.store(true);
            });
        }
    } reader_hold;
    uint64_t hold_blocked_packets=0, hold_blocked_epoch=0, hold_blocked_sequence=0;
    uint64_t hold_resets=0, hold_dropped_pending=0;
    uint64_t hold_dropped_epoch=0, hold_dropped_sequence=0;

    static IWindowSystem* Window() { return GetPlatform()->GetWindowSystem(); }
    std::array<float,4> ClearColour() const {
        const size_t at=size_t(clear_index)*4;
        return {palette[at]/255.0f,palette[at+1]/255.0f,palette[at+2]/255.0f,1};
    }
    void DropBuilder() {
        device.ResetRuntimePresentationPollObservation();
        swipe.reset(); ui.Unbind();
        if(world_bound){world.BindFrame(nullptr);world_bound=false;}
        builder={}; parchment_capture=false;indexed_requested=indexed_submitted=false;
        finishing=false;finishing_scene.reset();production_admitted=false;
    }
    void Fail(const std::exception& error) {
        if (failed) return;
        failed = true;
        RendererReportFatalFailure(error.what());
        ERRORLOG("DX12 stopped at epoch %llu after %llu submitted frames; restart with -opengl. Unsaved progress was not saved.",
            static_cast<unsigned long long>(epoch), static_cast<unsigned long long>(submitted));
        try {
            ERRORLOG("DX12 failure details: %s", device.DeviceRemovedDetails().c_str());
            const auto failure_validation = device.GetDiagnostics();
            SYNCLOG("DX12 failure validation: errors %llu, warnings %llu",
                static_cast<unsigned long long>(failure_validation.validation_errors),
                static_cast<unsigned long long>(failure_validation.validation_warnings));
            for (const auto& message : device.TakeValidationMessages())
                ERRORLOG("DX12 validation at failure: %s", message.c_str());
        } catch (...) { ERRORLOG("DX12 failure diagnostics unavailable"); }
        benchmark.Cancel();
        reader_hold.Stop();
        DropBuilder(); bridge.Abandon(); pending.reset();
    }
    void PollScreenshot() {
        if(!screenshot || !screenshot->ticket.Valid())return;
        dx12::Image pixels;
        if(!device.TryReadbackRuntimeFrame(screenshot->ticket,pixels))return;
        SDL_Surface* surface=SDL_CreateSurfaceFrom(int(pixels.width),int(pixels.height),SDL_PIXELFORMAT_RGBA32,
            pixels.rgba.data(),int(pixels.width)*4);
        bool saved=false;
        if(surface) {
            if(screenshot->scene) saved=SaveSceneReferenceImage(surface,*screenshot->scene,&screenshot->presentation);
            else saved=screenshot->format==1?IMG_SavePNG(surface,screenshot->path.c_str()):SDL_SaveBMP(surface,screenshot->path.c_str());
            SDL_DestroySurface(surface);
        }
        if(!saved) ERRORLOG("DX12 screenshot failed: %s",SDL_GetError());
        screenshot.reset();
    }
    void UpdateWindow() {
        int width=0,height=0;Window()->GetDrawableSize(&width,&height);
        int x=0,y=0,w=0,h=0;Window()->GetPresentRect(&x,&y,&w,&h);
        width=std::max(0,width);height=std::max(0,height);
        // SDL may retain the last drawable size while minimized or hidden.
        // An extremely narrow client can also round the shared fitted rectangle
        // to zero. Suspend rather than reuse a stale rectangle or alter input mapping.
        if(!Window()->HasWindow() || (Window()->GetWindowFlags() & (KFX_WF_HIDDEN | KFX_WF_MINIMIZED))
            || w<=0 || h<=0 || RendererPhysicalWidth()<=0 || RendererPhysicalHeight()<=0)
            width=height=0;
        if(width!=drawable_w || height!=drawable_h) {
            if(!reader_hold.Stop())throw std::runtime_error("Could not release diagnostic GPU readers before resize");
            device.Resize(uint32_t(width),uint32_t(height));
            drawable_w=width;drawable_h=height;
        }
        if(width>0 && height>0)
            device.SetRuntimePresentation(vsync_enabled!=0,x,y,uint32_t(w),uint32_t(h));
    }
    bool AdmitPresentation() {
        if(!drawable_w || !drawable_h) { presentation_blocked_since.reset(); return false; }
        if(device.TryAcquireRuntimePresentation()) { presentation_blocked_since.reset(); return true; }
        const auto now=std::chrono::steady_clock::now();
        if(!presentation_blocked_since)presentation_blocked_since=now;
        if(now-*presentation_blocked_since>=std::chrono::seconds(10))
            throw std::runtime_error("DX12 presentation admission stalled for ten visible seconds");
        return false;
    }    bool Backpressure(unsigned prepare_status,dx12::RuntimePressureReason pressure) {
        const auto now=std::chrono::steady_clock::now();
        if(!blocked_since)blocked_since=now;
        if(now-*blocked_since<std::chrono::seconds(10))return false;
        const auto stats=bridge.Stats();
        const auto resources=device.GetRuntimeResourceDiagnostics();
        ERRORLOG("DX12 frame admission stalled: prepare %u, pressure %u, sources %u/%u, GPU bytes %llu, uploads %llu",
            prepare_status,unsigned(pressure),stats.selected_slots,stats.occupied_slots,
            static_cast<unsigned long long>(resources.default_bytes),static_cast<unsigned long long>(resources.upload_bytes));
        throw std::runtime_error("DX12 frame could not be admitted within ten seconds");
    }
    void ObserveSceneEpoch() {
        const uint64_t current_scene=WorldSceneEpoch();
        if(scene_epoch==current_scene)return;

        if(pending && pending->Info().epoch==hold_blocked_epoch && pending->Info().sequence==hold_blocked_sequence) {
            // Count qualification only when this exact blocked old packet is
            // discarded before the watchdog or an explicit release opens the gate.
            if(reader_hold.hold.Valid() && !reader_hold.hold.Released()) ++hold_resets;
            ++hold_dropped_pending;
            hold_dropped_epoch=pending->Info().epoch;hold_dropped_sequence=pending->Info().sequence;
        }
        // A blocked packet from the previous level must never prevent a reset.
        // Accepted GPU work keeps its own fenced resources and readback ticket.
        DropBuilder();pending.reset();front.reset();bridge.Abandon();blocked_since.reset();presentation_blocked_since.reset();world_epoch=0;
        if(screenshot && screenshot->scene && !screenshot->ticket.Valid())screenshot.reset();
        if(scene_epoch) {
            if(epoch==UINT64_MAX-1)throw std::overflow_error("DX12 source epoch exhausted");
            ++epoch;
            palette_replay.ResetEpoch(epoch);
            if(!assets.AdvanceEpoch(epoch))throw std::runtime_error("DX12 source epoch reset failed");
            device.SetRuntimeEpoch(epoch);
        }
        scene_epoch=current_scene;
    }
    bool SealNormalFrame() {
        if(!finishing)return true;
        const auto status=builder.RefreshDisplayPalette(producer.DrawSettings().ui_palette,palette_replay);
        if(status==OwnedFrameStatus::WouldBlock)
            return Backpressure(unsigned(status),dx12::RuntimePressureReason::None);
        if(status!=OwnedFrameStatus::Ready)builder.Fail(status);
        auto packet=builder.Seal();finishing=false;
        if(!packet->Complete()) {
            if(rejected++<12) {
                ERRORLOG("DX12 incomplete development frame: status %u; terrain %lu, sprites %lu, outlines %lu, shadows %lu, quads %lu, images %lu", unsigned(packet->Status()),
                    (unsigned long)packet->Terrain().size(), (unsigned long)packet->Sprites().size(),
                    (unsigned long)packet->OutlineSprites().size(), (unsigned long)packet->Shadows().size(),
                    (unsigned long)packet->Quads().size(), (unsigned long)packet->Images().size());
                const auto memory=assets.Stats();
                ERRORLOG("DX12 producer resource pressure: bytes %llu, peak %llu, versions %llu, identities %llu, evictions %llu",
                    (unsigned long long)(memory.payload_bytes+memory.temporary_bytes),
                    (unsigned long long)memory.peak_owned_bytes,(unsigned long long)memory.live_versions,
                    (unsigned long long)memory.identities,(unsigned long long)memory.evictions);
                std::array<size_t,size_t(OwnedFramePhase::Count)> solid{},textured{},pixels{};
                for(const auto& q:packet->Quads()) {
                    const auto phase=size_t(q.phase);
                    if(q.mode==OwnedQuadMode::Solid)++solid[phase];else ++textured[phase];
                    if(q.x1-q.x0==1 && q.y1-q.y0==1)++pixels[phase];
                }
                for(size_t i=0;i<solid.size();++i)if(solid[i] || textured[i])
                    ERRORLOG("DX12 quad phase %u: solid %lu, textured %lu, single pixels %lu",unsigned(i),
                        (unsigned long)solid[i],(unsigned long)textured[i],(unsigned long)pixels[i]);
                for(size_t i=0;i<size_t(OwnedFramePhase::Count);++i)
                    if(packet->Phase(OwnedFramePhase(i))==OwnedPhaseState::Unsupported)
                        ERRORLOG("DX12 unsupported frame phase %u",unsigned(i));
            }
        } else {
            pending_world=packet->Phase(OwnedFramePhase::World)==OwnedPhaseState::Recorded
                || packet->Phase(OwnedFramePhase::WorldSprites)==OwnedPhaseState::Recorded || indexed_submitted;
            pending=std::move(packet);pending_palette_replay=false;
            benchmark.Sealed();
            // A diagnostic scene reference belongs only to this accepted packet.
            if(finishing_scene && !finishing_scene->reference_image.empty()) {
                if(!screenshot) {
                    screenshot=Screenshot{finishing_scene->reference_image,1,finishing_scene,{},{pending->Info().epoch,pending->Info().sequence}};
                } else {
                    ERRORLOG("DX12 reference image refused: another screenshot is pending");
                }
            }
        }
        finishing_scene.reset();RendererFrameCounter_Advance();
        return true;
    }
    bool Pump() {
        if (diagnostic_remove_after && submitted >= diagnostic_remove_after) {
            diagnostic_remove_after = 0;
            const auto reason = device.DiagnosticRemoveDevice();
            SYNCLOG("DX12 diagnostic RemoveDevice invoked: HRESULT 0x%08lx",
                static_cast<unsigned long>(static_cast<uint32_t>(reason)));
            // Ordinary fence collection must observe removal; never synthesize its failure.
            device.CollectRuntimeResources(false);
        }
        if (diagnostic_fail_after && submitted >= diagnostic_fail_after)
            throw std::runtime_error("Diagnostic DX12 renderer failure injection (no device reset)");
        if(reader_hold.release_failed.load())throw std::runtime_error("Diagnostic GPU reader watchdog could not release its fence");
        benchmark.Poll(device);
        ObserveSceneEpoch(); PollScreenshot(); UpdateWindow();
        if(!SealNormalFrame())return false;
        if(!drawable_w || !drawable_h){blocked_since.reset();presentation_blocked_since.reset();return false;}
        if(!pending)return true;
        if(!AdmitPresentation())return false;
        const double submission_start = benchmark.enabled ? DX12LiveBenchmark::Now() : 0;
        const auto prepared=bridge.PrepareFrame(pending);
        if(prepared==dx12::OwnedWorldPrepareStatus::WouldBlock) {
            if(benchmark.enabled)benchmark.Attempt((DX12LiveBenchmark::Now()-submission_start)*1000,nullptr);
            return Backpressure(unsigned(prepared),dx12::RuntimePressureReason::None);
        }
        if(prepared!=dx12::OwnedWorldPrepareStatus::Ready) {
            ERRORLOG("DX12 composition rejected frame: status %u, terrain %llu, sprites %llu, quads %llu, images %llu",
                unsigned(prepared),static_cast<unsigned long long>(pending->Terrain().size()),
                static_cast<unsigned long long>(pending->Sprites().size()),static_cast<unsigned long long>(pending->Quads().size()),
                static_cast<unsigned long long>(pending->Images().size()));
            throw std::runtime_error("Owned frame was rejected by DX12 composition");
        }
        const bool capture=screenshot && !screenshot->ticket.Valid();
        if(capture && screenshot->scene && (screenshot->presentation.epoch!=pending->Info().epoch
            || screenshot->presentation.sequence!=pending->Info().sequence))
            throw std::logic_error("DX12 reference image no longer matches its owned frame");
        auto result=bridge.TrySubmitPreparedFrame(capture);
        if(benchmark.enabled)benchmark.Attempt((DX12LiveBenchmark::Now()-submission_start)*1000,&result);
        if(result.status==dx12::RuntimeSubmitStatus::Suspended){blocked_since.reset();return false;}
        if(result.status!=dx12::RuntimeSubmitStatus::Submitted) {
            if(result.pressure==dx12::RuntimePressureReason::FrameSlot && reader_hold.hold.Valid()
                && !reader_hold.hold.Released() && pending_world
                && (pending->Info().epoch!=hold_blocked_epoch || pending->Info().sequence!=hold_blocked_sequence)) {
                ++hold_blocked_packets;
                hold_blocked_epoch=pending->Info().epoch;hold_blocked_sequence=pending->Info().sequence;
            }
            return Backpressure(unsigned(prepared),result.pressure);
        }
        benchmark.Submitted(result,pending->Info().epoch,pending->Info().sequence,
            pending->Info().native_width,pending->Info().native_height,pending_palette_replay,capture);
        blocked_since.reset();
        if(capture)screenshot->ticket=std::move(result.ticket);
        front=std::move(pending);++submitted;
        if(pending_world)world_epoch=epoch;
        if(pending_palette_replay)++palette_replayed;
        if(submitted==1) { SYNCLOG("DX12 submitted first normal engine frame"); }
        if(!submitted_world && pending_world) {
            submitted_world=true;
            SYNCLOG("DX12 submitted first world frame: terrain %llu, sprites %llu, quads %llu",
                static_cast<unsigned long long>(front->Terrain().size()),static_cast<unsigned long long>(front->Sprites().size()),
                static_cast<unsigned long long>(front->Quads().size()));
        }
        return true;
    }
    OwnedFrameInfo NewFrameInfo() {
        const auto width=RendererPhysicalWidth(),height=RendererPhysicalHeight();
        if(width<=0 || height<=0)return {};
        if(sequence==UINT64_MAX-1)throw std::overflow_error("DX12 frame sequence exhausted");
        OwnedFrameInfo info;
        info.epoch=epoch;info.sequence=++sequence;
        info.native_width=info.viewport_width=uint32_t(width);
        info.native_height=info.viewport_height=uint32_t(height);info.clear_rgba=ClearColour();
        return info;
    }
    OwnedFrameBuilder NewFrame() {
        const auto info=NewFrameInfo();
        return info.native_width?mailbox.Begin(info):OwnedFrameBuilder{};
    }
    bool EnsureFrame() {
        ASSERT_GAME_THREAD();
        if(!ready || failed)return false;
        const bool can_start=Pump();
        // Visibility may change between an admitted world traversal and its
        // Present. Finish that same write window so it can seal while suspended;
        // reusing a half-finished world after restore would begin it twice.
        // Pending submissions and finishing retries still enforce backpressure.
        if(builder && production_admitted && !finishing && !pending)return true;
        if(!can_start || !drawable_w || !drawable_h || !AdmitPresentation())return false;
        // A cached fade retained the next normal builder, but consumed its
        // presentation permit. Re-admit this cycle without replacing commands.
        if(builder && !finishing && !pending) { production_admitted=true; return true; }
        builder=NewFrame();
        if(builder)benchmark.Admit();
        if(!builder)return false;
        production_admitted=true;
        indexed_requested=indexed_submitted=false;
        if(!producer.PrepareFrameSources(builder) || !ui.Bind(builder,producer.DrawSettings())) {
            if(rejected++<12)ERRORLOG("DX12 incomplete development frame: source preparation failed");
            DropBuilder();return false;
        }
        world.BindFrame(&builder);world_bound=true;
        return true;
    }
    void Unsupported(OwnedFramePhase phase) {
        if(EnsureFrame())builder.Unsupported(phase);
    }
    void Present() {
        if(!EnsureFrame())return;
        RenderTaskProducerRegistry_ProduceAll(RendererFrameCounter_Current());
        if(RendererIsFadeCachePreserved() && !RendererConsumeForceUIFlip() && front
            && front->Info().native_width==uint32_t(RendererPhysicalWidth())
            && front->Info().native_height==uint32_t(RendererPhysicalHeight())) {
            // Keep the next normal write window intact, like the OpenGL path.
            // Only display-backed colours change; callbacks, cursor and world
            // producers are not rerun to reconstruct the displayed commands.
            const auto status=builder.ReplayCachedFrame(*front,producer.DrawSettings().ui_palette,palette_replay,NewFrameInfo(),pending);
            if(status==OwnedFrameStatus::WouldBlock){Backpressure(unsigned(status),dx12::RuntimePressureReason::None);return;}
            if(status!=OwnedFrameStatus::Ready)throw std::runtime_error("DX12 cached palette replay failed");
            pending_palette_replay=true;production_admitted=false;benchmark.Replay();
            RendererFrameCounter_Advance();Pump();return;
        }
        // The engine can change the base palette for possession after Begin.
        // Rebinding the same packet refreshes cursor/UI settings only.
        ui.Bind(builder,producer.DrawSettings());
        LbMouseOnBeginSwap();ui.Draw();LbMouseOnEndSwap();
        if(swipe) {swipe.reset();builder.Unsupported(OwnedFramePhase::Swipe);}
        if(parchment_capture)builder.Unsupported(OwnedFramePhase::ResolveMapFade);
        if(indexed_requested && !indexed_submitted)builder.Unsupported(OwnedFramePhase::World);
        world.EndWorldProduction();
        const auto scene=world.GetSceneSnapshot();
        builder.SetDisplayClear(clear_index,producer.DrawSettings().ui_palette);builder.RecordPhase(OwnedFramePhase::Clear);
        // Each producer above ran; optional passes remain empty unless their
        // entry point recorded commands or explicitly rejected unsupported work.
        for(auto phase:{OwnedFramePhase::PopulateUI,OwnedFramePhase::BeginWorldCapture,
            OwnedFramePhase::Swipe,OwnedFramePhase::LensPaletteUIExclusion,
            OwnedFramePhase::CaptureMapFadeWorld,OwnedFramePhase::ImagePresents,
            OwnedFramePhase::OverheadMap,OwnedFramePhase::PiPCaptures,OwnedFramePhase::GameUI,
            OwnedFramePhase::ResolveMapFade,OwnedFramePhase::ZoomBoxes,OwnedFramePhase::FrontOverlay,
            OwnedFramePhase::Text,OwnedFramePhase::ImagePresentOverlay,OwnedFramePhase::Cursor,
            OwnedFramePhase::DevTools,OwnedFramePhase::Screenshot,OwnedFramePhase::ScrgbLift})
            builder.FinishPhase(phase);
        world.BindFrame(nullptr);world_bound=false;ui.Unbind();
        // Cached fades may have presented newer packets while this normal
        // write window stayed open. Order it at presentation, after production.
        if(sequence==UINT64_MAX-1 || !builder.SetPresentationSequence(++sequence))
            throw std::overflow_error("DX12 presentation sequence exhausted");
        benchmark.FinishProducer();
        finishing_scene=scene;finishing=true;production_admitted=false;
        Pump();
    }
};

RendererDX12::RendererDX12()=default;
RendererDX12::~RendererDX12(){Shutdown();}
bool RendererDX12::Init()
{
    ASSERT_GAME_THREAD();
    try {
        SYNCLOG("DX12 emissive presentation: %s",EmissivePresentationEnabled()?"source-v1":"off");
        impl_=std::make_unique<Impl>();auto& r=*impl_;
        auto* window=Impl::Window();
        if(!window || !window->GetNativeWindowHandle())throw std::runtime_error("DX12 requires the native Windows game window");
        window->GetDrawableSize(&r.drawable_w,&r.drawable_h);
        dx12::Options options;options.debug=true;options.vsync=vsync_enabled!=0;
        options.gpu_validation=std::getenv("KFX_DX12_GPU_VALIDATION")!=nullptr;
        const auto lighting_enabled=[](const char* name) {
            const char* value=std::getenv(name);return value && std::string(value)=="1";
        };
        if(lighting_enabled("KFX_LIGHTING_SMOOTH_CELLS"))options.lighting_remaster|=dx12::SmoothLightCells;
        if(lighting_enabled("KFX_LIGHTING_BLEND_PALETTE"))options.lighting_remaster|=dx12::BlendPaletteRows;
        if(lighting_enabled("KFX_LIGHTING_FILTER_SHADOWS"))options.lighting_remaster|=dx12::FilterShadowCoverage;
        const char* performance = std::getenv("KFX_DX12_PERFORMANCE");
        if(performance && std::string(performance)=="1") {
            options.debug=false;options.gpu_validation=false;
        }
        r.benchmark.debug=options.debug;r.benchmark.gpu_validation=options.gpu_validation;
        r.benchmark.vsync=options.vsync;
        options.presentation_diagnostics=r.benchmark.enabled || local_camera_reference_enabled();
        options.waitable_presentation=true;
        const char* base=SDL_GetBasePath();
        options.shader_directory=(std::filesystem::path(base?base:".")/"shaders").wstring();
        r.device.Initialize(window->GetNativeWindowHandle(),uint32_t(std::max(1,r.drawable_w)),
            uint32_t(std::max(1,r.drawable_h)),options);
        const auto device_info = r.device.GetDiagnostics();
        SYNCLOG("DX12 adapter: %s (vendor %u); debug %d; GPU validation %d",
            device_info.adapter.c_str(), device_info.vendor_id,
            int(device_info.debug_enabled), int(device_info.gpu_validation_enabled));
        SYNCLOG("DX12 lighting remaster flags: %u", options.lighting_remaster);
        r.device.EnableRuntimeTimingCompletions(r.benchmark.enabled);
        if (local_camera_reference_enabled()) {
            if (const char* value = std::getenv("KFX_DX12_REMOVE_AFTER_SUBMISSIONS")) {
                char* end = nullptr;
                const unsigned long long count = std::strtoull(value, &end, 10);
                if (end != value && !*end && count > 0 && count <= 1000000000ULL)
                    r.diagnostic_remove_after = count;
            }
            if (const char* value = std::getenv("KFX_DX12_FAIL_AFTER_SUBMISSIONS")) {
                char* end = nullptr;
                const unsigned long long count = std::strtoull(value, &end, 10);
                if (end != value && !*end && count > 0 && count <= 1000000000ULL)
                    r.diagnostic_fail_after = count;
            }
        }
        std::array<uint8_t,768> rgb8{};const auto* pal6=LbPaletteGetReadonly();
        for(size_t i=0;i<rgb8.size();++i)rgb8[i]=RendererPaletteChannel8(pal6[i]);
        r.producer.SetSpritePalette({pal6,768});r.ready=true;SetDisplayPalette(rgb8.data());
        SYNCLOG("DX12 development backend initialized; live coverage gates remain open");
        return true;
    }catch(const std::exception& e){ERRORLOG("DX12 initialization failed: %s",e.what());impl_.reset();return false;}
}
void RendererDX12::Shutdown()
{
    if(!impl_)return;
    impl_->benchmark.Cancel();
    const bool readers_released=impl_->reader_hold.Stop();
    if(!readers_released)ERRORLOG("Could not release diagnostic GPU readers during shutdown");
    // Accepted readbacks own their pixels independently of the front packet.
    // Complete them before teardown invalidates tickets; only explicit shutdown
    // may wait. A save/device failure must not bypass the cleanup below.
    try {
        if(impl_->screenshot) {
            if(!impl_->screenshot->ticket.Valid()) {
                WARNLOG("DX12 screenshot cancelled during shutdown: no frame was submitted (%s)",impl_->screenshot->path.c_str());
            }else if(!readers_released) {
                ERRORLOG("DX12 screenshot cancelled during shutdown: GPU readers could not be released");
            }else {
                if(!impl_->device.IsRuntimeFrameComplete(impl_->screenshot->ticket))impl_->device.WaitIdle();
                impl_->PollScreenshot();
            }
        }
    }catch(const std::exception& e){ERRORLOG("DX12 shutdown screenshot failed: %s",e.what());}
    impl_->screenshot.reset();
    try {
        impl_->DropBuilder();impl_->bridge.Abandon();impl_->pending.reset();impl_->front.reset();
        impl_->device.ClearRuntimeResources();
        for(const auto& message:impl_->device.TakeValidationMessages())WARNLOG("DX12 validation message: %s",message.c_str());
        const auto resources=impl_->device.GetRuntimeResourceDiagnostics();
        const auto diagnostics=impl_->device.GetDiagnostics();
        const auto timing=impl_->device.GetRuntimeTimingDiagnostics();
        const auto presentation=impl_->device.GetPresentationDiagnostics();
        if(presentation.enabled) {
            SYNCLOG("DX12 presentation: S_OK %llu, OCCLUDED %llu, other %llu; last HRESULT 0x%08lx; output %s HRESULT 0x%08lx; nominal Hz %u available %d; frame statistics status %u HRESULT 0x%08lx; last count status %u HRESULT 0x%08lx count %u",
                static_cast<unsigned long long>(presentation.present_ok),static_cast<unsigned long long>(presentation.present_occluded),
                static_cast<unsigned long long>(presentation.present_other),static_cast<unsigned long>(static_cast<uint32_t>(presentation.last_present_hresult)),
                presentation.output_name.c_str(),static_cast<unsigned long>(static_cast<uint32_t>(presentation.output_hresult)),
                presentation.refresh_hz,int(presentation.refresh_available),unsigned(presentation.frame_statistics_status),
                static_cast<unsigned long>(static_cast<uint32_t>(presentation.frame_statistics_hresult)),unsigned(presentation.last_count_status),
                static_cast<unsigned long>(static_cast<uint32_t>(presentation.last_count_hresult)),presentation.last_present_count);
        }
        const auto& cpu=timing.world.normal.cpu_submitted;
        const auto& gpu=timing.world.normal.gpu_upload_render;
        SYNCLOG("DX12 validation: errors %llu, warnings %llu; runtime peak GPU %llu, upload %llu; CPU %.3f ms, GPU %.3f ms",
            static_cast<unsigned long long>(diagnostics.validation_errors),static_cast<unsigned long long>(diagnostics.validation_warnings),
            static_cast<unsigned long long>(resources.default_high_water),static_cast<unsigned long long>(resources.upload_high_water),
            cpu.samples?cpu.total_ms/cpu.samples:0,gpu.samples?gpu.total_ms/gpu.samples:0);
        if(diagnostics.validation_errors || diagnostics.validation_warnings) {
            ERRORLOG("DX12 validation failed; inspect the device diagnostics");
        }
        SYNCLOG("DX12 session: submitted %llu, rejected %llu",
            static_cast<unsigned long long>(impl_->submitted),static_cast<unsigned long long>(impl_->rejected));
        SYNCLOG("DX12 palette replay: submitted %llu",static_cast<unsigned long long>(impl_->palette_replayed));
    }catch(const std::exception& e){ERRORLOG("DX12 shutdown: %s",e.what());}
    impl_.reset();
}
void RendererDX12::SetDisplayPalette(const unsigned char* rgb8)
{
    if(!impl_ || !rgb8)return;
    try {
        auto& r=*impl_;r.producer.SetDisplayPalette({rgb8,768});
        for(size_t i=0;i<256;++i){std::copy_n(rgb8+i*3,3,r.palette.data()+i*4);r.palette[i*4+3]=255;}
        if(r.builder && r.producer.PrepareFrameSources(r.builder) && !r.finishing)r.ui.Bind(r.builder,r.producer.DrawSettings());
    }catch(const std::exception& e){impl_->Fail(e);}
}
void RendererDX12::ClearScreen(unsigned char colour){if(impl_)impl_->clear_index=colour;}
bool RendererDX12::BeginFrame()
{
    if(!impl_)return false;
    try{return impl_->EnsureFrame();}catch(const std::exception& e){impl_->Fail(e);return false;}
}
void RendererDX12::EndFrame(){} // Multiple draw windows contribute to one Present.
bool RendererDX12::CanDraw() const{return impl_ && impl_->ready && !impl_->failed;}
bool RendererDX12::CanPresent() const
{
    if(!CanDraw())return false;
    try{return impl_->Pump() && impl_->AdmitPresentation();}catch(const std::exception& e){impl_->Fail(e);return false;}
}
void RendererDX12::PresentFrame()
{
    if(!CanDraw())return;
    try{impl_->Present();}catch(const std::exception& e){impl_->Fail(e);}
}
ITextRenderer* RendererDX12::GetTextRenderer(){return impl_?&impl_->ui:nullptr;}
IUIRenderer* RendererDX12::GetUIRenderer(){return impl_?&impl_->ui:nullptr;}
ICursorLayer* RendererDX12::GetCursorLayer(){return impl_?&impl_->ui:nullptr;}
IWorldViewRenderer* RendererDX12::GetWorldViewRenderer(){return impl_?&impl_->world:nullptr;}
bool RendererDX12::PresentImage(const RendererPresentImageDesc* desc)
{
    if(!desc || !BeginFrame())return false;
    return impl_->ui.PresentImage(*desc);
}
void RendererDX12::PresentHugeSprite(const TbHugeSprite* sprite,int32_t length,int32_t x,int32_t y,int32_t scale)
{
    if(BeginFrame())impl_->ui.PresentHugeSprite(sprite,length,x,y,scale);
}
void RendererDX12::SubmitZoomBoxTiles(const uint16_t* ids,int nx,int ny,int x,int y,int w,int h)
{if(BeginFrame())impl_->ui.SubmitZoomBoxTiles(ids,nx,ny,x,y,w,h);}
bool RendererDX12::SubmitLandviewZoom(const unsigned char* pixels,int width,int height,
    float map_x,float map_y,float screen_x,float screen_y,float scale)
{
    return BeginFrame() && impl_->ui.SubmitLandviewZoom(pixels,width,height,map_x,map_y,screen_x,screen_y,scale);
}
bool RendererDX12::ScheduleScreenshot(const char* path,int format)
{
    if(!impl_ || !path || !*path || (format!=1 && format!=2) || impl_->screenshot)return false;
    impl_->screenshot=Impl::Screenshot{path,format,{},{},{}};return true;
}
bool RendererDX12::SetDiagnosticReaderHold(bool active)
{
    ASSERT_GAME_THREAD();
    if(!impl_ || !local_camera_reference_enabled())return false;
    auto& r=*impl_;
    if(!active)return r.reader_hold.Stop();
    if(!r.ready || r.failed || r.pending || r.finishing || r.screenshot || r.world_epoch!=r.epoch
        || (r.reader_hold.hold.Valid() && !r.reader_hold.hold.Released()))return false;
    try {
        // Reclaim a previous completed watchdog before rearming. Arming itself
        // polls slot fences and never waits for GPU completion.
        if(!r.reader_hold.Stop())return false;
        if(!r.device.TryBeginRuntimeReaderHold(r.reader_hold.hold))return false;
        r.reader_hold.StartWatchdog();
        return true;
    }catch(const std::exception& e) {
        r.reader_hold.Stop();
        ERRORLOG("DX12 diagnostic reader hold failed: %s",e.what());
        return false;
    }
}
bool RendererDX12::WriteDiagnostics(const char* path)
{
    ASSERT_GAME_THREAD();
    if(!impl_ || !impl_->ready || !path || !*path)return false;
    try {
        auto& r=*impl_;
        for(const auto& message:r.device.TakeValidationMessages())WARNLOG("DX12 validation message: %s",message.c_str());
        const auto runtime=r.device.GetRuntimeResourceDiagnostics();
        const auto validation=r.device.GetDiagnostics();
        const auto producer=r.assets.Stats();
        const auto packets=r.mailbox.Stats();
        const auto mapping=r.bridge.Stats();
        const auto terrain=TerrainColumnPlanGetStats();
        const auto front_terrain=FrontTerrainPlanGetStats();
        const dx12::RuntimeResourceBudgets budgets;
        const OwnedAssetLimits producer_limits;
        const auto window_flags=Impl::Window()->GetWindowFlags();
        int window_w=0,window_h=0,pixel_w=0,pixel_h=0,present_x=0,present_y=0,present_w=0,present_h=0;
        Impl::Window()->GetWindowSize(&window_w,&window_h);
        Impl::Window()->GetDrawableSize(&pixel_w,&pixel_h);
        Impl::Window()->GetPresentRect(&present_x,&present_y,&present_w,&present_h);
        std::ofstream out(std::filesystem::path(path),std::ios::binary|std::ios::trunc);
        out << "{\n\"schema\":1,\"backend\":\"DX12\",\"epoch\":" << r.epoch
            << ",\"worldEpoch\":" << r.world_epoch << ",\"sceneEpoch\":" << WorldSceneEpoch()
            << ",\"frameSequence\":" << (r.front?r.front->Info().sequence:0)
            << ",\"submitted\":" << r.submitted << ",\"rejected\":" << r.rejected
            << ",\"failed\":" << (r.failed?"true":"false")
            << ",\"window\":{\"flags\":" << window_flags
            << ",\"hidden\":" << ((window_flags&KFX_WF_HIDDEN)?"true":"false")
            << ",\"minimized\":" << ((window_flags&KFX_WF_MINIMIZED)?"true":"false")
            << ",\"resizable\":" << ((window_flags&KFX_WF_RESIZABLE)?"true":"false")
            << ",\"size\":[" << window_w << ',' << window_h << "]"
            << ",\"drawable\":[" << pixel_w << ',' << pixel_h << "]"
            << ",\"availableDrawable\":[" << r.drawable_w << ',' << r.drawable_h << "]"
            << ",\"native\":[" << RendererPhysicalWidth() << ',' << RendererPhysicalHeight() << "]"
            << ",\"presentRect\":[" << present_x << ',' << present_y << ',' << present_w << ',' << present_h << "]"
            << ",\"suspended\":" << ((!r.drawable_w || !r.drawable_h)?"true":"false")
            << ",\"resizes\":" << validation.resizes << '}'
            << ",\"readerHold\":{\"active\":" << (runtime.reader_hold_active?"true":"false")
            << ",\"slots\":" << runtime.reader_hold_slots << ",\"arms\":" << runtime.reader_hold_arms
            << ",\"releases\":" << runtime.reader_hold_releases << ",\"automaticReleases\":" << r.reader_hold.automatic_releases.load()
            << ",\"watchdogMs\":5000,\"heldSubmissions\":" << runtime.held_submissions << ",\"presentations\":" << runtime.presentations
            << ",\"blockedPackets\":" << r.hold_blocked_packets << ",\"blockedEpoch\":" << r.hold_blocked_epoch
            << ",\"blockedSequence\":" << r.hold_blocked_sequence
            << ",\"pendingEpoch\":" << (r.pending?r.pending->Info().epoch:0)
            << ",\"pendingSequence\":" << (r.pending?r.pending->Info().sequence:0)
            << ",\"pendingWorld\":" << (r.pending && r.pending_world?"true":"false")
            << ",\"resetsWhileHeld\":" << r.hold_resets << ",\"droppedPendingOnReset\":" << r.hold_dropped_pending
            << ",\"droppedEpoch\":" << r.hold_dropped_epoch << ",\"droppedSequence\":" << r.hold_dropped_sequence << '}'
            << ",\"validation\":{\"errors\":" << validation.validation_errors << ",\"warnings\":" << validation.validation_warnings << "},\n"
            << "\"memory\":{\"cpuBytes\":" << runtime.cpu_bytes << ",\"cpuHighWater\":" << runtime.cpu_high_water << ",\"cpuBudgetBytes\":" << budgets.cpu_bytes
            << ",\"defaultBytes\":" << runtime.default_bytes << ",\"defaultHighWater\":" << runtime.default_high_water << ",\"defaultBudgetBytes\":" << budgets.default_bytes
            << ",\"uploadBytes\":" << runtime.upload_bytes << ",\"uploadHighWater\":" << runtime.upload_high_water << ",\"uploadBudgetBytes\":" << budgets.upload_bytes
            << ",\"readbackBytes\":" << runtime.readback_bytes << ",\"readbackHighWater\":" << runtime.readback_high_water << ",\"readbackBudgetBytes\":" << budgets.readback_bytes
            << ",\"geometryCpuBytes\":" << runtime.terrain_cpu_bytes << ",\"geometryCpuHighWater\":" << runtime.terrain_cpu_high_water << ",\"geometryCpuBudgetBytes\":" << budgets.geometry_cpu_bytes
            << ",\"producerBytes\":" << producer.payload_bytes+producer.temporary_bytes << ",\"producerHighWater\":" << producer.peak_owned_bytes
            << ",\"producerBudgetBytes\":" << producer_limits.bytes << ",\"producerTableBytes\":" << producer.table_storage_bytes
            << ",\"packetCapacityBytes\":" << packets.command_capacity_bytes << ",\"uiScratchBytes\":" << r.ui.ScratchBytes() << "},\n"
            << "\"descriptors\":{\"current\":" << runtime.descriptors << ",\"highWater\":" << runtime.descriptor_high_water << ",\"budget\":" << budgets.descriptors << "},\n"
            << "\"counters\":{\"allocations\":" << runtime.resource_allocations << ",\"uploadBytes\":" << runtime.world_upload_bytes+runtime.source_upload_bytes+runtime.palette_upload_bytes
            << ",\"internalWaits\":" << runtime.fence_waits << ",\"globalIdleCalls\":" << runtime.global_idle_calls
            << ",\"publications\":" << runtime.publications << ",\"cacheHits\":" << mapping.cache_hits
            << ",\"geometryUploadBytes\":" << runtime.geometry_upload_bytes << ",\"terrainComparedBytes\":" << runtime.terrain_compared_bytes
            << ",\"terrainUploadBytes\":" << runtime.terrain_upload_bytes << ",\"terrainReusedBytes\":" << runtime.terrain_reused_bytes
            << ",\"terrainDirtyRanges\":" << runtime.terrain_dirty_ranges
            << ",\"wholeImageReuses\":" << runtime.whole_image_reuses << "},\n"
            << "\"assets\":{\"runtime\":" << runtime.asset_count << ",\"producerVersions\":" << producer.live_versions
            << ",\"producerRecords\":" << producer.version_records << ",\"producerIdentities\":" << producer.identities
            << ",\"mappingSlots\":" << mapping.occupied_slots << ",\"mappingLimit\":512"
            << ",\"wholeImages\":" << runtime.whole_images << ",\"wholeImagesIdle\":" << runtime.whole_image_idle << "},\n"
            << "\"terrainCache\":{\"requests\":" << terrain.requests << ",\"hits\":" << terrain.hits
            << ",\"builds\":" << terrain.builds << ",\"evictions\":" << terrain.evictions
            << ",\"resets\":" << terrain.epoch_resets << ",\"bytes\":" << terrain.capacity_bytes
            << ",\"entries\":" << terrain.capacity_entries
            << ",\"modeRequests\":[" << terrain.mode_requests[0] << ',' << terrain.mode_requests[1] << ',' << terrain.mode_requests[2] << ']'
            << ",\"modeHits\":[" << terrain.mode_hits[0] << ',' << terrain.mode_hits[1] << ',' << terrain.mode_hits[2] << ']'
            << ",\"modeBuilds\":[" << terrain.mode_builds[0] << ',' << terrain.mode_builds[1] << ',' << terrain.mode_builds[2] << ']'
            << ",\"gpuAllocationBytes\":" << runtime.terrain_default_bytes << ",\"gpuCapacityBytes\":" << runtime.terrain_capacity_bytes
            << ",\"front\":{\"requests\":" << front_terrain.requests << ",\"hits\":" << front_terrain.hits
            << ",\"builds\":" << front_terrain.builds << ",\"evictions\":" << front_terrain.evictions
            << ",\"epochResets\":" << front_terrain.epoch_resets << ",\"capacityBytes\":" << front_terrain.capacity_bytes
            << ",\"capacityEntries\":" << front_terrain.capacity_entries << "}}}\n";
        out.close();
        return bool(out);
    }catch(const std::exception& e){ERRORLOG("DX12 diagnostic snapshot failed: %s",e.what());return false;}
}
void RendererDX12::SubmitMapFadeStep(int tick,float step,float warp,bool in,const unsigned char* ghost)
{
    if(!BeginFrame())return;
    try { impl_->ui.SubmitMapFadeStep(tick,step,warp,in,ghost,impl_->builder.Fade()); }
    catch(const std::exception& e){impl_->Fail(e);}
}
void RendererDX12::BeginOverlayCapture(OverlayCaptureKind kind)
{
    if(!BeginFrame())return;
    if(kind==OVERLAY_CAPTURE_SWIPE) {
        if(impl_->swipe)impl_->builder.Unsupported(OwnedFramePhase::Swipe);
        else impl_->swipe=impl_->producer.BeginSwipe(impl_->builder);
    }else {
        impl_->parchment_capture=impl_->ui.SetParchmentCapture(true);
    }
}
void RendererDX12::EndOverlayCapture(OverlayCaptureKind kind)
{
    if(!impl_ || !impl_->builder)return;
    if(kind==OVERLAY_CAPTURE_SWIPE) {
        if(impl_->swipe){impl_->swipe->Complete();impl_->swipe.reset();}
        else impl_->builder.Unsupported(OwnedFramePhase::Swipe);
    }else {impl_->ui.SetParchmentCapture(false);impl_->parchment_capture=false;}
}
bool RendererDX12::BeginIndexedWorldFallback(int width,int height,unsigned char& clear_index)
{
    if(!BeginFrame())return false;
    auto& r=*impl_;
    if(width!=RendererPhysicalWidth() || height!=RendererPhysicalHeight() || r.indexed_requested
        || r.swipe || r.parchment_capture || r.ui.HasImagePresents() || !r.builder.EmptyDraws())return false;
    // The engine's existing scope runs the custom lens exactly once on an
    // initialized indexed surface. This backend only owns and presents its result.
    clear_index=r.clear_index;r.indexed_requested=true;return true;
}
bool RendererDX12::SubmitIndexedWorldFallback(const RendererPresentImageDesc* desc)
{
    if(!impl_ || !impl_->builder || !impl_->indexed_requested || impl_->indexed_submitted || !desc
        || desc->kind!=PRESENT_KIND_COMPOSITE || desc->format!=PRESENT_FORMAT_INDEXED8
        || desc->palette!=PRESENT_PALETTE_GAME || !desc->src || desc->coverage
        || desc->src_w<=0 || desc->src_h<=0 || desc->src_pitch<desc->src_w
        || desc->src_w!=desc->dst_w || desc->src_h!=desc->dst_h || desc->dst_x<0 || desc->dst_y<0
        || int64_t(desc->dst_x)+desc->dst_w>RendererPhysicalWidth()
        || int64_t(desc->dst_y)+desc->dst_h>RendererPhysicalHeight()
        || uint64_t(desc->src_w)*desc->src_h>64u*1024u*1024u)return false;
    try {
        impl_->indexed_submitted=impl_->ui.PresentImage(*desc);
        return impl_->indexed_submitted;
    }catch(const std::exception& e){impl_->Fail(e);return false;}
}
bool RendererDX12::GetIndexedWorldFallbackPalette(std::array<uint8_t,1024>& out) const
{if(!impl_)return false;out=impl_->palette;return true;}
#endif
