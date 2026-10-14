// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
// Writes the last completed owned fallback frame in an opted-in paused local
// diagnostic session. It does not run a callback or request another frame.
int WorldSceneWriteIndexedFallback(void);
#ifdef __cplusplus
}
#endif
