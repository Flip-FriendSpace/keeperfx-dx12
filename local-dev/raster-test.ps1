# SPDX-License-Identifier: GPL-2.0-or-later
# KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
$ErrorActionPreference = 'Stop'
$workspace = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$script = (& wsl.exe -d keeperfx-dev --exec wslpath -a (Join-Path $PSScriptRoot 'raster-test.sh')).Trim()
if ($LASTEXITCODE -ne 0) { throw 'keeperfx-dev is unavailable.' }
& wsl.exe -d keeperfx-dev --exec bash $script
if ($LASTEXITCODE -ne 0) { throw 'Rasterizer test compilation failed.' }
$results = @()
foreach ($arch in @('i686', 'x86_64')) {
  $results += & (Join-Path $workspace "artifacts/x64-port/raster-$arch.exe")
  if ($LASTEXITCODE -ne 0) { throw "Rasterizer clipping failed: $arch" }
}
if ($results.Count -ne 2 -or $results[0] -ne $results[1]) { throw 'x86/x64 rasterizer output differs.' }
$result = [ordered]@{ passed=$true; cases=6; framebufferHash=$results[0]; architectures=@('i686','x86_64') }
$result | ConvertTo-Json | Tee-Object -FilePath (Join-Path $workspace 'artifacts/x64-port/raster-test.json')
