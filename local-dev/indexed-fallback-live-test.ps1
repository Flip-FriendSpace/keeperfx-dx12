# SPDX-License-Identifier: GPL-2.0-or-later
# KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
param([ValidateSet('x86','x64')][string]$Architecture = 'x64', [ValidateSet('OPENGL','DX12')][string]$Renderer = 'OPENGL', [string]$PythonExecutable)
$ErrorActionPreference = 'Stop'
if ($Renderer -eq 'DX12' -and $Architecture -ne 'x64') { throw 'DX12 requires the local Windows x64 executable.' }
if ($Renderer -eq 'DX12' -and -not $PythonExecutable) {
  $PythonExecutable = (Get-Command python -ErrorAction Stop).Source
}
$workspace = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$runtime = Join-Path $workspace $(if ($Architecture -eq 'x64') { 'runtime/keeperfx-x64' } else { 'runtime/keeperfx' })
$executable = Join-Path $runtime $(if ($Renderer -eq 'DX12') { 'keeperfx-dx12.exe' } else { 'keeperfx-local.exe' })
if (Get-Process -Name keeperfx,keeperfx-local,keeperfx-local-hvlog,keeperfx-dx12,keeperfx-dx12-hvlog -ErrorAction SilentlyContinue) { throw 'Close KeeperFX before this focused live check.' }
$output = Join-Path $workspace ('artifacts/indexed-fallback-live/' + $Renderer + '-' + $Architecture + '-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
New-Item -ItemType Directory -Path $output | Out-Null
$configPath = Join-Path $runtime 'keeperfx.cfg'
$testConfigPath = Join-Path $runtime 'keeperfx-indexed-fallback-test.cfg'
$prefsPath = Join-Path $env:APPDATA 'keeperfx/keeperfx/renderer_prefs.ini'
$backups = @{}
foreach ($path in @($configPath,$testConfigPath,$prefsPath,(Join-Path $runtime 'save/settings.toml'),(Join-Path $runtime 'save/fx1lastf.sav'))) {
  $backups[$path] = if (Test-Path -LiteralPath $path) { [IO.File]::ReadAllBytes($path) } else { $null }
}
$oldScene = $env:KFX_SCENE_TEST_DIR
$oldReference = $env:KFX_SCENE_REFERENCE
$game = $null
$client = $null
$passed = $false
$dx12Validation = $null
try {
  $config = Get-Content -LiteralPath $configPath -Raw
  $settings = @{ API_ENABLED='TRUE'; API_PORT='5599'; AUTOMATIC_REPLAYS='OFF'; FREEZE_GAME_ON_FOCUS_LOST='OFF'; RENDERER=$Renderer; CAPTURE_CURSOR='OFF'; CURSOR_EDGE_CAMERA_PANNING='OFF'; VSYNC='OFF'; INGAME_RES='3840x2160w32' }
  foreach ($key in $settings.Keys) { $config = $config -replace ('(?m)^' + $key + '=.*$'), ($key + '=' + $settings[$key]) }
  [IO.File]::WriteAllText($testConfigPath,$config)
  $env:KFX_SCENE_TEST_DIR = $output
  $env:KFX_SCENE_REFERENCE = '1'
  foreach ($file in @($PSCommandPath,(Join-Path $PSScriptRoot 'indexed-fallback-live-test.mjs'),(Join-Path $PSScriptRoot 'reference-fingerprint.mjs'))) { Copy-Item -LiteralPath $file -Destination $output }
  if ($Renderer -eq 'DX12') { Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'indexed-fallback-pixels.py') -Destination $output }
  @{executable=$executable;executableSha256=(Get-FileHash $executable).Hash;renderer=$Renderer;architecture=$Architecture;apiAddress='127.0.0.1';scope='Three live custom lens callbacks; diagnostic snapshots are last completed outputs, not screenshot pairs'} | ConvertTo-Json | Set-Content (Join-Path $output 'run.json') -Encoding utf8
  $game = Start-Process -FilePath $executable -WorkingDirectory $runtime -WindowStyle Hidden -PassThru -ArgumentList '-config keeperfx-indexed-fallback-test.cfg -nointro -skipheartzoom -campaign keeporig -level 1 -nosound -alex'
  Write-Output "Live game PID $($game.Id); evidence $output"
  $ready = $false
  for ($attempt=0;$attempt -lt 60;$attempt++) {
    if ($game.HasExited) { throw "KeeperFX exited during startup ($($game.ExitCode))." }
    foreach ($listener in @(Get-NetTCPConnection -State Listen -ErrorAction Stop | Where-Object LocalPort -EQ 5599)) {
      if ($listener.OwningProcess -ne $game.Id -or $listener.LocalAddress -ne '127.0.0.1') { throw 'API listener is not the launched game on loopback.' }
      $ready = $true
    }
    if ($ready) { break }
    Start-Sleep -Milliseconds 500
  }
  if (-not $ready) { throw 'Owned loopback API listener did not start within 30 seconds.' }
  $arguments = @(('"'+(Join-Path $PSScriptRoot 'indexed-fallback-live-test.mjs')+'"'),('"'+$output+'"'),('"'+(Join-Path $workspace 'artifacts/indexed-fallback-codec')+'"'),$Renderer)
  if ($Renderer -eq 'DX12') { $arguments += ('"'+$PythonExecutable+'"') }
  $client = Start-Process -FilePath (Get-Command node).Source -ArgumentList $arguments -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $output 'client.stdout.log') -RedirectStandardError (Join-Path $output 'client.stderr.log')
  $timer = [Diagnostics.Stopwatch]::StartNew()
  while (-not $client.WaitForExit(100)) {
    if ($game.HasExited -and (-not $client.WaitForExit(1000) -or $client.ExitCode -ne 0)) { throw 'Game exited before successful callback verification.' }
    if ($timer.Elapsed.TotalSeconds -gt 180) { throw 'Focused callback client exceeded 180 seconds.' }
  }
  $client.WaitForExit()
  if ($client.ExitCode -ne 0) { throw "Live callback verification failed; inspect $output/client.stderr.log." }
  if (-not $game.WaitForExit(10000) -or $game.ExitCode -ne 0) { throw 'KeeperFX did not quit cleanly.' }
  if ($Renderer -eq 'DX12') {
    $gameLog = Get-Content -LiteralPath (Join-Path $runtime 'keeperfx.log') -Raw
    $sessions = [regex]::Matches($gameLog, 'DX12 session: submitted (?<submitted>[0-9]+), rejected (?<rejected>[0-9]+)\b')
    $errors = [regex]::Matches($gameLog, '(?im)^.*(?:\bError\b[^\r\n]*\bDX12\b|\bDX12\b[^\r\n]*\bError\b|DX12[^\r\n]*(?:\bstopped\b|\binitialization failed\b|\bincomplete\b|\bunsupported\b|\bvalidation failed\b)|Owned frame was rejected by DX12 composition|DX12 shutdown:).*$')
    $dx12Validation = [ordered]@{ worldFrameSubmitted=$gameLog.Contains('DX12 submitted first world frame'); submitted=[uint64]0; rejected=[uint64]0; errors=@($errors | ForEach-Object { $_.Value.Trim() }) }
    foreach ($session in $sessions) {
      $dx12Validation.submitted += [uint64]$session.Groups['submitted'].Value
      $dx12Validation.rejected += [uint64]$session.Groups['rejected'].Value
    }
    if (-not $dx12Validation.worldFrameSubmitted) { throw 'DX12 did not record a normal world submission.' }
    if (-not $sessions.Count -or -not $dx12Validation.submitted -or $dx12Validation.rejected) { throw 'DX12 shutdown did not record submitted frames with zero rejections.' }
    if ($errors.Count) { throw "DX12 rendering errors: $($dx12Validation.errors -join '; ')" }
  }
  $passed = $true
} finally {
  $gameExited = $game -and $game.HasExited
  $gameExitCode = if ($gameExited) { $game.ExitCode } else { $null }
  if ($client -and -not $client.HasExited) { Stop-Process -Id $client.Id }
  if ($game -and -not $game.HasExited) { Stop-Process -Id $game.Id }
  if (Test-Path -LiteralPath (Join-Path $runtime 'keeperfx.log')) { Copy-Item -LiteralPath (Join-Path $runtime 'keeperfx.log') -Destination (Join-Path $output 'keeperfx.log') }
  $env:KFX_SCENE_TEST_DIR = $oldScene
  $env:KFX_SCENE_REFERENCE = $oldReference
  $restored = @()
  foreach ($path in $backups.Keys) {
    if ($null -ne $backups[$path]) { [IO.File]::WriteAllBytes($path,$backups[$path]); $same = [Convert]::ToBase64String([IO.File]::ReadAllBytes($path)) -eq [Convert]::ToBase64String($backups[$path]) }
    else { if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path }; $same = -not (Test-Path -LiteralPath $path) }
    $restored += @{path=$path;restored=$same}
  }
  $lifecycle = @{passed=$passed;gameProcessId=if($game){$game.Id}else{$null};gameExitedBeforeCleanup=$gameExited;gameExitCode=$gameExitCode;restored=$restored}
  if ($dx12Validation) { $lifecycle.dx12Validation = $dx12Validation }
  $lifecycle | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $output 'lifecycle.json') -Encoding utf8
  Write-Output "Live fallback evidence: $output"
  if (Test-Path -LiteralPath (Join-Path $output 'client.stdout.log')) { Get-Content -LiteralPath (Join-Path $output 'client.stdout.log') }
  if (Test-Path -LiteralPath (Join-Path $output 'client.stderr.log')) { Get-Content -LiteralPath (Join-Path $output 'client.stderr.log') }
}
