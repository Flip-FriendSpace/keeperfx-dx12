# SPDX-License-Identifier: GPL-2.0-or-later
# KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
param(
  [ValidateSet('x86', 'x64')][string]$Architecture = 'x64',
  [switch]$Upstream,
  [switch]$HeavyLog,
  [switch]$QuickStart,
  [switch]$DX12Preview,
  [switch]$OpenGLFallback,
  [string]$HdTerrainPack,
  [switch]$NativeTerrain,
  [switch]$PassThru,
  [switch]$Hidden
)
$ErrorActionPreference = 'Stop'
# Packaged Windows PowerShell launches may inherit pwsh's module path, leaving Get-FileHash unavailable.
function Get-PreviewFileSha256([string]$Path) {
  $algorithm = [Security.Cryptography.SHA256]::Create()
  try {
    $stream = [IO.File]::OpenRead($Path)
    try { return ([BitConverter]::ToString($algorithm.ComputeHash($stream))).Replace('-', '') }
    finally { $stream.Dispose() }
  } finally { $algorithm.Dispose() }
}
$workspace = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if ($OpenGLFallback -and -not $DX12Preview) { throw '-OpenGLFallback requires -DX12Preview.' }
if ($NativeTerrain -and $HdTerrainPack) { throw '-NativeTerrain cannot be combined with -HdTerrainPack.' }
if ($HdTerrainPack) {
  if (-not $DX12Preview -or $OpenGLFallback) { throw '-HdTerrainPack requires the DX12 preview.' }
  $HdTerrainPack = [IO.Path]::GetFullPath($HdTerrainPack)
  if (-not (Test-Path -LiteralPath $HdTerrainPack -PathType Leaf)) { throw 'The HD terrain pack is missing.' }
  $packBytes = (Get-Item -LiteralPath $HdTerrainPack).Length
  if ($packBytes -lt 16 -or $packBytes -gt 80MB) { throw 'The HD terrain pack exceeds its file bounds.' }
  $packStream = [IO.File]::OpenRead($HdTerrainPack)
  try {
    $packHeader = New-Object byte[] 16
    if ($packStream.Read($packHeader, 0, 16) -ne 16) { throw 'The HD terrain pack header is incomplete.' }
    $packVersion = [BitConverter]::ToUInt32($packHeader, 4)
    $packCount = [BitConverter]::ToUInt32($packHeader, 8)
    if ([BitConverter]::ToUInt32($packHeader, 0) -ne 0x5448464b -or [BitConverter]::ToUInt32($packHeader, 12) -ne 0 -or $packVersion -notin @(1,2) -or $packCount -eq 0 -or $packCount -gt 1088 -or ($packVersion -eq 1 -and ($packBytes -gt 8MB -or $packCount -gt 64))) { throw 'The HD terrain pack header is invalid.' }
  } finally { $packStream.Dispose() }
}
if ($DX12Preview -and ($Architecture -ne 'x64' -or $Upstream -or $HeavyLog)) {
  throw 'The DX12 preview requires x64 and cannot be combined with -Upstream or -HeavyLog.'
}
if (Get-Process -Name keeperfx,keeperfx-local,keeperfx-local-hvlog,keeperfx-dx12,keeperfx-dx12-hvlog -ErrorAction SilentlyContinue) {
  throw 'Close KeeperFX before launching another instance; no existing process will be stopped.'
}
if ($Upstream) { $Architecture = 'x86' }
$runtime = Join-Path $workspace 'runtime/keeperfx'
if ($Architecture -eq 'x64') {
  $runtime += '-x64'
}
if ($DX12Preview) { $name = 'keeperfx-dx12.exe' }
elseif ($Upstream) { $name = 'keeperfx.exe' }
elseif ($HeavyLog) { $name = 'keeperfx-local-hvlog.exe' }
else { $name = 'keeperfx-local.exe' }
$executable = Join-Path $runtime $name
if (-not (Test-Path -LiteralPath $executable)) {
  if ($DX12Preview) { throw "Missing $executable. Run local-dev/build.ps1 -Architecture x64 -DX12 first." }
  throw "Missing $executable. Run Build KeeperFX.cmd first."
}
$arguments = @('-nointro', '-skipheartzoom')
if ($QuickStart) { $arguments += @('-campaign', 'keeporig', '-level', '1') }
if (-not $DX12Preview) {
  $launchWindowStyle = if ($Hidden) { 'Hidden' } else { 'Normal' }
  $process = Start-Process -FilePath $executable -WorkingDirectory $runtime -ArgumentList $arguments -WindowStyle $launchWindowStyle -PassThru
  if ($PassThru) { return $process }
  $process.Dispose()
  return
}
$required = @('SDL3.dll', 'SDL3_image.dll', 'SDL3_mixer.dll')
if (-not $OpenGLFallback) {
  $required += 'D3D12/D3D12Core.dll'
  $required += @('textured.vs','textured.ps','outline.vs','outline.ps',
    'enhanced-world-terrain.vs','enhanced-world-terrain.ps','enhanced-world-shadow.vs','enhanced-world-shadow.ps',
    'world-overlay.vs','world-overlay.ps','lens.vs','lens.ps','mapfade.ps',
    'runtime-indexed-present.cs','runtime-clear.vs','runtime-clear.ps','runtime-present.ps') | ForEach-Object { "shaders/$_.dxil" }
}
foreach ($relative in $required) {
  if (-not (Test-Path -LiteralPath (Join-Path $runtime $relative) -PathType Leaf)) {
    throw "Missing preview dependency $relative. Run local-dev/build.ps1 -Architecture x64 -DX12."
  }
}
if ((Get-Item -LiteralPath $runtime -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Refusing a linked preview runtime directory.' }
$logDirectory = Join-Path $runtime 'preview-logs'
$logBackend = if ($OpenGLFallback) { 'OPENGL' } else { 'DX12' }
$logStamp = [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss-fff') + '-' + [Guid]::NewGuid().ToString('N').Substring(0,12)
$relativeLog = 'preview-logs/' + $logBackend + '-' + $logStamp + '.log'
$previewLog = [IO.Path]::GetFullPath((Join-Path $runtime $relativeLog))
# Native DISKPATH_SIZE is 144 bytes, including the terminator. Leave spare room.
$nativeLogBytes = [Text.Encoding]::UTF8.GetByteCount($previewLog)
if ($nativeLogBytes -gt 140) { throw "Preview log path is $nativeLogBytes UTF-8 bytes; the engine's 144-byte path buffer requires a conservative maximum of 140. Use a shorter local workspace/runtime path before launching. Target: $previewLog" }
if (Test-Path -LiteralPath $logDirectory) {
  $directoryItem = Get-Item -LiteralPath $logDirectory -Force
  if (-not $directoryItem.PSIsContainer -or ($directoryItem.Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw 'Refusing a linked or non-directory preview log target.' }
} else {
  New-Item -ItemType Directory -Path $logDirectory | Out-Null
}
if (Test-Path -LiteralPath $previewLog) { throw 'Preview log collision; refusing to replace an existing file.' }
# Reserve a fresh ordinary file. Native logging may truncate only this launch's file.
$logReservation = [IO.File]::Open($previewLog, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
$logReservation.Dispose()
$defaultLog = Join-Path $runtime 'keeperfx.log'
$preservedLog = $null
if (Test-Path -LiteralPath $defaultLog) {
  $defaultItem = Get-Item -LiteralPath $defaultLog -Force
  if ($defaultItem.PSIsContainer -or ($defaultItem.Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw 'Refusing a linked or non-file keeperfx.log.' }
  $preservedLog = Join-Path $logDirectory ('previous-keeperfx-' + $logStamp + '.log')
  $previousHash = (Get-PreviewFileSha256 -Path $defaultLog)
  [IO.File]::Copy($defaultLog, $preservedLog, $false)
  if ((Get-PreviewFileSha256 -Path $preservedLog) -ne $previousHash -or (Get-PreviewFileSha256 -Path $defaultLog) -ne $previousHash) { throw 'keeperfx.log changed while preserving its diagnostic copy; preview not launched.' }
}
$arguments += @('-log', $relativeLog)
$previewName = 'keeperfx-dx12-preview.cfg'
$previewPath = Join-Path $runtime $previewName
$marker = '; KeeperFX local DX12 preview launcher configuration'
if (Test-Path -LiteralPath $previewPath) {
  if ((Get-Item -LiteralPath $previewPath).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Refusing a linked preview config.' }
  $config = [IO.File]::ReadAllText($previewPath)
  if (-not $config.StartsWith($marker)) { throw "Existing $previewName is not launcher-owned; rename it before first preview launch." }
} else {
  $config = $marker + "`r`n" + [IO.File]::ReadAllText((Join-Path $runtime 'keeperfx.cfg'))
}
# Keep all other preview preferences; these three safety/default keys belong to the launcher.
foreach ($pair in @{RENDERER=$logBackend;API_ENABLED='FALSE';REPLAY_MAX_SIZE='131072'}.GetEnumerator()) {
  $pattern = '(?im)^\s*' + $pair.Key + '\s*=.*$'
  if ($config -match $pattern) { $config = [regex]::Replace($config, $pattern, ($pair.Key + '=' + $pair.Value)) }
  else { $config += "`r`n$($pair.Key)=$($pair.Value)`r`n" }
}
[IO.File]::WriteAllText($previewPath, $config, (New-Object Text.UTF8Encoding($false)))
$arguments += @('-config', $previewName)
if ($OpenGLFallback) { $arguments += '-opengl' }
$start = New-Object Diagnostics.ProcessStartInfo
$start.FileName = $executable
$start.WorkingDirectory = $runtime
$start.Arguments = $arguments -join ' '
$start.UseShellExecute = $false
$start.WindowStyle = if ($Hidden) { [Diagnostics.ProcessWindowStyle]::Hidden } else { [Diagnostics.ProcessWindowStyle]::Normal }
foreach ($key in @($start.EnvironmentVariables.Keys)) {
  if ($key -like 'KFX_*') { $start.EnvironmentVariables.Remove($key) }
}
# This disables validation only. Benchmark capture requires a separate directory, absent here.
$start.EnvironmentVariables['KFX_DX12_PERFORMANCE'] = '1'
if ($NativeTerrain) { $start.EnvironmentVariables['KFX_HD_TERRAIN_PACK'] = 'off' }
elseif ($HdTerrainPack) { $start.EnvironmentVariables['KFX_HD_TERRAIN_PACK'] = $HdTerrainPack }
Write-Host 'Single-player preview. Save regularly; a renderer failure does not recover unsaved progress.'
Write-Host "Preview diagnostic log: $previewLog"
if ($preservedLog) { Write-Host "Previous default log preserved: $preservedLog" }
Write-Host 'For a manual OpenGL restart: Play KeeperFX DX12 Preview OpenGL.cmd'
$process = [Diagnostics.Process]::Start($start)
if ($null -eq $process) { throw 'KeeperFX preview did not start.' }
if ($PassThru) { return $process }
$process.Dispose()
