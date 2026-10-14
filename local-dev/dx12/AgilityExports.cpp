// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#include <windows.h>

// The Agility runtime is loaded relative to the executable, never the cwd.
extern "C" {
__declspec(dllexport) extern const UINT D3D12SDKVersion = 619;
__declspec(dllexport) const char* D3D12SDKPath = ".\\D3D12\\";
}
