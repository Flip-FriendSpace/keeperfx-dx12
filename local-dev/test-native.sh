#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Builds a self-contained test in keeperfx-dev with sanitizers, then runs it.
# usage: test-native.sh <id> <source>...   (sources are relative to the checkout)
set -euo pipefail
repo="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
id="${1:?Usage: test-native.sh <id> <source>...}"; shift
out="/opt/keeperfx-build/test-runner/$(basename -- "$repo")/$id"
deps="${KFX_TEST_DEPS_ROOT:-/opt/keeperfx-build/x64/RelWithDebInfo/deps}"
mkdir -p "$out"
cd "$repo"
flags=(-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -I"$repo/src")
for sdl in "$deps"/sdl3/SDL3-*/x86_64-w64-mingw32/include; do flags+=(-isystem "$sdl"); done
objects=()
for source in "$@"; do
  object="$out/$(basename -- "$source").o"
  case "$source" in
    *.c) gcc -std=gnu11 "${flags[@]}" -c "$source" -o "$object" ;;
    *) g++ -std=c++20 "${flags[@]}" -c "$source" -o "$object" ;;
  esac
  objects+=("$object")
done
g++ -fsanitize=address,undefined "${objects[@]}" -o "$out/test"
"$out/test"
