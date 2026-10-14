# SPDX-License-Identifier: GPL-2.0-or-later
# KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
param(
  [ValidateSet('x86', 'x64')][string]$Architecture = 'x64',
  [ValidateSet('OPENGL','SOFTWARE','DX12')][string]$Renderer = 'OPENGL',
  [ValidateSet('OPENGL','SOFTWARE','DX12')][string]$PlaybackRenderer = 'OPENGL',
  [switch]$MidGame,
  [string]$ReplayFile,
  [uint32]$TargetTurn,
  [uint32]$FinishGameTurn,
  [ValidateSet('LEVEL_STATS','LAND_VIEW')][string]$ExpectedFrontend,
  [ValidateRange(30, 1800)][int]$PlaybackTimeoutSeconds = 120,
  [ValidateRange(1, 200)][int]$PlaybackTurnsPerSecond = 20,
  [string]$SetupScript,
  [string]$VerifyScript,
  [ValidatePattern('^[A-Za-z0-9_-]+$')][string]$Campaign = 'keeporig',
  [ValidateRange(1, 65535)][int]$Level = 1,
  [ValidateRange(1, 36000)][int]$RecordTargetTurn,
  [ValidateRange(1, 36000)][int]$PlaybackTurns,
  [ValidateRange(30, 1800)][int]$RecordTimeoutSeconds = 120
)
$ErrorActionPreference = 'Stop'
if ($Architecture -ne 'x64' -and ($Renderer -eq 'DX12' -or $PlaybackRenderer -eq 'DX12')) { throw 'DX12 requires x64.' }
$existingReplay = -not [string]::IsNullOrWhiteSpace($ReplayFile)
$terminalReplay = $PSBoundParameters.ContainsKey('FinishGameTurn') -or $PSBoundParameters.ContainsKey('ExpectedFrontend')
if ($terminalReplay -and (-not $existingReplay -or -not $PSBoundParameters.ContainsKey('FinishGameTurn') -or -not $PSBoundParameters.ContainsKey('ExpectedFrontend'))) { throw 'FinishGameTurn and ExpectedFrontend must be supplied together with ReplayFile.' }
if ($terminalReplay -and ($FinishGameTurn -le $TargetTurn)) { throw 'FinishGameTurn must be after the minimum verified TargetTurn.' }
if ($existingReplay) {
  foreach ($option in @('Campaign','Level','RecordTargetTurn','PlaybackTurns','RecordTimeoutSeconds')) {
    if ($PSBoundParameters.ContainsKey($option)) { throw "$option cannot be supplied with ReplayFile; use its absolute TargetTurn and PlaybackTimeoutSeconds." }
  }
}
$recordTarget = if ($PSBoundParameters.ContainsKey('RecordTargetTurn')) { $RecordTargetTurn } elseif ($MidGame) { 420 } else { 220 }
$playbackTurnCount = if ($PSBoundParameters.ContainsKey('PlaybackTurns')) { $PlaybackTurns } elseif ($MidGame) { 200 } else { 180 }
if (-not $existingReplay -and $recordTarget -lt ($(if ($MidGame) { 120 } else { 0 }) + $playbackTurnCount + 2)) { throw 'RecordTargetTurn must leave at least two recorded turns beyond the requested playback window.' }
$setupScriptHash = $null
$setupCompletedSteps = 0
$verifyScriptHash = $null
if (-not [string]::IsNullOrWhiteSpace($VerifyScript)) {
  if ($terminalReplay) { throw 'VerifyScript requires a running world and cannot be used with terminal replay.' }
  $VerifyScript = (Resolve-Path -LiteralPath $VerifyScript -ErrorAction Stop).Path
  if (-not (Test-Path -LiteralPath $VerifyScript -PathType Leaf) -or $VerifyScript.Contains('"')) { throw 'VerifyScript must be an existing read-only Lua assertion file without quotes in its path.' }
  $verifyScriptHash = (Get-FileHash -LiteralPath $VerifyScript -Algorithm SHA256).Hash
}
if (-not [string]::IsNullOrWhiteSpace($SetupScript)) {
  if (-not $MidGame -or $existingReplay) { throw 'SetupScript requires MidGame recording and cannot be used with ReplayFile.' }
  $SetupScript = (Resolve-Path -LiteralPath $SetupScript -ErrorAction Stop).Path
  if (-not (Test-Path -LiteralPath $SetupScript -PathType Leaf) -or $SetupScript.Contains('"')) { throw 'SetupScript must be an existing file path without quotes.' }
  $setupScriptHash = (Get-FileHash -LiteralPath $SetupScript -Algorithm SHA256).Hash
}
if (-not $existingReplay -and $PSBoundParameters.ContainsKey('TargetTurn')) { throw 'TargetTurn requires ReplayFile.' }
$recordBaseTurn = 0
$recordIntegrity = $null
$sourcePacketHash = $null
$sourceLogHash = $null
$sourceLogPath = $null
if ($existingReplay) {
  if ($ReplayFile -notmatch '^(?:[A-Za-z]:[\\/]|\\\\)' -or $ReplayFile.Contains('"')) { throw 'ReplayFile must be an absolute file path without quotes.' }
  $ReplayFile = (Resolve-Path -LiteralPath $ReplayFile -ErrorAction Stop).Path
  if (-not (Test-Path -LiteralPath $ReplayFile -PathType Leaf)) { throw 'ReplayFile must be an existing file.' }
  $sourceLogPath = Join-Path (Split-Path -Parent $ReplayFile) 'keeperfx.log'
  $sourceLog = Get-Content -LiteralPath $sourceLogPath -Raw
  $recordingReceipts = [regex]::Matches($sourceLog, 'Replay recording: base_turn=(?<turn>[0-9]+) integrity=(?<integrity>[0-9]+)')
  if ($recordingReceipts.Count -ne 1) { throw 'Adjacent keeperfx.log must contain exactly one mid-game recording receipt.' }
  $sourceLevels = [regex]::Matches($sourceLog.Substring(0, $recordingReceipts[0].Index), 'Started level (?<level>[0-9]+) from ')
  if ($sourceLevels.Count -eq 0) { throw 'Adjacent keeperfx.log must identify the source level before recording.' }
  $Level = [int]$sourceLevels[-1].Groups['level'].Value
  $recordBaseTurn = [uint32]$recordingReceipts[0].Groups['turn'].Value
  $recordIntegrity = $recordingReceipts[0].Groups['integrity'].Value
  if ($recordBaseTurn -eq 0 -or -not $PSBoundParameters.ContainsKey('TargetTurn') -or $TargetTurn -le $recordBaseTurn) { throw 'Existing mid-game replay requires an absolute TargetTurn greater than its recorded base turn.' }
  $starts = [regex]::Matches($sourceLog, 'Starting packet saving, turn (?<turn>[0-9]+)')
  if ($starts.Count -ne 1 -or [uint32]$starts[0].Groups['turn'].Value -ne $recordBaseTurn) { throw 'Source recording base-turn receipts disagree.' }
  if ($sourceLog -match 'recording stopped|Replay reached|Packet File unreadable|replay aborted') { throw 'Source log records an incomplete/failed recording.' }
  if ($terminalReplay) {
    $outcomeState = if ($ExpectedFrontend -eq 'LEVEL_STATS') { 'Victory achieved' } else { 'Lost level' }
    $sourceBoundary = [regex]::Match($sourceLog.Substring($recordingReceipts[0].Index), ('(?m)^.*\[' + $FinishGameTurn + '\] get_startup_menu_state: ' + $outcomeState + ' state selected\r?$'))
    if (-not $sourceBoundary.Success -or $sourceBoundary.Value -eq '') { throw 'Source recording does not contain the requested exact outcome boundary.' }
    $sourceTail = $sourceLog.Substring($recordingReceipts[0].Index + $sourceBoundary.Index + $sourceBoundary.Length)
    if ($sourceTail -notmatch ('Frontend state change from 0 \(FeSt_INITIAL\) into [0-9]+ \(FeSt_' + $ExpectedFrontend + '\)')) { throw 'Source outcome does not enter the requested frontend.' }
  }
  $MidGame = $true
  $sourcePacketHash = (Get-FileHash -LiteralPath $ReplayFile -Algorithm SHA256).Hash
  $sourceLogHash = (Get-FileHash -LiteralPath $sourceLogPath -Algorithm SHA256).Hash
}
$workspace = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$runtime = Join-Path $workspace 'runtime/keeperfx'
if ($Architecture -eq 'x64') { $runtime += '-x64' }
if (-not $existingReplay) {
  $campaignConfig = Join-Path $runtime "campgns/$Campaign.cfg"
  $campaignText = Get-Content -LiteralPath $campaignConfig -Raw
  $levelLists = [regex]::Matches($campaignText, '(?mi)^\s*(?:SINGLE_LEVELS|BONUS_LEVELS|EXTRA_LEVELS)\s*=([^;\r\n]*)')
  $installedLevels = @($levelLists | ForEach-Object { [regex]::Matches($_.Groups[1].Value, '\b[0-9]+\b') } | ForEach-Object { [int]$_.Value })
  if ($Level -notin $installedLevels) { throw "Level $Level is not listed in installed campaign $Campaign." }
}
if (Get-Process -Name keeperfx,keeperfx-local,keeperfx-local-hvlog,keeperfx-dx12,keeperfx-dx12-hvlog -ErrorAction SilentlyContinue) { throw 'Close KeeperFX before the replay test.' }
if (@(Get-NetTCPConnection -State Listen -ErrorAction Stop | Where-Object LocalPort -EQ 5599).Count) { throw 'API port 5599 is occupied.' }
$output = Join-Path $workspace ('artifacts/x64-port/replay-' + $Architecture + '-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
New-Item -ItemType Directory -Path $output -Force | Out-Null
$packet = if ($existingReplay) { $ReplayFile } else { Join-Path $output $(if ($MidGame) { 'midgame.fxpkt' } else { 'start.fxpkt' }) }
if ($existingReplay) { Copy-Item -LiteralPath $sourceLogPath -Destination (Join-Path $output 'source-recording.log') }
if ($setupScriptHash) {
  Copy-Item -LiteralPath $SetupScript -Destination (Join-Path $output 'setup.lua')
  if ((Get-FileHash -LiteralPath (Join-Path $output 'setup.lua') -Algorithm SHA256).Hash -ne $setupScriptHash) { throw 'Setup script changed while copying its snapshot.' }
}
if ($verifyScriptHash) {
  Copy-Item -LiteralPath $VerifyScript -Destination (Join-Path $output 'verify.lua')
  if ((Get-FileHash -LiteralPath (Join-Path $output 'verify.lua') -Algorithm SHA256).Hash -ne $verifyScriptHash) { throw 'Verification script changed while copying its snapshot.' }
}
$testConfig = Join-Path $runtime 'keeperfx-replay-test.cfg'
$backups = @{}
foreach ($path in @($testConfig, (Join-Path $runtime 'keeperfx.cfg'), (Join-Path $runtime 'save/settings.toml'), (Join-Path $runtime 'save/fx1lastf.sav'), (Join-Path $env:APPDATA 'keeperfx/keeperfx/renderer_prefs.ini'))) {
  $backups[$path] = if (Test-Path -LiteralPath $path) { [IO.File]::ReadAllBytes($path) } else { $null }
}
$saveDirectory = Join-Path $runtime 'save'
$saveFilesBefore = @()
$progressUnchanged = $null
$progressBefore = @()
$progressAfter = @()
$changedProgressFiles = @()
if ($terminalReplay) {
  $saveFilesBefore = @(Get-ChildItem -LiteralPath $saveDirectory -File -Recurse | ForEach-Object FullName)
  foreach ($path in $saveFilesBefore) { $backups[$path] = [IO.File]::ReadAllBytes($path) }
  $progressBefore = @(foreach ($path in $saveFilesBefore) {
    if ([IO.Path]::GetFileName($path) -ne 'settings.toml') { [ordered]@{path=$path;sha256=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash;bytes=$backups[$path].Length} }
  })
}
$config = Get-Content -LiteralPath (Join-Path $runtime 'keeperfx.cfg') -Raw
foreach ($pair in @{API_ENABLED='TRUE'; API_PORT='5599'; AUTOMATIC_REPLAYS='OFF'; REPLAY_MAX_SIZE='131072'; FREEZE_GAME_ON_FOCUS_LOST='OFF'; INGAME_RES='1280x720w32'; VSYNC='OFF'; CURSOR_EDGE_CAMERA_PANNING='OFF'; CAPTURE_CURSOR='OFF'}.GetEnumerator()) {
  $config = $config -replace ('(?m)^' + $pair.Key + '=.*$'), ($pair.Key + '=' + $pair.Value)
}
$runs = @()
$passed = $false
$oldStateTest = $env:KFX_STATE_TEST
$oldStateDirectory = $env:KFX_STATE_TEST_DIR
$oldSceneReference = $env:KFX_SCENE_REFERENCE
$oldSceneDirectory = $env:KFX_SCENE_TEST_DIR
try {
  $modes = if ($existingReplay) { @('playback') } else { @('record', 'playback') }
  foreach ($mode in $modes) {
    $backend = if ($mode -eq 'record') { $Renderer } else { $PlaybackRenderer }
    [IO.File]::WriteAllText($testConfig, ($config -replace '(?m)^RENDERER=.*$', "RENDERER=$backend"))
    $executable = Join-Path $runtime $(if ($backend -eq 'DX12') { 'keeperfx-dx12.exe' } else { 'keeperfx-local.exe' })
    $arguments = '-config keeperfx-replay-test.cfg -nointro -skipheartzoom -nosound'
    if ($verifyScriptHash) { $arguments += ' -alex' }
    $env:KFX_STATE_TEST = $null
    $env:KFX_STATE_TEST_DIR = $null
    if ($setupScriptHash) { $env:KFX_SCENE_REFERENCE = $null; $env:KFX_SCENE_TEST_DIR = $null }
    if ($mode -eq 'record') {
      $arguments += ' -campaign ' + $Campaign + ' -level ' + $Level
      $target = $recordTarget
      if ($MidGame) { $env:KFX_STATE_TEST = '1'; $env:KFX_STATE_TEST_DIR = $output }
      else { $arguments += ' -packetsave "' + $packet + '"' }
      if ($setupScriptHash) { $env:KFX_SCENE_REFERENCE = '1'; $env:KFX_SCENE_TEST_DIR = $output; if (-not $verifyScriptHash) { $arguments += ' -alex' } }
    }
    else { $arguments += ' -packetload "' + $packet + '" -fps ' + $PlaybackTurnsPerSecond; $target = if ($existingReplay) { $TargetTurn } else { $recordBaseTurn + $playbackTurnCount } }
    $game = Start-Process -FilePath $executable -WorkingDirectory $runtime -ArgumentList $arguments -WindowStyle Hidden -PassThru
    $forced = $false
    try {
      $ready = $false
      $timer = [Diagnostics.Stopwatch]::StartNew()
      while ($timer.Elapsed.TotalSeconds -lt 30) {
        if ($game.HasExited) { throw "KeeperFX exited early during $mode." }
        foreach ($listener in @(Get-NetTCPConnection -State Listen -ErrorAction Stop | Where-Object LocalPort -EQ 5599)) {
          if ($listener.OwningProcess -ne $game.Id -or $listener.LocalAddress -ne '127.0.0.1') { throw 'Replay API listener ownership/address mismatch.' }
          $ready = $true
        }
        if ($ready) { break }
        Start-Sleep -Milliseconds 250
      }
      if (-not $ready) { throw 'Replay API unavailable.' }
      $clientArguments = @((Join-Path $PSScriptRoot 'replay-test.mjs'), $target, (Join-Path $output "$mode.json"))
      $clientArguments += @('--level', $Level)
      if (-not $existingReplay) { $clientArguments += @('--campaign', $Campaign) }
      if ($MidGame -and $mode -eq 'record') { $clientArguments += '--midgame-record' }
      if ($setupScriptHash -and $mode -eq 'record') { $clientArguments += @('--setup-script', (Join-Path $output 'setup.lua')) }
      if ($terminalReplay) { $clientArguments += @('--finish-game-turn', $FinishGameTurn, '--expected-frontend', $ExpectedFrontend, '--log-file', (Join-Path $runtime 'keeperfx.log'), '--base-turn', $recordBaseTurn, '--integrity', $recordIntegrity) }
      if ($verifyScriptHash) { $clientArguments += @('--verify-script', (Join-Path $output 'verify.lua'), '--verify-hash', $verifyScriptHash.ToLowerInvariant(), '--verify-mode', $mode, '--log-file', (Join-Path $runtime 'keeperfx.log')) }
      $clientTimeout = if ($mode -eq 'playback') { $PlaybackTimeoutSeconds } else { $RecordTimeoutSeconds }
      $clientArguments += @('--timeout-seconds', $clientTimeout)
      $nodeExecutable = (Get-Command node -ErrorAction Stop).Source
      $quotedArguments = @($clientArguments | ForEach-Object { '"' + $_ + '"' })
      $client = Start-Process -FilePath $nodeExecutable -ArgumentList $quotedArguments -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $output "$mode-api.stdout.log") -RedirectStandardError (Join-Path $output "$mode-api.stderr.log")
      try {
        if (-not $client.WaitForExit($clientTimeout * 1000)) { throw "Replay $mode client exceeded its $clientTimeout-second bound." }
        $client.WaitForExit()
        if ($client.ExitCode -ne 0) { throw "Replay $mode failed." }
      } finally {
        if (-not $client.HasExited) { Stop-Process -Id $client.Id; $null = $client.WaitForExit(5000) }
      }
      if ($terminalReplay) { Write-Output 'Replay frontend reached; awaiting normal UI close' }
      $exitTimeout = if ($terminalReplay) { 90000 } else { 10000 }
      if (-not $game.WaitForExit($exitTimeout) -or $game.ExitCode -ne 0) { throw 'Replay test did not exit cleanly.' }
    } finally {
      if (-not $game.HasExited) { $forced = $true; Stop-Process -Id $game.Id; $game.WaitForExit() }
      Copy-Item -LiteralPath (Join-Path $runtime 'keeperfx.log') -Destination (Join-Path $output "$mode.log")
      $runs += [ordered]@{mode=$mode;renderer=$backend;binarySha256=(Get-FileHash -LiteralPath $executable -Algorithm SHA256).Hash;exitCode=$game.ExitCode;forced=$forced;apiOwnerVerified=$ready}
    }
    $log = Get-Content -LiteralPath (Join-Path $output "$mode.log") -Raw
    if ($log -match 'Out of sync|Cannot replay|Recorded resync could not|Lua error|Failed to execute Lua code|Segmentation|recording stopped|Replay reached|Packet File unreadable|replay aborted|level on disk differs') { throw "Replay errors found in $output/$mode.log" }
    if ($verifyScriptHash) {
      $verificationMarkers = [regex]::Matches($log, '(?m)^\[([0-9]+)\] lua_print: KFX_REPLAY_VERIFY_SCRIPT_OK\s+([0-9a-f]{64})\s+([0-9]+)\s+([0-9]+)\s+(record|playback)\r?$')
      $clientReport = Get-Content -LiteralPath (Join-Path $output "$mode.json") -Raw | ConvertFrom-Json
      if ($verificationMarkers.Count -ne 1 -or $verificationMarkers[0].Groups[2].Value -ne $verifyScriptHash.ToLowerInvariant() -or [int]$verificationMarkers[0].Groups[3].Value -ne $clientReport.verify.completedSteps -or [uint32]$verificationMarkers[0].Groups[4].Value -lt $target -or $verificationMarkers[0].Groups[5].Value -ne $mode) { throw 'Read-only Lua verification success marker absent or mismatched.' }
      $runs[-1].scriptVerification = $clientReport.verify
    }
    if ($terminalReplay) {
      if ($log -match 'Replay terminal outcome mismatch:|Replay terminal cleanup failed for player|Replay script player identity is invalid|process_dungeon_control_packet_clicks: Unrecognized[^\r\n]*work state|light_turn_light_on: Attempt to turn on unallocated light|move_object: Attempt to move non-existing object') { throw 'Terminal replay logged an outcome, script identity or cleanup failure.' }
      $terminalReceipts = [regex]::Matches($log, 'Replay terminal outcome: turn=([0-9]+) player=([0-9]+) simulated=(-?[0-9]+) recorded=(-?[0-9]+) verified=([0-9]+)\b')
      $expectedOutcome = if ($ExpectedFrontend -eq 'LEVEL_STATS') { 1 } else { 2 }
      if ($terminalReceipts.Count -ne 1 -or [uint32]$terminalReceipts[0].Groups[1].Value -ne $FinishGameTurn -or [int]$terminalReceipts[0].Groups[3].Value -ne $expectedOutcome -or [int]$terminalReceipts[0].Groups[4].Value -ne $expectedOutcome -or [int]$terminalReceipts[0].Groups[5].Value -ne 1) { throw 'Terminal replay outcome receipt does not match the recorded FinishGame.' }
    }
    if ($mode -eq 'record') {
      if ($log -notmatch 'Starting packet saving, turn ([0-9]+)') { throw 'Replay recording did not start.' }
      $recordBaseTurn = [uint32]$Matches[1]
      if ($recordTarget -lt $recordBaseTurn + $playbackTurnCount + 2) { throw 'Actual recording base leaves too few turns for the requested playback window.' }
      if ($MidGame -and $recordBaseTurn -lt 120) { throw 'Mid-game replay did not record a nonzero-turn snapshot.' }
      if ($MidGame) {
        if ($log -notmatch 'Replay recording: base_turn=([0-9]+) integrity=([0-9]+)') { throw 'Mid-game recording state receipt absent.' }
        if ([uint32]$Matches[1] -ne $recordBaseTurn) { throw 'Recording base-turn receipts disagree.' }
        $recordIntegrity = $Matches[2]
      }
    } else {
      if ($log -notmatch 'Replay startup: base_turn=([0-9]+) snapshot=([01]) checksum=1 integrity=([0-9]+)') { throw 'Replay startup/checksum verification not confirmed.' }
      if ([uint32]$Matches[1] -ne $recordBaseTurn -or [int]$Matches[2] -ne [int][bool]$MidGame) { throw 'Replay did not restore the recorded base turn and snapshot kind.' }
      if ($MidGame -and $Matches[3] -ne $recordIntegrity) { throw 'Mid-game startup integrity differs from the recorded snapshot.' }
      if ($log -notmatch 'Replay verification: base_turn=([0-9]+) verified=([0-9]+) mismatches=0\b') { throw 'Replay verified-turn summary absent or mismatched.' }
      $minimumVerifiedTurns = $target - $recordBaseTurn - $(if ($existingReplay) { 0 } else { 1 })
      if ([uint32]$Matches[1] -ne $recordBaseTurn -or [uint32]$Matches[2] -lt $minimumVerifiedTurns) { throw 'Replay verified too few turns.' }
      $runs[-1].verifiedTurns = [uint32]$Matches[2]
    }
    if ($backend -eq 'DX12') {
      if ($log -notmatch 'DX12 submitted first world frame' -or $log -notmatch 'DX12 session: submitted [1-9][0-9]*, rejected 0\b' -or $log -match 'DX12.*(?:rejected [1-9]|errors [1-9]|warnings [1-9]|initialization failed|validation failed|stopped)') { throw 'DX12 replay presentation failed.' }
      if ($log -match '(?im)^.*(?:\bError\b[^\r\n]*\bDX12\b|\bDX12\b[^\r\n]*\bError\b|Owned frame was rejected by DX12 composition|DX12 shutdown:).*$') { throw 'DX12 replay logged a rendering error.' }
    }
  }
  $passed = $true
} finally {
  $env:KFX_STATE_TEST = $oldStateTest
  $env:KFX_STATE_TEST_DIR = $oldStateDirectory
  $env:KFX_SCENE_REFERENCE = $oldSceneReference
  $env:KFX_SCENE_TEST_DIR = $oldSceneDirectory
  if ($terminalReplay) {
    # settings.toml is mutable presentation configuration; fx1lastf.sav is a
    # continue pointer and remains protected with saves, campaign and score files.
    $progressAfter = @(Get-ChildItem -LiteralPath $saveDirectory -File -Recurse | Where-Object Name -NE 'settings.toml' | ForEach-Object { [ordered]@{path=$_.FullName;sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash;bytes=$_.Length} })
    $progressUnchanged = $true
    foreach ($entry in $progressBefore) {
      $afterEntry = @($progressAfter | Where-Object { $_.path -eq $entry.path })
      if ($afterEntry.Count -ne 1 -or $afterEntry[0].sha256 -ne $entry.sha256) { $progressUnchanged=$false; $changedProgressFiles += $entry.path }
    }
    foreach ($entry in $progressAfter) {
      if ($entry.path -notin @($progressBefore | ForEach-Object { $_.path })) { $progressUnchanged=$false; $changedProgressFiles += $entry.path }
    }
    foreach ($path in @(Get-ChildItem -LiteralPath $saveDirectory -File -Recurse | ForEach-Object FullName)) {
      if ($path -notin $saveFilesBefore) { Remove-Item -LiteralPath $path }
    }
  }
  foreach ($path in $backups.Keys) {
    if ($null -ne $backups[$path]) { [IO.File]::WriteAllBytes($path, $backups[$path]) }
    elseif (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path }
  }
  $restored = $true
  foreach ($path in $backups.Keys) {
    if ($null -eq $backups[$path]) { $restored = $restored -and -not (Test-Path -LiteralPath $path) }
    else { $restored = $restored -and ([Convert]::ToBase64String([IO.File]::ReadAllBytes($path)) -eq [Convert]::ToBase64String($backups[$path])) }
  }
  if ($terminalReplay) {
    $restoredFiles = @(Get-ChildItem -LiteralPath $saveDirectory -File -Recurse | ForEach-Object FullName)
    $restored = $restored -and ($restoredFiles.Count -eq $saveFilesBefore.Count)
    foreach ($path in $restoredFiles) { $restored = $restored -and ($path -in $saveFilesBefore) }
  }
  $packetHash = if(Test-Path -LiteralPath $packet){(Get-FileHash -LiteralPath $packet -Algorithm SHA256).Hash}else{$null}
  $sourceUnchanged = -not $existingReplay -or ($packetHash -eq $sourcePacketHash -and (Get-FileHash -LiteralPath $sourceLogPath -Algorithm SHA256).Hash -eq $sourceLogHash)
  if ($setupScriptHash -and (Test-Path -LiteralPath (Join-Path $output 'record.json'))) { $setupCompletedSteps = (Get-Content -LiteralPath (Join-Path $output 'record.json') -Raw | ConvertFrom-Json).setup.completedSteps }
  [ordered]@{passed=($passed -and $restored -and $sourceUnchanged -and (-not $terminalReplay -or $progressUnchanged));architecture=$Architecture;midGame=[bool]$MidGame;existingReplay=$existingReplay;terminalReplay=$terminalReplay;finishGameTurn=if($terminalReplay){$FinishGameTurn}else{$null};expectedFrontend=$ExpectedFrontend;normalUiCloseTimeoutSeconds=if($terminalReplay){90}else{$null};progressUnchangedBeforeRestoration=$progressUnchanged;progressInventoryBefore=$progressBefore;progressInventoryAfterExit=$progressAfter;changedProgressFiles=$changedProgressFiles;progressExcludedFiles=if($terminalReplay){@('save/settings.toml')}else{@()};campaign=if($existingReplay){$null}else{$Campaign};level=$Level;recordTargetTurn=if($existingReplay){$null}else{$recordTarget};playbackTurns=if($existingReplay){$null}else{$playbackTurnCount};recordTimeoutSeconds=if($existingReplay){$null}else{$RecordTimeoutSeconds};sourceReplayFile=if($existingReplay){$ReplayFile}else{$null};sourceRecordingLog=$sourceLogPath;sourceRecordingLogSha256=$sourceLogHash;sourceUnchanged=$sourceUnchanged;targetTurn=if($existingReplay){$TargetTurn}else{$null};playbackTimeoutSeconds=$PlaybackTimeoutSeconds;playbackTurnsPerSecond=$PlaybackTurnsPerSecond;verifyScript=$VerifyScript;verifyScriptSha256=$verifyScriptHash;setupScript=$SetupScript;setupScriptSha256=$setupScriptHash;setupCompletedSteps=$setupCompletedSteps;recordBaseTurn=$recordBaseTurn;recordIntegrity=$recordIntegrity;runs=$runs;byteExactRestoration=$restored;packetSha256=$packetHash} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $output 'verification.json') -Encoding utf8
  if (-not $restored) { throw 'Runtime restoration failed.' }
  if (-not $sourceUnchanged) { throw 'Original replay or recording log changed during playback.' }
  if ($terminalReplay -and -not $progressUnchanged) { throw 'Terminal replay changed save/progression files before restoration.' }
}
Write-Output "Replay recording/playback passed: $output"
