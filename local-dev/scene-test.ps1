# SPDX-License-Identifier: GPL-2.0-or-later
# KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
param(
  [ValidateSet('x86', 'x64')][string]$Architecture = 'x64',
  [ValidateSet('OPENGL', 'SOFTWARE')][string]$Renderer = 'OPENGL',
  [switch]$UnitOnly,
  [switch]$SkipUnit,
  [switch]$CaptureAssets
)
$ErrorActionPreference = 'Stop'
$workspace = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not $SkipUnit) {
  $script = (& wsl.exe -d keeperfx-dev --exec wslpath -a (Join-Path $PSScriptRoot 'scene-test.sh')).Trim()
  if ($LASTEXITCODE -ne 0) { throw 'keeperfx-dev is unavailable.' }
  & wsl.exe -d keeperfx-dev --exec bash $script
  if ($LASTEXITCODE -ne 0) { throw 'Scene test compilation failed.' }
  foreach ($arch in @('i686', 'x86_64')) {
    & (Join-Path $workspace "artifacts/scene/scene-$arch.exe")
    if ($LASTEXITCODE -ne 0) { throw "Scene tests failed: $arch" }
  }
}
if ($UnitOnly) { return }
if (Get-Process -Name keeperfx,keeperfx-local,keeperfx-local-hvlog -ErrorAction SilentlyContinue) {
  throw 'Close KeeperFX before the live scene test.'
}
$runtime = Join-Path $workspace 'runtime/keeperfx'
if ($Architecture -eq 'x64') { $runtime += '-x64' }
$output = Join-Path $workspace ('artifacts/scene/live-' + $Architecture + '-' + $Renderer + '-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
New-Item -ItemType Directory -Path $output -Force | Out-Null
$testSave = Join-Path $runtime 'save/fx1g9997.sav'
if (Test-Path -LiteralPath $testSave) { throw 'Save slot 9997 is occupied.' }
$backups = @{}
foreach ($relative in @('save/fx1lastf.sav', 'save/settings.toml')) {
  $path = Join-Path $runtime $relative
  $backups[$path] = if (Test-Path -LiteralPath $path) { [IO.File]::ReadAllBytes($path) } else { $null }
}
$originalConfig = Join-Path $runtime 'keeperfx.cfg'
$prefsPath = Join-Path $env:APPDATA 'keeperfx/keeperfx/renderer_prefs.ini'
$configHash = (Get-FileHash -LiteralPath $originalConfig).Hash
$prefsHash = (Get-FileHash -LiteralPath $prefsPath).Hash
$config = Get-Content -LiteralPath $originalConfig -Raw
$settings = @{ API_ENABLED='TRUE'; API_PORT='5599'; AUTOMATIC_REPLAYS='OFF'; FREEZE_GAME_ON_FOCUS_LOST='OFF'; RENDERER=$Renderer; CAPTURE_CURSOR='OFF'; CURSOR_EDGE_CAMERA_PANNING='OFF'; VSYNC='OFF'; INGAME_RES='3840x2160w32' }
foreach ($key in $settings.Keys) { $config = $config -replace ('(?m)^' + $key + '=.*$'), ($key + '=' + $settings[$key]) }
[IO.File]::WriteAllText((Join-Path $runtime 'keeperfx-scene-test.cfg'), $config)
$oldScene = $env:KFX_SCENE_TEST_DIR
$oldState = $env:KFX_STATE_TEST
$oldStateDir = $env:KFX_STATE_TEST_DIR
$process = $null
try {
  $env:KFX_SCENE_TEST_DIR = $output
  $env:KFX_STATE_TEST = '1'
  $env:KFX_STATE_TEST_DIR = $output
  $process = Start-Process -FilePath (Join-Path $runtime 'keeperfx-local.exe') -WorkingDirectory $runtime -WindowStyle Hidden -PassThru -ArgumentList '-config keeperfx-scene-test.cfg -nointro -skipheartzoom -campaign keeporig -level 1 -nosound'
  $ready = $false
  for ($attempt = 0; $attempt -lt 60; $attempt++) {
    if ($process.HasExited) { throw 'KeeperFX exited before connection.' }
    $client = New-Object Net.Sockets.TcpClient
    try { $client.Connect('127.0.0.1', 5599); $ready = $true } catch {} finally { $client.Dispose() }
    if ($ready) { break }
    Start-Sleep -Milliseconds 500
  }
  if (-not $ready) { throw 'Scene test API unavailable.' }
  Start-Sleep -Seconds 3
  $nodeArgs = @((Join-Path $PSScriptRoot 'scene-live-test.mjs'), $output)
  if ($CaptureAssets) { $nodeArgs += '--assets' }
  & node @nodeArgs
  if ($LASTEXITCODE -ne 0) { throw 'Live scene tests failed.' }
  if (-not $process.WaitForExit(10000) -or $process.ExitCode -ne 0) { throw 'KeeperFX did not exit cleanly.' }
} finally {
  if ($process -and -not $process.HasExited) { Stop-Process -Id $process.Id }
  $env:KFX_SCENE_TEST_DIR = $oldScene
  $env:KFX_STATE_TEST = $oldState
  $env:KFX_STATE_TEST_DIR = $oldStateDir
  Copy-Item -LiteralPath (Join-Path $runtime 'keeperfx.log') -Destination (Join-Path $output 'keeperfx.log')
  if (Test-Path -LiteralPath $testSave) { Remove-Item -LiteralPath $testSave }
  foreach ($path in $backups.Keys) {
    if ($null -ne $backups[$path]) { [IO.File]::WriteAllBytes($path, $backups[$path]) }
    elseif (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path }
  }
  if ((Get-FileHash -LiteralPath $originalConfig).Hash -ne $configHash) { throw 'Normal config changed.' }
  if ((Get-FileHash -LiteralPath $prefsPath).Hash -ne $prefsHash) { throw 'Renderer preferences changed.' }
  Write-Output "Scene test evidence: $output"
}
