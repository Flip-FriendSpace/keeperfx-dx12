# SPDX-License-Identifier: GPL-2.0-or-later
# KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
param(
  [string]$WorkspacePath,
  [ValidateSet('x86', 'x64')][string]$Architecture = 'x64',
  [ValidateSet('RelWithDebInfo', 'Debug')][string]$Configuration = 'RelWithDebInfo',
  [switch]$HeavyLog,
  [switch]$DX12,
  [ValidateRange(1, 32)][int]$Jobs = 6,
  [string]$BuildRoot = '/opt/keeperfx-build'
)
$ErrorActionPreference = 'Stop'
if (-not $WorkspacePath) { $WorkspacePath = Split-Path -Parent (Split-Path -Parent $PSScriptRoot) }
$workspace = (Resolve-Path -LiteralPath $WorkspacePath).Path
if ($DX12 -and $Architecture -ne 'x64') { throw 'The DX12 developer backend requires -Architecture x64.' }
if (Get-Process -Name keeperfx,keeperfx-local,keeperfx-local-hvlog,keeperfx-dx12,keeperfx-dx12-hvlog -ErrorAction SilentlyContinue) {
  throw 'Close running KeeperFX instances before rebuilding.'
}
& (Join-Path $PSScriptRoot 'prepare-dependencies.ps1') -Architecture $Architecture -WorkspacePath $workspace
if ($Architecture -eq 'x64') { & (Join-Path $PSScriptRoot 'prepare-x64-runtime.ps1') -WorkspacePath $workspace }
if ($DX12) { & (Join-Path $PSScriptRoot 'prepare-dx12-dependencies.ps1') -WorkspacePath $workspace }
$scriptPath = Join-Path $PSScriptRoot 'build.sh'
$linuxScript = (& wsl.exe -d keeperfx-dev --exec wslpath -a $scriptPath).Trim()
if ($LASTEXITCODE -ne 0) { throw 'The keeperfx-dev WSL build environment is unavailable.' }
$target = if ($HeavyLog) { 'keeperfx_hvlog' } else { 'keeperfx' }
New-Item -ItemType Directory -Path (Join-Path $workspace 'artifacts') -Force | Out-Null
$backend = if ($DX12) { 'dx12' } else { 'default' }
$suffix = if ($DX12) { '-dx12' } else { '' }
$log = Join-Path $workspace "artifacts/build-$target-$Architecture$suffix-$Configuration.log"
$linuxWorkspace = (& wsl.exe -d keeperfx-dev --exec wslpath -a $workspace).Trim()
if ($LASTEXITCODE -ne 0) { throw 'Cannot resolve the build workspace in keeperfx-dev.' }
& wsl.exe -d keeperfx-dev --exec bash $linuxScript $Configuration $target $Jobs $Architecture $backend $linuxWorkspace $BuildRoot 2>&1 | Tee-Object -FilePath $log
if ($LASTEXITCODE -ne 0) { throw "KeeperFX build failed. See $log" }
& (Join-Path $PSScriptRoot 'sync-config.ps1') -WorkspacePath $workspace

if ($DX12) {
  & (Join-Path $PSScriptRoot 'terrain/install_enhanced_terrain.ps1') -RuntimePath (Join-Path $workspace 'runtime/keeperfx-x64')
}
