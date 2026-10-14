# SPDX-License-Identifier: GPL-2.0-or-later
# KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$workspace = Split-Path -Parent $repo
$artifacts = Join-Path $workspace 'artifacts'
New-Item -ItemType Directory -Path $artifacts -Force | Out-Null
$linuxRepo = (& wsl.exe -d keeperfx-dev --exec wslpath -a $repo).Trim()
if ($LASTEXITCODE -ne 0) { throw 'The keeperfx-dev WSL build environment is unavailable.' }
$linuxArtifacts = (& wsl.exe -d keeperfx-dev --exec wslpath -a $artifacts).Trim()
if ($LASTEXITCODE -ne 0) { throw 'Cannot resolve the artifacts directory.' }
& wsl.exe -d keeperfx-dev --exec i686-w64-mingw32-g++ -std=c++17 -O2 -static `
  "-I$linuxRepo/src" "-I$linuxRepo/deps/glad/include" `
  "$linuxRepo/local-dev/lighting-test.cpp" "$linuxRepo/deps/glad/src/glad.c" `
  -lopengl32 -lgdi32 -o "$linuxArtifacts/lighting-test.exe"
if ($LASTEXITCODE -ne 0) { throw 'Lighting regression test compilation failed.' }
& (Join-Path $artifacts 'lighting-test.exe') 2>&1 | Tee-Object -FilePath (Join-Path $artifacts 'lighting-test.log')
if ($LASTEXITCODE -ne 0) { throw 'Lighting regression test failed.' }
