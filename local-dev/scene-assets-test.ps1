# SPDX-License-Identifier: GPL-2.0-or-later
# KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
param([string]$CaptureDirectory)
$ErrorActionPreference = 'Stop'
$workspace = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$output = Join-Path $workspace 'artifacts/scene-assets'
$script = (& wsl.exe -d keeperfx-dev --exec wslpath -a (Join-Path $PSScriptRoot 'scene-assets-test.sh')).Trim()
if ($LASTEXITCODE -ne 0) { throw 'keeperfx-dev is unavailable.' }
& wsl.exe -d keeperfx-dev --exec bash $script
if ($LASTEXITCODE -ne 0) { throw 'Scene assets test compilation failed.' }
foreach ($arch in @('i686', 'x86_64')) {
    & (Join-Path $output "scene-assets-$arch.exe") (Join-Path $output "fixture-$arch.kfxscene")
    if ($LASTEXITCODE -ne 0) { throw "Scene asset tests failed: $arch" }
}
$fixtureHashes = @{}
$suffixes = @('', '.v1', '.v3', '.v4', '.v5', '.v6', '.v7', '.v8', '.v8-lens', '.v9', '.v10', '.v11', '.v11-empty', '.v11-copy', '.v11-fallback', '.v11-remap', '.v11-mist', '.v11-overlay', '.v11-not-drawn', '.v11-not-drawn-source', '.v11-budget')
foreach ($suffix in $suffixes) {
    $hash32 = (Get-FileHash -LiteralPath (Join-Path $output "fixture-i686.kfxscene$suffix")).Hash
    $hash64 = (Get-FileHash -LiteralPath (Join-Path $output "fixture-x86_64.kfxscene$suffix")).Hash
    if ($hash32 -ne $hash64) { throw "Cross-architecture capture bytes differ: $suffix" }
    $version = if ($suffix) { $suffix.Substring(1) } else { 'v2' }
    $fixtureHashes[$version] = $hash64
}
# Retain the established deterministic v2 fixture exactly, including its CRC/header.
if ($fixtureHashes.v2 -ne 'A474264446687787BF9EB29A296A1F4AE5A93BDBC992C8DFFC772AE67E49C6E6') { throw 'Legacy v2 fixture bytes changed.' }
if ($fixtureHashes.v1 -ne 'EC655C7B2453DAC80BAA4A642545179AE22C71CE2356D8B22ED0996B5F121E0E') { throw 'Legacy v1 fixture bytes changed.' }
if ($fixtureHashes.v3 -ne 'EE5E68D87057BDCD3204DBADA364CF7A5E51D39010563E4B13A74E2FE4FBEF28') { throw 'Legacy v3 fixture bytes changed.' }
if ($fixtureHashes.v4 -ne '77D48EA935B05E0EAC7138F1D51B371823D2BABB6C8C6D5212594E0F424FAEB8') { throw 'Legacy v4 fixture bytes changed.' }
if ($fixtureHashes.v5 -ne '7F4C4E3089A9079070D9FFB2F2E3A7C2FB90197D8955FB0F796F6A5442764C8B') { throw 'Legacy v5 fixture bytes changed.' }
if ($fixtureHashes.v6 -ne '045E82B20D659A397F0CED3E27E1C0D2612A525D9EC96B59FB18381539AF396B') { throw 'Legacy v6 fixture bytes changed.' }
if ($fixtureHashes.v7 -ne '1CAA7D4BF55CF1C856C10E27A06281748A2F20DA9ADD1C2ECAB5467E7E90E29E') { throw 'Legacy v7 fixture bytes changed.' }
if ($fixtureHashes.v8 -ne '5E4F40F01761368F5FF5D77AB8352573EBCE60EEF826A8307B3873C35BEDAC4A') { throw 'Legacy v8 screen fixture bytes changed.' }
if ($fixtureHashes.'v8-lens' -ne '09C5900CD2DC430031DCEAC1F1F8FDB2DCB21A55D659302720FA8203237551B2') { throw 'Legacy v8 lens fixture bytes changed.' }
if ($fixtureHashes.v9 -ne '19B9B20B02A2174150441A55A70199E73B5D1AEC9A8A70D3DAC79C2FBBB54ED7') { throw 'Legacy v9 fixture bytes changed.' }
if ($fixtureHashes.v10 -ne '83F4678A4F71D9FCD1411F26881299AE8124639BD5A13D74686E17FEA93B6483') { throw 'Legacy v10 fixture bytes changed.' }
foreach ($arch in @('i686', 'x86_64')) {
    foreach ($fixture in @('i686', 'x86_64')) {
        foreach ($suffix in $suffixes) {
            & (Join-Path $output "scene-assets-$arch.exe") --inspect (Join-Path $output "fixture-$fixture.kfxscene$suffix")
            if ($LASTEXITCODE -ne 0) { throw "Cross-architecture load failed: $suffix" }
        }
    }
}
$captures = @()
if ($CaptureDirectory) {
    if (Get-Process -Name keeperfx-local -ErrorAction SilentlyContinue) { throw 'Close the game before testing independent capture loading.' }
    $live = Get-Content -LiteralPath (Join-Path $CaptureDirectory 'report.json') -Raw | ConvertFrom-Json
    foreach ($item in $live.assetFiles) {
        $file = Join-Path $CaptureDirectory $item.file
        if ((Get-FileHash -LiteralPath $file).Hash -ne $item.sha256) { throw 'Capture hash mismatch.' }
        $json = & (Join-Path $output 'scene-assets-x86_64.exe') --inspect $file
        if ($LASTEXITCODE -ne 0) { throw "Independent capture load failed: $file" }
        $result = $json | ConvertFrom-Json
        if ($result.view -ne $item.view -or $result.sprites -eq 0) { throw 'Capture contents mismatch.' }
        $captures += $result
    }
    if ($captures.Count -ne 5 -or 17 -notin $captures[0].variations) { throw 'Missing lifecycle captures or Keeper Enhanced variation.' }
}
@{passed=$true; fixtureSha256=$fixtureHashes.v2; fixtureHashes=$fixtureHashes; captures=$captures} | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $output 'report.json') -Encoding utf8
Write-Output "Scene asset tests passed. Evidence: $output"
