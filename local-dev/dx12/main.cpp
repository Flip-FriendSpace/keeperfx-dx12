// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
// Standalone DX12 host: procedural tests, owned asset sheets and terrain.
// No live game files, engine globals or game preferences.
#include "Dx12Device.h"
#include "PaletteTests.h"
#include "SpriteReplayTests.h"
#include "TerrainReplayTests.h"
#include "ShadowOperatorTests.h"
#include "ShadowEdgeTests.h"
#include "OutlineReplayTests.h"
#include "OutlineOperatorTests.h"
#include "EnhancedSpriteOperatorTests.h"
#include "EnhancedSpriteReplayTests.h"
#include "EnhancedWorldOperatorTests.h"
#include "EnhancedWorldReplayTests.h"
#include "CompositionOperatorTests.h"
#include "LensOperatorTests.h"
#include "WorldOverlayOperatorTests.h"
#include "FrozenFrameTests.h"
#include "FrozenWorld.h"
#include "IndexedFallbackReplay.h"
#include "RuntimeResourceTests.h"
#include "RuntimeWorldTests.h"
#include "RuntimeReclamationTests.h"
#include "RuntimeStagingTests.h"
#include "RuntimeTimingTests.h"
#include "RuntimeLightmapTests.h"
#include "RuntimeProcessBudgetTests.h"
#include "kfx/renderer/SceneAssets.h"
#include "kfx/renderer/TerrainMesh.h"
#include "kfx/renderer/SceneSprites.h"
#include <windows.h>
#include <dwmapi.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <dxguids.h>
#include <wrl/client.h>
#include <cstring>
#include <algorithm>
#include <climits>
#include <chrono>
#include <iomanip>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <sstream>

extern "C" {
__declspec(dllexport) extern const UINT D3D12SDKVersion = 619;
__declspec(dllexport) const char* D3D12SDKPath = ".\\D3D12\\";
}

using namespace kfx::render::dx12;
namespace {
// Documented power-setting IDs; no power requests or settings changes.
constexpr GUID console_display_state{0x6fe69556,0x704a,0x47a0,{0x8f,0x24,0xc2,0x8d,0x93,0x6f,0xda,0x47}};
constexpr GUID session_display_state{0x2b84c20e,0xad23,0x4ddf,{0x93,0xdb,0x05,0xff,0xbd,0x7e,0xfc,0xa5}};
struct WindowState {
    bool closed = false, resized = false;
    UINT width = 0, height = 0;
    int console_display = -1, session_display = -1;
    uint64_t console_notifications = 0, session_notifications = 0;
    uint64_t console_observed_tick = 0, session_observed_tick = 0;
};
struct PowerRegistration {
    HPOWERNOTIFY console=nullptr,session=nullptr;DWORD console_error=0,session_error=0;
    explicit PowerRegistration(HWND target) {
        console=RegisterPowerSettingNotification(target,&console_display_state,DEVICE_NOTIFY_WINDOW_HANDLE);
        if(!console)console_error=GetLastError();
        session=RegisterPowerSettingNotification(target,&session_display_state,DEVICE_NOTIFY_WINDOW_HANDLE);
        if(!session)session_error=GetLastError();
    }
    ~PowerRegistration(){if(console)UnregisterPowerSettingNotification(console);if(session)UnregisterPowerSettingNotification(session);}
};
LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    auto* state = reinterpret_cast<WindowState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        state = static_cast<WindowState*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (state) {
        if(message==WM_POWERBROADCAST && wparam==PBT_POWERSETTINGCHANGE && lparam) {
            const auto* setting=reinterpret_cast<const POWERBROADCAST_SETTING*>(lparam);
            if(setting->DataLength==sizeof(DWORD)) {
                DWORD value=0;std::memcpy(&value,setting->Data,sizeof(value));
                if(value<=2 && IsEqualGUID(setting->PowerSetting,console_display_state)) {
                    state->console_display=int(value);++state->console_notifications;
                    state->console_observed_tick=GetTickCount64();return TRUE;
                }
                if(value<=2 && IsEqualGUID(setting->PowerSetting,session_display_state)) {
                    state->session_display=int(value);++state->session_notifications;
                    state->session_observed_tick=GetTickCount64();return TRUE;
                }
            }
        }
        if (message == WM_CLOSE || (message == WM_KEYDOWN && wparam == VK_ESCAPE)) {
            state->closed = true; return 0;
        }
        if (message == WM_SIZE) {
            state->resized = true;
            state->width = wparam == SIZE_MINIMIZED ? 0 : LOWORD(lparam);
            state->height = wparam == SIZE_MINIMIZED ? 0 : HIWORD(lparam);
            return 0;
        }
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
void Pump(Device& device, WindowState& state)
{
    MSG message;
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message); DispatchMessageW(&message);
    }
    if (state.resized) {
        device.Resize(state.width, state.height); state.resized = false;
    }
}
void SetSize(HWND window, Device& device, WindowState& state, UINT width, UINT height)
{
    if (!SetWindowPos(window, nullptr, 0, 0, static_cast<int>(width), static_cast<int>(height), SWP_NOZORDER | SWP_NOACTIVATE))
        throw std::runtime_error("Window resize failed");
    Pump(device, state);
    RECT client;
    if (!GetClientRect(window, &client) || UINT(client.right) != width || UINT(client.bottom) != height)
        throw std::runtime_error("Window did not receive requested client size");
    device.Resize(width, height);
}
std::string Json(const std::string& text)
{
    std::string escaped = "\"";
    for (unsigned char c : text) {
        if (c == '\\' || c == '"') { escaped += '\\'; escaped += char(c); }
        else if (c < 32) { char code[8]; std::snprintf(code, sizeof(code), "\\u%04x", c); escaped += code; }
        else escaped += char(c);
    }
    return escaped + '"';
}
std::string LuidText(uint64_t value)
{
    std::ostringstream out;out << std::hex << std::setw(16) << std::setfill('0') << value;return out.str();
}
uint64_t ParseAdapterLuid(const std::wstring& text)
{
    if(text.size()!=16)throw std::invalid_argument("Adapter LUID requires exactly16 hexadecimal digits, high32 then low32");
    uint64_t value=0;
    for(wchar_t c:text) {
        const unsigned digit=c>=L'0'&&c<=L'9'?unsigned(c-L'0'):c>=L'a'&&c<=L'f'?unsigned(c-L'a'+10)
            :c>=L'A'&&c<=L'F'?unsigned(c-L'A'+10):16;
        if(digit==16)throw std::invalid_argument("Invalid hexadecimal adapter LUID");
        value=(value<<4)|digit;
    }
    return value;
}
std::string AdapterUtf8(const wchar_t* text)
{
    const int size=WideCharToMultiByte(CP_UTF8,0,text,-1,nullptr,0,nullptr,nullptr);
    if(size<=0)throw std::runtime_error("Adapter UTF8 conversion failed");
    std::string result(size,'\0');
    if(WideCharToMultiByte(CP_UTF8,0,text,-1,result.data(),size,nullptr,nullptr)!=size)
        throw std::runtime_error("Adapter UTF8 conversion failed");
    result.pop_back();return result;
}
int ListHardwareAdapters(const std::filesystem::path& output)
{
    using Microsoft::WRL::ComPtr;
    ComPtr<IDXGIFactory6> factory;
    if(FAILED(CreateDXGIFactory2(0,IID_PPV_ARGS(factory.GetAddressOf()))))throw std::runtime_error("Adapter enumeration factory creation failed");
    std::ofstream out(output/"adapters.json");
    out << "{\"schema\":1,\"scope\":\"Current hardware DXGI adapters; actual Agility D3D12 device creation and feature queries, no queue/window/rendering. LUIDs identify this boot, not persistent hardware IDs. No presentation or performance qualification.\",\"agilitySdkVersion\":619,\"debugEnabled\":false,\"adapters\":[";
    uint32_t count=0;
    for(UINT i=0;;++i) {
        ComPtr<IDXGIAdapter1> adapter;
        const HRESULT enumerated=factory->EnumAdapterByGpuPreference(i,DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,IID_PPV_ARGS(adapter.GetAddressOf()));
        if(enumerated==DXGI_ERROR_NOT_FOUND)break;
        if(FAILED(enumerated))throw std::runtime_error("Hardware adapter enumeration failed");
        DXGI_ADAPTER_DESC1 desc{};
        if(FAILED(adapter->GetDesc1(&desc)))throw std::runtime_error("Hardware adapter description failed");
        if(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)continue;
        const uint64_t luid=(uint64_t(uint32_t(desc.AdapterLuid.HighPart))<<32)|desc.AdapterLuid.LowPart;
        ComPtr<ID3D12Device> device;
        const HRESULT created=D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(device.GetAddressOf()));
        UINT shader_model=0;HRESULT shader_result=E_PENDING,level_result=E_PENDING;
        D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_12_2,D3D_FEATURE_LEVEL_12_1,D3D_FEATURE_LEVEL_12_0,D3D_FEATURE_LEVEL_11_1,D3D_FEATURE_LEVEL_11_0};
        D3D12_FEATURE_DATA_FEATURE_LEVELS feature_levels{5,levels,D3D_FEATURE_LEVEL_11_0};
        if(SUCCEEDED(created)) {
            level_result=device->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS,&feature_levels,sizeof(feature_levels));
            for(UINT model=0x69;model>=0x60;--model) {
                D3D12_FEATURE_DATA_SHADER_MODEL sm{D3D_SHADER_MODEL(model)};
                shader_result=device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL,&sm,sizeof(sm));
                if(SUCCEEDED(shader_result)){shader_model=sm.HighestShaderModel;break;}
            }
        }
        if(count++)out << ',';
        out << "{\"luid\":" << Json(LuidText(luid)) << ",\"name\":" << Json(AdapterUtf8(desc.Description))
            << ",\"vendorId\":" << desc.VendorId << ",\"deviceId\":" << desc.DeviceId << ",\"dedicatedVideoMemoryBytes\":" << desc.DedicatedVideoMemory
            << ",\"sharedSystemMemoryBytes\":" << desc.SharedSystemMemory << ",\"deviceHRESULT\":" << int32_t(created)
            << ",\"featureLevelHRESULT\":" << int32_t(level_result) << ",\"maximumFeatureLevel\":" << (SUCCEEDED(level_result)?unsigned(feature_levels.MaxSupportedFeatureLevel):0)
            << ",\"shaderModelHRESULT\":" << int32_t(shader_result) << ",\"shaderModel\":" << shader_model
            << ",\"minimumRendererFeatures\":" << (SUCCEEDED(created)&&shader_model>=0x60?"true":"false") << '}';
    }
    wchar_t runtime_path[32768]{};HMODULE core=GetModuleHandleW(L"D3D12Core.dll");
    const bool runtime_known=core && GetModuleFileNameW(core,runtime_path,32768)>0;
    out << "],\"hardwareCount\":" << count << ",\"runtimePath\":" << Json(runtime_known?AdapterUtf8(runtime_path):std::string{}) << "}\n";
    out.close();if(!out)throw std::runtime_error("Cannot write adapter enumeration report");
    std::cout << "Enumerated " << count << " hardware adapters; see adapters.json\n";return 0;
}

