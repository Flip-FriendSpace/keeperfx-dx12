# SPDX-License-Identifier: GPL-2.0-or-later
# KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
$ErrorActionPreference = 'Stop'
$source = Split-Path -Parent $PSScriptRoot
$workspace = Split-Path -Parent $source
$runtime = Join-Path $workspace 'runtime/keeperfx'
if (Get-Process -Name keeperfx,keeperfx-local,keeperfx-local-hvlog -ErrorAction SilentlyContinue) {
  throw 'Close running KeeperFX instances before syncing definitions.'
}
if (-not (Test-Path -LiteralPath (Join-Path $runtime 'keeperfx.exe'))) { throw 'Runtime is missing.' }
# Copy editable game definitions; retain generated assets and user settings.
foreach ($folder in @('creatrs', 'fxdata', 'mods')) {
  $from = Join-Path $source "config/$folder"
  if (-not (Test-Path -LiteralPath $from -PathType Container)) { continue }
  $to = Join-Path $runtime $folder
  New-Item -ItemType Directory -Path $to -Force | Out-Null
  Get-ChildItem -LiteralPath $from -File -Recurse | ForEach-Object {
    $relative = $_.FullName.Substring($from.Length).TrimStart('\', '/')
    $destination = Join-Path $to $relative
    New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
    Copy-Item -LiteralPath $_.FullName -Destination $destination -Force
  }
}
Write-Output 'Synced source config/creatrs, config/fxdata and config/mods into the runtime.'
