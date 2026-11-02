#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
set -euo pipefail
repo="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
workspace="${1:-$(dirname -- "$repo")}"
cache="$workspace/downloads/keeperfx-dx12"
build=/opt/keeperfx-build/dx12
deps="$build/deps"
out="$workspace/artifacts/dx12/bin"
bash "$repo/local-dev/dx12-prepare.sh" "$workspace"
# CMake refuses a build folder made from another checkout.
target="$build/target-$(printf %s "$repo" | sha1sum | cut -c1-8)"
cmake -S "$repo/local-dev/dx12" -B "$target" \
    -DCMAKE_TOOLCHAIN_FILE="$repo/build/cmake/toolchains/mingw64.cmake" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo -DDX12_DEPS="$deps" -DDX12_OUTPUT="$out"
cmake --build "$target" --parallel 4