// Exit0 means eligible now; exit2 is a valid but ineligible observation.
// Setup/write failures use the host's existing exit1 error report path.
int DisplayPowerCheck(const std::filesystem::path& output)
{
    WNDCLASSW wc{};wc.lpfnWndProc=WindowProc;wc.hInstance=GetModuleHandleW(nullptr);
    wc.lpszClassName=L"KeeperFX.DX12.DisplayPowerCheck";
    if(!RegisterClassW(&wc))throw std::runtime_error("Display-power window registration failed");
    WindowState state;
    struct HiddenWindow {
        HWND handle=nullptr;
        ~HiddenWindow(){if(handle)DestroyWindow(handle);}
    } window;
    window.handle=CreateWindowExW(0,wc.lpszClassName,L"KeeperFX display-power observation",WS_POPUP,
        0,0,1,1,nullptr,nullptr,wc.hInstance,&state);
    if(!window.handle)throw std::runtime_error("Display-power hidden window creation failed");
    // Never show, focus, wake a display, change power settings, or create a Device.
    const uint64_t started=GetTickCount64(),deadline=started+2000;
    PowerRegistration power(window.handle);
    for(;;) {
        MSG message{};
        while(GetTickCount64()<deadline && PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) {
            TranslateMessage(&message);DispatchMessageW(&message);
        }
        const uint64_t now=GetTickCount64();
        if(now>=deadline || state.closed)break;
        const DWORD result=MsgWaitForMultipleObjectsEx(0,nullptr,DWORD(deadline-now),QS_ALLINPUT,MWMO_INPUTAVAILABLE);
        if(result==WAIT_FAILED)throw std::runtime_error("Display-power notification wait failed");
    }
    const bool eligible=!state.closed && !power.console_error && !power.session_error && state.session_display==1
        && (state.console_display==-1 || state.console_display==1);
    const char* reason=state.closed?"window-closed":power.console_error || power.session_error?"registration-failed"
        :state.session_display==-1?"session-unknown":state.session_display==0?"session-off":state.session_display==2?"session-dim"
        :state.console_display==0?"console-off":state.console_display==2?"console-dim":"eligible";
    std::ofstream out(output/"display-power.json");
    out << "{\"schema\":1,\"eligible\":" << (eligible?"true":"false") << ",\"reason\":" << Json(reason)
        << ",\"scope\":\"Passive endpoint eligibility only, not continuous display-on or scanout evidence. Session ON and successful registrations required; absent console observation is allowed, contradictory observed console state is not. No graphics device created.\""
        << ",\"stateLegend\":\"-1 unknown,0 off,1 on,2 dim; tick timestamps are GetTickCount64 milliseconds\""
        << ",\"startedTickMs\":" << started << ",\"finishedTickMs\":" << GetTickCount64()
        << ",\"consoleDisplay\":" << state.console_display << ",\"sessionDisplay\":" << state.session_display
        << ",\"consoleNotifications\":" << state.console_notifications << ",\"sessionNotifications\":" << state.session_notifications
        << ",\"consoleObservedTickMs\":" << state.console_observed_tick << ",\"sessionObservedTickMs\":" << state.session_observed_tick
        << ",\"consoleRegistrationError\":" << power.console_error << ",\"sessionRegistrationError\":" << power.session_error
        << ",\"remoteSession\":" << (GetSystemMetrics(SM_REMOTESESSION)?"true":"false") << "}\n";
    out.close();if(!out)throw std::runtime_error("Cannot write display-power report");
    std::cout << "Display-power check: " << reason << " (session=" << state.session_display << ", console=" << state.console_display << ")\n";
    return eligible?0:2;
}

// Bounded native HWND control: the normal runtime clear/present path, without SDL,
// gameplay, readbacks or per-frame output. These timings are not scanout timings.
void PresentationProbe(Device& device, HWND window, WindowState& state, const std::filesystem::path& output, uint32_t latency)
{
    using Clock = std::chrono::steady_clock;
    PowerRegistration power(window);
    struct DwmEndpoint {
        DWM_TIMING_INFO timing{};HRESULT result=E_PENDING;
        void Take(){timing.cbSize=sizeof(timing);result=DwmGetCompositionTimingInfo(nullptr,&timing);}
        void Write(std::ostream& out) const {
            out << "{\"hresult\":" << int32_t(result) << ",\"rateRefresh\":[" << timing.rateRefresh.uiNumerator << ',' << timing.rateRefresh.uiDenominator
                << "],\"rateCompose\":[" << timing.rateCompose.uiNumerator << ',' << timing.rateCompose.uiDenominator
                << "],\"qpcRefreshPeriod\":" << timing.qpcRefreshPeriod << ",\"qpcVBlank\":" << timing.qpcVBlank << ",\"cRefresh\":" << timing.cRefresh << '}';
        }
    } dwm_before,dwm_after;
    LARGE_INTEGER qpc_frequency{};QueryPerformanceFrequency(&qpc_frequency);
    WindowState power_before{};
    struct Aggregate {
        uint64_t count = 0;
        double total = 0, maximum = 0;
        std::array<uint64_t,5> buckets{}; // <2, [2,8), [8,20), [20,40), >=40ms.
        void Add(double ms) {
            ++count; total += ms; maximum = std::max(maximum, ms);
            ++buckets[ms < 2 ? 0 : ms < 8 ? 1 : ms < 20 ? 2 : ms < 40 ? 3 : 4];
        }
        void Write(std::ostream& out) const {
            out << "{\"count\":" << count << ",\"meanMs\":" << (count ? total/count : 0)
                << ",\"maximumMs\":" << maximum << ",\"bucketsLt2_2To8_8To20_20To40_Ge40\":[";
            for (size_t i=0;i<buckets.size();++i) { if(i)out << ','; out << buckets[i]; }
            out << "]}";
        }
    } calls, intervals;
    const auto histogram = [](std::ostream& out, const PresentationGapHistogram& h) {
        out << "{\"count\":" << h.count << ",\"maximumMs\":" << h.maximum_ms << ",\"bucketsLt2_2To8_8To20_Ge20\":[";
        for(size_t i=0;i<h.buckets.size();++i){if(i)out << ',';out << h.buckets[i];}out << "]}";
    };
    const auto endpoint = [&](std::ostream& out, const PresentationDiagnostics& d) {
        out << "{\"presentOk\":" << d.present_ok << ",\"presentOccluded\":" << d.present_occluded
            << ",\"presentOther\":" << d.present_other << ",\"lastPresentHRESULT\":" << d.last_present_hresult
            << ",\"admissionPolls\":" << d.admission_polls << ",\"admissionGrants\":" << d.admission_grants
            << ",\"admissionNotReady\":" << d.admission_not_ready << ",\"pendingPollGaps\":";
        histogram(out,d.pending_poll_gaps);out << ",\"grantPollGaps\":";histogram(out,d.grant_poll_gaps);
        out << ",\"pendingWaitDurations\":";histogram(out,d.pending_wait_durations);
        out << ",\"outputStatus\":" << unsigned(d.output_status) << ",\"outputHRESULT\":" << d.output_hresult
            << ",\"outputName\":" << Json(d.output_name) << ",\"desktopRect\":[" << d.desktop_rect[0] << ','
            << d.desktop_rect[1] << ',' << d.desktop_rect[2] << ',' << d.desktop_rect[3]
            << "],\"attachedToDesktop\":" << d.attached_to_desktop << ",\"nominalRefreshAvailable\":" << d.refresh_available
            << ",\"nominalRefreshHz\":" << d.refresh_hz << ",\"frameStatisticsStatus\":" << unsigned(d.frame_statistics_status)
            << ",\"frameStatisticsHRESULT\":" << d.frame_statistics_hresult << ",\"presentCount\":" << d.present_count
            << ",\"presentRefreshCount\":" << d.present_refresh_count << ",\"syncRefreshCount\":" << d.sync_refresh_count
            << ",\"syncQPCTime\":" << d.sync_qpc_time << ",\"syncGPUTime\":" << d.sync_gpu_time
            << ",\"lastCountStatus\":" << unsigned(d.last_count_status) << ",\"lastCountHRESULT\":" << d.last_count_hresult
            << ",\"lastPresentCount\":" << d.last_present_count << '}';
    };
    RECT client{};
    if(!GetClientRect(window,&client) || client.right!=3840 || client.bottom!=2160)
        throw std::runtime_error("Presentation probe requires exact3840x2160 client pixels");
    RuntimeWorldFrame frame;
    frame.native_width=frame.viewport_width=3840;frame.native_height=frame.viewport_height=2160;
    frame.clear_rgba={0.04f,0.09f,0.16f,1};
    device.SetRuntimePresentation(true,0,0,3840,2160);
    const bool activation_requested=SetForegroundWindow(window)!=FALSE;
    const auto started=Clock::now(), hard_deadline=started+std::chrono::seconds(20);
    auto measure_start=started, measure_end=started, previous=started;
    bool measuring=false, have_previous=false;
    uint64_t warmup_frames=0, frames=0, not_foreground_frames=0, retries=0;
    PresentationDiagnostics before{};
    for(;;) {
        Pump(device,state);
        const auto now=Clock::now();
        if(state.closed || !state.width || !state.height || state.width!=3840 || state.height!=2160)
            throw std::runtime_error("Presentation probe closed, minimized or resized before completion");
        if(now>=hard_deadline)throw std::runtime_error("Presentation probe exceeded20second loop deadline");
        if(!measuring && now-started>=std::chrono::seconds(2)) {
            before=device.GetPresentationDiagnostics();dwm_before.Take();power_before=state;
            measure_start=Clock::now();measuring=true;
        }
        if(measuring && now-measure_start>=std::chrono::seconds(10)){measure_end=now;break;}
        if(!device.TryAcquireRuntimePresentation()){SwitchToThread();continue;}
        const auto submitted=device.TrySubmitRuntimeWorldFrame(frame,false);
        if(submitted.status!=RuntimeSubmitStatus::Submitted){++retries;SwitchToThread();continue;}
        const auto returned=Clock::now();
        if(measuring) {
            ++frames;calls.Add(submitted.cpu_present_ms);
            if(have_previous)intervals.Add(std::chrono::duration<double,std::milli>(returned-previous).count());
            previous=returned;have_previous=true;
            if(GetForegroundWindow()!=window)++not_foreground_frames;
        } else ++warmup_frames;
    }
    const auto after=device.GetPresentationDiagnostics();dwm_after.Take();
    const auto power_after=state;
    // Hardware Flip Queue may defer DXGI statistics. Flush only AFTER the sample.
    const auto flush_start=Clock::now();const HRESULT flush_result=DwmFlush();
    const double flush_ms=std::chrono::duration<double,std::milli>(Clock::now()-flush_start).count();
    const auto after_flush=device.GetPresentationDiagnostics();
    const auto runtime=device.GetRuntimeResourceDiagnostics();
    const double seconds=std::chrono::duration<double>(measure_end-measure_start).count();
    // Explicit teardown only, after both timing endpoints have been captured.
    device.ClearRuntimeResources();
    const auto diagnostics=device.GetDiagnostics();
    const bool passed=frames>0 && !diagnostics.validation_errors && !diagnostics.validation_warnings;
    std::ofstream out(output/"presentation-probe.json");
    out << std::setprecision(12) << "{\"schema\":1,\"passed\":" << (passed?"true":"false")
        << ",\"scope\":\"Native Win32 popup; runtime empty-world clear; waitable selected latency; Present(1,0). CPU Present-return cadence, not display FPS. Cumulative diagnostic endpoints bracket measurement, with no warmup samples in interval aggregates. Deadline checked between nonblocking admission and submissions; OS Present/DwmFlush/teardown calls cannot be interrupted. DWM timing is session/compositor data, not game scanout; DwmFlush and the final statistics query occur after measurement.\""
        << ",\"queryStatusLegend\":\"0 disabled,1 valid,2 disjoint,3 unsupported,4 failed; S_OK zero frame-statistics values are not scanout evidence\""
        << ",\"waitableVSyncLatency\":" << latency
        << ",\"adapter\":" << Json(diagnostics.adapter) << ",\"adapterLuid\":" << Json(LuidText(diagnostics.adapter_luid))
        << ",\"vendorId\":" << diagnostics.vendor_id << ",\"deviceId\":" << diagnostics.device_id
        << ",\"debugEnabled\":" << diagnostics.debug_enabled << ",\"gpuValidationEnabled\":" << diagnostics.gpu_validation_enabled
        << ",\"width\":3840,\"height\":2160,\"warmupSeconds\":2,\"requestedMeasureSeconds\":10"
        << ",\"measuredSeconds\":" << seconds << ",\"warmupFrames\":" << warmup_frames << ",\"frames\":" << frames
        << ",\"presentReturnsPerSecond\":" << frames/seconds << ",\"activationRequestSucceeded\":" << activation_requested
        << ",\"remoteSession\":" << (GetSystemMetrics(SM_REMOTESESSION)!=0)
        << ",\"notForegroundFrames\":" << not_foreground_frames << ",\"submissionRetries\":" << retries
        << ",\"imageReadbacks\":" << runtime.image_readbacks << ",\"validationErrors\":" << diagnostics.validation_errors
        << ",\"validationWarnings\":" << diagnostics.validation_warnings << ",\"presentCalls\":";
    calls.Write(out);out << ",\"presentReturnIntervals\":";intervals.Write(out);
    out << ",\"presentationBefore\":";endpoint(out,before);out << ",\"presentationAfter\":";endpoint(out,after);
    out << ",\"qpcFrequency\":" << qpc_frequency.QuadPart << ",\"dwmBefore\":";dwm_before.Write(out);
    out << ",\"dwmAfter\":";dwm_after.Write(out);
    out << ",\"dwmFlushHRESULT\":" << int32_t(flush_result) << ",\"dwmFlushMs\":" << flush_ms
        << ",\"presentationAfterFlush\":";endpoint(out,after_flush);
    out << ",\"powerStateLegend\":\"-1 unknown/no valid notification,0 off,1 on,2 dim; passive last-observed values at measurement endpoints\""
        << ",\"consoleDisplayRegistrationError\":" << power.console_error << ",\"sessionDisplayRegistrationError\":" << power.session_error
        << ",\"consoleDisplayBefore\":" << power_before.console_display << ",\"consoleDisplayAfter\":" << power_after.console_display
        << ",\"consoleNotificationsBefore\":" << power_before.console_notifications << ",\"consoleNotificationsAfter\":" << power_after.console_notifications
        << ",\"sessionDisplayBefore\":" << power_before.session_display << ",\"sessionDisplayAfter\":" << power_after.session_display
        << ",\"sessionNotificationsBefore\":" << power_before.session_notifications << ",\"sessionNotificationsAfter\":" << power_after.session_notifications << "}\n";
    out.close();if(!out)throw std::runtime_error("Cannot write presentation probe report");
    if(!passed)throw std::runtime_error("Presentation probe had no frames or validation messages");
    std::cout << "Native presentation probe: " << frames << " Present returns in " << seconds << "s; not display FPS\n";
}

