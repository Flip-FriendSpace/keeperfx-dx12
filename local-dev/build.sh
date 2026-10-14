#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
set -euo pipefail

source_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
workspace_dir="$(dirname -- "$source_dir")"
configuration="${1:-RelWithDebInfo}"
target="${2:-keeperfx}"
jobs="${3:-6}"
architecture="${4:-x64}"
backend="${5:-default}"
case "$configuration" in RelWithDebInfo|Debug) ;; *) exit 2 ;; esac
case "$target" in keeperfx|keeperfx_hvlog) ;; *) exit 2 ;; esac
case "$backend" in default|dx12) ;; *) exit 2 ;; esac
if [[ "$backend" == dx12 && "$architecture" != x64 ]]; then
  printf 'The DX12 developer backend requires Windows x64.\n' >&2
  exit 2
fi
build_dir="/opt/keeperfx-build/$configuration"
runtime_dir="$workspace_dir/runtime/keeperfx"
cache_dir="$workspace_dir/downloads/keeperfx-deps"
toolchain=mingw32
triplet=i686-w64-mingw32
case "$architecture" in
  x86) ;;
  x64)
    build_dir="/opt/keeperfx-build/x64/$configuration"
    runtime_dir="$workspace_dir/runtime/keeperfx-x64"
    cache_dir+="-x64"
    toolchain=mingw64
    triplet=x86_64-w64-mingw32
    ;;
  *) exit 2 ;;
esac
dx12_options=(-DKFX_ENABLE_DX12=OFF)
if [[ "$backend" == dx12 ]]; then
  build_dir="/opt/keeperfx-build/x64-dx12/$configuration"
  bash "$source_dir/local-dev/dx12-prepare.sh"
  dx12_options=(-DKFX_ENABLE_DX12=ON -DDX12_DEPS=/opt/keeperfx-build/dx12/deps)
fi
test -f "$runtime_dir/fxdata/creature.cfg"
cd "$source_dir"

# Use Windows-verified downloads. Keep extraction on the Linux filesystem.
dependencies=(sdl3 sdl3_mixer sdl3_image enet6 zlib spng astronomy centijson ffmpeg luajit miniupnpc libnatpmp libcurl openal)
if [[ "$architecture" == x64 ]]; then dependencies+=(zlib-source); fi
for name in "${dependencies[@]}"; do
  if [[ ! -f "$build_dir/deps/$name/.local-complete" ]]; then
    archive="$cache_dir/$name.tar.gz"
    mkdir -p "$build_dir/deps/$name"
    tar -tzf "$archive" > /dev/null
    tar -xzf "$archive" -C "$build_dir/deps/$name"
    touch "$build_dir/deps/$name/.local-complete"
  fi
done
if [[ ! -f "$source_dir/tools/png2ico/Linux/png2ico" ]]; then
  mkdir -p "$source_dir/tools/png2ico/Linux"
  tar -xzf "$cache_dir/png2ico.tar.gz" -C "$source_dir/tools/png2ico/Linux"
fi

cmake -S "$source_dir" -B "$build_dir" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$source_dir/build/cmake/toolchains/$toolchain.cmake" \
  -DCMAKE_BUILD_TYPE="$configuration" \
  -DBUILD_NUMBER=5491 -DPACKAGE_SUFFIX=Local "${dx12_options[@]}"
# Configure first: the schema parser includes generated src/ver_defs.h.
/usr/bin/python3 "$source_dir/local-dev/state-schema.py" --check --deps-dir "$build_dir/deps"
cmake --build "$build_dir" --target "$target" --parallel "$jobs"

if [[ "$target" == keeperfx ]]; then output=keeperfx-local; else output=keeperfx-local-hvlog; fi
if [[ "$backend" == dx12 ]]; then
  if [[ "$target" == keeperfx ]]; then output=keeperfx-dx12; else output=keeperfx-dx12-hvlog; fi
fi
binary_name="$target"
if [[ "$backend" == dx12 ]]; then binary_name="$output"; fi
cp "$build_dir/$binary_name.exe" "$runtime_dir/$output.exe"
debug_output="$target"
if [[ "$backend" == dx12 ]]; then debug_output="$output"; fi
cp "$build_dir/$binary_name.exe.debug" "$runtime_dir/$debug_output.exe.debug"
cp "$build_dir/$target.map" "$runtime_dir/$debug_output.map"
find "$build_dir/deps" -path "*/$triplet/bin/SDL3*.dll" -exec cp {} "$runtime_dir/" \;
mkdir -p "$runtime_dir/licenses"
cp "$source_dir/LICENSE" "$runtime_dir/licenses/KeeperFX.txt"
for name in sdl3 sdl3_mixer sdl3_image; do
  for notice_dir in "$build_dir/deps/$name/"*/"$triplet/share/licenses"; do
    cp -R "$notice_dir/." "$runtime_dir/licenses/"
  done
done
if [[ "$backend" == dx12 ]]; then
  mkdir -p "$runtime_dir/D3D12" "$runtime_dir/shaders" "$runtime_dir/licenses"
  cp "$build_dir/D3D12/"*.dll "$runtime_dir/D3D12/"
  cp "$build_dir/shaders/"*.dxil "$runtime_dir/shaders/"
  cp "$workspace_dir/artifacts/dx12/bin/licenses/"*.txt "$runtime_dir/licenses/"
fi
printf 'Built and deployed %s\nSource revision: %s\n' "$runtime_dir/$output.exe" "$(git -C "$source_dir" rev-parse HEAD)"
