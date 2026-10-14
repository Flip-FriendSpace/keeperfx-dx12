# SPDX-License-Identifier: GPL-2.0-or-later
# KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
param(
  [ValidateSet('OPENGL','SOFTWARE','DX12')][string]$Renderer = 'OPENGL',
  [ValidateSet('x86','x64')][string]$Architecture = 'x64',
  [ValidateSet('3840x2160','1920x1080','640x480')][string]$Resolution = '3840x2160',
  [ValidateRange(1,99)][int]$Level = 1,
  [switch]$ClearUI,
  [switch]$PaletteFade,
  [ValidateRange(0,8)][int]$Reloads = 0,
  [switch]$TerrainUpdates,
  [switch]$TemporalUpdates,
  [switch]$WindowUpdates
)
$ErrorActionPreference = 'Stop'
if ($Renderer -eq 'DX12' -and $Architecture -ne 'x64') { throw 'DX12 requires the local Windows x64 executable.' }
if ($Reloads -gt 0 -and $Renderer -ne 'DX12') { throw 'Reload qualification requires DX12 renderer statistics.' }
if ($TerrainUpdates -and $Renderer -ne 'DX12') { throw 'Terrain cache updates require DX12 renderer statistics.' }
if ($TemporalUpdates -and $Renderer -ne 'DX12') { throw 'Temporal receipts require the DX12 renderer.' }
if ($TerrainUpdates -and $TemporalUpdates) { throw 'Run terrain reuse and live temporal surfaces separately.' }
if ($WindowUpdates -and $Renderer -ne 'DX12') { throw 'Window lifecycle qualification requires DX12 renderer statistics.' }
if ($WindowUpdates -and ($TerrainUpdates -or $TemporalUpdates)) { throw 'Run window lifecycle separately from terrain and temporal updates.' }
$workspace = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$runtime = Join-Path $workspace $(if ($Architecture -eq 'x64') { 'runtime/keeperfx-x64' } else { 'runtime/keeperfx' })
$executable = Join-Path $runtime $(if ($Renderer -eq 'DX12') { 'keeperfx-dx12.exe' } else { 'keeperfx-local.exe' })
$testSave = Join-Path $runtime 'save/fx1g9997.sav'
if ($Reloads -gt 0 -and (Test-Path -LiteralPath $testSave)) { throw 'Save slot 9997 is occupied; this runner will not overwrite it.' }
if (-not (Test-Path -LiteralPath $executable)) { throw "Missing executable: $executable" }
if (Get-Process -Name keeperfx,keeperfx-local,keeperfx-local-hvlog,keeperfx-dx12,keeperfx-dx12-hvlog -ErrorAction SilentlyContinue) {
  throw 'Close KeeperFX before inspecting a live renderer.'
}
if (@(Get-NetTCPConnection -State Listen -ErrorAction Stop | Where-Object LocalPort -EQ 5599).Count) {
  throw 'API port 5599 already has a listener.'
}
$nodeExecutable = (Get-Command node -ErrorAction Stop).Source
$output = Join-Path $workspace ('artifacts/live-renderer/' + $Renderer + '-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
New-Item -ItemType Directory -Path $output | Out-Null
$configPath = Join-Path $runtime 'keeperfx.cfg'
$testConfigPath = Join-Path $runtime 'keeperfx-live-renderer.cfg'
$prefsPath = Join-Path $env:APPDATA 'keeperfx/keeperfx/renderer_prefs.ini'
$backups = @{}
foreach ($path in @($configPath,$testConfigPath,$prefsPath,(Join-Path $runtime 'save/settings.toml'),(Join-Path $runtime 'save/fx1lastf.sav'))) {
  $backups[$path] = if (Test-Path -LiteralPath $path) { [IO.File]::ReadAllBytes($path) } else { $null }
}
$oldScene = $env:KFX_SCENE_TEST_DIR
$oldReference = $env:KFX_SCENE_REFERENCE
$oldLiveSurfaces = $env:KFX_SCENE_LIVE_SURFACES
$game = $null
$client = $null
$phase = 'configuration'
$passed = $false
$failureMessage = $null
$apiOwnerVerified = $false
$dx12Validation = $null
try {
  $config = Get-Content -LiteralPath $configPath -Raw
  $settings = @{ API_ENABLED='TRUE'; API_PORT='5599'; AUTOMATIC_REPLAYS='OFF'; FREEZE_GAME_ON_FOCUS_LOST='OFF'; RENDERER=$Renderer; CAPTURE_CURSOR='OFF'; CURSOR_EDGE_CAMERA_PANNING='OFF'; VSYNC='OFF'; INGAME_RES=($Resolution+'w32') }
  foreach ($key in $settings.Keys) {
    $pattern = '(?m)^' + $key + '=.*$'
    if ($config -match $pattern) { $config = $config -replace $pattern, ($key + '=' + $settings[$key]) }
    else { $config += "`r`n$key=$($settings[$key])`r`n" }
  }
  [IO.File]::WriteAllText($testConfigPath,$config)
  $env:KFX_SCENE_TEST_DIR = $output
  $env:KFX_SCENE_REFERENCE = '1'
  if ($TemporalUpdates) { $env:KFX_SCENE_LIVE_SURFACES = '1' }
  $phase = 'listener-readiness'
  $game = Start-Process -FilePath $executable -WorkingDirectory $runtime -WindowStyle Hidden -PassThru `
    -ArgumentList "-config keeperfx-live-renderer.cfg -nointro -skipheartzoom -campaign keeporig -level $Level -nosound -alex"
  $timer = [Diagnostics.Stopwatch]::StartNew()
  while ($timer.Elapsed.TotalSeconds -lt 30) {
    if ($game.HasExited) { throw "KeeperFX exited before API readiness ($($game.ExitCode))." }
    $listeners = @(Get-NetTCPConnection -State Listen -ErrorAction Stop | Where-Object LocalPort -EQ 5599)
    foreach ($listener in $listeners) {
      if ($listener.OwningProcess -ne $game.Id) { throw "API port 5599 belongs to process $($listener.OwningProcess), expected $($game.Id)." }
      if ($listener.LocalAddress -ne '127.0.0.1') { throw "API listener is not loopback-only: $($listener.LocalAddress)." }
      $apiOwnerVerified = $true
    }
    if ($apiOwnerVerified) { break }
    Start-Sleep -Milliseconds 250
  }
  if (-not $apiOwnerVerified) { throw 'KeeperFX did not own a loopback API listener within 30 seconds.' }
  $phase = 'live-screenshot'
  $arguments = @(('"' + (Join-Path $PSScriptRoot 'live-renderer.mjs') + '"'), ('"' + $output + '"'), $Level, $Renderer, $Resolution)
  if ($ClearUI) { $arguments += '--clear-ui' }
  if ($PaletteFade) { $arguments += '--palette-fade' }
  if ($TerrainUpdates) { $arguments += '--terrain-updates' }
  if ($TemporalUpdates) { $arguments += '--temporal-updates' }
  if ($WindowUpdates) {
    $windowHost = Join-Path $env:SystemRoot 'System32/WindowsPowerShell/v1.0/powershell.exe'
    if (-not (Test-Path -LiteralPath $windowHost)) { throw 'The scoped window helper requires Windows PowerShell.' }
    $arguments += '--window-updates', "--game-pid=$($game.Id)", ('"--window-exe=' + $executable + '"'), ('"--window-host=' + $windowHost + '"')
  }
  if ($Reloads -gt 0) { $arguments += "--reloads=$Reloads", ('"--runtime=' + $runtime + '"') }
  $client = Start-Process -FilePath $nodeExecutable -ArgumentList $arguments -WindowStyle Hidden -PassThru `
    -RedirectStandardOutput (Join-Path $output 'client.stdout.log') -RedirectStandardError (Join-Path $output 'client.stderr.log')
  $timer.Restart()
  $clientLimit = 120 + $Reloads * 60 + $(if ($TerrainUpdates) { 900 } else { 0 }) + $(if ($TemporalUpdates) { 600 } else { 0 }) + $(if ($WindowUpdates) { 180 } else { 0 })
  while (-not $client.WaitForExit(100)) {
    if ($game.HasExited -and (-not $client.WaitForExit(1000) -or $client.ExitCode -ne 0)) { throw 'KeeperFX exited before the live screenshot client completed.' }
    if ($timer.Elapsed.TotalSeconds -ge $clientLimit) { throw "Live screenshot client exceeded its $clientLimit-second bound." }
  }
  $client.WaitForExit()
  if ($client.ExitCode -ne 0) { throw "Live screenshot failed ($($client.ExitCode)); inspect client.stderr.log." }
  $phase = 'game-exit'
  if (-not $game.WaitForExit(10000)) { throw 'KeeperFX did not exit within 10 seconds after quit.' }
  if ($game.ExitCode -ne 0) { throw "KeeperFX exited with code $($game.ExitCode)." }
  if ($Renderer -eq 'DX12') {
    $phase = 'dx12-validation'
    $gameLog = Get-Content -LiteralPath (Join-Path $runtime 'keeperfx.log') -Raw
    $sessions = [regex]::Matches($gameLog, 'DX12 session: submitted (?<submitted>[0-9]+), rejected (?<rejected>[0-9]+)\b')
    $errorLines = [regex]::Matches($gameLog, '(?im)^.*(?:\bError\b[^\r\n]*\bDX12\b|\bDX12\b[^\r\n]*\bError\b|DX12[^\r\n]*(?:\bstopped\b|\binitialization failed\b|\bincomplete\b|\bunsupported\b|\bvalidation failed\b)|Owned frame was rejected by DX12 composition|DX12 shutdown:).*$')
    $dx12Validation = [ordered]@{ initialized=$gameLog.Contains('DX12 development backend initialized'); worldFrameSubmitted=$gameLog.Contains('DX12 submitted first world frame'); submitted=[uint64]0; rejected=[uint64]0; errors=@($errorLines | ForEach-Object { $_.Value.Trim() }) }
    foreach ($session in $sessions) {
      $dx12Validation.submitted += [uint64]$session.Groups['submitted'].Value
      $dx12Validation.rejected += [uint64]$session.Groups['rejected'].Value
    }
    if (-not $dx12Validation.initialized -or -not $dx12Validation.worldFrameSubmitted) { throw 'DX12 did not record initialization and a normal world submission.' }
    if (-not $sessions.Count -or -not $dx12Validation.submitted -or $dx12Validation.rejected) { throw 'DX12 shutdown did not record submitted frames with zero rejections.' }
    if ($errorLines.Count) { throw "DX12 rendering errors: $($dx12Validation.errors -join '; ')" }
    if ($PaletteFade) {
      $dx12Validation.paletteReplaySubmitted = [uint64]0
      foreach ($replay in [regex]::Matches($gameLog, 'DX12 palette replay: submitted (?<submitted>[0-9]+)\b')) {
        $dx12Validation.paletteReplaySubmitted += [uint64]$replay.Groups['submitted'].Value
      }
      if (-not $dx12Validation.paletteReplaySubmitted) { throw 'DX12 did not record a submitted palette replay during the blocking fade.' }
    }
  }
  $passed = $true
  $phase = 'complete'
} catch {
  $failureMessage = $_.Exception.Message
  throw
} finally {
  $env:KFX_SCENE_TEST_DIR = $oldScene
  $env:KFX_SCENE_REFERENCE = $oldReference
  $env:KFX_SCENE_LIVE_SURFACES = $oldLiveSurfaces
  try {
    $gameExitCode = if ($game -and $game.HasExited) { $game.ExitCode } else { $null }
    if ($client -and -not $client.HasExited) { Stop-Process -Id $client.Id -ErrorAction SilentlyContinue }
    if ($game -and -not $game.HasExited) { Stop-Process -Id $game.Id -ErrorAction SilentlyContinue }
    if ($game) { $game.WaitForExit(10000) | Out-Null }
    $saveCleanup = $null
    if ($Reloads -gt 0) {
      # The client refuses this slot again immediately before marking its save
      # request. Only a save belonging to that marked request may be removed.
      $ownedSave = Test-Path -LiteralPath (Join-Path $output 'reload-save-requested.json')
      if ($ownedSave -and (Test-Path -LiteralPath $testSave)) { Remove-Item -LiteralPath $testSave }
      $saveCleanup = @{ slot=9997; ownedRequest=$ownedSave; removed=($ownedSave -and -not (Test-Path -LiteralPath $testSave)) }
    }
    if (Test-Path -LiteralPath (Join-Path $runtime 'keeperfx.log')) { Copy-Item -LiteralPath (Join-Path $runtime 'keeperfx.log') -Destination (Join-Path $output 'keeperfx.log') -Force }
    $reportPath = Join-Path $output 'report.json'
    $report = if (Test-Path -LiteralPath $reportPath) { Get-Content -LiteralPath $reportPath -Raw | ConvertFrom-Json } else { [pscustomobject]@{ renderer=$Renderer; level=$Level; resolution=$Resolution } }
    $reportedError = if ($report.PSObject.Properties['error'] -and $report.error) { $report.error } else { $failureMessage }
    foreach ($entry in @{ passed=$passed; phase=$phase; error=$reportedError; executable=$executable; architecture=$Architecture; apiOwnerVerified=$apiOwnerVerified; gameExitCode=$gameExitCode }.GetEnumerator()) {
      $report | Add-Member -NotePropertyName $entry.Key -NotePropertyValue $entry.Value -Force
    }
    if ($dx12Validation) { $report | Add-Member -NotePropertyName dx12Validation -NotePropertyValue $dx12Validation -Force }
    if ($saveCleanup) { $report | Add-Member -NotePropertyName saveCleanup -NotePropertyValue $saveCleanup -Force }
    $report | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $reportPath -Encoding utf8
  } finally {
    foreach ($path in $backups.Keys) {
      if ($null -ne $backups[$path]) { [IO.File]::WriteAllBytes($path,$backups[$path]) }
      elseif (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path }
    }
    Write-Output "Live renderer evidence: $output"
  }
}
