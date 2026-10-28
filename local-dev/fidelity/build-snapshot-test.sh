#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
set -euo pipefail
repo="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
out="${1:?Usage: build-snapshot-test.sh /path/to/new-output-directory}"
deps="${KFX_TEST_DEPS_ROOT:-/opt/keeperfx-build/RelWithDebInfo/deps}"
mkdir -p "$(dirname -- "$out")"
mkdir "$out"
# The writer does not use build version metadata. Supply a test-only header so
# a clean checkout can compile without configuring or modifying engine sources.
cat > "$out/ver_defs.h" <<'EOF'
#define VER_MAJOR 0
#define VER_MINOR 0
#define VER_RELEASE 0
#define VER_BUILD 0
#define VER_STRING "fidelity-test"
#define PACKAGE_SUFFIX "test"
#define GIT_REVISION "isolated-snapshot-test"
EOF
for arch in i686 x86_64; do
  sdl=("$deps"/sdl3/SDL3-*/"$arch-w64-mingw32/include")
  mixer=("$deps"/sdl3_mixer/SDL3_mixer-*/"$arch-w64-mingw32/include")
  "$arch-w64-mingw32-gcc" -std=c11 -O2 -Wall -Wextra -Wformat=2 \
    -I"$repo/src" -I"$repo" -I"$out" -I"${sdl[0]}" -I"${mixer[0]}" \
    -c "$repo/src/fidelity_state.c" -o "$out/fidelity-state-$arch.o"
  "$arch-w64-mingw32-g++" -std=c++20 -O2 -Wall -Wextra -Wformat=2 -static \
    -I"$repo/src" -I"$repo" -I"$out" -I"${sdl[0]}" -I"${mixer[0]}" \
    "$repo/local-dev/fidelity/snapshot-test.cpp" "$out/fidelity-state-$arch.o" \
    -o "$out/snapshot-test-$arch.exe"
done
