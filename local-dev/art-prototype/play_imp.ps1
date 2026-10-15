# SPDX-License-Identifier: GPL-2.0-or-later
param([switch]$QuickStart, [ValidateSet('imp-set','imp-actions')][string]$AssetSet='imp-actions')
$ErrorActionPreference = 'Stop'
$workspace = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $PSScriptRoot))
$runtime = Join-Path $workspace 'runtime/keeperfx-x64'
$source = Join-Path $workspace ('artifacts/art-prototype/'+$AssetSet+'/mod/imp_art_prototype')
$target = Join-Path $runtime 'mods/imp_art_prototype'
$loadOrder = Join-Path $runtime 'mods/load_order.cfg'
if (Get-Process -Name keeperfx,keeperfx-local,keeperfx-local-hvlog,keeperfx-dx12,keeperfx-dx12-hvlog -ErrorAction SilentlyContinue) {
    throw 'Close KeeperFX before starting the HD imp playtest.'
}
if (-not (Test-Path -LiteralPath (Join-Path $source 'fxdata/imp-sprites.zip'))) {
    throw 'Build the selected local imp package with package_imp_set.py or package_imp_actions.py first.'
}
if ((Test-Path -LiteralPath $target) -and -not (Test-Path -LiteralPath (Join-Path $target 'local-prototype.json'))) {
    throw 'The installed imp directory is not the local prototype; refusing to overwrite it.'
}
$original = [IO.File]::ReadAllBytes($loadOrder)
$order = [IO.File]::ReadAllText($loadOrder)
if ($order -match '(?m)^imp_art_prototype\s*$') { throw 'The imp prototype is already enabled.' }
$evidence = Join-Path $workspace ('artifacts/art-prototype/'+$AssetSet+'/play-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
New-Item -ItemType Directory -Path $evidence | Out-Null
[IO.File]::WriteAllBytes((Join-Path $evidence 'load_order.before.cfg'), $original)
$enabled = $order.TrimEnd() + "`r`nimp_art_prototype`r`n"
$algorithm = [Security.Cryptography.SHA256]::Create()
try {
    $stream = [IO.File]::OpenRead((Join-Path $source 'fxdata/imp-sprites.zip'))
    try { $packageHash = ([BitConverter]::ToString($algorithm.ComputeHash($stream))).Replace('-', '') }
    finally { $stream.Dispose() }
} finally { $algorithm.Dispose() }
$installed = $false
try {
    New-Item -ItemType Directory -Path $target -Force | Out-Null
    Copy-Item -Path (Join-Path $source '*') -Destination $target -Recurse -Force
    [IO.File]::WriteAllText($loadOrder, $enabled)
    $installed = $true
    & (Join-Path (Split-Path -Parent $PSScriptRoot) 'play.ps1') -DX12Preview -QuickStart:$QuickStart
    $games = @(Get-Process -Name keeperfx-dx12 -ErrorAction Stop)
    if ($games.Count -ne 1 -or $games[0].Path -ne (Join-Path $runtime 'keeperfx-dx12.exe')) {
        throw 'Unable to identify the playtest process.'
    }
    $game = $games[0]
    @{pid=$game.Id;packageSHA256=$packageHash;apiEnabled=$false} |
        ConvertTo-Json | Set-Content -LiteralPath (Join-Path $evidence 'launch.json') -Encoding utf8
    Write-Output "HD imp package $AssetSet enabled. The previous mod order will be restored when the game exits."
    $game.WaitForExit()
} finally {
    if ($installed -and [IO.File]::ReadAllText($loadOrder) -eq $enabled) {
        [IO.File]::WriteAllBytes($loadOrder, $original)
        'Previous mod order restored.' | Set-Content -LiteralPath (Join-Path $evidence 'restored.txt')
    } elseif ($installed) {
        Write-Warning "Mod order was edited during play; it has been preserved. Original backup: $evidence/load_order.before.cfg"
    }
}
