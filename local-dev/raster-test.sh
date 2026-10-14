#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
set -euo pipefail
repo="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
out="$(dirname -- "$repo")/artifacts/x64-port"
mkdir -p "$out"
for arch in i686 x86_64; do
  sdl=(/opt/keeperfx-build/RelWithDebInfo/deps/sdl3/SDL3-*/"$arch-w64-mingw32/include")
  "$arch-w64-mingw32-gcc" -std=c11 -O3 -static -I"$repo/src" -I"${sdl[0]}" \
    "$repo/local-dev/raster-test.c" "$repo/src/kfx/renderer/software/bflib_render_gpoly.c" \
    -o "$out/raster-$arch.exe"
done
