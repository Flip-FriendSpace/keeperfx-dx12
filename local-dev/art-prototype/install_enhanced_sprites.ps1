# SPDX-License-Identifier: GPL-2.0-or-later
param(
    [string]$RuntimePath = (Join-Path (Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $PSScriptRoot))) 'runtime/keeperfx-x64'),
    [string]$OriginalDataPath,
    [string]$PythonExecutable = 'python'
)
$ErrorActionPreference = 'Stop'
function Assert-GameClosed {
    if (Get-Process -Name keeperfx,keeperfx-local,keeperfx-local-hvlog,keeperfx-dx12,keeperfx-dx12-hvlog -ErrorAction SilentlyContinue) {
        throw 'Close KeeperFX before installing enhanced creature sprites. No game will be stopped.'
    }
}
function Assert-Unlinked([string]$Path) {
    if ((Get-Item -LiteralPath $Path -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Refusing linked path: $Path" }
}
function Resolve-SelectedDirectory([string]$Path) {
    $resolved = [IO.Path]::GetFullPath($Path).TrimEnd('\', '/')
    for ($hop = 0; $hop -lt 32; $hop++) {
        $ancestors = @()
        $candidate = $resolved
        while ($candidate) {
            $ancestors = @($candidate) + $ancestors
            $parent = Split-Path -Parent $candidate
            if ($parent -eq $candidate) { break }
            $candidate = $parent
        }
        $changed = $false
        foreach ($candidate in $ancestors) {
            $item = Get-Item -LiteralPath $candidate -Force
            if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) {
                $targets = @($item.Target)
                if ($targets.Count -ne 1 -or -not $targets[0]) { throw "Cannot resolve selected directory link: $candidate" }
                $destination = [string]$targets[0]
                if (-not [IO.Path]::IsPathRooted($destination)) { $destination = Join-Path (Split-Path -Parent $candidate) $destination }
                $suffix = $resolved.Substring($candidate.Length).TrimStart('\', '/')
                $resolved = [IO.Path]::GetFullPath((Join-Path $destination $suffix)).TrimEnd('\', '/')
                $changed = $true
                break
            }
        }
        if (-not $changed) {
            if (-not (Test-Path -LiteralPath $resolved -PathType Container)) { throw "Not a directory: $resolved" }
            return $resolved
        }
    }
    throw 'Selected directory contains too many nested links or a link cycle.'
}
function Assert-InstallChild([string]$Path) {
    # Resolve the selected root again immediately before each mutation. Every
    # installation path is one direct child of that physical root.
    if ((Resolve-SelectedDirectory $mods) -ine $mods) { throw 'Physical mod root changed during installation.' }
    $absolute = [IO.Path]::GetFullPath($Path)
    if ((Split-Path -Parent $absolute) -ine $mods) { throw "Installation path escapes physical mod root: $Path" }
    if (Test-Path -LiteralPath $absolute) { Assert-Unlinked $absolute }
}
function Move-InstallChild([string]$From, [string]$To) {
    Assert-InstallChild $From
    Assert-InstallChild $To
    Move-Item -LiteralPath $From -Destination $To
}
function Get-InstallSha256([string]$Path) {
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        $stream = [IO.File]::OpenRead($Path)
        try { return ([BitConverter]::ToString($sha.ComputeHash($stream))).Replace('-', '').ToLowerInvariant() }
        finally { $stream.Dispose() }
    } finally { $sha.Dispose() }
}
function Assert-OwnedInstall([string]$Root) {
    Assert-Unlinked $Root
    $receiptPath = Join-Path $Root 'install-receipt.json'
    Assert-Unlinked $receiptPath
    $receipt = Get-Content -LiteralPath $receiptPath -Raw | ConvertFrom-Json
    if ($receipt.schema -ne 1 -or $receipt.kind -ne 'keeperfx-enhanced-sprites-install' -or
        $receipt.id -ne 'original-heroes-and-imp-4x-v1' -or $receipt.manifest_sha256 -notmatch '^[a-f0-9]{64}$') {
        throw 'Existing mod is not a recognized enhanced creature installation.'
    }
    $files = @($receipt.files.PSObject.Properties)
    if ($files.Count -ne 28) { throw 'Expected fourteen sprite ZIPs and fourteen sprite configs in the receipt.' }
    $expected = @('install-receipt.json')
    foreach ($file in $files) {
        if ($file.Name -cnotmatch '^(fxdata/[a-z][a-z0-9_-]*-sprites\.zip|creatrs/[a-z][a-z0-9_-]*\.cfg)$' -or
            $file.Value -notmatch '^[a-f0-9]{64}$') { throw 'Invalid installed file receipt.' }
        $path = Join-Path $Root $file.Name
        Assert-Unlinked $path
        if ((Get-InstallSha256 $path) -ne $file.Value) { throw "Installed file was modified: $($file.Name)" }
        $expected += $file.Name
    }
    foreach ($item in Get-ChildItem -LiteralPath $Root -Recurse -Force) {
        Assert-Unlinked $item.FullName
        if (-not $item.PSIsContainer -and $item.FullName.Substring($Root.Length + 1).Replace('\', '/') -notin $expected) {
            throw "Unexpected file in installed mod: $($item.FullName)"
        }
    }
}
function Get-EnhancedLoadOrder([string]$Order) {
    $heroes = 'wizard barbarian archer monk dwarfa knight avatar tunneller witch giant fairy thief samurai'.Split(' ')
    $names = @('enhanced_creatures', 'heroes_art_prototype', 'imp_art_prototype') + @($heroes | ForEach-Object { 'hero_' + $_ + '_art_prototype' })
    $alternatives = ($names | ForEach-Object { [regex]::Escape($_) }) -join '|'
    $filtered = [regex]::Replace($Order, ('(?im)^[ \t]*(?:' + $alternatives + ')[ \t]*(?:\r?\n|$)'), '')
    $section = [regex]::Match($filtered, '(?im)^[ \t]*\[after_map\][ \t]*\r?$')
    if (-not $section.Success) { return $filtered.TrimEnd() + "`r`n[after_map]`r`nenhanced_creatures`r`n" }
    $afterHeader = $section.Index + $section.Length
    $next = [regex]::Match($filtered.Substring($afterHeader), '(?m)^[ \t]*\[[^\r\n]+\]')
    $insertAt = if ($next.Success) { $afterHeader + $next.Index } else { $filtered.Length }
    return $filtered.Substring(0, $insertAt).TrimEnd() + "`r`nenhanced_creatures`r`n" + $filtered.Substring($insertAt)
}
Assert-GameClosed
$RuntimePath = Resolve-SelectedDirectory $RuntimePath
if (-not $OriginalDataPath) { $OriginalDataPath = Join-Path $RuntimePath 'data' }
$OriginalDataPath = [IO.Path]::GetFullPath($OriginalDataPath)
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$assets = Join-Path $repo 'assets/creatures'
$core = Join-Path $PSScriptRoot 'install_enhanced_sprites.py'
$python = (Get-Command $PythonExecutable -CommandType Application -ErrorAction Stop).Source
$mods = Resolve-SelectedDirectory (Join-Path $RuntimePath 'mods')
$orderPath = Join-Path $mods 'load_order.cfg'
$target = Join-Path $mods 'enhanced_creatures'
Assert-Unlinked $orderPath
if (Test-Path -LiteralPath $target) { Assert-OwnedInstall $target }
$original = [IO.File]::ReadAllBytes($orderPath)
$enabled = Get-EnhancedLoadOrder ([IO.File]::ReadAllText($orderPath))
$id = (Get-Date -Format 'yyyyMMdd-HHmmss-fff') + '-' + [guid]::NewGuid().ToString('N').Substring(0, 8)
$stage = Join-Path $mods ('.enhanced-creatures-stage-' + $id)
$backup = Join-Path $mods ('enhanced-creatures-backup-' + $id)
$orderBackup = Join-Path $mods ('enhanced-creatures-load-order-' + $id + '.cfg')
Assert-InstallChild $stage
& $python $core --assets $assets --data $OriginalDataPath --output $stage
if ($LASTEXITCODE -ne 0) { throw "Sprite assembly failed. Any staging evidence remains at $stage" }
& $python $core --assets $assets --output $stage --verify-only
if ($LASTEXITCODE -ne 0) { throw "Staged sprite verification failed: $stage" }
Assert-OwnedInstall $stage
Assert-GameClosed
# Recheck immediately before replacing anything; preserve intervening user changes.
if ([Convert]::ToBase64String([IO.File]::ReadAllBytes($orderPath)) -ne [Convert]::ToBase64String($original)) { throw 'Mod order changed during assembly; installation was not activated.' }
if (Test-Path -LiteralPath $target) { Assert-OwnedInstall $target }
Assert-InstallChild $orderBackup
[IO.File]::WriteAllBytes($orderBackup, $original)
$movedOld = $false
$movedNew = $false
try {
    if (Test-Path -LiteralPath $target) { Move-InstallChild $target $backup; $movedOld = $true }
    Move-InstallChild $stage $target
    $movedNew = $true
    Assert-InstallChild $orderPath
    [IO.File]::WriteAllText($orderPath, $enabled, (New-Object Text.UTF8Encoding($false)))
} catch {
    Assert-InstallChild $orderPath
    [IO.File]::WriteAllBytes($orderPath, $original)
    if ($movedNew) { Move-InstallChild $target $stage }
    if ($movedOld) { Move-InstallChild $backup $target }
    throw
}
Write-Output "Enhanced creature sprites installed and enabled: $target"
Write-Output "Previous mod order: $orderBackup"
if ($movedOld) { Write-Output "Previous verified mod retained: $backup" }
Write-Output 'Start KeeperFX normally. Original fallback packages stay local; API settings are unchanged.'
