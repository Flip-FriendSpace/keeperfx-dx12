// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
// Read-only probe of the installed Windows runtime/driver, without Agility overrides.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dxgi1_6.h>
#include <d3d12.h>
#include <cstdio>
#include <string>

static std::string utf8(const wchar_t* text)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, &s[0], n, nullptr, nullptr);
    s.resize(n - 1);
    std::string escaped;
    for (char c : s) { if (c == '"' || c == '\\') escaped += '\\'; escaped += c; }
    return escaped;
}
int main()
{
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return 1;
    std::printf("{\"probe_bits\":%u,\"runtime\":\"Windows inbox D3D12; no Agility override\",\"adapters\":[", unsigned(sizeof(void*)*8));
    bool first = true;
    for (UINT i = 0;; ++i) {
        IDXGIAdapter1* adapter = nullptr;
        if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 desc = {}; adapter->GetDesc1(&desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) { adapter->Release(); continue; }
        if (!first) std::printf(","); first = false;
        std::printf("{\"name\":\"%s\",\"vendor_id\":%u,\"device_id\":%u,\"dedicated_vram_bytes\":%llu",
            utf8(desc.Description).c_str(), desc.VendorId, desc.DeviceId, (unsigned long long)desc.DedicatedVideoMemory);
        LARGE_INTEGER version = {};
        if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &version)))
            std::printf(",\"driver_version\":\"%u.%u.%u.%u\"", HIWORD(version.HighPart), LOWORD(version.HighPart), HIWORD(version.LowPart), LOWORD(version.LowPart));
        ID3D12Device* device = nullptr;
        HRESULT hr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
        std::printf(",\"device_hresult\":\"0x%08lx\"", (unsigned long)hr);
        if (SUCCEEDED(hr)) {
            D3D_FEATURE_LEVEL levels[] = {(D3D_FEATURE_LEVEL)0xc200, D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
            D3D12_FEATURE_DATA_FEATURE_LEVELS fl = {5, levels, D3D_FEATURE_LEVEL_11_0};
            if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &fl, sizeof(fl))))
                std::printf(",\"feature_level\":\"%u_%u\"", fl.MaxSupportedFeatureLevel >> 12, (fl.MaxSupportedFeatureLevel >> 8) & 15);
            for (int model = 0x6a; model >= 0x60; --model) {
                D3D12_FEATURE_DATA_SHADER_MODEL sm = {(D3D_SHADER_MODEL)model};
                if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm)))) {
                    std::printf(",\"shader_model\":\"%u.%u\"", sm.HighestShaderModel >> 4, sm.HighestShaderModel & 15); break;
                }
            }
            D3D12_FEATURE_DATA_D3D12_OPTIONS opt = {};
            if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &opt, sizeof(opt))))
                std::printf(",\"resource_binding_tier\":%u", opt.ResourceBindingTier);
            D3D12_FEATURE_DATA_D3D12_OPTIONS5 opt5 = {};
            if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &opt5, sizeof(opt5))))
                std::printf(",\"raytracing_tier\":%u", opt5.RaytracingTier);
            // Exact 32-bit field layouts from Microsoft's DirectX-Headers.
            // Raw feature IDs allow probing newer runtimes with MinGW's older header.
            struct { UINT mesh; UINT feedback; } opt7 = {};
            hr = device->CheckFeatureSupport((D3D12_FEATURE)32, &opt7, sizeof(opt7));
            std::printf(",\"options7_hresult\":\"0x%08lx\"", (unsigned long)hr);
            if (SUCCEEDED(hr)) std::printf(",\"mesh_shader_tier\":%u,\"sampler_feedback_tier\":%u", opt7.mesh, opt7.feedback);
            struct { int statistics; BOOL enhanced; BOOL casting; } opt12 = {};
            hr = device->CheckFeatureSupport((D3D12_FEATURE)41, &opt12, sizeof(opt12));
            std::printf(",\"options12_hresult\":\"0x%08lx\"", (unsigned long)hr);
            if (SUCCEEDED(hr)) std::printf(",\"enhanced_barriers\":%s", opt12.enhanced ? "true" : "false");
            IDXGIAdapter3* memory = nullptr;
            if (SUCCEEDED(adapter->QueryInterface(IID_PPV_ARGS(&memory)))) {
                DXGI_QUERY_VIDEO_MEMORY_INFO mi = {};
                if (SUCCEEDED(memory->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &mi)))
                    std::printf(",\"local_budget_bytes\":%llu,\"probe_process_usage_bytes\":%llu", (unsigned long long)mi.Budget, (unsigned long long)mi.CurrentUsage);
                memory->Release();
            }
            device->Release();
        }
        std::printf("}"); adapter->Release();
    }
    std::printf("]}\n"); factory->Release();
    return first ? 2 : 0;
}
