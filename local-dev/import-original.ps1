# SPDX-License-Identifier: GPL-2.0-or-later
# KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
param([string]$OriginalPath = 'C:\GOG Games\Dungeon Keeper Gold')
$ErrorActionPreference = 'Stop'
$workspace = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$runtime = Join-Path $workspace 'runtime/keeperfx'
if (-not (Test-Path -LiteralPath (Join-Path $runtime 'keeperfx.exe'))) {
  throw 'Extract the official release and matching alpha into runtime/keeperfx first.'
}
$files = @(
  'data/bluepal.dat', 'data/bluepall.dat', 'data/dogpal.pal',
  'data/hitpall.dat', 'data/lightng.pal', 'data/redpal.col',
  'data/redpall.dat', 'data/slab0-0.dat', 'data/slab0-1.dat',
  'data/vampal.pal', 'data/whitepal.col',
  'sound/atmos1.sbk', 'sound/atmos2.sbk', 'sound/bullfrog.sbk'
)
foreach ($relative in $files) {
  if (-not (Test-Path -LiteralPath (Join-Path $OriginalPath $relative))) {
    throw "Missing original game file: $relative"
  }
}
$manifest = @()
foreach ($relative in $files) {
  $source = Join-Path $OriginalPath $relative
  $destination = Join-Path $runtime $relative
  Copy-Item -LiteralPath $source -Destination $destination -Force
  $hash = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash
  if ((Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash -ne $hash) {
    throw "Copied file failed verification: $relative"
  }
  $manifest += [pscustomobject]@{ source = $source; runtimePath = $relative; sha256 = $hash }
}
foreach ($movie in @('drag.smk', 'intromix.smk', 'outromix.smk')) {
  $source = Join-Path $OriginalPath "ldata/$movie"
  if (Test-Path -LiteralPath $source) {
    $relative = "ldata/$movie"
    $destination = Join-Path $runtime $relative
    Copy-Item -LiteralPath $source -Destination $destination -Force
    $hash = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash
    if ((Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash -ne $hash) {
      throw "Copied movie failed verification: $relative"
    }
    $manifest += [pscustomobject]@{ source = $source; runtimePath = $relative; sha256 = $hash }
  }
}
Get-ChildItem -LiteralPath $OriginalPath -Filter 'keeper*.ogg' -File | ForEach-Object {
  $source = $_.FullName
  $relative = "music/$($_.Name)"
  $destination = Join-Path $runtime $relative
  Copy-Item -LiteralPath $source -Destination $destination -Force
  $hash = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash
  if ((Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash -ne $hash) {
    throw "Copied music failed verification: $relative"
  }
  $manifest += [pscustomobject]@{ source = $source; runtimePath = $relative; sha256 = $hash }
}
$manifest | ConvertTo-Json -Depth 3 | Set-Content -LiteralPath (Join-Path $workspace 'artifacts/original-data-manifest.json') -Encoding utf8
Write-Output "Imported and hash-verified $($manifest.Count) files, including $($files.Count) required files and available movies/music."
