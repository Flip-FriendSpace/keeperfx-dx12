#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
set -euo pipefail
repo="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
out="$(dirname -- "$repo")/artifacts/terrain"
mkdir -p "$out"
for arch in i686 x86_64; do
  "$arch-w64-mingw32-g++-win32" -std=c++20 -O2 -Wall -Wextra -Werror -static -I"$repo/src" \
    "$repo/local-dev/terrain-test.cpp" "$repo/src/kfx/renderer/WorldScene.cpp" \
    "$repo/src/kfx/renderer/SceneAssets.cpp" "$repo/src/kfx/renderer/SceneCaptureCodec.cpp" \
    "$repo/src/kfx/renderer/TerrainMesh.cpp" -o "$out/terrain-$arch.exe"
done
