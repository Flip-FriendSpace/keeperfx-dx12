#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
set -euo pipefail
repo="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
workspace="$(dirname -- "$repo")"
cache="$workspace/downloads/keeperfx-dx12"
build=/opt/keeperfx-build/dx12
deps="$build/deps"
out="$workspace/artifacts/dx12/bin"
bash "$repo/local-dev/dx12-prepare.sh"
cmake -S "$repo/local-dev/dx12" -B "$build/target" \
    -DCMAKE_TOOLCHAIN_FILE="$repo/build/cmake/toolchains/mingw64.cmake" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo -DDX12_DEPS="$deps" -DDX12_OUTPUT="$out"
cmake --build "$build/target" --parallel 4
