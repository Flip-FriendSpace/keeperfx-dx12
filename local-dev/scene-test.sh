#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
set -euo pipefail
repo="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
out="$(dirname -- "$repo")/artifacts/scene"
mkdir -p "$out"
for arch in i686 x86_64; do
  sdl=(/opt/keeperfx-build/RelWithDebInfo/deps/sdl3/SDL3-*/"$arch-w64-mingw32/include")
  mixer=(/opt/keeperfx-build/RelWithDebInfo/deps/sdl3_mixer/SDL3_mixer-*/"$arch-w64-mingw32/include")
  "$arch-w64-mingw32-g++-win32" -std=c++20 -O2 -Wall -Wextra -Werror -static -I"$repo/src" -I"${sdl[0]}" -I"${mixer[0]}" \
    "$repo/local-dev/scene-test.cpp" "$repo/src/kfx/renderer/WorldScene.cpp" \
    "$repo/src/kfx/renderer/WorldSceneCapture.cpp" -o "$out/scene-$arch.exe"
done
