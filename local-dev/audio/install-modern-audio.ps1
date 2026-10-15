# SPDX-License-Identifier: GPL-2.0-or-later
[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [string]$PackageDirectory,
    [string]$RuntimeDirectory
)
$ErrorActionPreference = 'Stop'
$sourceRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$workspace = Split-Path -Parent $sourceRoot
$modName = 'dk_modern_audio'
if (-not $RuntimeDirectory) { $RuntimeDirectory = Join-Path $workspace 'runtime/keeperfx-x64' }
$RuntimeDirectory = [IO.Path]::GetFullPath($RuntimeDirectory)
$target = Join-Path $RuntimeDirectory ('mods/' + $modName)
if (-not $PackageDirectory) {
    $PackageDirectory = Join-Path $sourceRoot 'config/mods/dk_modern_audio'
}
$PackageDirectory = [IO.Path]::GetFullPath($PackageDirectory)
if (-not (Test-Path -LiteralPath (Join-Path $RuntimeDirectory 'keeperfx-dx12.exe') -PathType Leaf)) {
    throw 'Build the local x64 DX12 engine before installing Modern audio.'
}
function Get-AudioFileHash([string]$Path) {
    $algorithm = [Security.Cryptography.SHA256]::Create()
    try {
        $stream = [IO.File]::OpenRead($Path)
        try { return ([BitConverter]::ToString($algorithm.ComputeHash($stream))).Replace('-', '').ToLowerInvariant() }
        finally { $stream.Dispose() }
    } finally { $algorithm.Dispose() }
}
function Get-AudioAssetPath([string]$Root, [string]$Relative) {
    if ([IO.Path]::IsPathRooted($Relative) -or $Relative -match '(^|[/\\])\.\.?([/\\]|$)') {
        throw "Invalid package-relative path: $Relative"
    }
    $prefix = [IO.Path]::GetFullPath($Root).TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    $path = [IO.Path]::GetFullPath((Join-Path $Root $Relative))
    if (-not $path.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Package path escapes its directory: $Relative"
    }
    return $path
}
function Set-ModernAudioOrder([string]$Text) {
    $newline = [Environment]::NewLine
    $without = [regex]::Replace($Text, '(?im)^[ \t]*dk_modern_audio[ \t]*(?:;[^\r\n]*)?(?:\r?\n|$)', '')
    $section = [regex]::Match($without, '(?ims)^\[after_base\][^\r\n]*(?:\r?\n|$).*?(?=^\[|\z)')
    if ($section.Success) {
        $replacement = $section.Value.TrimEnd() + $newline + 'dk_modern_audio' + $newline + $newline
        return $without.Substring(0, $section.Index) + $replacement + $without.Substring($section.Index + $section.Length)
    }
    return '[after_base]' + $newline + 'dk_modern_audio' + $newline + $newline + $without
}
$manifestPath = Join-Path $PackageDirectory 'local-audio-mod.json'
if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
    throw 'The bundled Modern audio manifest is missing. Restore config/mods/dk_modern_audio or supply -PackageDirectory.'
}
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
if ($manifest.package -ne $modName -or $manifest.status -ne 'complete' -or $manifest.files.Count -ne 734 -or
    $manifest.coverage.speech -ne 125 -or $manifest.coverage.effect -ne 569 -or $manifest.coverage.campaign -ne 40) {
    throw 'The complete original English Modern audio v1 package is required.'
}
$seen = @{}
foreach ($entry in $manifest.files) {
    $asset = Get-AudioAssetPath $PackageDirectory $entry.file
    if ($seen.ContainsKey($entry.file)) { throw "Duplicate asset in manifest: $($entry.file)" }
    $seen[$entry.file] = $true
    if ((Get-AudioFileHash $asset) -ne $entry.sha256) { throw "Package checksum mismatch: $($entry.file)" }
}
foreach ($relative in @('fxdata/sounds.cfg', 'sounds.cfg', 'NOTICE.md')) {
    if (-not (Test-Path -LiteralPath (Join-Path $PackageDirectory $relative) -PathType Leaf)) {
        throw "Missing audio configuration: $relative"
    }
}
if (Test-Path -LiteralPath $target) {
    $item = Get-Item -LiteralPath $target -Force
    if (-not $item.PSIsContainer -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw 'Refusing a linked or non-directory mod target.'
    }
    $marker = Join-Path $target 'local-audio-mod.json'
    if (-not (Test-Path -LiteralPath $marker -PathType Leaf) -or
        (Get-Content -LiteralPath $marker -Raw | ConvertFrom-Json).package -ne $modName) {
        throw 'The existing mod folder is not this managed audio package.'
    }
}
$runtimeOrder = Join-Path $RuntimeDirectory 'mods/load_order.cfg'
$sourceOrder = Join-Path $sourceRoot 'config/mods/load_order.cfg'
$runtimeBefore = if (Test-Path -LiteralPath $runtimeOrder) { [IO.File]::ReadAllText($runtimeOrder) } else { '' }
$sourceBefore = if (Test-Path -LiteralPath $sourceOrder) { [IO.File]::ReadAllText($sourceOrder) } else { $runtimeBefore }
# A fresh checkout has only the factory audio default. Carry an existing
# runtime's visual mod choices back into source defaults before future syncs.
$sourceExtraMods = @($sourceBefore -split '\r?\n' | ForEach-Object {
    ($_ -split ';', 2)[0].Trim()
} | Where-Object { $_ -and $_ -ne $modName -and $_ -notmatch '^\[(after_base|after_campaign|after_map)\]$' })
if ($sourceExtraMods.Count -eq 0 -and $runtimeBefore.Trim()) { $sourceBefore = $runtimeBefore }
$runtimeEnabled = Set-ModernAudioOrder $runtimeBefore
$sourceEnabled = Set-ModernAudioOrder $sourceBefore
if (-not $PSCmdlet.ShouldProcess($RuntimeDirectory, 'Install 734 verified audio files and enable Modern audio in runtime and local source defaults')) { return }
if (Get-Process -Name keeperfx,keeperfx-local,keeperfx-local-hvlog,keeperfx-dx12,keeperfx-dx12-hvlog -ErrorAction SilentlyContinue) {
    throw 'Close KeeperFX before installing or changing its audio defaults.'
}
$backup = Join-Path $workspace ('artifacts/audio-mod/default-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
New-Item -ItemType Directory -Path $backup | Out-Null
if (Test-Path -LiteralPath $runtimeOrder) { Copy-Item -LiteralPath $runtimeOrder -Destination (Join-Path $backup 'runtime-load-order.before.cfg') }
if (Test-Path -LiteralPath $sourceOrder) { Copy-Item -LiteralPath $sourceOrder -Destination (Join-Path $backup 'source-load-order.before.cfg') }
$payload = @($manifest.files | ForEach-Object { $_.file }) + @('fxdata/sounds.cfg', 'sounds.cfg', 'local-audio-mod.json', 'NOTICE.md')
if (-not $PackageDirectory.Equals([IO.Path]::GetFullPath($target), [StringComparison]::OrdinalIgnoreCase)) {
    foreach ($relative in $payload) {
        $from = Get-AudioAssetPath $PackageDirectory $relative
        $to = Get-AudioAssetPath $target $relative
        New-Item -ItemType Directory -Path (Split-Path -Parent $to) -Force | Out-Null
        [IO.File]::Copy($from, $to, $true)
    }
}
foreach ($entry in $manifest.files) {
    if ((Get-AudioFileHash (Get-AudioAssetPath $target $entry.file)) -ne $entry.sha256) {
        throw "Installed checksum mismatch: $($entry.file)"
    }
}
New-Item -ItemType Directory -Path (Split-Path -Parent $sourceOrder) -Force | Out-Null
New-Item -ItemType Directory -Path (Split-Path -Parent $runtimeOrder) -Force | Out-Null
[IO.File]::WriteAllText($runtimeOrder, $runtimeEnabled)
[IO.File]::WriteAllText($sourceOrder, $sourceEnabled)
[ordered]@{package=$modName;files=734;tier='after_base';runtime=$RuntimeDirectory;sourceDefaults=$sourceOrder;originalBanksReplaced=$false} |
    ConvertTo-Json | Set-Content -LiteralPath (Join-Path $backup 'installed.json') -Encoding utf8
Write-Output "Modern audio is now the default for this runtime. Existing texture and creature mods are preserved."
Write-Output "Previous mod orders: $backup"