void SaveBitmap(const std::filesystem::path& path, const Image& image)
{
    BITMAPFILEHEADER file = {};
    file.bfType = 0x4d42; file.bfOffBits = sizeof(file) + sizeof(BITMAPINFOHEADER);
    file.bfSize = file.bfOffBits + static_cast<DWORD>(image.rgba.size());
    BITMAPINFOHEADER info = {};
    info.biSize = sizeof(info); info.biWidth = image.width; info.biHeight = -LONG(image.height);
    info.biPlanes = 1; info.biBitCount = 32; info.biCompression = BI_RGB;
    auto bgra = image.rgba;
    for (size_t i = 0; i < bgra.size(); i += 4) std::swap(bgra[i], bgra[i + 2]);
    std::ofstream stream(path, std::ios::binary);
    stream.write(reinterpret_cast<const char*>(&file), sizeof(file));
    stream.write(reinterpret_cast<const char*>(&info), sizeof(info));
    stream.write(reinterpret_cast<const char*>(bgra.data()), bgra.size());
    if (!stream) throw std::runtime_error("Screenshot write failed");
}
const std::array<float, 16> identity = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
const std::array<std::array<uint8_t, 4>, 4> colors = {{{208,64,48,255}, {232,176,48,255}, {40,168,200,255}, {136,80,184,255}}};
Image CalibrationTexture(uint8_t shift = 0)
{
    Image image;
    // Odd dimensions deliberately exercise D3D12 row-pitch alignment.
    image.width = 65; image.height = 67;
    image.rgba.resize(size_t(image.width) * image.height * 4);
    for (UINT y = 0; y < image.height; ++y) for (UINT x = 0; x < image.width; ++x) {
        auto color = colors[(x >= image.width / 2 ? 1 : 0) + (y >= image.height / 2 ? 2 : 0)];
        color[0] = uint8_t(color[0] + shift);
        std::copy(color.begin(), color.end(), image.rgba.begin() + (size_t(y) * image.width + x) * 4);
    }
    return image;
}
void Quad(std::vector<Vertex>& vertices, std::vector<uint32_t>& indices, const std::array<Vertex, 4>& quad)
{
    uint32_t base = static_cast<uint32_t>(vertices.size());
    vertices.insert(vertices.end(), quad.begin(), quad.end());
    for (auto index : {0u,1u,2u,0u,2u,3u}) indices.push_back(base + index);
}
void CalibrationMesh(std::vector<Vertex>& vertices, std::vector<uint32_t>& indices)
{
    Quad(vertices, indices, {{{-.75f,.75f,.3f,0,0}, {.75f,.75f,.3f,1,0}, {.75f,-.75f,.3f,1,1}, {-.75f,-.75f,.3f,0,1}}});
    // Draw the back plane last. It must remain occluded by the front plane.
    Quad(vertices, indices, {{{-.25f,.25f,.7f,.2f,.8f}, {.25f,.25f,.7f,.2f,.8f}, {.25f,-.25f,.7f,.2f,.8f}, {-.25f,-.25f,.7f,.2f,.8f}}});
}
void Require(bool condition, const char* message, UINT& checks)
{
    if (!condition) throw std::runtime_error(message);
    ++checks;
}
void CheckPixel(const Image& image, float fx, float fy, const std::array<uint8_t, 4>& expected, UINT& checks)
{
    const size_t offset = (size_t(image.height * fy) * image.width + size_t(image.width * fx)) * 4;
    for (UINT c = 0; c < 4; ++c)
        Require(std::abs(int(image.rgba.at(offset + c)) - int(expected[c])) <= 1, "GPU pixel check failed", checks);
}
void CheckCalibration(const Image& image, UINT width, UINT height, UINT& checks, uint8_t shift = 0)
{
    Require(image.width == width && image.height == height, "Readback dimensions mismatch", checks);
    for (UINT y = 0; y < 2; ++y) for (UINT x = 0; x < 2; ++x) {
        auto expected = colors[x + 2 * y]; expected[0] = uint8_t(expected[0] + shift);
        CheckPixel(image, x ? .7f : .3f, y ? .7f : .3f, expected, checks);
    }
    auto occluded = colors[0]; occluded[0] = uint8_t(occluded[0] + shift);
    CheckPixel(image, .45f, .45f, occluded, checks);
    CheckPixel(image, .02f, .02f, {6,9,14,255}, checks);
}
void DungeonMesh(std::vector<Vertex>& vertices, std::vector<uint32_t>& indices)
{
    auto block = [&](float x, float y, float h) {
        Quad(vertices, indices, {{{x,y,h,0,0},{x+1,y,h,1,0},{x+1,y+1,h,1,1},{x,y+1,h,0,1}}});
        Quad(vertices, indices, {{{x,y,0,0,1},{x+1,y,0,1,1},{x+1,y,h,1,0},{x,y,h,0,0}}});
        Quad(vertices, indices, {{{x,y+1,0,0,1},{x,y,0,1,1},{x,y,h,1,0},{x,y+1,h,0,0}}});
        Quad(vertices, indices, {{{x+1,y,0,0,1},{x+1,y+1,0,1,1},{x+1,y+1,h,1,0},{x+1,y,h,0,0}}});
        Quad(vertices, indices, {{{x+1,y+1,0,0,1},{x,y+1,0,1,1},{x,y+1,h,1,0},{x+1,y+1,h,0,0}}});
    };
    for (int y = -4; y < 4; ++y) for (int x = -4; x < 4; ++x) {
        Quad(vertices, indices, {{{float(x),float(y),0,0,0},{float(x+1),float(y),0,1,0},
            {float(x+1),float(y+1),0,1,1},{float(x),float(y+1),0,0,1}}});
        if (x == 3 || y == 3 || (x == -3 && y == -2) || (x == 1 && y == -2)) block(float(x), float(y), 1.4f);
    }
}
Image StoneTexture()
{
    Image image; image.width = image.height = 128; image.rgba.resize(128 * 128 * 4);
    for (UINT y = 0; y < 128; ++y) for (UINT x = 0; x < 128; ++x) {
        bool seam = y % 32 < 3 || (x + ((y / 32) % 2) * 32) % 64 < 3;
        int grain = int((x * 17 + y * 31) % 11) - 5;
        size_t offset = (y * 128 + x) * 4;
        image.rgba[offset] = uint8_t(seam ? 36 : 138 + grain);
        image.rgba[offset+1] = uint8_t(seam ? 32 : 108 + grain);
        image.rgba[offset+2] = uint8_t(seam ? 29 : 76 + grain);
        image.rgba[offset+3] = 255;
    }
    return image;
}
std::array<float, 16> DungeonTransform(float angle = 0)
{
    float c = std::cos(angle), s = std::sin(angle);
    return {.11f*(c-s), .08f*(c+s), .03f*(c+s), 0,
        -.11f*(s+c), .08f*(c-s), .03f*(c-s), 0, 0,.22f,-.055f,0, 0,-.12f,.5f,1};
}
void TerrainPreview(Device& device, HWND window, WindowState& state, const std::filesystem::path& path,
    const std::filesystem::path& output, bool interactive, bool full_walls, UINT& checks)
{
    using namespace kfx::render;
    const auto owned = LoadSceneCapture(path);
    TerrainOptions options; options.walls = full_walls ? TerrainWalls::Full : TerrainWalls::Captured;
    auto mesh = BuildTerrainMesh(*owned, options);
    auto atlas = BuildTerrainAtlas(*owned, mesh);
    Image texture{atlas.width, atlas.height, atlas.rgba};
    // Verify GPU palette lookup against the original captured indexed storage,
    // fade table and palette, including atlas edge texels and fractional shade.
    SetSize(window, device, state, 256, 256);
    uint64_t palette_pixels = 0;
    for (size_t material : {size_t(0), mesh.materials.size()/2, mesh.materials.size()-1}) {
        for (float shade : {0.0f, 31.75f, 63.0f}) {
            const auto origin = atlas.origins[material];
            std::vector<Vertex> vertices;
            std::vector<uint32_t> indices;
            Quad(vertices, indices, {{{-1,-1,.3f,0,1}, {1,-1,.3f,1,1}, {1,1,.3f,1,0}, {-1,1,.3f,0,0}}});
            for (auto& v : vertices) { v.shade=shade; v.tile_x=float(origin[0]); v.tile_y=float(origin[1]); }
            device.Upload(vertices,indices,texture);
            Image pixels; device.Draw(identity,&pixels);
            Require(pixels.width==256 && pixels.height==256,"Terrain palette fixture size mismatch",checks);
            const auto& mat = mesh.materials[material];
            bool matched = true;
            for (unsigned y=0;y<256;++y) for (unsigned x=0;x<256;++x) {
                const auto index = owned->assets.texture_pixels[size_t(mat.pixel_offset)+(y/8)*owned->scene.texture_row_stride+x/8];
                const auto remap = owned->assets.fade[size_t(shade)*256+index];
                for (unsigned channel=0;channel<3;++channel)
                    matched &= pixels.rgba[(y*256+x)*4+channel] == owned->assets.display_palette[remap*3+channel]*255/63;
                matched &= pixels.rgba[(y*256+x)*4+3] == 255;
                ++palette_pixels;
            }
            if (!matched) SaveBitmap(output/"palette-failed.bmp",pixels);
            Require(matched,"Terrain palette/UV readback differs from captured source",checks);
        }
    }
    bool bright = false;
    auto upload = [&] {
        std::vector<Vertex> vertices;
        vertices.reserve(mesh.vertices.size());
        for (const auto& v : mesh.vertices) {
            const auto origin = atlas.origins[v.material];
            vertices.push_back({v.x,v.y,v.z,v.u,v.v,v.shade,float(origin[0]),float(origin[1])});
        }
        if (bright) for (const auto& q : mesh.quads) for (unsigned i=0;i<4;++i)
            vertices[q.first_vertex+i].shade = q.revealed ? 32.0f : 0.0f;
        device.Upload(vertices,mesh.indices,texture);
    };
    upload();
    SetWindowTextW(window,L"KeeperFX DX12 terrain | Arrows: orbit/tilt | +/-: zoom | W: wall height | F: light diagnostic | Escape: close");
    SetSize(window,device,state,3840,2160);
    float yaw=.78539816f, elevation=.95f, zoom=1.25f;
    auto transform = [&] { return TerrainInspectionTransform(mesh,float(state.width)/std::max(1u,state.height),yaw,elevation,zoom); };
    Image pixels; device.Draw(transform(),&pixels);
    size_t terrain_pixels = 0;
    for (size_t i=0;i<pixels.rgba.size();i+=4)
        terrain_pixels += pixels.rgba[i]!=6 || pixels.rgba[i+1]!=9 || pixels.rgba[i+2]!=14;
    Require(terrain_pixels > 1000,"Terrain preview contains no visible surfaces",checks);
    SaveBitmap(output/"terrain-4k.bmp",pixels);
    if (!interactive) {
        yaw += 1.57079633f; device.Draw(transform(),&pixels); SaveBitmap(output/"terrain-rotated.bmp",pixels);
        elevation=.4f; device.Draw(transform(),&pixels); SaveBitmap(output/"terrain-low.bmp",pixels);
        bright=true; upload(); device.Draw(transform(),&pixels); SaveBitmap(output/"terrain-geometry.bmp",pixels);
    }
    std::ofstream report(output/"terrain.json");
    report << "{\"epoch\":" << mesh.epoch << ",\"sequence\":" << mesh.sequence
        << ",\"columns\":" << mesh.columns << ",\"hiddenColumns\":" << mesh.hidden_columns
        << ",\"faces\":" << mesh.quads.size() << ",\"triangles\":" << mesh.indices.size()/3
        << ",\"vertices\":" << mesh.vertices.size() << ",\"chunks\":" << mesh.chunks.size()
        << ",\"materials\":" << mesh.materials.size() << ",\"culledSides\":" << mesh.culled_sides
        << ",\"missingNeighbors\":" << mesh.missing_neighbors << ",\"ceilingFallbacks\":" << mesh.ceiling_fallbacks
        << ",\"wibbleColumns\":" << mesh.wibble_columns << ",\"liquidColumns\":" << mesh.liquid_columns
        << ",\"palettePixelChecks\":" << palette_pixels << ",\"terrainPixels\":" << terrain_pixels
        << ",\"atlasBytes\":" << atlas.rgba.size() << ",\"camera\":\"orthographic inspection camera\"}\n";
    if (!report) throw std::runtime_error("Terrain report write failed");
    if (!interactive) {
        // Exercise resource replacement with both wall profiles, just as W does.
        for (auto walls : {TerrainWalls::Full, TerrainWalls::Short}) {
            options.walls=walls;
            mesh=BuildTerrainMesh(*owned,options); atlas=BuildTerrainAtlas(*owned,mesh);
            texture={atlas.width,atlas.height,atlas.rgba}; upload();
            Require(device.Draw(transform()),"Terrain wall-profile replacement failed",checks);
        }
        // Minimized windows must suspend presentation without losing the mesh.
        device.Resize(0,0);
        Require(!device.Draw(transform()),"Minimized terrain viewer still presented",checks);
        device.Resize(state.width,state.height);
        Require(device.Draw(transform()),"Terrain viewer did not recover after minimization",checks);
    }
    bool last_w=false,last_f=false;
    while(interactive && !state.closed) {
        Pump(device,state);
        if (state.closed) break;
        if (!state.width || !state.height) { Sleep(16); continue; }
        // Only the foreground viewer responds to keys; no global input capture.
        if(GetForegroundWindow()==window) {
            auto key=[](int k){return (GetAsyncKeyState(k)&0x8000)!=0;};
            yaw += (key(VK_RIGHT)-key(VK_LEFT))*.018f;
            elevation=std::clamp(elevation+(key(VK_UP)-key(VK_DOWN))*.012f,.1f,1.5f);
            zoom=std::clamp(zoom*(key(VK_OEM_PLUS)?1.015f:key(VK_OEM_MINUS)?.985f:1),.3f,8.0f);
            bool w=key('W'), f=key('F');
            if(w&&!last_w) {
                options.walls=options.walls==TerrainWalls::Full?TerrainWalls::Short:TerrainWalls::Full;
                mesh=BuildTerrainMesh(*owned,options); atlas=BuildTerrainAtlas(*owned,mesh);
                texture={atlas.width,atlas.height,atlas.rgba}; upload();
            }
            if(f&&!last_f) {bright=!bright;upload();}
            last_w=w; last_f=f;
        }
        if(!device.Draw(transform())) Sleep(16);
    }
}

