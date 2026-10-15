# SPDX-License-Identifier: GPL-2.0-or-later
param(
    [string]$RuntimePath = (Join-Path (Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $PSScriptRoot))) 'runtime/keeperfx-x64'),
    [string]$OriginalDataPath,
    [string]$AssetsPath = (Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'assets/terrain/standard-hd'),
    [switch]$VerifyOnly
)
$ErrorActionPreference = 'Stop'
function Assert-GameClosed {
    if (Get-Process -Name keeperfx,keeperfx-local,keeperfx-local-hvlog,keeperfx-dx12,keeperfx-dx12-hvlog -ErrorAction SilentlyContinue) {
        throw 'Close KeeperFX before installing terrain. No game will be stopped.'
    }
}
function Get-BytesSha256([byte[]]$Bytes) {
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($sha.ComputeHash($Bytes))).Replace('-', '').ToLowerInvariant() }
    finally { $sha.Dispose() }
}
function Read-Regular([string]$Path) {
    $item = Get-Item -LiteralPath $Path -Force
    if ($item.PSIsContainer -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw "Expected an ordinary file: $Path" }
    return ,([IO.File]::ReadAllBytes($Path))
}
# Allocate only the declared, bounded decoded size; reject shorter or longer streams.
function Expand-TerrainPayload([byte[]]$Compressed, [int]$Length) {
    $inputStream = [IO.MemoryStream]::new($Compressed, $false)
    try {
        $gzip = [IO.Compression.GZipStream]::new($inputStream, [IO.Compression.CompressionMode]::Decompress)
        try {
            $decoded = New-Object byte[] $Length
            $offset = 0
            while ($offset -lt $Length) {
                $count = $gzip.Read($decoded, $offset, $Length - $offset)
                if ($count -eq 0) { throw 'Compressed terrain payload is shorter than declared.' }
                $offset += $count
            }
            if ($gzip.ReadByte() -ne -1) { throw 'Compressed terrain payload exceeds its declared length.' }
            return ,$decoded
        } finally { $gzip.Dispose() }
    } finally { $inputStream.Dispose() }
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

function Assert-Output([string]$Path) {
    $parent = Split-Path -Parent $Path
    if ($parent -notin @($fxdata,$mods) -or (Resolve-SelectedDirectory $parent) -ine $parent) { throw "Output directory changed: $Path" }
    if (Test-Path -LiteralPath $Path) { $null = Read-Regular $Path }
}
Assert-GameClosed
$RuntimePath = Resolve-SelectedDirectory $RuntimePath
$AssetsPath = Resolve-SelectedDirectory $AssetsPath
$fxdata = Resolve-SelectedDirectory (Join-Path $RuntimePath 'fxdata')
$mods = Resolve-SelectedDirectory (Join-Path $RuntimePath 'mods')
if (-not $OriginalDataPath) { $OriginalDataPath = Join-Path $RuntimePath 'data' }
$OriginalDataPath = Resolve-SelectedDirectory $OriginalDataPath
$manifestBytes = Read-Regular (Join-Path $AssetsPath 'manifest.json')
$manifest = [Text.Encoding]::UTF8.GetString($manifestBytes) | ConvertFrom-Json
if ($manifest.schema -ne 2 -or $manifest.id -ne 'dungeon-keeper-standard-terrain-4x-v1' -or
    $manifest.count -lt 1 -or $manifest.count -gt 1088 -or @($manifest.tiles).Count -ne $manifest.count -or
    $manifest.payload -cne 'terrain.index8.gz' -or $manifest.payload_compression -cne 'gzip' -or
    $manifest.payload_bytes -lt 1 -or $manifest.payload_bytes -gt 80MB -or
    $manifest.compressed_bytes -lt 1 -or $manifest.compressed_bytes -gt 80MB -or
    $manifest.compressed_sha256 -notmatch '^[a-f0-9]{64}$' -or $manifest.payload_sha256 -notmatch '^[a-f0-9]{64}$' -or
    $manifest.runtime_pack_sha256 -notmatch '^[a-f0-9]{64}$') { throw 'Unsupported terrain manifest.' }
$compressed = Read-Regular (Join-Path $AssetsPath 'terrain.index8.gz')
if ($compressed.Length -ne $manifest.compressed_bytes -or (Get-BytesSha256 $compressed) -ne $manifest.compressed_sha256) { throw 'Compressed terrain payload hash/length mismatch.' }
$payload = Expand-TerrainPayload $compressed ([int]$manifest.payload_bytes)
if ($payload.Length -ne $manifest.payload_bytes -or (Get-BytesSha256 $payload) -ne $manifest.payload_sha256) { throw 'Enhanced terrain payload hash/length mismatch.' }
$banks = @{}
foreach ($name in @('tmapa000.dat','tmapb000.dat')) {
    $expected = $manifest.native_files.$name
    $banks[$name] = [IO.File]::ReadAllBytes((Join-Path $OriginalDataPath $name))
    if ($banks[$name].Length -ne $expected.bytes -or (Get-BytesSha256 $banks[$name]) -ne $expected.sha256) { throw "Original STANDARD source mismatch: $name" }
}
if ((Get-BytesSha256 ([IO.File]::ReadAllBytes((Join-Path $OriginalDataPath 'palette.dat')))) -ne $manifest.palette_sha256) { throw 'Original palette mismatch.' }
$stream = New-Object IO.MemoryStream
$writer = New-Object IO.BinaryWriter($stream)
$ids = New-Object 'Collections.Generic.HashSet[int]'
$offset = 0
try {
    foreach ($word in @([uint32]0x5448464b,[uint32]2,[uint32]$manifest.count,[uint32]0)) { $writer.Write($word) }
    foreach ($tile in $manifest.tiles) {
        $id = [int]$tile.tile
        $dimension = [int]$tile.dimension
        if (-not $ids.Add($id) -or -not (($id -ge 0 -and $id -lt 544) -or ($id -ge 1000 -and $id -lt 1544)) -or
            $dimension -notin @(64,128,256) -or $tile.bytes -ne ($dimension*$dimension) -or $tile.offset -ne $offset -or
            $offset+$tile.bytes -gt $payload.Length -or $tile.png -cne ('tiles/{0:D4}.png' -f $id)) { throw 'Invalid terrain tile bounds or identity.' }
        $png = Read-Regular (Join-Path $AssetsPath $tile.png)
        if ((Get-BytesSha256 $png) -ne $tile.png_sha256) { throw "Enhanced PNG mismatch: $id" }
        $pixels = New-Object byte[] ([int]$tile.bytes)
        [Array]::Copy($payload,$offset,$pixels,0,$pixels.Length)
        if ((Get-BytesSha256 $pixels) -ne $tile.sha256) { throw "Enhanced tile mismatch: $id" }
        $bank = if ($id -lt 544) { $banks['tmapa000.dat'] } else { $banks['tmapb000.dat'] }
        $index = if ($id -lt 544) { $id } else { $id-1000 }
        if ($index -ge ($bank.Length/1024)) { throw "Native tile is outside the populated bank: $id" }
        $native = New-Object byte[] 1024
        $origin = [int]([Math]::Floor($index/8)*8192+($index%8)*32)
        for ($y=0;$y -lt 32;$y++) { [Array]::Copy($bank,($origin+$y*256),$native,($y*32),32) }
        if ((Get-BytesSha256 $native) -ne $tile.native_sha256) { throw "Original source tile mismatch: $id" }
        $writer.Write([uint32]$id);$writer.Write([uint32]$dimension);$writer.Write([uint32]0)
        $writer.Write($native);$writer.Write($pixels);$offset += $pixels.Length
    }
    $writer.Flush()
    $pack = $stream.ToArray()
} finally { $writer.Dispose();$stream.Dispose() }
if ($offset -ne $payload.Length -or $pack.Length -gt 80MB -or (Get-BytesSha256 $pack) -ne $manifest.runtime_pack_sha256) { throw 'Assembled terrain pack does not match its expected hash.' }
$target = Join-Path $fxdata 'standard-hd.kfht'
$receiptPath = Join-Path $fxdata 'standard-hd-install.json'
$receipt = [ordered]@{
    schema=1;kind='keeperfx-enhanced-terrain-install';id=$manifest.id
    manifest_sha256=(Get-BytesSha256 $manifestBytes);pack_sha256=$manifest.runtime_pack_sha256;tiles=$manifest.count
}
$receiptBytes = [Text.Encoding]::UTF8.GetBytes(($receipt | ConvertTo-Json) + [Environment]::NewLine)
$existingReceipt = $null
if (Test-Path -LiteralPath $receiptPath) {
    $existingReceipt = [Text.Encoding]::UTF8.GetString((Read-Regular $receiptPath)) | ConvertFrom-Json
    if ($existingReceipt.schema -ne 1 -or $existingReceipt.kind -ne $receipt.kind -or
        $existingReceipt.id -ne $manifest.id -or $existingReceipt.pack_sha256 -notmatch '^[a-f0-9]{64}$' -or
        $existingReceipt.manifest_sha256 -notmatch '^[a-f0-9]{64}$') { throw 'Existing terrain receipt is unrecognized or modified.' }
}
$existingPack = $null
if (Test-Path -LiteralPath $target) {
    $existingPack = Read-Regular $target
    $existingHash = Get-BytesSha256 $existingPack
    if ($existingHash -ne $manifest.runtime_pack_sha256) {
        $old = [Text.Encoding]::UTF8.GetString((Read-Regular $receiptPath)) | ConvertFrom-Json
        if ($old.schema -ne 1 -or $old.kind -ne $receipt.kind -or $old.id -ne $manifest.id -or $old.pack_sha256 -ne $existingHash) { throw 'Existing terrain pack is unrecognized or modified.' }
    }
}
if ($VerifyOnly) {
    if ($null -eq $existingPack -or (Get-BytesSha256 $existingPack) -ne $manifest.runtime_pack_sha256) { throw 'Installed terrain pack differs from this asset bundle.' }
    $installed = [Text.Encoding]::UTF8.GetString((Read-Regular $receiptPath)) | ConvertFrom-Json
    if ($installed.schema -ne 1 -or $installed.tiles -ne $receipt.tiles -or $installed.kind -ne $receipt.kind -or $installed.id -ne $receipt.id -or $installed.manifest_sha256 -ne $receipt.manifest_sha256 -or $installed.pack_sha256 -ne $receipt.pack_sha256) { throw 'Installed terrain receipt does not match.' }
    Write-Host ("Verified {0} enhanced terrain tiles." -f $manifest.count)
    return
}
$changes = [ordered]@{}
$changes[$target] = $pack
$changes[$receiptPath] = $receiptBytes
$orderPath = Join-Path $mods 'load_order.cfg'
if (Test-Path -LiteralPath $orderPath) {
    $orderBytes = Read-Regular $orderPath
    $order = [Text.Encoding]::UTF8.GetString($orderBytes)
    $enabled = [regex]::Replace($order,'(?im)^[\t ]*keeper_enhanced_default[\t ]*(?=\r?$)','; Original HD terrain: keeper_enhanced_default disabled')
    if ($enabled -cne $order) { $changes[$orderPath] = [Text.Encoding]::UTF8.GetBytes($enabled) }
}
$oldBytes = @{};$committed = @();$stamp = [guid]::NewGuid().ToString('N')
foreach ($path in $changes.Keys) { Assert-Output $path;$oldBytes[$path] = if (Test-Path -LiteralPath $path) { Read-Regular $path } else { $null } }
Assert-GameClosed
try {
    foreach ($path in $changes.Keys) {
        Assert-Output $path
        $current = if (Test-Path -LiteralPath $path) { Read-Regular $path } else { $null }
        if (($null -eq $current) -ne ($null -eq $oldBytes[$path]) -or ($null -ne $current -and (Get-BytesSha256 $current) -ne (Get-BytesSha256 $oldBytes[$path]))) { throw "File changed during installation: $path" }
        if ($null -ne $current -and (Get-BytesSha256 $current) -eq (Get-BytesSha256 $changes[$path])) { continue }
        $staged = $path+'.'+$stamp+'.tmp';$backup=$path+'.'+$stamp+'.bak'
        Assert-Output $staged;Assert-Output $backup
        $file = [IO.File]::Open($staged,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None)
        try { $file.Write($changes[$path],0,$changes[$path].Length) } finally { $file.Dispose() }
        if ($null -ne $current) { [IO.File]::Replace($staged,$path,$backup) } else { [IO.File]::Move($staged,$path) }
        $committed += $path
    }
} catch {
    foreach ($path in $committed) {
        Assert-Output $path
        if ((Get-BytesSha256 (Read-Regular $path)) -ne (Get-BytesSha256 $changes[$path])) { throw "Installation interrupted and file changed; backups retained: $path" }
        if ($null -eq $oldBytes[$path]) { Remove-Item -LiteralPath $path }
        else { [IO.File]::WriteAllBytes($path,$oldBytes[$path]) }
    }
    throw
}
Write-Host ("Installed {0} enhanced terrain tiles: {1}" -f $manifest.count,$target)
