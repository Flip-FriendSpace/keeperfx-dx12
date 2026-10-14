# SPDX-License-Identifier: GPL-2.0-or-later
# KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
param([string[]]$CaptureDirectory)
$ErrorActionPreference = 'Stop'
$workspace = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$output = Join-Path $workspace 'artifacts/scene-view'
$script = (& wsl.exe -d keeperfx-dev --exec wslpath -a (Join-Path $PSScriptRoot 'scene-view-test.sh')).Trim()
if ($LASTEXITCODE -ne 0) { throw 'keeperfx-dev unavailable.' }
& wsl.exe -d keeperfx-dev --exec bash $script
if ($LASTEXITCODE -ne 0) { throw 'Scene view test compilation failed.' }
foreach ($arch in @('i686','x86_64')) {
    & (Join-Path $output "scene-view-$arch.exe")
    if ($LASTEXITCODE -ne 0) { throw "Scene view tests failed: $arch" }
}
$captures = foreach ($dir in $CaptureDirectory) {
    $report = Get-Content -LiteralPath (Join-Path $dir 'report.json') -Raw | ConvertFrom-Json
    foreach ($file in $report.assetFiles) {
        $path = Join-Path $dir $file.file
        if ((Get-FileHash -LiteralPath $path).Hash -ne $file.sha256) { throw 'Capture hash mismatch.' }
        $pair = foreach ($arch in @('i686','x86_64')) {
            $json = & (Join-Path $output "scene-view-$arch.exe") $path
            if ($LASTEXITCODE -ne 0) { throw "Scene view comparison failed: $path ($arch): $json" }
            $json | ConvertFrom-Json
        }
        if ($pair[0].contentFingerprint -ne $pair[1].contentFingerprint) { throw 'Sprite selection or atlas pixels differ between architectures.' }
        $a=$pair[0].vertexComponents; $b=$pair[1].vertexComponents
        if ($a.Count -ne $b.Count) { throw 'Sprite vertex counts differ between architectures.' }
        $maxDelta=0.0
        for ($i=0; $i -lt $a.Count; $i++) {
            $delta=[Math]::Abs($a[$i]-$b[$i])
            $limit=if (($i%5) -lt 3) { 0.0001 } else { 0.000001 }
            if ($delta -gt $limit) { throw "Sprite geometry differs beyond float precision: $path ($delta)" }
            $maxDelta=[Math]::Max($maxDelta,$delta)
        }
        $pair[1].PSObject.Properties.Remove('vertexComponents')
        @{file=$path; result=$pair[1]; maxArchitectureDelta=$maxDelta}
    }
}
@{passed=$true; captures=@($captures)} | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $output 'report.json') -Encoding utf8
Write-Output "Scene view tests passed: $output"
