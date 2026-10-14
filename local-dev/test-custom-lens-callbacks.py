# SPDX-License-Identifier: GPL-2.0-or-later
# KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
"""Build an isolated Windows CPU test using the existing keeperfx-dev dependencies.

Run in keeperfx-dev; this neither builds nor launches the game. Execute the
resulting EXE from Windows and retain stdout in the private artifact directory.
"""
import argparse
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument("--architecture", choices=("x64", "x86"), default="x64")
parser.add_argument("--output", type=Path)
args = parser.parse_args()
repo = Path(__file__).resolve().parent.parent
output = args.output or repo.parent / "artifacts" / "custom-lens-cpu" / args.architecture
output.mkdir(parents=True, exist_ok=True)
triple = "x86_64-w64-mingw32" if args.architecture == "x64" else "i686-w64-mingw32"
build = Path("/opt/keeperfx-build")
if args.architecture == "x64":
    build /= "x64"
deps = build / "RelWithDebInfo" / "deps"
sdl = list((deps / "sdl3").glob(f"*/{triple}/include/SDL3/SDL.h"))
if len(sdl) != 1:
    raise RuntimeError("Expected one existing architecture-specific SDL header")
includes = ["-I" + str(repo / "src"), "-I" + str(deps / "luajit/include"), "-I" + str(sdl[0].parent.parent)]
includes += ["-I" + str(path) for path in deps.glob(f"*/*/{triple}/include") if path != sdl[0].parent.parent]
objects = []
for name in ("src/kfx/lense/LuaLensEffect.cpp", "src/lua_api_lens.c", "local-dev/custom-lens-callback-test.cpp"):
    c = name.endswith(".c")
    obj = output / (Path(name).name + ".o")
    objects.append(str(obj))
    subprocess.run([triple + ("-gcc" if c else "-g++"), "-std=" + ("gnu11" if c else "c++20"),
                    "-O0", "-ffunction-sections", "-fdata-sections", "-Wall", "-Wextra", *includes,
                    "-c", str(repo / name), "-o", str(obj)], check=True)
exe = output / "custom-lens-callback-test.exe"
subprocess.run([triple + "-g++", "-static", "-Wl,--gc-sections", *objects,
                str(deps / "luajit/lib/libluajit.a"), "-o", str(exe)], check=True)
print(exe)
