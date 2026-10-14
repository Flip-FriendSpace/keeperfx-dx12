// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#ifndef KFX_RENDER_BENCHMARK_H
#define KFX_RENDER_BENCHMARK_H

#ifdef __cplusplus
extern "C" {
#endif
// Opt-in local diagnostics. No files or GL queries without the environment flag.
int RenderBenchmarkEnabled(void);
int RenderBenchmarkRequest(int scene);
int RenderBenchmarkTakeRequest(int live);
void RenderBenchmarkComplete(void);
const char* RenderBenchmarkOutputDirectory(void);
#ifdef __cplusplus
}
#endif
#endif
