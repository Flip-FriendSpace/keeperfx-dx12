#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
set -euo pipefail
repo="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
out="${1:?Usage: render-thread-test.sh /path/to/new-output-directory [x64|x86|both]}"
architecture="${2:-x64}"
case "$architecture" in
  x64) architectures=(x86_64) ;;
  x86) architectures=(i686) ;;
  both) architectures=(i686 x86_64) ;;
  *) printf 'Unknown architecture: %s\n' "$architecture" >&2; exit 1 ;;
esac
mkdir -p "$(dirname -- "$out")"
mkdir "$out"
for arch in "${architectures[@]}"; do
  "$arch-w64-mingw32-g++-win32" -std=c++20 -O2 -Wall -Wextra -Werror -static \
    -I"$repo/src" "$repo/local-dev/render-thread-test.cpp" \
    "$repo/src/kfx/renderer/RenderThreadManager.cpp" \
    "$repo/src/kfx/renderer/RendererThread.cpp" \
    -o "$out/render-thread-test-$arch.exe" -luser32
done
