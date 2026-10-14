# SPDX-License-Identifier: GPL-2.0-or-later
# KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
$ErrorActionPreference = 'Stop'
$workspace = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$out = Join-Path $workspace 'artifacts/foundation'
$linuxScript = '/mnt/' + $PSScriptRoot.Substring(0,1).ToLower() + $PSScriptRoot.Substring(2).Replace('\','/') + '/platform-probe.sh'
& wsl.exe -d keeperfx-dev --exec bash $linuxScript
if ($LASTEXITCODE -ne 0) { throw 'Platform probe compilation failed.' }
$layouts = @{}
foreach ($arch in @('i686','x86_64')) {
    $assembly = Get-Content -LiteralPath (Join-Path $out "abi-$arch.s") -Raw
    $values = [ordered]@{}
    foreach ($match in [regex]::Matches($assembly, '(?m)^_?kfx_(\w+):\r?\n\s*\.long\s+(\d+)')) {
        $values[$match.Groups[1].Value] = [long]$match.Groups[2].Value
    }
    if ($values.Count -ne 36) { throw "Incomplete ABI probe for $arch ($($values.Count) constants)." }
    $layouts[$arch] = $values
}
$capabilities = & (Join-Path $out 'd3d12-probe.exe')
if ($LASTEXITCODE -ne 0) { throw 'DirectX probe failed.' }
$report = [ordered]@{
    checkedAt = (Get-Date).ToUniversalTime().ToString('o')
    os = [Environment]::OSVersion.VersionString
    capabilities = ($capabilities | ConvertFrom-Json)
    layouts = $layouts
    scope = 'Compile-time Windows ABI comparison, not a full 64-bit engine build or save round-trip test.'
}
$json = $report | ConvertTo-Json -Depth 8
[IO.File]::WriteAllText((Join-Path $out 'platform-report.json'), $json)
$json