void WorldPreview(Device& device, HWND window, WindowState& state, const std::filesystem::path& path,
    const std::filesystem::path& output, bool interactive, bool full_walls, UINT& checks)
{
    using namespace kfx::render;
    // Draw the near cutout first, the background second, and a nearer wall
    // last. Opaque palette-zero pixels stay black; holes preserve background
    // depth; the wall occludes both. This checks actual GPU depth/coverage.
    SetSize(window,device,state,256,256);
    std::vector<Vertex> fixture_vertices;
    std::vector<uint32_t> fixture_indices;
    Image fixture_texture{4,1,{0,0,0,255, 255,0,0,0, 0,0,255,255, 0,255,0,255}};
    Quad(fixture_vertices,fixture_indices,{{{-1,-1,.3f,0,0},{1,-1,.3f,.5f,0},{1,1,.3f,.5f,0},{-1,1,.3f,0,0}}});
    Quad(fixture_vertices,fixture_indices,{{{-1,-1,.6f,.625f,0},{1,-1,.6f,.625f,0},{1,1,.6f,.625f,0},{-1,1,.6f,.625f,0}}});
    Quad(fixture_vertices,fixture_indices,{{{-.125f,-1,.1f,.875f,0},{.125f,-1,.1f,.875f,0},{.125f,1,.1f,.875f,0},{-.125f,1,.1f,.875f,0}}});
    for (bool perspective : {false,true}) {
        auto vertices=fixture_vertices;
        auto transform=identity;
        for (auto& v:vertices) {
            v.shade=-2;
            if (perspective) {
                const float depth=v.z==.1f?.5f:v.z==.6f?3.0f:v.x<0?1.0f:2.0f;
                v.x*=depth; v.y*=depth; v.z=depth;
            }
        }
        if (perspective) {transform[11]=1;transform[14]=-.05f;transform[15]=0;}
        device.Upload(vertices,fixture_indices,fixture_texture);
        Image fixture; device.Draw(transform,&fixture);
        bool coverage_ok=fixture.width==256 && fixture.height==256;
        for (unsigned y=0;y<256 && coverage_ok;++y) for (unsigned x=0;x<256;++x) {
            const auto* pixel=&fixture.rgba[(y*256+x)*4];
            const bool wall=x>=112 && x<144;
            coverage_ok &= pixel[0]==0 && pixel[1]==(wall?255:0) && pixel[2]==(!wall&&x>=128?255:0) && pixel[3]==255;
        }
        if (!coverage_ok) SaveBitmap(output/"sprite-depth-failed.bmp",fixture);
        Require(coverage_ok,"Sprite coverage/depth/screen-sampling readback failed",checks);
    }
    const auto capture = LoadSceneCapture(path);
    const CapturedCamera camera(capture->scene.camera);
    const auto comparison = CompareCapturedCamera(*capture);
    Require(comparison.passed, "Captured camera differs from the original projection reference", checks);
    const auto terrain = BuildTerrainMesh(*capture, {full_walls ? TerrainWalls::Full : TerrainWalls::Captured});
    const auto sprites = BuildSceneSprites(*capture, terrain);
    auto upload = [&](bool show_sprites) {
        std::vector<Vertex> vertices;
        auto indices = terrain.indices;
        if (camera.reverse_winding) for (size_t i=0;i<indices.size();i+=3) std::swap(indices[i+1],indices[i+2]);
        for (const auto& v : terrain.vertices) {
            const auto origin = sprites.atlas.origins[v.material];
            vertices.push_back({v.x,v.y,v.z,v.u,v.v,v.shade,float(origin[0]),float(origin[1])});
        }
        if (show_sprites) for (const auto& q : sprites.quads) {
            const auto base = uint32_t(vertices.size());
            for (const auto& v : q.vertices) vertices.push_back({v.x,v.y,v.z,v.u,v.v,-2});
            for (auto i : {0u,1u,2u,0u,2u,3u}) indices.push_back(base+i);
        }
        device.Upload(vertices,indices,{sprites.atlas.width,sprites.atlas.height,sprites.atlas.rgba});
    };
    SetWindowTextW(window,L"KeeperFX DX12 captured world | S: sprites on/off | Escape: close");
    SetSize(window,device,state,camera.source.viewport_w,camera.source.viewport_h);
    upload(false); Image background; device.Draw(camera.transform,&background);
    SaveBitmap(output/"world-terrain.bmp",background);
    upload(true); Image pixels; device.Draw(camera.transform,&pixels);
    SaveBitmap(output/"world-captured.bmp",pixels);
    size_t changed = 0;
    for (size_t i = 0; i < pixels.rgba.size(); i += 4)
        changed += !std::equal(pixels.rgba.begin()+i,pixels.rgba.begin()+i+4,background.rgba.begin()+i);
    Require(sprites.quads.empty() || changed > 0, "Captured sprites contributed no pixels", checks);
    std::ofstream report(output/"world.json");
    report << "{\"view\":" << unsigned(camera.source.view) << ",\"width\":" << pixels.width << ",\"height\":" << pixels.height
        << ",\"cameraChecks\":" << comparison.checked << ",\"cameraMaxErrorPixels\":" << comparison.maximum_error
        << ",\"cameraRmsErrorPixels\":" << comparison.rms_error << ",\"objects\":" << capture->scene.objects.size()
        << ",\"spriteQuads\":" << sprites.quads.size() << ",\"emptySprites\":" << sprites.empty
        << ",\"clippedSprites\":" << sprites.clipped << ",\"deferredEffects\":" << sprites.deferred_effects
        << ",\"spritePixels\":" << changed << ",\"coveragePixelChecks\":131072,\"triangles\":" << terrain.indices.size()/3+sprites.quads.size()*2 << "}\n";
    if (!report) throw std::runtime_error("World preview report write failed");
    bool show_sprites = true, last_s = false;
    while (interactive && !state.closed) {
        Pump(device,state);
        if (state.closed) break;
        if (GetForegroundWindow()==window) {
            const bool s = (GetAsyncKeyState('S')&0x8000)!=0;
            if (s && !last_s) { show_sprites=!show_sprites; upload(show_sprites); }
            last_s=s;
        }
        if (!device.Draw(camera.transform)) Sleep(16);
    }
}

