#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
set -euo pipefail
repo="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
out="$(dirname -- "$repo")/artifacts/state-codec"
mkdir -p "$out"
/usr/bin/python3 "$repo/local-dev/state-schema.py" --check
for arch in i686 x86_64; do
  sdl=(/opt/keeperfx-build/RelWithDebInfo/deps/sdl3/SDL3-*/"$arch-w64-mingw32/include")
  mixer=(/opt/keeperfx-build/RelWithDebInfo/deps/sdl3_mixer/SDL3_mixer-*/"$arch-w64-mingw32/include")
  "$arch-w64-mingw32-g++" -std=c++20 -O2 -static -I"$repo/src" -I"$repo" -I"${sdl[0]}" -I"${mixer[0]}" \
    "$repo/local-dev/state-codec-test.cpp" "$repo/src/state_codec.cpp" -o "$out/state-codec-$arch.exe"
done
