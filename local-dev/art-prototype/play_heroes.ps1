# SPDX-License-Identifier: GPL-2.0-or-later
param([switch]$QuickStart)
$ErrorActionPreference = 'Stop'
$workspace = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $PSScriptRoot))
$runtime = Join-Path $workspace 'runtime/keeperfx-x64'
$artifacts = Join-Path $workspace 'artifacts/art-prototype/heroes'
$source = Join-Path $artifacts 'mod/heroes_art_prototype'
$target = Join-Path $runtime 'mods/heroes_art_prototype'
$loadOrder = Join-Path $runtime 'mods/load_order.cfg'
function Get-Sha256([string]$Path) {
    $algorithm = [Security.Cryptography.SHA256]::Create()
    try {
        $stream = [IO.File]::OpenRead($Path)
        try { return ([BitConverter]::ToString($algorithm.ComputeHash($stream))).Replace('-', '').ToLowerInvariant() }
        finally { $stream.Dispose() }
    } finally { $algorithm.Dispose() }
}
function Get-PackageManifestHash($Packages) {
    $records = @($Packages | ForEach-Object {
        [ordered]@{config=$_.config;config_sha256=$_.config_sha256;slug=$_.slug;zip=$_.zip;zip_sha256=$_.zip_sha256}
    })
    $json = ConvertTo-Json -InputObject $records -Compress -Depth 5
    $algorithm = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($algorithm.ComputeHash([Text.Encoding]::UTF8.GetBytes($json)))).Replace('-', '').ToLowerInvariant() }
    finally { $algorithm.Dispose() }
}
function Get-HeroLoadOrder([string]$Order, $Packages) {
    $previewNames = @('imp_art_prototype', 'heroes_art_prototype')
    $previewNames += @($Packages | Where-Object slug -ne 'imp' | ForEach-Object { 'hero_' + $_.slug + '_art_prototype' })
    $alternatives = ($previewNames | ForEach-Object { [regex]::Escape($_) }) -join '|'
    $filtered = [regex]::Replace($Order, ('(?im)^[ \t]*(?:' + $alternatives + ')[ \t]*(?:\r?\n|$)'), '')
    return $filtered.TrimEnd() + "`r`nheroes_art_prototype`r`n"
}
function Assert-Unlinked([string]$Path) {
    $item = Get-Item -LiteralPath $Path -Force
    if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Refusing linked path: $Path" }
}
function Assert-Mod($Root, $Report) {
    Assert-Unlinked $Root
    $marker = Get-Content -LiteralPath (Join-Path $Root 'local-prototype.json') -Raw | ConvertFrom-Json
    if ($marker.local_only -ne $true -or $marker.kind -ne 'heroes_and_accepted_imp' -or
        $marker.package_manifest_sha256 -ne $Report.package_manifest_sha256 -or
        (Get-PackageManifestHash $marker.packages) -ne $Report.package_manifest_sha256) {
        throw "Combined mod marker does not match the package report: $Root"
    }
    $expected = @('local-prototype.json')
    foreach ($package in $Report.packages) {
        foreach ($pair in @(@('fxdata', $package.zip, $package.zip_sha256), @('creatrs', $package.config, $package.config_sha256))) {
            $relative = $pair[0] + '/' + $pair[1]
            $path = Join-Path $Root $relative
            Assert-Unlinked $path
            if ((Get-Sha256 $path) -ne $pair[2]) { throw "Package content hash mismatch: $path" }
            $expected += $relative
        }
    }
    foreach ($item in Get-ChildItem -LiteralPath $Root -Recurse -Force) {
        Assert-Unlinked $item.FullName
        if (-not $item.PSIsContainer) {
            $relative = $item.FullName.Substring($Root.Length + 1).Replace('\', '/')
            if ($relative -notin $expected) { throw "Unexpected combined mod file: $relative" }
        }
    }
}
if (Get-Process -Name keeperfx,keeperfx-local,keeperfx-local-hvlog,keeperfx-dx12,keeperfx-dx12-hvlog -ErrorAction SilentlyContinue) {
    throw 'Close KeeperFX before starting the HD heroes playtest.'
}
$report = Get-Content -LiteralPath (Join-Path $artifacts 'combined-package-report.json') -Raw | ConvertFrom-Json
if ($report.local_only -ne $true -or $report.heroes -ne 13 -or @($report.packages).Count -ne 14) { throw 'Expected thirteen local hero packages and the accepted imp.' }
$seen = @{}
foreach ($package in $report.packages) {
    if ($package.slug -notmatch '^[a-z][a-z0-9_-]*$' -or $seen.ContainsKey($package.slug) -or
        $package.zip -cne ($package.slug + '-sprites.zip') -or $package.config -cne ($package.slug + '.cfg') -or
        $package.zip_sha256 -notmatch '^[a-f0-9]{64}$' -or $package.config_sha256 -notmatch '^[a-f0-9]{64}$') { throw 'Invalid or duplicate combined package record.' }
    $seen[$package.slug] = $true
}
$imp = @($report.packages | Where-Object slug -eq 'imp')
if ($imp.Count -ne 1 -or $imp[0].zip_sha256 -ne 'a1c742fcd4d207de741b0432723910633da3e35db60afb97477dc242fdf89819') { throw 'The accepted imp package has changed.' }
if ((Get-PackageManifestHash $report.packages) -ne $report.package_manifest_sha256) { throw 'Combined report manifest hash mismatch.' }
Assert-Mod $source $report
Assert-Unlinked (Join-Path $runtime 'mods')
Assert-Unlinked $loadOrder
if (Test-Path -LiteralPath $target) { Assert-Mod $target $report }
$original = [IO.File]::ReadAllBytes($loadOrder)
$enabled = Get-HeroLoadOrder ([IO.File]::ReadAllText($loadOrder)) $report.packages
$evidence = Join-Path $artifacts ('play-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
New-Item -ItemType Directory -Path $evidence | Out-Null
[IO.File]::WriteAllBytes((Join-Path $evidence 'load_order.before.cfg'), $original)
$orderChanged = $false
try {
    New-Item -ItemType Directory -Path $target -Force | Out-Null
    Copy-Item -Path (Join-Path $source '*') -Destination $target -Recurse -Force
    Assert-Mod $target $report
    [IO.File]::WriteAllText($loadOrder, $enabled)
    $orderChanged = $true
    $game = & (Join-Path (Split-Path -Parent $PSScriptRoot) 'play.ps1') -DX12Preview -QuickStart:$QuickStart -PassThru
    if ($null -eq $game -or $game.Path -ne (Join-Path $runtime 'keeperfx-dx12.exe')) { throw 'Unable to identify the playtest process.' }
    @{pid=$game.Id;packageManifestSHA256=$report.package_manifest_sha256;apiEnabled=$false} |
        ConvertTo-Json | Set-Content -LiteralPath (Join-Path $evidence 'launch.json') -Encoding utf8
    Write-Output 'HD heroes and accepted imp enabled for review. Previous mod order will be restored after exit.'
    $game.WaitForExit()
} finally {
    if ($orderChanged -and [IO.File]::ReadAllText($loadOrder) -eq $enabled) {
        [IO.File]::WriteAllBytes($loadOrder, $original)
        'Previous mod order restored.' | Set-Content -LiteralPath (Join-Path $evidence 'restored.txt')
    } elseif ($orderChanged) {
        Write-Warning "Mod order changed during play; preserving those edits. Original backup: $evidence/load_order.before.cfg"
    }
}