void WriteReport(const std::filesystem::path& directory, const Diagnostics& d, UINT checks, bool interactive, bool assets, bool terrain, bool world, bool palette, bool sprite_replay, bool terrain_replay, bool outline_replay, bool comparison_review, bool frozen_test, bool frozen_world, bool enhanced_test, bool enhanced_replay, bool enhanced_world_test, bool enhanced_world, bool lens_test, bool world_overlay_test, bool composition_test, bool indexed_fallback, bool runtime_resource_test, bool runtime_world_test, bool runtime_process_budget_test)
{
    std::ofstream report(directory / "report.json");
    report << "{\n  \"status\": \"passed\",\n  \"scope\": "
        << Json(runtime_process_budget_test ? "Injected process-budget admission and resident progress on actual runtime resources; real OS observations separate, no live producer or complete Gate3 acceptance"
        : runtime_world_test ? "Asynchronous terrain, shadow, ordered sprite, overlay, lens and owned-packet resource fixtures; execution evidence only, with live-engine and gate acceptance recorded separately"
        : runtime_resource_test ? "Asynchronous versioned indexed-world resources, bounded uploads and actual native presentation; first runtime-resource slice, not general world caches or live gameplay acceptance"
        : indexed_fallback ? "Owned custom-lens indexed output upload, exact native index/palette comparison and resident presentation; no callback execution or runtime-cache acceptance"
        : composition_test ? "Resident native world, depth/flat/swipe and lens integration controls against production OpenGL; no live UI or gameplay acceptance"
        : world_overlay_test ? "World status, flat and swipe overlay operators on declared native targets and terrain depth against production OpenGL shaders; no complete captured world, live UI or gameplay acceptance"
            : lens_test ? "Standard enhanced lens operators on declared native images against production OpenGL shaders and independent analytic fixtures; no actual possession-frame, Lua or gameplay acceptance"
            : enhanced_world ? "Captured enhanced world and, for v11, executed overlays and lens composition on GPU-resident surfaces; independent production OpenGL comparison, no live UI or gameplay acceptance"
            : enhanced_world_test ? "Enhanced terrain, lighting, shadow and sprite composition operator diagnostics against production OpenGL shaders; no complete-world or gameplay acceptance"
            : enhanced_replay ? "Captured v9 OpenGL normal sprites, glow and ordered outlines on declared backgrounds with actual terrain depth; independent production shader reference, no terrain colour, full-world acceptance or gameplay"
            : enhanced_test ? "Normal sprite, effective colour lookup, glow and outline operator diagnostics against production OpenGL shaders; no world or gameplay acceptance"
            : frozen_world ? "Captured v8 software world viewport with proven clear and direct GPU-resident presentation; no UI, lens composition or gameplay"
            : frozen_test ? "GPU-resident frozen indexed frame and direct presentation; resource ownership tests, no runtime cache or gameplay"
            : outline_replay ? "Captured OpenGL outline depth and RGBA coverage diagnostics; independent production shader reference, no full-world acceptance or gameplay"
            : terrain_replay ? "Ordered captured terrain and sprite replay; hardware raster differences require review, no full world acceptance or gameplay"
            : sprite_replay ? "Ordered captured sprite replay on declared indexed backgrounds; original C comparisons, no terrain interleaving or gameplay"
            : palette ? "Controlled indexed palette-composition fixture; not world draw order, additive effects or gameplay"
            : world ? "Captured game camera, terrain and depth-tested cutout sprites; frozen scene, no gameplay"
            : terrain ? "Captured terrain mesh and original palette shading; standalone inspection camera, no gameplay or sprites"
            : assets ? "Owned scene assets: independent file load and GPU contact-sheet pixel parity"
            : "Standalone DX12 foundation; procedural assets, not game rendering") << ","
        << "\n  \"adapter\": " << Json(d.adapter) << ",\n  \"vendorId\": " << d.vendor_id
        << ",\n  \"deviceId\": " << d.device_id << ",\n  \"adapterLuid\": " << Json(LuidText(d.adapter_luid))
        << ",\n  \"runtimePath\": " << Json(d.runtime_path) << ",\n  \"agilitySdkVersion\": 619"
        << ",\n  \"shaderModel\": " << Json(std::to_string(d.shader_model >> 4) + "." + std::to_string(d.shader_model & 15))
        << ",\n  \"barrierPath\": \"legacy\",\n  \"enhancedBarriersSupported\": " << (d.enhanced_barriers_supported ? "true" : "false")
        << ",\n  \"rasterizerOrderedViewsQuerySucceeded\": " << (d.rasterizer_ordered_views_query_succeeded ? "true" : "false")
        << ",\n  \"rasterizerOrderedViewsSupported\": " << (d.rasterizer_ordered_views_supported ? "true" : "false")
        << ",\n  \"tearingSupported\": " << (d.tearing_supported ? "true" : "false")
        << ",\n  \"debugLayer\": " << (d.debug_enabled ? "true" : "false")
        << ",\n  \"gpuValidation\": " << (d.gpu_validation_enabled ? "true" : "false")
        << ",\n  \"interactive\": " << (interactive ? "true" : "false")
        << ",\n  \"comparisonReviewRequired\": " << (comparison_review ? "true" : "false")
        << ",\n  \"checks\": " << checks << ",\n  \"frames\": " << d.frames << ",\n  \"resizes\": " << d.resizes
        << ",\n  \"allocationBytes\": " << d.allocation_bytes << ",\n  \"allocationCount\": " << d.allocation_count
        << ",\n  \"frozenPreparations\": " << d.frozen_preparations << ",\n  \"frozenPresentations\": " << d.frozen_presentations
        << ",\n  \"frozenImageReadbacks\": " << d.frozen_image_readbacks
        << ",\n  \"validationErrors\": " << d.validation_errors << ",\n  \"validationWarnings\": " << d.validation_warnings << "\n}\n";
    if (!report) throw std::runtime_error("Report write failed");
}
} // namespace

