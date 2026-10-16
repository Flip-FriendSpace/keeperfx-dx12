# SPDX-License-Identifier: GPL-2.0-or-later
# KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
param([string]$WorkspacePath, [switch]$Upstream, [ValidateSet('OPENGL', 'SOFTWARE', 'DX12')][string]$Renderer = 'OPENGL', [ValidateSet('x86', 'x64')][string]$Architecture = 'x64', [switch]$Sound)
$ErrorActionPreference = 'Stop'
if (-not $WorkspacePath) { $WorkspacePath = Split-Path -Parent (Split-Path -Parent $PSScriptRoot) }
$workspace = (Resolve-Path -LiteralPath $WorkspacePath).Path
if ($Renderer -eq 'DX12' -and ($Upstream -or $Architecture -ne 'x64')) {
  throw 'The DX12 smoke check requires the local Windows x64 development executable.'
}
if ($Upstream) { $Architecture = 'x86' }
$runtime = Join-Path $workspace 'runtime/keeperfx'
if ($Architecture -eq 'x64') {
  $runtime += '-x64'
}
$name = if ($Upstream) { 'keeperfx.exe' } elseif ($Renderer -eq 'DX12') { 'keeperfx-dx12.exe' } else { 'keeperfx-local.exe' }
$executable = Join-Path $runtime $name
if (-not (Test-Path -LiteralPath $executable)) { throw 'Build KeeperFX before running this check.' }
if (Get-Process -Name keeperfx,keeperfx-local,keeperfx-local-hvlog,keeperfx-dx12,keeperfx-dx12-hvlog -ErrorAction SilentlyContinue) {
  throw 'Close running KeeperFX instances before running the smoke check.'
}
$config = Get-Content -LiteralPath (Join-Path $runtime 'keeperfx.cfg') -Raw
$config = $config -replace '(?m)^API_ENABLED=.*$', 'API_ENABLED=TRUE'
$config = $config -replace '(?m)^API_PORT=.*$', 'API_PORT=5599'
$config = $config -replace '(?m)^AUTOMATIC_REPLAYS=.*$', 'AUTOMATIC_REPLAYS=OFF'
$config = $config -replace '(?m)^RENDERER=.*$', "RENDERER=$Renderer"
$config = $config -replace '(?m)^FREEZE_GAME_ON_FOCUS_LOST=.*$', 'FREEZE_GAME_ON_FOCUS_LOST=OFF'
[IO.File]::WriteAllText((Join-Path $runtime 'keeperfx-smoke.cfg'), $config)
$gameArguments = '-config keeperfx-smoke.cfg -nointro -skipheartzoom -campaign keeporig -level 1'
if (-not $Sound) { $gameArguments += ' -nosound' }
$game = Start-Process -FilePath $executable -WorkingDirectory $runtime -ArgumentList $gameArguments -WindowStyle Hidden -PassThru
$label = if ($Upstream) { 'upstream' } else { 'local' }
$client = $null
$clientStdout = Join-Path $workspace "artifacts/smoke-$label-$Architecture-$Renderer-api.stdout.log"
$clientStderr = Join-Path $workspace "artifacts/smoke-$label-$Architecture-$Renderer-api.stderr.log"
$listenerReady = $false
$apiListenerProcessId = $null
$smokePhase = 'listener-readiness'
$smokePassed = $false
$dx12Validation = $null
try {
  # Inspect the actual listening socket without creating a probe connection.
  # This also prevents attaching to an unrelated/stale listener during startup.
  $listenerTimer = [Diagnostics.Stopwatch]::StartNew()
  while ($listenerTimer.Elapsed.TotalSeconds -lt 30) {
    if ($game.HasExited) { throw "KeeperFX exited before its API listened ($($game.ExitCode)). Inspect $runtime/keeperfx.log." }
    $listeners = @(Get-NetTCPConnection -State Listen -ErrorAction Stop | Where-Object LocalPort -EQ 5599)
    foreach ($listener in $listeners) {
      $apiListenerProcessId = $listener.OwningProcess
      if ($listener.OwningProcess -ne $game.Id) { throw "API port 5599 belongs to process $($listener.OwningProcess), expected KeeperFX $($game.Id)." }
      if ($listener.LocalAddress -ne '127.0.0.1') { throw "KeeperFX API listener is not loopback-only: $($listener.LocalAddress)." }
      $listenerReady = $true
    }
    if ($listenerReady) { break }
    Start-Sleep -Milliseconds 250
  }
  if (-not $listenerReady) { throw 'KeeperFX did not own a loopback API listener within 30 seconds.' }
  # Node owns one persistent API connection and its bounded startup retry.
  # A disposable TCP probe can fill the one-slot listen backlog during loading.
  $arguments = @(('"' + (Join-Path $PSScriptRoot 'smoke.mjs') + '"'), '--quit')
  if ($Upstream) { $arguments += '--upstream' }
  $nodeExecutable = (Get-Command node -ErrorAction Stop).Source
  $smokePhase = 'persistent-api-client'
  $client = Start-Process -FilePath $nodeExecutable -ArgumentList $arguments -WindowStyle Hidden -PassThru `
    -RedirectStandardOutput $clientStdout -RedirectStandardError $clientStderr
  $checkTimer = [Diagnostics.Stopwatch]::StartNew()
  while (-not $client.WaitForExit(100)) {
    if ($game.HasExited) {
      # A successful --quit can close the game just before Node exits. Accept
      # that only when the client finishes successfully within a bounded grace.
      if (-not $client.WaitForExit(1000) -or $client.ExitCode -ne 0) {
        throw "KeeperFX exited before a successful smoke check ($($game.ExitCode)). Inspect $runtime/keeperfx.log."
      }
      break
    }
    if ($checkTimer.Elapsed.TotalSeconds -ge 120) { throw 'Campaign smoke client exceeded its 120-second bound.' }
  }
  $client.WaitForExit()
  if ($client.ExitCode -ne 0) { throw "Campaign smoke check failed ($($client.ExitCode)). See $clientStderr." }
  Copy-Item -LiteralPath (Join-Path $workspace "artifacts/smoke-$label.json") -Destination (Join-Path $workspace "artifacts/smoke-$label-$Architecture-$Renderer.json") -Force
  $smokePhase = 'game-exit'
  if (-not $game.WaitForExit(10000)) { throw 'KeeperFX did not exit after the check.' }
  if ($game.ExitCode -ne 0) { throw "KeeperFX exited with code $($game.ExitCode)." }
  if ($Renderer -eq 'DX12') {
    $smokePhase = 'renderer-validation'
    $gameLog = Get-Content -LiteralPath (Join-Path $runtime 'keeperfx.log') -Raw
    $sessions = [regex]::Matches($gameLog, 'DX12 session: submitted (?<submitted>[0-9]+), rejected (?<rejected>[0-9]+)\b')
    $errors = [regex]::Matches($gameLog, '(?im)^.*(?:\bError\b[^\r\n]*\bDX12\b|\bDX12\b[^\r\n]*\bError\b|DX12[^\r\n]*(?:\bstopped\b|\binitialization failed\b|\bincomplete\b|\bunsupported\b|\bvalidation failed\b)|Owned frame was rejected by DX12 composition|DX12 shutdown:).*$')
    $dx12Validation = [ordered]@{
      initialized = $gameLog.Contains('DX12 development backend initialized')
      worldFrameSubmitted = $gameLog.Contains('DX12 submitted first world frame')
      sessionCount = $sessions.Count
      submitted = [uint64]0
      rejected = [uint64]0
      errors = @($errors | ForEach-Object { $_.Value.Trim() })
    }
    foreach ($session in $sessions) {
      $dx12Validation.submitted += [uint64]$session.Groups['submitted'].Value
      $dx12Validation.rejected += [uint64]$session.Groups['rejected'].Value
    }
    if (-not $dx12Validation.initialized) { throw 'DX12 backend initialization was not recorded.' }
    if (-not $dx12Validation.worldFrameSubmitted) { throw 'DX12 did not record submission of a normal world frame.' }
    if ($sessions.Count -eq 0 -or $dx12Validation.submitted -eq 0) { throw 'DX12 shutdown did not record submitted frames.' }
    if ($dx12Validation.rejected -ne 0) { throw "DX12 rejected $($dx12Validation.rejected) frame(s)." }
    if ($errors.Count -ne 0) { throw "DX12 rendering errors were recorded: $($dx12Validation.errors -join '; ')" }
  }
  $smokePassed = $true
} finally {
  $lifecycle = [ordered]@{
    timestamp = (Get-Date).ToUniversalTime().ToString('o')
    phase = $smokePhase
    passed = $smokePassed
    executable = $executable
    gameProcessId = $game.Id
    apiOwnerVerified = $listenerReady
    apiListenerProcessId = $apiListenerProcessId
    gameExitedBeforeCleanup = $game.HasExited
    gameExitCode = if ($game.HasExited) { $game.ExitCode } else { $null }
    clientExitedBeforeCleanup = if ($client) { $client.HasExited } else { $null }
    clientExitCode = if ($client -and $client.HasExited) { $client.ExitCode } else { $null }
  }
  if ($dx12Validation) { $lifecycle.dx12Validation = $dx12Validation }
  $lifecycle | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $workspace "artifacts/smoke-$label-$Architecture-$Renderer-lifecycle.json") -Encoding utf8
  if ($client -and -not $client.HasExited) { Stop-Process -Id $client.Id -ErrorAction SilentlyContinue }
  if (Test-Path -LiteralPath $clientStdout) { Get-Content -LiteralPath $clientStdout }
  if (Test-Path -LiteralPath $clientStderr) { Get-Content -LiteralPath $clientStderr | Write-Host }
  if (-not $game.HasExited) { Stop-Process -Id $game.Id }
  Copy-Item -LiteralPath (Join-Path $runtime 'keeperfx.log') -Destination (Join-Path $workspace "artifacts/smoke-$label-$Architecture-$Renderer.log") -Force
}
