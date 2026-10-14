# SPDX-License-Identifier: GPL-2.0-or-later
# KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
param([string[]]$CaptureDirectory)
$ErrorActionPreference = 'Stop'
$workspace = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$output = Join-Path $workspace 'artifacts/terrain'
$script = (& wsl.exe -d keeperfx-dev --exec wslpath -a (Join-Path $PSScriptRoot 'terrain-test.sh')).Trim()
if ($LASTEXITCODE -ne 0) { throw 'keeperfx-dev unavailable.' }
& wsl.exe -d keeperfx-dev --exec bash $script
if ($LASTEXITCODE -ne 0) { throw 'Terrain test compilation failed.' }
$units = foreach ($arch in @('i686','x86_64')) {
    $json = & (Join-Path $output "terrain-$arch.exe")
    if ($LASTEXITCODE -ne 0) { throw "Terrain tests failed: $arch" }
    $json | ConvertFrom-Json
}
if ($units[0].fixtureFingerprint -ne $units[1].fixtureFingerprint) { throw 'Architecture mismatch.' }
$captures = foreach ($dir in $CaptureDirectory) {
    $report = Get-Content -LiteralPath (Join-Path $dir 'report.json') -Raw | ConvertFrom-Json
    foreach ($file in $report.assetFiles) {
        $path = Join-Path $dir $file.file
        if ((Get-FileHash -LiteralPath $path).Hash -ne $file.sha256) { throw 'Capture hash mismatch.' }
        $pair = foreach ($arch in @('i686','x86_64')) {
            $json = & (Join-Path $output "terrain-$arch.exe") $path
            if ($LASTEXITCODE -ne 0) { throw "Capture meshing failed: $path ($arch)" }
            $json | ConvertFrom-Json
        }
        if ($pair[0].fingerprint -ne $pair[1].fingerprint) { throw 'Capture mesh differs between architectures.' }
        @{file=$path; result=$pair[1]}
    }
}
@{passed=$true; units=$units; captures=@($captures)} | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $output 'report.json') -Encoding utf8
Write-Output "Terrain tests passed: $output"