int wmain(int argc, wchar_t** argv)
{
    std::filesystem::path output, scene_path, indexed_fallback_path, runtime_resource_input;
    Options options;
    bool interactive = false, presentation_probe = false, probe_latency_set = false, display_power_check = false, list_adapters = false;
    bool comparison_review = false;
    bool frozen_test = false, frozen_world = false;
    bool enhanced_test = false, enhanced_replay = false;
    bool enhanced_world_test = false, enhanced_world = false;
    bool enhanced_world_native_origin = false;
    bool enhanced_world_fade_texel_load = false;
    bool lens_test = false;
    bool world_overlay_test = false;
    bool composition_test = false;
    bool indexed_fallback_test = false;
    bool runtime_resource_test = false, runtime_world_test = false;
    bool runtime_process_budget_test = false;
    bool terrain = false, full_walls = false, world = false, palette = false, sprite_replay = false, terrain_replay = false, outline_replay = false;
    HWND window = nullptr;
    std::unique_ptr<Device> device;
    try {
        for (int i = 1; i < argc; ++i) {
            std::wstring arg = argv[i];
            if (arg == L"--output" && i + 1 < argc) output = argv[++i];
            else if (arg == L"--scene-capture" && i + 1 < argc) scene_path = argv[++i];
            else if (arg == L"--indexed-fallback" && i + 1 < argc) indexed_fallback_path = argv[++i];
            else if (arg == L"--indexed-fallback-test") indexed_fallback_test = true;
            else if (arg == L"--runtime-world-test") runtime_world_test = true;
            else if (arg == L"--runtime-process-budget-test") runtime_process_budget_test = true;
            else if (arg == L"--runtime-resource-test") runtime_resource_test = true;
            else if (arg == L"--runtime-resource-input" && i + 1 < argc) runtime_resource_input = argv[++i];
            else if (arg == L"--interactive") interactive = true;
            else if (arg == L"--presentation-probe") presentation_probe = true;
            else if (arg == L"--display-power-check") display_power_check = true;
            else if (arg == L"--list-adapters") list_adapters = true;
            else if (arg == L"--resize-warning-trace") options.resize_warning_trace = true;
            else if (arg == L"--adapter-luid" && i+1<argc) {
                if(options.hardware_adapter_luid)throw std::invalid_argument("Duplicate adapter LUID selection");
                options.hardware_adapter_luid=ParseAdapterLuid(argv[++i]);
            }
            else if (arg == L"--presentation-probe-latency" && i+1<argc) {
                const std::wstring value=argv[++i];
                if(value!=L"1" && value!=L"2")throw std::invalid_argument("Probe latency must be1 or2");
                options.waitable_vsync_latency=value==L"1"?1:2;probe_latency_set=true;
            }
            else if (arg == L"--terrain") terrain = true;
            else if (arg == L"--world") world = true;
            else if (arg == L"--palette-test") palette = true;
            else if (arg == L"--sprite-replay") sprite_replay = true;
            else if (arg == L"--terrain-replay") terrain_replay = true;
            else if (arg == L"--outline-replay") outline_replay = true;
            else if (arg == L"--enhanced-sprite-test") enhanced_test = true;
            else if (arg == L"--enhanced-sprite-replay") enhanced_replay = true;
            else if (arg == L"--enhanced-world-test") enhanced_world_test = true;
            else if (arg == L"--enhanced-world") enhanced_world = true;
            else if (arg == L"--enhanced-world-native-origin") enhanced_world_native_origin = true;
            else if (arg == L"--enhanced-world-fade-texel-load") enhanced_world_fade_texel_load = true;
            else if (arg == L"--lens-test") lens_test = true;
            else if (arg == L"--world-overlay-test") world_overlay_test = true;
            else if (arg == L"--composition-test") composition_test = true;
            else if (arg == L"--frozen-frame-test") frozen_test = true;
            else if (arg == L"--frozen-world") frozen_world = true;
            else if (arg == L"--full-walls") full_walls = true;
            else if (arg == L"--gpu-validation") options.gpu_validation = true;
            else if (arg == L"--no-validation") options.debug = false;
            else if (arg == L"--warp") options.warp = true;
            else if (arg == L"--no-vsync") options.vsync = false;
            else throw std::invalid_argument("Unknown argument");
        }
        if (output.empty() || !std::filesystem::is_directory(output)) throw std::invalid_argument("An existing --output directory is required");
        if(options.warp && options.hardware_adapter_luid)throw std::invalid_argument("WARP and hardware adapter LUID are mutually exclusive");
        if(options.resize_warning_trace && !options.debug)throw std::invalid_argument("Resize warning trace requires validation");
        if(list_adapters) {
            for(int i=1;i<argc;++i) {
                const std::wstring arg=argv[i];
                if(arg==L"--output"){++i;continue;}
                if(arg!=L"--list-adapters")throw std::invalid_argument("Adapter enumeration is an exclusive mode");
            }
            return ListHardwareAdapters(output);
        }
        if(display_power_check) {
            for(int i=1;i<argc;++i) {
                const std::wstring arg=argv[i];
                if(arg==L"--output"){++i;continue;}
                if(arg!=L"--display-power-check")throw std::invalid_argument("Display-power check is an exclusive mode");
            }
            return DisplayPowerCheck(output);
        }
        if (((terrain || world) && scene_path.empty()) || (full_walls && !terrain && !world) || (terrain && world))
            throw std::invalid_argument("Select terrain or world with a capture; full walls requires either mode");
        if (palette && (scene_path.empty() || terrain || world || full_walls || sprite_replay))
            throw std::invalid_argument("Palette tests require a capture and no world/terrain mode");
        if (sprite_replay && (scene_path.empty() || terrain || world || full_walls))
            throw std::invalid_argument("Sprite replay requires a capture and no world/terrain mode");
        if (terrain_replay && (scene_path.empty() || terrain || world || full_walls || palette || sprite_replay))
            throw std::invalid_argument("Terrain replay requires a capture and no other rendering mode");
        if (outline_replay && (scene_path.empty() || terrain || world || full_walls || palette || sprite_replay || terrain_replay))
            throw std::invalid_argument("Outline replay requires a capture and no other rendering mode");
        if (frozen_world && (scene_path.empty() || terrain || world || full_walls || palette || sprite_replay || terrain_replay || outline_replay || frozen_test))
            throw std::invalid_argument("Frozen world requires a v8 capture and no other rendering mode");
        if (frozen_test && (!scene_path.empty() || interactive || terrain || world || full_walls || palette || sprite_replay || terrain_replay || outline_replay || frozen_world))
            throw std::invalid_argument("Frozen frame fixtures cannot be combined with other rendering modes");
        if (enhanced_test && (!scene_path.empty() || interactive || terrain || world || full_walls || palette || sprite_replay || terrain_replay || outline_replay || frozen_world || frozen_test || enhanced_replay))
            throw std::invalid_argument("Enhanced sprite fixtures cannot be combined with other rendering modes");
        if (enhanced_replay && (scene_path.empty() || terrain || world || full_walls || palette || sprite_replay || terrain_replay || outline_replay || frozen_world || frozen_test || enhanced_test))
            throw std::invalid_argument("Enhanced sprite replay requires a v9 capture and no other rendering mode");
        if (enhanced_world_test && (!scene_path.empty() || interactive || terrain || world || full_walls || palette || sprite_replay || terrain_replay || outline_replay || frozen_world || frozen_test || enhanced_test || enhanced_replay || enhanced_world))
            throw std::invalid_argument("Enhanced world fixtures cannot be combined with other rendering modes");
        if (enhanced_world && (scene_path.empty() || terrain || world || full_walls || palette || sprite_replay || terrain_replay || outline_replay || frozen_world || frozen_test || enhanced_test || enhanced_replay || enhanced_world_test))
            throw std::invalid_argument("Enhanced world requires a v10 or v11 capture and no other rendering mode");
        if (enhanced_world_native_origin && !enhanced_world)
            throw std::invalid_argument("Native world raster origin requires enhanced world replay");
        if (enhanced_world_fade_texel_load && !enhanced_world && !enhanced_world_test)
            throw std::invalid_argument("Explicit fade addressing requires enhanced world replay or operators");
        if (lens_test && (!scene_path.empty() || interactive || terrain || world || full_walls || palette || sprite_replay || terrain_replay || outline_replay || frozen_world || frozen_test || enhanced_test || enhanced_replay || enhanced_world_test || enhanced_world))
            throw std::invalid_argument("Lens fixtures cannot be combined with other rendering modes");
        if (world_overlay_test && (!scene_path.empty() || interactive || terrain || world || full_walls || palette || sprite_replay || terrain_replay || outline_replay || frozen_world || frozen_test || enhanced_test || enhanced_replay || enhanced_world_test || enhanced_world || lens_test))
            throw std::invalid_argument("World overlay fixtures cannot be combined with other rendering modes");
        if (composition_test && (!scene_path.empty() || interactive || terrain || world || full_walls || palette || sprite_replay || terrain_replay || outline_replay || frozen_world || frozen_test || enhanced_test || enhanced_replay || enhanced_world_test || enhanced_world || lens_test || world_overlay_test))
            throw std::invalid_argument("Composition fixtures cannot be combined with other rendering modes");
        const bool indexed_fallback = !indexed_fallback_path.empty() || indexed_fallback_test;
        if (!runtime_resource_input.empty() && !runtime_resource_test)
            throw std::invalid_argument("Runtime resource input requires runtime resource test mode");
        if (runtime_process_budget_test && (!scene_path.empty() || !runtime_resource_input.empty()
            || interactive || terrain || world || full_walls || palette || sprite_replay || terrain_replay
            || outline_replay || frozen_test || frozen_world || enhanced_test || enhanced_replay
            || enhanced_world_test || enhanced_world || enhanced_world_native_origin || enhanced_world_fade_texel_load
            || lens_test || world_overlay_test || composition_test || indexed_fallback
            || runtime_resource_test || runtime_world_test))
            throw std::invalid_argument("Process-budget fixtures require their separate synthetic test mode");
        if (runtime_world_test && (interactive || terrain || world || full_walls || palette || sprite_replay
            || terrain_replay || outline_replay || frozen_test || frozen_world || enhanced_test || enhanced_replay
            || enhanced_world_test || enhanced_world || enhanced_world_native_origin || enhanced_world_fade_texel_load
            || lens_test || world_overlay_test || composition_test || indexed_fallback || runtime_resource_test || !runtime_resource_input.empty()))
            throw std::invalid_argument("Runtime world tests cannot be combined with other rendering modes");
        if (runtime_resource_test && (!scene_path.empty() || interactive || terrain || world || full_walls || palette
            || sprite_replay || terrain_replay || outline_replay || frozen_test || frozen_world || enhanced_test
            || enhanced_replay || enhanced_world_test || enhanced_world || enhanced_world_native_origin
            || enhanced_world_fade_texel_load || lens_test || world_overlay_test || composition_test || indexed_fallback))
            throw std::invalid_argument("Runtime resource tests cannot be combined with other rendering modes");
        if (indexed_fallback && (!scene_path.empty() || terrain || world || full_walls || palette || sprite_replay
            || terrain_replay || outline_replay || frozen_world || frozen_test || enhanced_test || enhanced_replay
            || enhanced_world_test || enhanced_world || enhanced_world_native_origin || lens_test || world_overlay_test || composition_test
            || (indexed_fallback_test && (!indexed_fallback_path.empty() || interactive))))
            throw std::invalid_argument("Indexed fallback requires its own file or synthetic fixture and no other rendering mode");
        if(probe_latency_set && !presentation_probe)throw std::invalid_argument("Probe latency requires presentation probe mode");
        if(presentation_probe) {
            if(!scene_path.empty() || !runtime_resource_input.empty() || indexed_fallback || interactive || terrain || world || full_walls
                || palette || sprite_replay || terrain_replay || outline_replay || frozen_test || frozen_world || enhanced_test
                || enhanced_replay || enhanced_world_test || enhanced_world || enhanced_world_native_origin || enhanced_world_fade_texel_load
                || lens_test || world_overlay_test || composition_test || runtime_resource_test || runtime_world_test || runtime_process_budget_test
                || options.warp || !options.vsync)
                throw std::invalid_argument("Presentation probe requires its own hardware VSync mode");
            options.waitable_presentation=true;options.presentation_diagnostics=true;
        }
        options.cull_back_faces = terrain || world;
        wchar_t exe[32768] = {};
        if (!GetModuleFileNameW(nullptr, exe, 32768)) throw std::runtime_error("Cannot resolve executable path");
        options.shader_directory = (std::filesystem::path(exe).parent_path() / "shaders").wstring();
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        WNDCLASSW wc = {};
        wc.lpfnWndProc = WindowProc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"KeeperFX.DX12.Foundation";
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        if (!RegisterClassW(&wc)) throw std::runtime_error("Window registration failed");
        WindowState state;
        int window_x=0,window_y=0;UINT initial_width=1280,initial_height=720;
        if(presentation_probe) {
            MONITORINFO monitor{};monitor.cbSize=sizeof(monitor);
            if(!GetMonitorInfoW(MonitorFromPoint(POINT{0,0},MONITOR_DEFAULTTOPRIMARY),&monitor)
                || monitor.rcMonitor.right-monitor.rcMonitor.left!=3840 || monitor.rcMonitor.bottom-monitor.rcMonitor.top!=2160)
                throw std::runtime_error("Presentation probe requires a native3840x2160 primary desktop");
            window_x=monitor.rcMonitor.left;window_y=monitor.rcMonitor.top;initial_width=3840;initial_height=2160;
        }
        window = CreateWindowExW(0, wc.lpszClassName, L"KeeperFX DX12 foundation — procedural test scene — Escape to close",
            WS_POPUP | WS_THICKFRAME, window_x, window_y, int(initial_width), int(initial_height), nullptr, nullptr, wc.hInstance, &state);
        if (!window) throw std::runtime_error("Window creation failed");
        // Borderless gives exact client dimensions, including native 3840x2160.
        SetWindowLongPtrW(window, GWL_STYLE, WS_POPUP);
        SetWindowPos(window, nullptr, window_x, window_y, int(initial_width), int(initial_height), SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        device = std::make_unique<Device>();
        device->Initialize(window, initial_width, initial_height, options);
        // Device-wide timestamp readbacks survive scene/runtime cache cleanup.
        const auto timing_readbacks = device->GetRuntimeTimingDiagnostics().readback_resources;

        ShowWindow(window, (interactive || presentation_probe) ? SW_SHOW : SW_SHOWNOACTIVATE);
        Pump(*device, state);
        if(presentation_probe) {
            SetWindowTextW(window,L"KeeperFX native DX12 presentation probe - 2s warmup + 10s measurement");
            PresentationProbe(*device,window,state,output,options.waitable_vsync_latency);
            device.reset();DestroyWindow(window);return 0;
        }
        UINT checks = 0;
        Require(timing_readbacks == 3, "Unexpected fixed timestamp readback count", checks);
        std::vector<Vertex> vertices;
        std::vector<uint32_t> indices;
        Image capture;
        if (runtime_process_budget_test) {
            SetWindowTextW(window, L"KeeperFX DX12 process-budget diagnostics");
            const auto tested = RunRuntimeProcessBudgetTests(*device, options, output);
            Require(tested.cases > 0 && tested.submitted_frames > 0 && tested.rgba_byte_checks > 0,
                "Process-budget tests did not submit and compare real runtime work", checks);
            Require(tested.checks < uint64_t(UINT_MAX - checks), "Process-budget check count exceeds report range", checks);
            checks += UINT(tested.checks);
        } else if (runtime_world_test) {
            SetWindowTextW(window, L"KeeperFX DX12 runtime world diagnostics");
            const auto tested = RunRuntimeWorldTests(*device, options, scene_path, output);
            Require(tested.cases > 0 && tested.submitted_frames > 0 && tested.rgba_byte_checks > 0,
                "Runtime world tests did not submit and compare real work", checks);
            Require(tested.checks < uint64_t(UINT_MAX - checks), "World runtime check count exceeds report range", checks);
            checks += UINT(tested.checks);
            if (!tested.preview.rgba.empty()) SaveBitmap(output / "runtime-world-preview.bmp", tested.preview);
            const auto reclamation = RunRuntimeReclamationTests(*device, output);
            Require(reclamation.cases > 0 && reclamation.submitted_frames > 0 && reclamation.rgba_byte_checks > 0,
                "Runtime reclamation tests did not submit and compare real work", checks);
            Require(reclamation.checks < uint64_t(UINT_MAX - checks), "Reclamation check count exceeds report range", checks);
            checks += UINT(reclamation.checks);
            const auto staging = RunRuntimeStagingTests(*device, options, output);
            Require(staging.cases > 0 && staging.submitted_frames > 0 && staging.rgba_byte_checks > 0,
                "Runtime staging tests did not submit and compare real work", checks);
            Require(staging.checks < uint64_t(UINT_MAX - checks), "Staging check count exceeds report range", checks);
            checks += UINT(staging.checks);
            const auto timing = RunRuntimeTimingTests(*device, options, output);
            Require(timing.cases > 0 && timing.submitted_frames > 0 && timing.rgba_byte_checks > 0,
                "Runtime timing tests did not submit and compare real work", checks);
            Require(timing.checks < uint64_t(UINT_MAX - checks), "Timing check count exceeds report range", checks);
            checks += UINT(timing.checks);
            const auto lightmap = RunRuntimeLightmapTests(*device, output);
            Require(lightmap.cases > 0 && lightmap.submitted_frames > 0 && lightmap.rgba_byte_checks > 0,
                "Runtime lightmap tests did not submit and compare real work", checks);
            Require(lightmap.checks < uint64_t(UINT_MAX - checks), "Lightmap check count exceeds report range", checks);
            checks += UINT(lightmap.checks);
        } else if (runtime_resource_test) {
            SetWindowTextW(window, L"KeeperFX DX12 runtime resource diagnostics");
            auto tested = RunRuntimeResourceTests(*device, options, runtime_resource_input, output);
            Require(tested.cases > 0 && tested.submitted_frames > 0 && tested.rgba_byte_checks > 0,
                "Runtime resource tests must exercise real submissions and exact output", checks);
            Require(tested.checks < uint64_t(UINT_MAX - checks), "Runtime check count exceeds report range", checks);
            checks += UINT(tested.checks);
            if (!tested.preview.rgba.empty()) SaveBitmap(output / "runtime-resource-preview.bmp", tested.preview);
        } else if (indexed_fallback) {
            SetWindowTextW(window, L"KeeperFX DX12 owned indexed fallback - Escape to close");
            if (indexed_fallback_test) indexed_fallback_path = WriteIndexedFallbackFixture(output);
            const auto baseline = device->GetDiagnostics();
            auto frame = PrepareIndexedFallbackReplay(*device, indexed_fallback_path, output, indexed_fallback_test);
            Require(frame.Valid(), "Indexed fallback did not retain a GPU frame", checks);
            SetSize(window, *device, state, frame.Width(), frame.Height());
            const auto before = device->GetDiagnostics();
            UINT tick = 0;
            while (!state.closed && (interactive || tick < 6)) {
                Pump(*device, state);
                if (!device->PresentFrozenFrame(frame)) Sleep(16);
                ++tick;
            }
            const auto after = device->GetDiagnostics();
            Require(after.frozen_preparations == before.frozen_preparations
                && after.frozen_image_readbacks == before.frozen_image_readbacks,
                "Indexed fallback presentation prepared or read back an image", checks);
            Require(interactive || after.frozen_presentations == before.frozen_presentations + 6,
                "Indexed fallback presentation did not complete six frames", checks);
            Require(after.frozen_preparations == baseline.frozen_preparations + 1
                && after.frozen_image_readbacks == baseline.frozen_image_readbacks + 2,
                "Indexed fallback did not use one preparation and two explicit readbacks", checks);
            frame.Reset();
            device->WaitIdle();
            Require(device->GetDiagnostics().allocation_count == baseline.allocation_count,
                "Indexed fallback resources survived their presentation fence", checks);
        } else if (composition_test) {
            SetWindowTextW(window, L"KeeperFX DX12 resident composition diagnostics");
            comparison_review = !RunCompositionOperatorTests(*device, output);
            Require(true, "Resident composition integration fixtures completed", checks);
        } else if (world_overlay_test) {
            SetWindowTextW(window, L"KeeperFX DX12 world overlay operator diagnostics");
            comparison_review = !RunWorldOverlayOperatorTests(*device, output);
            Require(true, "World overlay operator fixtures completed", checks);
        } else if (lens_test) {
            SetWindowTextW(window, L"KeeperFX DX12 standard lens operator diagnostics");
            comparison_review = !RunLensOperatorTests(*device, output);
            Require(true, "Lens operator fixtures completed", checks);
        } else if (enhanced_world_test) {
            SetWindowTextW(window, L"KeeperFX DX12 enhanced world operator diagnostics");
            comparison_review = !RunEnhancedWorldOperatorTests(*device, output,
                enhanced_world_fade_texel_load ? EnhancedFadeAddressing::TexelLoad : EnhancedFadeAddressing::Sampler);
            Require(true, "Enhanced world operator fixtures completed", checks);
        } else if (enhanced_world) {
            SetWindowTextW(window, L"KeeperFX DX12 captured world - Escape to close");
            auto tested = RunEnhancedWorldReplayTests(*device, scene_path, output,
                enhanced_world_native_origin ? EnhancedWorldRasterOrigin::Native : EnhancedWorldRasterOrigin::ViewportLocal,
                enhanced_world_fade_texel_load ? EnhancedFadeAddressing::TexelLoad : EnhancedFadeAddressing::Sampler);
            Require(tested.compared_pixels > 0, "No enhanced world replay pixels compared", checks);
            auto frame = std::move(tested.frame);
            SetSize(window, *device, state, frame.Width(), frame.Height());
            const auto before = device->GetDiagnostics();
            UINT tick = 0;
            while (!state.closed && (interactive || tick < 6)) {
                Pump(*device, state);
                if (!device->PresentFrozenFrame(frame)) Sleep(16);
                ++tick;
            }
            const auto after = device->GetDiagnostics();
            Require(after.frozen_preparations == before.frozen_preparations
                && after.frozen_image_readbacks == before.frozen_image_readbacks,
                "Enhanced world presentation prepared or read back an image", checks);
            Require(interactive || after.frozen_presentations == before.frozen_presentations + 6,
                "Enhanced world presentation did not complete six frames", checks);
            comparison_review = true;
            frame.Reset();
        } else if (enhanced_test || enhanced_replay) {
            SetWindowTextW(window, L"KeeperFX DX12 normal sprite and glow diagnostics - Escape to close");
            comparison_review = !RunEnhancedSpriteOperatorTests(*device, output);
            Require(true, "Enhanced sprite operator fixtures completed", checks);
            if (enhanced_replay) {
                const auto tested = RunEnhancedSpriteReplayTests(*device, scene_path, output);
                Require(tested.compared_pixels > 0, "No enhanced sprite replay pixels compared", checks);
                comparison_review = true; // Full terrain colour and visual acceptance remain pending.
                Quad(vertices, indices, {{{-1,1,.3f,0,0},{1,1,.3f,1,0},{1,-1,.3f,1,1},{-1,-1,.3f,0,1}}});
                device->Upload(vertices, indices, tested.preview);
                SetSize(window, *device, state, tested.preview.width, tested.preview.height);
                device->Draw(identity, &capture);
                Require(capture.rgba == tested.preview.rgba, "Enhanced sprite diagnostic display conversion differs", checks);
                SaveBitmap(output / "enhanced-sprite-replay.bmp", capture);
                while (interactive && !state.closed) {
                    Pump(*device, state);
                    if (!device->Draw(identity)) Sleep(16);
                }
            }
        } else if (frozen_world) {
            SetWindowTextW(window, L"KeeperFX DX12 frozen world viewport - Escape to close");
            auto frame = PrepareFrozenWorld(*device, scene_path, output);
            SetSize(window, *device, state, frame.Width(), frame.Height());
            const auto before = device->GetDiagnostics();
            UINT tick = 0;
            while (!state.closed && (interactive || tick < 6)) {
                Pump(*device, state);
                if (!device->PresentFrozenFrame(frame)) Sleep(16);
                ++tick;
            }
            const auto after = device->GetDiagnostics();
            Require(after.frozen_preparations == before.frozen_preparations
                && after.frozen_image_readbacks == before.frozen_image_readbacks,
                "Frozen world presentation prepared or read back an image", checks);
            Require(interactive || after.frozen_presentations == before.frozen_presentations + 6,
                "Frozen world presentation did not complete six frames", checks);
            comparison_review = true; // Raster and omitted UI/lens work remain tracked.
            frame.Reset();
        } else if (frozen_test) {
            SetWindowTextW(window, L"KeeperFX DX12 GPU-resident frozen-frame tests");
            RunFrozenFrameTests(*device, options, output, [&](uint32_t width,uint32_t height) {
                SetSize(window,*device,state,width,height);
            });
            Require(true, "Frozen frame fixtures completed", checks);
        } else if (outline_replay) {
            SetWindowTextW(window, L"KeeperFX DX12 outline diagnostic - Escape to close");
            comparison_review = !RunOutlineOperatorTests(*device, output);
            const auto tested = RunOutlineReplayTests(*device, scene_path, output);
            Require(tested.compared_pixels > 0, "No outline replay pixels compared", checks);
            Quad(vertices, indices, {{{-1,1,.3f,0,0},{1,1,.3f,1,0},{1,-1,.3f,1,1},{-1,-1,.3f,0,1}}});
            device->Upload(vertices, indices, tested.preview);
            SetSize(window, *device, state, tested.preview.width, tested.preview.height);
            device->Draw(identity, &capture);
            Require(capture.rgba == tested.preview.rgba, "Outline replay display conversion differs", checks);
            SaveBitmap(output / "outline-replay.bmp", capture);
            while (interactive && !state.closed) {
                Pump(*device, state);
                if (!device->Draw(identity)) Sleep(16);
            }
        } else if (terrain_replay) {
            comparison_review = true;
            SetWindowTextW(window, L"KeeperFX DX12 ordered terrain replay - Escape to close");
            RunShadowOperatorTests(*device, output);
            RunShadowEdgeTests(*device, output);
            const auto tested = RunTerrainReplayTests(*device, scene_path, output);
            Require(tested.indexed_checks > 0, "No terrain replay pixels checked", checks);
            Quad(vertices, indices, {{{-1,1,.3f,0,0},{1,1,.3f,1,0},{1,-1,.3f,1,1},{-1,-1,.3f,0,1}}});
            device->Upload(vertices, indices, tested.preview);
            SetSize(window, *device, state, tested.preview.width, tested.preview.height);
            device->Draw(identity, &capture);
            Require(capture.rgba == tested.preview.rgba, "Terrain replay display conversion differs", checks);
            SaveBitmap(output / "terrain-replay.bmp", capture);
            while (interactive && !state.closed) {
                Pump(*device, state);
                if (!device->Draw(identity)) Sleep(16);
            }
        } else if (sprite_replay) {
            SetWindowTextW(window, L"KeeperFX DX12 captured sprite replay - Escape to close");
            const auto tested = RunSpriteReplayTests(*device, scene_path, output);
            Require(tested.indexed_checks > 0, "No replay pixels checked", checks);
            Quad(vertices, indices, {{{-1,1,.3f,0,0},{1,1,.3f,1,0},{1,-1,.3f,1,1},{-1,-1,.3f,0,1}}});
            device->Upload(vertices, indices, tested.preview);
            SetSize(window, *device, state, tested.preview.width, tested.preview.height);
            device->Draw(identity, &capture);
            Require(capture.rgba == tested.preview.rgba, "Sprite replay display conversion differs", checks);
            SaveBitmap(output / "sprite-replay.bmp", capture);
            while (interactive && !state.closed) {
                Pump(*device, state);
                if (!device->Draw(identity)) Sleep(16);
            }
        } else if (palette) {
            SetWindowTextW(window, L"KeeperFX DX12 palette composition - controlled fixture - Escape to close");
            const auto tested = RunPaletteTests(*device, scene_path, output);
            Require(tested.indexed_checks > 0, "No indexed palette pixels checked", checks);
            Quad(vertices, indices, {{{-1,1,.3f,0,0},{1,1,.3f,1,0},{1,-1,.3f,1,1},{-1,-1,.3f,0,1}}});
            device->Upload(vertices, indices, tested.preview);
            SetSize(window, *device, state, tested.preview.width, tested.preview.height);
            device->Draw(identity, &capture);
            Require(capture.rgba == tested.preview.rgba, "Palette preview display conversion differs", checks);
            SaveBitmap(output / "palette-overlap.bmp", capture);
            while (interactive && !state.closed) {
                Pump(*device, state);
                if (!device->Draw(identity)) Sleep(16);
            }
        } else if (world) {
            WorldPreview(*device,window,state,scene_path,output,interactive,full_walls,checks);
        } else if (terrain) {
            TerrainPreview(*device,window,state,scene_path,output,interactive,full_walls,checks);
        } else if (!scene_path.empty()) {
            SetWindowTextW(window, L"KeeperFX captured assets - textures and sprites - Escape to close");
            auto owned = kfx::render::LoadSceneCapture(scene_path);
            Image texture;
            texture.rgba = kfx::render::SceneAssetContactSheet(*owned, texture.width, texture.height);
            std::ofstream summary(output / "capture.json");
            summary << "{\"epoch\":" << owned->scene.epoch << ",\"sequence\":" << owned->scene.sequence
                << ",\"view\":" << unsigned(owned->scene.camera.view) << ",\"sprites\":" << owned->assets.sprites.size()
                << ",\"pixelChecks\":" << uint64_t(texture.width) * texture.height << "}\n";
            owned.reset(); // GPU data remains owned even after the CPU bundle retires.
            Quad(vertices, indices, {{{-1,1,.3f,0,0}, {1,1,.3f,1,0}, {1,-1,.3f,1,1}, {-1,-1,.3f,0,1}}});
            device->Upload(vertices, indices, texture);
            SetSize(window, *device, state, texture.width, texture.height);
            device->Draw(identity, &capture);
            Require(capture.width == texture.width && capture.height == texture.height, "Asset readback size mismatch", checks);
            Require(capture.rgba == texture.rgba, "Captured asset GPU pixels differ from CPU palette decoding", checks);
            SaveBitmap(output / "captured-assets.bmp", capture);
            UINT tick = 0;
            while (!state.closed && (interactive || tick < 6)) {
                Pump(*device, state);
                if (!device->Draw(identity)) Sleep(16);
                ++tick;
            }
        } else {
        if (!interactive) {
            CalibrationMesh(vertices, indices);
            auto texture = CalibrationTexture();
            device->Upload(vertices, indices, texture);
            auto bad_indices = indices; bad_indices[1] = UINT_MAX;
            bool rejected = false;
            try { device->Upload(vertices, bad_indices, texture); } catch (const std::invalid_argument&) { rejected = true; }
            Require(rejected, "Invalid index upload was accepted", checks);
            auto bad_texture = texture; bad_texture.rgba.pop_back(); rejected = false;
            try { device->Upload(vertices, indices, bad_texture); } catch (const std::invalid_argument&) { rejected = true; }
            Require(rejected, "Invalid texture upload was accepted", checks);
            for (auto size : {std::array<UINT,2>{3840,2160}, {1280,720}, {1001,733}, {3840,2160}}) {
                SetSize(window, *device, state, size[0], size[1]);
                for (UINT i = 0; i < 60; ++i) {
                    Pump(*device, state);
                    Require(!state.closed, "Test window closed before completion", checks);
                    device->Draw(identity);
                }
                device->Draw(identity, &capture);
                CheckCalibration(capture, size[0], size[1], checks);
            }
            SaveBitmap(output / "calibration-4k.bmp", capture);
            auto allocation_count = device->GetDiagnostics().allocation_count;
            Require(allocation_count == 4 + timing_readbacks, "Unexpected persistent GPU resource count", checks);
            // Replace resources with frames in flight and verify new pixels/SRV.
            for (uint8_t i = 1; i <= 8; ++i) {
                device->Draw(identity);
                device->Upload(vertices, indices, CalibrationTexture(i));
                device->Draw(identity, &capture);
                CheckCalibration(capture, 3840, 2160, checks, i);
                Require(device->GetDiagnostics().allocation_count == allocation_count, "GPU allocation leaked during replacement", checks);
            }
            ShowWindow(window, SW_MINIMIZE); Pump(*device, state);
            Require(IsIconic(window) != FALSE, "Window did not minimize", checks);
            auto frames = device->GetDiagnostics().frames;
            Require(!device->Draw(identity), "Minimized window submitted a frame", checks);
            Require(device->GetDiagnostics().frames == frames, "Minimized frame count advanced", checks);
            ShowWindow(window, SW_RESTORE); Pump(*device, state);
            SetSize(window, *device, state, 3840, 2160);
            Require(!IsIconic(window), "Window did not restore", checks);
            device->Upload(vertices, indices, CalibrationTexture());
            device->Draw(identity, &capture);
            CheckCalibration(capture, 3840, 2160, checks);
            device->Draw(identity);
            device->ClearScene();
            Require(device->GetDiagnostics().allocation_count == 1 + timing_readbacks, "Cleared scene retained GPU allocations", checks);
            rejected = false;
            try { device->Draw(identity); } catch (const std::logic_error&) { rejected = true; }
            Require(rejected, "Empty scene draw was accepted", checks);
            vertices.clear(); indices.clear();
        }
        DungeonMesh(vertices, indices);
        device->Upload(vertices, indices, StoneTexture());
        SetSize(window, *device, state, 3840, 2160);
        device->Draw(DungeonTransform(), &capture);
        SaveBitmap(output / "scene-4k.bmp", capture);
        UINT tick = 0;
        while (!state.closed && (interactive || tick < 60)) {
            Pump(*device, state);
            if (!device->Draw(DungeonTransform(interactive ? float(tick) * .001f : 0))) Sleep(16);
            ++tick;
        }
        }
        device->WaitIdle();
        auto diagnostics = device->GetDiagnostics();
        // Keep scene statistics for the report, then verify all scene allocations
        // can be reclaimed after the final draw. Depth and fixed timing readbacks remain owned.
        device->ClearScene();
        Require(device->GetDiagnostics().allocation_count == 1 + timing_readbacks, "Final scene cleanup leaked GPU allocations", checks);
        diagnostics.validation_errors = device->GetDiagnostics().validation_errors;
        diagnostics.validation_warnings = device->GetDiagnostics().validation_warnings;
        auto messages = device->TakeValidationMessages();
        std::ofstream validation(output / "validation.log");
        for (const auto& message : messages) validation << message << '\n';
        Require(diagnostics.validation_errors == 0, "D3D12 validation reported errors", checks);
        Require(diagnostics.validation_warnings == 0, "D3D12 validation reported warnings", checks);
        WriteReport(output, diagnostics, checks, interactive, !scene_path.empty(), terrain, world, palette, sprite_replay, terrain_replay, outline_replay, comparison_review, frozen_test, frozen_world, enhanced_test, enhanced_replay, enhanced_world_test, enhanced_world, lens_test, world_overlay_test, composition_test, indexed_fallback, runtime_resource_test, runtime_world_test, runtime_process_budget_test);
        std::cout << "Passed " << checks << " checks; " << diagnostics.frames << " frames; " << diagnostics.adapter << '\n';
        device.reset(); DestroyWindow(window);
        return 0;
    } catch (const std::exception& error) {
        std::string details = error.what();
        if (device) {
            details += "\n" + device->DeviceRemovedDetails();
            try { for (auto& message : device->TakeValidationMessages()) details += "\n" + message; } catch (...) {}
        }
        std::cerr << details << '\n';
        if (!output.empty()) {
            std::ofstream report(output / "report.json");
            report << "{\"status\":\"failed\",\"error\":" << Json(details) << "}\n";
        }
        device.reset(); if (window) DestroyWindow(window);
        return 1;
    }
}
