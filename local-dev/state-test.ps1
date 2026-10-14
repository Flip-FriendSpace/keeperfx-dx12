# SPDX-License-Identifier: GPL-2.0-or-later
# KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
param(
  [ValidateSet('x86', 'x64')][string]$Architecture = 'x64',
  [ValidateSet('OPENGL', 'SOFTWARE', 'DX12')][string]$Renderer = 'OPENGL'
)
$ErrorActionPreference = 'Stop'
if ($Renderer -eq 'DX12' -and $Architecture -ne 'x64') { throw 'DX12 requires x64.' }
$workspace = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$runtime = Join-Path $workspace 'runtime/keeperfx'
$outputRoot = Join-Path $workspace 'artifacts/state-codec'
if ($Architecture -eq 'x64') { $runtime += '-x64'; $outputRoot += '-x64' }
if (Get-Process -Name keeperfx,keeperfx-local,keeperfx-local-hvlog,keeperfx-dx12,keeperfx-dx12-hvlog -ErrorAction SilentlyContinue) {
  throw 'Close running KeeperFX instances before the state format check.'
}
if (@(Get-NetTCPConnection -State Listen -ErrorAction Stop | Where-Object LocalPort -EQ 5599).Count) { throw 'API port 5599 is occupied.' }
$testSave = Join-Path $runtime 'save/fx1g9998.sav'
if (Test-Path -LiteralPath $testSave) { throw 'Save slot 9998 is occupied; the check will not overwrite it.' }
$executable = Join-Path $runtime $(if ($Renderer -eq 'DX12') { 'keeperfx-dx12.exe' } else { 'keeperfx-local.exe' })
if (-not (Test-Path -LiteralPath $executable)) { throw "Executable missing: $executable" }
$nodeExecutable = (Get-Command node -ErrorAction Stop).Source
$output = Join-Path $outputRoot ($Renderer + '-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
New-Item -ItemType Directory -Path $output -ErrorAction Stop | Out-Null
$testConfig = Join-Path $runtime 'keeperfx-state-test.cfg'
$normalConfig = Join-Path $runtime 'keeperfx.cfg'
$backups = @{}
foreach ($path in @($testConfig, $normalConfig, (Join-Path $runtime 'save/settings.toml'), (Join-Path $runtime 'save/fx1lastf.sav'), (Join-Path $env:APPDATA 'keeperfx/keeperfx/renderer_prefs.ini'))) {
  $backups[$path] = if (Test-Path -LiteralPath $path) { [IO.File]::ReadAllBytes($path) } else { $null }
}
$config = Get-Content -LiteralPath $normalConfig -Raw
foreach ($pair in @{API_ENABLED='TRUE'; API_PORT='5599'; AUTOMATIC_REPLAYS='OFF'; FREEZE_GAME_ON_FOCUS_LOST='OFF'; RENDERER=$Renderer}.GetEnumerator()) {
  $pattern = '(?m)^' + $pair.Key + '=.*$'
  if ($config -match $pattern) { $config = $config -replace $pattern, ($pair.Key + '=' + $pair.Value) }
  else { $config += "`r`n$($pair.Key)=$($pair.Value)`r`n" }
}
$previous = $env:KFX_STATE_TEST
$previousDirectory = $env:KFX_STATE_TEST_DIR
$game = $null
$client = $null
$ready = $false
$listenersObserved = @()
$forced = $false
$passed = $false
$failure = $null
$dx12Validation = $null
$clientStdout = Join-Path $output 'api.stdout.log'
$clientStderr = Join-Path $output 'api.stderr.log'
try {
  [IO.File]::WriteAllText($testConfig, $config)
  $env:KFX_STATE_TEST = '1'
  $env:KFX_STATE_TEST_DIR = $output
  $game = Start-Process -FilePath $executable -WorkingDirectory $runtime -ArgumentList '-config keeperfx-state-test.cfg -nointro -skipheartzoom -campaign keeporig -level 1 -nosound' -WindowStyle Hidden -PassThru
  $timer = [Diagnostics.Stopwatch]::StartNew()
  while ($timer.Elapsed.TotalSeconds -lt 30) {
    if ($game.HasExited) { throw 'KeeperFX exited before the test could connect.' }
    $listeners = @(Get-NetTCPConnection -State Listen -ErrorAction Stop | Where-Object LocalPort -EQ 5599)
    $listenersObserved = @($listeners | ForEach-Object { [ordered]@{address=$_.LocalAddress;port=$_.LocalPort;processId=$_.OwningProcess} })
    foreach ($listener in $listeners) {
      if ($listener.OwningProcess -ne $game.Id -or $listener.LocalAddress -ne '127.0.0.1') {
        throw "State API listener mismatch: expected PID $($game.Id) at 127.0.0.1:5599; observed PID $($listener.OwningProcess) at $($listener.LocalAddress):$($listener.LocalPort)."
      }
      $ready = $true
    }
    if ($ready) { break }
    Start-Sleep -Milliseconds 250
  }
  if (-not $ready) { throw 'KeeperFX test API did not start within 30 seconds.' }
  # The existing client keeps one connection through startup and all codec checks.
  $clientArguments = @(('"' + (Join-Path $PSScriptRoot 'state-test.mjs') + '"'), ('"' + $runtime + '"'), ('"' + $output + '"'))
  $client = Start-Process -FilePath $nodeExecutable -ArgumentList $clientArguments -WindowStyle Hidden -PassThru -RedirectStandardOutput $clientStdout -RedirectStandardError $clientStderr
  $timer.Restart()
  while (-not $client.WaitForExit(100)) {
    if ($game.HasExited) {
      if (-not $client.WaitForExit(1000) -or $client.ExitCode -ne 0) { throw 'KeeperFX exited before a successful state check.' }
      break
    }
    if ($timer.Elapsed.TotalSeconds -ge 120) { throw 'State-format client exceeded its 120-second bound.' }
  }
  $client.WaitForExit()
  if ($client.ExitCode -ne 0) { throw "Live state-format regression failed; inspect $clientStderr." }
  if (-not $game.WaitForExit(10000) -or $game.ExitCode -ne 0) { throw 'KeeperFX did not exit cleanly.' }
  if ($Renderer -eq 'DX12') {
    $log = Get-Content -LiteralPath (Join-Path $runtime 'keeperfx.log') -Raw
    $sessions = [regex]::Matches($log, 'DX12 session: submitted (?<submitted>[0-9]+), rejected (?<rejected>[0-9]+)\b')
    $validation = [regex]::Matches($log, 'DX12 validation: errors (?<errors>[0-9]+), warnings (?<warnings>[0-9]+)\b')
    $errors = [regex]::Matches($log, '(?im)^.*(?:\bError\b[^\r\n]*\bDX12\b|\bDX12\b[^\r\n]*\bError\b|DX12[^\r\n]*(?:\bstopped\b|\binitialization failed\b|\bincomplete\b|\bunsupported\b|\bvalidation failed\b)|Owned frame was rejected by DX12 composition|DX12 shutdown:).*$')
    $dx12Validation = [ordered]@{initialized=$log.Contains('DX12 development backend initialized');worldFrameSubmitted=$log.Contains('DX12 submitted first world frame');sessionCount=$sessions.Count;submitted=[uint64]0;rejected=[uint64]0;gpuErrors=[uint64]0;gpuWarnings=[uint64]0;errors=@($errors | ForEach-Object { $_.Value.Trim() })}
    foreach ($session in $sessions) { $dx12Validation.submitted += [uint64]$session.Groups['submitted'].Value; $dx12Validation.rejected += [uint64]$session.Groups['rejected'].Value }
    foreach ($entry in $validation) { $dx12Validation.gpuErrors += [uint64]$entry.Groups['errors'].Value; $dx12Validation.gpuWarnings += [uint64]$entry.Groups['warnings'].Value }
    if (-not $dx12Validation.initialized -or -not $dx12Validation.worldFrameSubmitted -or $sessions.Count -eq 0 -or $dx12Validation.submitted -eq 0 -or $validation.Count -eq 0 -or $dx12Validation.rejected -ne 0 -or $dx12Validation.gpuErrors -ne 0 -or $dx12Validation.gpuWarnings -ne 0 -or $errors.Count -ne 0) { throw 'DX12 state check presentation/diagnostics failed.' }
  }
  $passed = $true
} catch {
  $failure = $_.Exception.Message
  throw
} finally {
  $env:KFX_STATE_TEST = $previous
  $env:KFX_STATE_TEST_DIR = $previousDirectory
  $cleanupErrors = @()
  try {
    if ($client -and -not $client.HasExited) { Stop-Process -Id $client.Id; $null = $client.WaitForExit(5000) }
  } catch { $cleanupErrors += $_.Exception.Message }
  try {
    if ($game -and -not $game.HasExited) { $forced = $true; Stop-Process -Id $game.Id; if (-not $game.WaitForExit(5000)) { throw 'Game cleanup did not complete.' } }
  } catch { $cleanupErrors += $_.Exception.Message }
  try {
    if ($game -and (Test-Path -LiteralPath (Join-Path $runtime 'keeperfx.log'))) { Copy-Item -LiteralPath (Join-Path $runtime 'keeperfx.log') -Destination (Join-Path $output 'live-state-test.log') }
    if (Test-Path -LiteralPath $testSave) { Copy-Item -LiteralPath $testSave -Destination (Join-Path $output 'live-save.sav'); Remove-Item -LiteralPath $testSave }
  } catch { $cleanupErrors += $_.Exception.Message }
  foreach ($path in $backups.Keys) {
    try {
      if ($null -ne $backups[$path]) { [IO.File]::WriteAllBytes($path, $backups[$path]) }
      elseif (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path }
    } catch { $cleanupErrors += $_.Exception.Message }
  }
  $restored = $cleanupErrors.Count -eq 0 -and -not (Test-Path -LiteralPath $testSave)
  foreach ($path in $backups.Keys) {
    try {
      if ($null -eq $backups[$path]) { $restored = $restored -and -not (Test-Path -LiteralPath $path) }
      else { $restored = $restored -and ([Convert]::ToBase64String([IO.File]::ReadAllBytes($path)) -eq [Convert]::ToBase64String($backups[$path])) }
    } catch { $restored = $false; $cleanupErrors += $_.Exception.Message }
  }
  [ordered]@{passed=($passed -and $restored -and -not $forced);architecture=$Architecture;renderer=$Renderer;executable=$executable;binarySha256=(Get-FileHash -LiteralPath $executable -Algorithm SHA256).Hash;gameProcessId=if($game){$game.Id}else{$null};apiOwnerVerified=$ready;listeners=$listenersObserved;exitCode=if($game -and $game.HasExited){$game.ExitCode}else{$null};forced=$forced;byteExactRestoration=$restored;failure=$failure;cleanupErrors=$cleanupErrors;dx12Validation=$dx12Validation} | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $output 'verification.json') -Encoding utf8
  if (-not $restored) { throw "Runtime restoration failed: $($cleanupErrors -join '; ')" }
}
Write-Output "Live state-format regression passed: $output"
