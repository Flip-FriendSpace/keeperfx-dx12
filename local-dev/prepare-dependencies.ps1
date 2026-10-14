# SPDX-License-Identifier: GPL-2.0-or-later
# KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
param([ValidateSet('x86', 'x64')][string]$Architecture = 'x64')
$ErrorActionPreference = 'Stop'
$workspace = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$cache = Join-Path $workspace 'downloads/keeperfx-deps'
if ($Architecture -eq 'x64') { $cache += '-x64' }
New-Item -ItemType Directory -Path $cache -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $workspace 'artifacts') -Force | Out-Null
$dependencies = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'dependencies.json') -Raw | ConvertFrom-Json
if ($Architecture -eq 'x64') {
  $dependencies = @($dependencies | Where-Object name -in @('sdl3', 'sdl3_mixer', 'sdl3_image', 'png2ico')) + @(Get-Content -LiteralPath (Join-Path $PSScriptRoot 'dependencies-x64.json') -Raw | ConvertFrom-Json)
}
$manifest = @()
foreach ($dependency in $dependencies) {
  $extension = if ($dependency.url.EndsWith('.zip')) { '.zip' } else { '.tar.gz' }
  $path = Join-Path $cache ($dependency.name + $extension)
  $shared = Join-Path $workspace ('downloads/keeperfx-deps/' + $dependency.name + $extension)
  if ($Architecture -eq 'x64' -and $dependency.name -in @('sdl3', 'sdl3_mixer', 'sdl3_image', 'png2ico') -and (Test-Path -LiteralPath $shared) -and -not (Test-Path -LiteralPath $path)) {
    Copy-Item -LiteralPath $shared -Destination $path
  }
  if (-not (Test-Path -LiteralPath $path)) {
    Write-Output "Downloading $($dependency.name)"
    & curl.exe -fL --http1.1 --retry 4 --retry-all-errors --retry-delay 2 --silent --show-error $dependency.url -o ($path + '.partial')
    if ($LASTEXITCODE -ne 0) { throw "Download failed: $($dependency.name)" }
    Move-Item -LiteralPath ($path + '.partial') -Destination $path
  }
  $hash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
  if ($dependency.sha256 -and $dependency.sha256 -ne $hash) { throw "SHA256 mismatch: $($dependency.name)" }
  $manifest += [pscustomobject]@{ name = $dependency.name; url = $dependency.url; sha256 = $hash }
}
$manifest | ConvertTo-Json -Depth 3 | Set-Content -LiteralPath (Join-Path $workspace "artifacts/dependency-downloads-$Architecture.json") -Encoding utf8
