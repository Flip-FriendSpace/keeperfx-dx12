#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
set -euo pipefail
repo="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
out="$(dirname -- "$repo")/artifacts/foundation"
mkdir -p "$out"
for arch in i686 x86_64; do
  sdl=(/opt/keeperfx-build/RelWithDebInfo/deps/sdl3/SDL3-*/"$arch-w64-mingw32/include")
  mixer=(/opt/keeperfx-build/RelWithDebInfo/deps/sdl3_mixer/SDL3_mixer-*/"$arch-w64-mingw32/include")
  "$arch-w64-mingw32-g++" -std=c++17 -S -I"$repo/src" -I"$repo" -I"${sdl[0]}" -I"${mixer[0]}" \
    "$repo/local-dev/abi-probe.cpp" -o "$out/abi-$arch.s"
done
x86_64-w64-mingw32-g++ -std=c++17 -static "$repo/local-dev/d3d12-probe.cpp" \
  -ld3d12 -ldxgi -ldxguid -o "$out/d3d12-probe.exe"
