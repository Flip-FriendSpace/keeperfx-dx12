#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Checks the save field tables and builds the save tests as Windows programs.
# The save tools need LF line endings, so they read a copy of the checkout.
# usage: test-save.sh tables | build <output-directory>
set -euo pipefail
repo="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
mode="${1:?Usage: test-save.sh tables | build <output-directory>}"
deps="${KFX_TEST_DEPS_ROOT:-/opt/keeperfx-build/x64/RelWithDebInfo/deps}"
work="/opt/keeperfx-build/test-runner/$(basename -- "$repo")/save"
copy="$work/source"
mkdir -p "$copy"
for folder in src tools tests deps/centitoml config/fxdata/lua; do
  mkdir -p "$copy/$(dirname -- "$folder")"
  rsync -a --delete --exclude png2ico "$repo/$folder/" "$copy/$folder/"
done
find "$copy/tools" "$copy/tests" -type f \( -name '*.sh' -o -name '*.py' -o -name '*.txt' \) -exec sed -i 's/\r$//' {} +
includes() {
  local arch="$1" list=""
  for package in sdl3/SDL3 sdl3_mixer/SDL3_mixer sdl3_image/SDL3_image; do
    for path in "$deps/$package"-*/"$arch-w64-mingw32/include"; do list="$list -I$path"; done
  done
  echo "$list"
}
cd "$copy"
case "$mode" in
  tables)
    SDL_INCLUDES="$(includes i686)" bash tools/save-tables/check_tables.sh
    ;;
  build)
    output="${2:?Usage: test-save.sh build <output-directory>}"
    mkdir -p "$work/zlib" "$output"
    cp "$deps/zlib-build/libzs.a" "$work/zlib/libz.a"
    zlib="$(dirname -- "$(find "$deps" -name zlib.h | head -1)")"
    SDL_INCLUDES="$(includes x86_64)" CC=x86_64-w64-mingw32-gcc CFLAGS_EXTRA="-I$zlib" \
      LDFLAGS_EXTRA="-static -L$work/zlib" bash tests/save/build.sh "$work/kfx_save_tests.exe"
    cp "$work"/*.exe "$output/"
    ;;
  *) echo "Unknown mode: $mode" >&2; exit 2 ;;
esac
