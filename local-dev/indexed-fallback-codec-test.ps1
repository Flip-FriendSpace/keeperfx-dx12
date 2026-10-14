# SPDX-License-Identifier: GPL-2.0-or-later
# KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
$ErrorActionPreference = 'Stop'
$workspace = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$output = Join-Path $workspace 'artifacts/indexed-fallback-codec'
$script = (& wsl.exe -d keeperfx-dev --exec wslpath -a (Join-Path $PSScriptRoot 'indexed-fallback-codec-test.sh')).Trim()
if ($LASTEXITCODE -ne 0) { throw 'keeperfx-dev is unavailable.' }
& wsl.exe -d keeperfx-dev --exec bash $script
if ($LASTEXITCODE -ne 0) { throw 'Indexed fallback codec compilation failed.' }
$tests = @{}
foreach ($arch in @('i686','x86_64')) {
    $json = & (Join-Path $output "indexed-fallback-codec-$arch.exe") (Join-Path $output "fixture-$arch.kfxidx")
    if ($LASTEXITCODE -ne 0) { throw "Indexed fallback codec tests failed: $arch" }
    $tests[$arch] = $json | ConvertFrom-Json
}
$hashes = @{}
foreach ($suffix in @('','.max','.min')) {
    $hash32 = (Get-FileHash -LiteralPath (Join-Path $output "fixture-i686.kfxidx$suffix")).Hash
    $hash64 = (Get-FileHash -LiteralPath (Join-Path $output "fixture-x86_64.kfxidx$suffix")).Hash
    if ($hash32 -ne $hash64) { throw "Cross-architecture snapshot bytes differ: $suffix" }
    $hashes[$(if ($suffix) { $suffix.Substring(1) } else { 'normal' })] = $hash64
}
$inspections = @()
foreach ($reader in @('i686','x86_64')) {
    foreach ($fixture in @('i686','x86_64')) {
        foreach ($suffix in @('','.max','.min')) {
            $json = & (Join-Path $output "indexed-fallback-codec-$reader.exe") --inspect (Join-Path $output "fixture-$fixture.kfxidx$suffix")
            if ($LASTEXITCODE -ne 0) { throw "Cross-architecture snapshot load failed: $reader/$fixture/$suffix" }
            $inspections += @{reader=$reader;fixture=$fixture;suffix=$suffix;result=($json | ConvertFrom-Json)}
        }
    }
}
@{schema=1;passed=$true;scope='CPU-only indexed fallback codec, bounded allocation and canonical cross-architecture fixtures';tests=$tests;fixtureHashes=$hashes;inspections=$inspections} |
    ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $output 'report.json') -Encoding utf8
Write-Output "Indexed fallback codec tests passed. Evidence: $output"
