# SPDX-License-Identifier: GPL-2.0-or-later
# KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
$ErrorActionPreference = 'Stop'
$workspace = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$original = Join-Path $workspace 'runtime/keeperfx'
$runtime = Join-Path $workspace 'runtime/keeperfx-x64'
if (-not (Test-Path -LiteralPath (Join-Path $original 'fxdata/creature.cfg'))) { throw 'Original runtime data is missing.' }
New-Item -ItemType Directory -Path $runtime -Force | Out-Null
# Large, shared game assets remain in their existing location. Executables,
# DLLs, logs, saves and replays are isolated for each architecture.
foreach ($folder in @('campgns', 'creatrs', 'data', 'fxdata', 'ldata', 'levels', 'mods', 'multiplayer', 'music', 'sound')) {
  $path = Join-Path $runtime $folder
  if (-not (Test-Path -LiteralPath $path)) {
    New-Item -ItemType Junction -Path $path -Target (Join-Path $original $folder) | Out-Null
  }
}
foreach ($folder in @('save', 'replays', 'scrshots')) {
  New-Item -ItemType Directory -Path (Join-Path $runtime $folder) -Force | Out-Null
}
foreach ($file in @('keeperfx.cfg', 'save/settings.toml', 'keeperfx_readme.txt')) {
  $destination = Join-Path $runtime $file
  if (-not (Test-Path -LiteralPath $destination) -and (Test-Path -LiteralPath (Join-Path $original $file))) {
    Copy-Item -LiteralPath (Join-Path $original $file) -Destination $destination
  }
}
