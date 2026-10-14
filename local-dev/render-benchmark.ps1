# SPDX-License-Identifier: GPL-2.0-or-later
# KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
param(
    [ValidateSet('ON','OFF')][string]$VSync = 'OFF',
    [ValidateSet('x86','x64')][string]$Architecture = 'x64',
    [ValidateSet('OPENGL','DX12')][string]$Renderer = 'OPENGL',
    [ValidatePattern('^[A-Za-z0-9_-]+$')][string]$Campaign = 'keeporig',
    [ValidateRange(1,65535)][int]$Level = 1,
    [ValidateRange(1,120)][int]$WarmupSeconds = 30,
    [ValidateRange(1,300)][int]$SampleSeconds = 120,
    [string]$SetupScript,
    [string]$ObserveScript,
    [ValidateRange(1,40)][int]$Cycles = 1,
    [ValidateRange(0,2)][int]$Reloads = 0,
    [ValidateRange(15,120)][int]$SessionLimitMinutes = 15,
    [switch]$AwaitMeasurementStart,
    [switch]$Sound,
    [ValidateRange(1,200)][int]$TurnsPerSecond = 20
)
$ErrorActionPreference = 'Stop'
if ($Renderer -eq 'DX12' -and $Architecture -ne 'x64') { throw 'DX12 requires x64.' }
if ($AwaitMeasurementStart -and $Renderer -ne 'DX12') { throw 'AwaitMeasurementStart requires live DX12.' }
if ($ObserveScript -and ($Renderer -ne 'DX12' -or -not $SetupScript)) { throw 'ObserveScript requires a live DX12 SetupScript workload.' }
if (($Cycles -gt 1 -or $Reloads -gt 0) -and $Renderer -ne 'DX12') { throw 'Cycles and Reloads require live DX12.' }
if ($Reloads -gt 0 -and $Cycles -lt 3) { throw 'Reloads require at least three capture cycles.' }
$clientLimit = if ($Renderer -eq 'DX12') { $Cycles * ($WarmupSeconds + $SampleSeconds + $(if($Cycles -gt 1){5}else{0})) + $Reloads * 35 + $(if($ObserveScript){$WarmupSeconds}else{0}) + 90 } else { 450 }
if ($Renderer -eq 'DX12' -and ($clientLimit + $(if($AwaitMeasurementStart){120}else{0})) -gt ($SessionLimitMinutes * 60 - 50)) { throw "Requested cycles/warm-up/reloads/start handoff exceed the $SessionLimitMinutes-minute total run budget." }
if ($Renderer -eq 'OPENGL' -and ($Campaign -ne 'keeporig' -or $Level -ne 1 -or $SetupScript -or $PSBoundParameters.ContainsKey('WarmupSeconds') -or $PSBoundParameters.ContainsKey('SampleSeconds'))) { throw 'The existing OpenGL baseline uses paused keeporig level 1 and fixed frame counts; live controls require DX12.' }
$workspace = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$runtime = Join-Path $workspace 'runtime/keeperfx'
if ($Architecture -eq 'x64') { $runtime += '-x64' }
$testSave = Join-Path $runtime 'save/fx1g9997.sav'
if ($Reloads -gt 0 -and (Test-Path -LiteralPath $testSave)) { throw 'Save slot 9997 is occupied; refusing to overwrite it.' }
if (Get-Process -Name keeperfx,keeperfx-local,keeperfx-local-hvlog,keeperfx-dx12,keeperfx-dx12-hvlog -ErrorAction SilentlyContinue) { throw 'Close KeeperFX before benchmarking.' }
if (@(Get-NetTCPConnection -State Listen -ErrorAction Stop | Where-Object LocalPort -EQ 5599).Count) { throw 'API port 5599 is occupied.' }
$executable = Join-Path $runtime $(if ($Renderer -eq 'DX12') { 'keeperfx-dx12.exe' } else { 'keeperfx-local.exe' })
if (-not (Test-Path -LiteralPath $executable)) { throw "Executable missing: $executable" }
$nodeExecutable = (Get-Command node -ErrorAction Stop).Source
$campaignText = Get-Content -LiteralPath (Join-Path $runtime "campgns/$Campaign.cfg") -Raw
$levelLists = [regex]::Matches($campaignText, '(?mi)^\s*(?:SINGLE_LEVELS|BONUS_LEVELS|EXTRA_LEVELS)\s*=([^;\r\n]*)')
$levels = @($levelLists | ForEach-Object { [regex]::Matches($_.Groups[1].Value, '\b[0-9]+\b') } | ForEach-Object { [int]$_.Value })
if ($Level -notin $levels) { throw "Level $Level is not listed in installed campaign $Campaign." }
$setupHash = $null
if ($SetupScript) { $SetupScript = (Resolve-Path -LiteralPath $SetupScript).Path; $setupHash = (Get-FileHash -LiteralPath $SetupScript -Algorithm SHA256).Hash }
$observeHash = $null
if ($ObserveScript) { $ObserveScript = (Resolve-Path -LiteralPath $ObserveScript).Path; $observeHash = (Get-FileHash -LiteralPath $ObserveScript -Algorithm SHA256).Hash }
$out = Join-Path $workspace ('artifacts/foundation/benchmark-' + $Renderer + '-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
New-Item -ItemType Directory -Path $out | Out-Null
if ($setupHash) {
    Copy-Item -LiteralPath $SetupScript -Destination (Join-Path $out 'setup.lua')
    if ((Get-FileHash -LiteralPath (Join-Path $out 'setup.lua') -Algorithm SHA256).Hash -ne $setupHash) { throw 'Setup changed while copying its snapshot.' }
}
if ($observeHash) {
    Copy-Item -LiteralPath $ObserveScript -Destination (Join-Path $out 'observe.lua')
    if ((Get-FileHash -LiteralPath (Join-Path $out 'observe.lua') -Algorithm SHA256).Hash -ne $observeHash) { throw 'Observer changed while copying its snapshot.' }
}
$displayGuard = [ordered]@{required=($VSync -eq 'ON');before=$null;after=$null;scope=if($VSync -eq 'ON'){'Observed session ON at pre/post endpoints; absent console notification allowed only with successful registration and no contradictory console state. Continuous display ON is not proven.'}else{'VSync OFF resource/headroom measurement; no visible display cadence qualification.'}}
function Get-BenchmarkDisplayState([string]$phase) {
    $checkDirectory = Join-Path $out ('display-' + $phase)
    New-Item -ItemType Directory -Path $checkDirectory | Out-Null
    $checker = Join-Path $workspace 'artifacts/dx12/bin/keeperfx-dx12.exe'
    $reportPath = Join-Path $checkDirectory 'display-power.json'
    $session = [ordered]@{phase=$phase;startedUtc=[DateTime]::UtcNow.ToString('o');executable=$checker;executableSHA256=$null;processId=$null;arguments=@('--display-power-check','--output',$checkDirectory);timeoutSeconds=5;scope='Hidden native display-state check; no game or GPU initialization.'}
    $check = $null; $forcedCheck = $false; $checkFailure = $null; $observation = $null
    $sessionPath = Join-Path $checkDirectory 'session.json'
    $session | ConvertTo-Json | Set-Content -LiteralPath $sessionPath -Encoding utf8
    try {
        if (-not (Test-Path -LiteralPath $checker)) { throw "Display-state checker missing: $checker" }
        $session.executableSHA256 = (Get-FileHash -LiteralPath $checker -Algorithm SHA256).Hash
        $check = Start-Process -FilePath $checker -WorkingDirectory $workspace -WindowStyle Hidden -PassThru -ArgumentList @('--display-power-check','--output',('"' + $checkDirectory + '"')) -RedirectStandardOutput (Join-Path $checkDirectory 'stdout.log') -RedirectStandardError (Join-Path $checkDirectory 'stderr.log')
        $session.processId = $check.Id
        $session | ConvertTo-Json | Set-Content -LiteralPath $sessionPath -Encoding utf8
        if (-not $check.WaitForExit(5000)) { throw 'Display-state checker exceeded its 5-second startup/observation bound.' }
        $check.WaitForExit()
        if (-not (Test-Path -LiteralPath $reportPath)) { throw "Display-state checker exited $($check.ExitCode) without display-power.json." }
        $observation = Get-Content -LiteralPath $reportPath -Raw | ConvertFrom-Json
        if ($observation.schema -ne 1 -or $observation.eligible -isnot [bool] -or $observation.consoleDisplay -notin @(-1,1) -or $observation.sessionDisplay -ne 1 -or $observation.consoleRegistrationError -ne 0 -or $observation.sessionRegistrationError -ne 0 -or -not $observation.eligible -or $check.ExitCode -ne 0) {
            throw "Display-state check ineligible: exit $($check.ExitCode); console $($observation.consoleDisplay), session $($observation.sessionDisplay), registration errors $($observation.consoleRegistrationError)/$($observation.sessionRegistrationError), remote $($observation.remoteSession); $($observation.reason). ON=1, OFF=0, dim=2, unknown=-1."
        }
        if ((Get-FileHash -LiteralPath $checker -Algorithm SHA256).Hash -ne $session.executableSHA256) { throw 'Display-state checker binary changed during the check.' }
    } catch { $checkFailure = $_.Exception.Message }
    finally {
        if ($check -and -not $check.HasExited) {
            try { $forcedCheck=$true; Stop-Process -Id $check.Id -ErrorAction Stop; if (-not $check.WaitForExit(5000)) { throw 'Owned display-state checker cleanup timed out.' } }
            catch { $checkFailure = "$checkFailure Cleanup: $($_.Exception.Message)" }
        }
    }
    $result = [ordered]@{passed=($null -eq $checkFailure -and -not $forcedCheck);phase=$phase;completedUtc=[DateTime]::UtcNow.ToString('o');processId=$session.processId;executable=$checker;executableSHA256=$session.executableSHA256;exitCode=if($check -and $check.HasExited){$check.ExitCode}else{$null};forced=$forcedCheck;failure=$checkFailure;observation=$observation;consoleNotificationAbsent=($observation -and $observation.consoleDisplay -eq -1);report=$reportPath;reportSHA256=if(Test-Path -LiteralPath $reportPath){(Get-FileHash -LiteralPath $reportPath -Algorithm SHA256).Hash}else{$null};scope='Endpoint observation only; not proof of continuous ON or scanout pacing. Remote-session state is retained metadata.'}
    $resultPath = Join-Path $checkDirectory 'result.json'
    $result | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $resultPath -Encoding utf8
    $result.Add('result',$resultPath); $result.Add('resultSHA256',(Get-FileHash -LiteralPath $resultPath -Algorithm SHA256).Hash)
    return $result
}
if ($displayGuard.required) {
    $displayGuard.before = Get-BenchmarkDisplayState 'before'
    if (-not $displayGuard.before.passed) {
        [ordered]@{renderer=$Renderer;vsync=$VSync;displayState=$displayGuard;runtimeMutated=$false;failure=$displayGuard.before.failure} | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $out 'run-metadata.json') -Encoding utf8
        throw "VSync ON benchmark refused before runtime/config mutation: $($displayGuard.before.failure) Evidence: $out"
    }
}
$originalConfig = Join-Path $runtime 'keeperfx.cfg'
$testConfig = Join-Path $runtime 'keeperfx-benchmark.cfg'
$prefsPath = Join-Path $env:APPDATA 'keeperfx/keeperfx/renderer_prefs.ini'
$saveDirectory = Join-Path $runtime 'save'
function Get-BenchmarkSaveFiles([string]$directory) {
    $resolved=[IO.Path]::GetFullPath($directory)
    $saveRoot=[IO.Path]::GetFullPath($saveDirectory)
    if ($resolved -ne $saveRoot -and -not $resolved.StartsWith($saveRoot+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)) { throw 'Save scan escaped its intended directory.' }
    if ((Get-Item -LiteralPath $directory -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Save scan refuses a reparse point.' }
    foreach ($entry in @(Get-ChildItem -LiteralPath $directory -Force)) {
        if ($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Save scan refuses a reparse point.' }
        if ($entry.PSIsContainer) { Get-BenchmarkSaveFiles $entry.FullName } else { $entry }
    }
}
if ((Get-Item -LiteralPath $runtime -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Runtime directory is a reparse point.' }
$saveFilesBefore = @(Get-BenchmarkSaveFiles $saveDirectory | ForEach-Object FullName)
$backupPaths = @($testConfig,$originalConfig,$prefsPath) + $saveFilesBefore
$expectedTestSaveFiles = @('settings.toml','fx1lastf.sav','fx1contn.sav',"$Campaign.cid") | ForEach-Object { Join-Path $saveDirectory $_ }
$backupPaths += $expectedTestSaveFiles
$score = [regex]::Match($campaignText, '(?mi)^\s*HIGH_SCORES\s*=\s*[0-9]+\s+([^;\s]+)')
if ($score.Success) { $scorePath=[IO.Path]::GetFullPath((Join-Path $saveDirectory $score.Groups[1].Value));if(-not $scorePath.StartsWith([IO.Path]::GetFullPath($saveDirectory)+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)){throw 'Campaign score filename escapes save directory.'};$backupPaths+=$scorePath;$expectedTestSaveFiles+=$scorePath }
if ($Reloads -gt 0) { $backupPaths += $testSave; $expectedTestSaveFiles += $testSave }
$backups = @{}
foreach ($path in $backupPaths) { $backups[$path] = if (Test-Path -LiteralPath $path) { [IO.File]::ReadAllBytes($path) } else { $null } }
$config = Get-Content -LiteralPath $originalConfig -Raw
$settings = [ordered]@{API_ENABLED='TRUE';API_PORT='5599';RENDERER=$Renderer;VSYNC=$VSync;FREEZE_GAME_ON_FOCUS_LOST='OFF';AUTOMATIC_REPLAYS='OFF';FRAMES_PER_SECOND='0';INGAME_RES='3840x2160w32';CAPTURE_CURSOR='OFF';CURSOR_EDGE_CAMERA_PANNING='OFF'}
foreach ($key in $settings.Keys) {
    $pattern = '(?m)^' + $key + '=.*$'
    if ($config -notmatch $pattern) { throw "Missing configuration key: $key" }
    $config = $config -replace $pattern, ($key + '=' + $settings[$key])
}
$envNames = @('KFX_RENDER_BENCHMARK_DIR','KFX_RENDER_BENCHMARK_WARMUP_SECONDS','KFX_RENDER_BENCHMARK_SAMPLE_SECONDS','KFX_DX12_PERFORMANCE','KFX_DX12_GPU_VALIDATION','KFX_DX12_FAIL_AFTER_SUBMISSIONS','KFX_DX12_REMOVE_AFTER_SUBMISSIONS','KFX_DX12_FAILURE_NO_DIALOG','KFX_SCENE_TEST_DIR','KFX_SCENE_REFERENCE','KFX_SCENE_LIVE_SURFACES','KFX_STATE_TEST','KFX_STATE_TEST_DIR')
$oldEnvironment = @{}
foreach ($key in $envNames) { $oldEnvironment[$key] = [Environment]::GetEnvironmentVariable($key) }
$meta = [ordered]@{createdAt=(Get-Date).ToUniversalTime().ToString('o');sourceRevision=(& git -C (Split-Path -Parent $PSScriptRoot) rev-parse HEAD);sourceStatus=@(& git -C (Split-Path -Parent $PSScriptRoot) status --short);executable=$executable;executableSHA256=(Get-FileHash -LiteralPath $executable -Algorithm SHA256).Hash;normalConfigSHA256=(Get-FileHash -LiteralPath $originalConfig -Algorithm SHA256).Hash;renderer=$Renderer;architecture=$Architecture;vsync=$VSync;campaign=$Campaign;level=$Level;resolution=@(3840,2160);sound=[bool]$Sound;turnsPerSecond=$TurnsPerSecond;warmupSeconds=if($Renderer -eq 'DX12'){$WarmupSeconds}else{$null};sampleSeconds=if($Renderer -eq 'DX12'){$SampleSeconds}else{$null};cycles=$Cycles;reloads=$Reloads;measuredSeconds=if($Renderer -eq 'DX12'){$Cycles*$SampleSeconds}else{$null};sessionLimitMinutes=$SessionLimitMinutes;clientWallLimitSeconds=$clientLimit;pauseTurn=if($Renderer -eq 'OPENGL'){120}else{$null};setupScript=$SetupScript;setupScriptSHA256=$setupHash;observeScript=$ObserveScript;observeScriptSHA256=$observeHash;observerWarmupSeconds=if($observeHash){$WarmupSeconds}else{0};scope=if($Renderer -eq 'DX12'){'Live campaign/prepared workload candidate; no demanding-map, FPS or Gate 8 acceptance implied.'}else{'Existing paused Eversmile OpenGL reference.'};performanceRequested=($Renderer -eq 'DX12');debugRequested=if($Renderer -eq 'DX12'){$false}else{$null};gpuValidationRequested=if($Renderer -eq 'DX12'){$false}else{$null}}
$meta.Add('displayState',$displayGuard)
$meta.Add('measurementHandoff',[ordered]@{requested=[bool]$AwaitMeasurementStart;timeoutSeconds=120;ready=$null;accepted=$false;acceptedUtc=$null;acceptedQpc=$null})
$meta | ConvertTo-Json -Depth 9 | Set-Content -LiteralPath (Join-Path $out 'run-metadata.json') -Encoding utf8
$game = $null; $client = $null; $ready = $false; $passed = $false; $forced = $false; $failure = $null; $observedListeners = @()
try {
    [IO.File]::WriteAllText($testConfig,$config)
    Copy-Item -LiteralPath $testConfig -Destination (Join-Path $out 'keeperfx-benchmark.cfg')
    foreach ($key in $envNames) { [Environment]::SetEnvironmentVariable($key,$null) }
    $env:KFX_RENDER_BENCHMARK_DIR = $out
    if ($Renderer -eq 'DX12') {
        $env:KFX_DX12_PERFORMANCE = '1'
        $env:KFX_RENDER_BENCHMARK_WARMUP_SECONDS = [string]$WarmupSeconds
        $env:KFX_RENDER_BENCHMARK_SAMPLE_SECONDS = [string]$SampleSeconds
    }
    $arguments = '-config keeperfx-benchmark.cfg -nointro -skipheartzoom -campaign ' + $Campaign + ' -level ' + $Level + ' -fps ' + $TurnsPerSecond
    if (-not $Sound) { $arguments += ' -nosound' }
    if ($Renderer -eq 'OPENGL') { $arguments += ' -pause_at_gameturn 120' }
    if ($setupHash -or $Cycles -gt 1) { $env:KFX_SCENE_REFERENCE='1';$env:KFX_SCENE_TEST_DIR=$out;$arguments += ' -alex' }
    $game = Start-Process -FilePath $executable -WorkingDirectory $runtime -WindowStyle Hidden -PassThru -ArgumentList $arguments
    $timer = [Diagnostics.Stopwatch]::StartNew()
    while ($timer.Elapsed.TotalSeconds -lt 30) {
        if ($game.HasExited) { throw 'KeeperFX exited before the benchmark API started.' }
        $listeners = @(Get-NetTCPConnection -State Listen -ErrorAction Stop | Where-Object LocalPort -EQ 5599)
        $observedListeners = @($listeners | ForEach-Object { [ordered]@{address=$_.LocalAddress;processId=$_.OwningProcess;port=$_.LocalPort} })
        foreach ($listener in $listeners) {
            if ($listener.OwningProcess -ne $game.Id -or $listener.LocalAddress -ne '127.0.0.1') { throw "Benchmark API listener mismatch: expected PID $($game.Id) 127.0.0.1:5599; observed PID $($listener.OwningProcess) $($listener.LocalAddress):$($listener.LocalPort)." }
            $ready = $true
        }
        if ($ready) { break }
        Start-Sleep -Milliseconds 250
    }
    if (-not $ready) { throw 'Benchmark API did not start within 30 seconds.' }
    if ($AwaitMeasurementStart) {
        $handoff=[ordered]@{schema=1;gameProcessId=$game.Id;executable=$executable;executableSHA256=$meta.executableSHA256;nonce=[Guid]::NewGuid().ToString('N');createdUtc=[DateTime]::UtcNow.ToString('o');createdQpc=[Diagnostics.Stopwatch]::GetTimestamp();qpcFrequency=[Diagnostics.Stopwatch]::Frequency;scope='Owned API ready; client/setup/benchmark has not started. No pause or foreground operation is performed.'}
        $readyPath=Join-Path $out 'measurement-ready.json'
        $startPath=Join-Path $out 'measurement-start.json'
        $meta.measurementHandoff.ready=$handoff
        $meta | ConvertTo-Json -Depth 9 | Set-Content -LiteralPath (Join-Path $out 'run-metadata.json') -Encoding utf8
        $handoff | ConvertTo-Json | Set-Content -LiteralPath $readyPath -Encoding utf8
        Write-Output "Measurement ready: $readyPath; write gameProcessId and nonce to $startPath within 120 seconds."
        $handoffTimer=[Diagnostics.Stopwatch]::StartNew()
        while ($handoffTimer.Elapsed.TotalSeconds -lt 120) {
            if ($game.HasExited) { throw 'Owned game exited while awaiting measurement start.' }
            if (Test-Path -LiteralPath $startPath) {
                $startFile=Get-Item -LiteralPath $startPath -Force
                if ($startFile.PSIsContainer -or ($startFile.Attributes -band [IO.FileAttributes]::ReparsePoint) -or $startFile.Length -gt 8192) { throw 'Unsafe measurement-start.json refused.' }
                $signal=$null
                try { $signal=Get-Content -LiteralPath $startPath -Raw | ConvertFrom-Json } catch { $signal=$null }
                if ($null -ne $signal) {
                    if ($signal.gameProcessId -ne $game.Id -or $signal.nonce -cne $handoff.nonce) { throw 'Measurement start PID/nonce does not match this owned run.' }
                    $meta.measurementHandoff.accepted=$true
                    $meta.measurementHandoff.acceptedUtc=[DateTime]::UtcNow.ToString('o')
                    $meta.measurementHandoff.acceptedQpc=[Diagnostics.Stopwatch]::GetTimestamp()
                    $meta | ConvertTo-Json -Depth 9 | Set-Content -LiteralPath (Join-Path $out 'run-metadata.json') -Encoding utf8
                    break
                }
            }
            Start-Sleep -Milliseconds 100
        }
        if (-not $meta.measurementHandoff.accepted) { throw 'Measurement start signal unavailable within 120 seconds.' }
    }
    $nodeArgs = @((Join-Path $PSScriptRoot 'render-benchmark.mjs'),$out)
    if ($Renderer -eq 'DX12') {
        $nodeArgs += @('--live','--level',$Level,'--campaign',$Campaign,'--warmup-seconds',$WarmupSeconds,'--sample-seconds',$SampleSeconds,'--vsync',$VSync)
        if ($setupHash) { $nodeArgs += @('--setup-script',(Join-Path $out 'setup.lua')) }
        if ($observeHash) { $nodeArgs += @('--observe-script',(Join-Path $out 'observe.lua'),'--observe-hash',$observeHash.ToLowerInvariant()) }
        if ($Cycles -gt 1) { $nodeArgs += @('--cycles',$Cycles,'--reloads',$Reloads,'--runtime',$runtime,'--process-id',$game.Id,'--wall-seconds',$clientLimit) }
    }
    $quotedArgs = @($nodeArgs | ForEach-Object { '"' + $_ + '"' })
    $client = Start-Process -FilePath $nodeExecutable -ArgumentList $quotedArgs -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $out 'api.stdout.log') -RedirectStandardError (Join-Path $out 'api.stderr.log')
    if ($Cycles -gt 1) {
        $clientTimer=[Diagnostics.Stopwatch]::StartNew()
        $lastMemorySequence=0
        while (-not $client.WaitForExit(100)) {
            if ($clientTimer.Elapsed.TotalSeconds -gt $clientLimit) { throw "Benchmark client exceeded its $clientLimit-second bound." }
            $requestPath=Join-Path $out 'process-memory-request.json'
            $memoryRequest=$null
            if (Test-Path -LiteralPath $requestPath) {
                try { $memoryRequest=Get-Content -LiteralPath $requestPath -Raw | ConvertFrom-Json } catch { $memoryRequest=$null }
            }
            if ($memoryRequest -and $memoryRequest.sequence -ne $lastMemorySequence) {
                if ($memoryRequest.processId -ne $game.Id -or $memoryRequest.sequence -le $lastMemorySequence -or $memoryRequest.sequence -gt 100 -or $memoryRequest.label -notmatch '^(initial|cycle-[0-9]{2}|reload-[12])$') { throw 'Process-memory request does not match the owned benchmark process/phase.' }
                $game.Refresh()
                if ($game.HasExited -or $game.MainModule.FileName -ne $executable) { throw 'Owned game identity unavailable for process-memory sampling.' }
                $memorySample=[ordered]@{sequence=$memoryRequest.sequence;processId=$game.Id;label=$memoryRequest.label;observedUtc=[DateTime]::UtcNow.ToString('o');privateBytes=$game.PrivateMemorySize64;workingSetBytes=$game.WorkingSet64;peakWorkingSetBytes=$game.PeakWorkingSet64;scope='Owned process sample while Node holds the game paused outside benchmark capture; OS/process heaps and caches included.'}
                [IO.File]::WriteAllText((Join-Path $out 'process-memory-response.json'),($memorySample|ConvertTo-Json))
                $lastMemorySequence=$memoryRequest.sequence
            }
        }
    } elseif (-not $client.WaitForExit($clientLimit * 1000)) { throw "Benchmark client exceeded its $clientLimit-second bound." }
    $client.WaitForExit()
    if ($client.ExitCode -ne 0) { throw 'Render benchmark failed; inspect api.stderr.log.' }
    if (-not $game.WaitForExit(10000) -or $game.ExitCode -ne 0) { throw 'Benchmark game did not exit normally.' }
    $log = Get-Content -LiteralPath (Join-Path $runtime 'keeperfx.log') -Raw
    if ($Renderer -eq 'DX12') {
        $adapter = [regex]::Matches($log, 'DX12 adapter: (?<name>[^\r\n]+) \(vendor (?<vendor>[0-9]+)\); debug (?<debug>[01]); GPU validation (?<validation>[01])')
        if ($adapter.Count -ne 1) { throw 'Benchmark device identity was not uniquely recorded.' }
        $identity = $adapter[0].Groups
        [ordered]@{adapter=$identity['name'].Value;vendorId=[uint32]$identity['vendor'].Value;debug=($identity['debug'].Value -eq '1');gpuValidation=($identity['validation'].Value -eq '1');processId=$game.Id;binarySHA256=$meta.executableSHA256;source='Device-reported initialization diagnostics'} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $out 'device.json') -Encoding utf8
        if ($identity['debug'].Value -ne '0' -or $identity['validation'].Value -ne '0') { throw 'Performance device diagnostics were not disabled.' }
    }
    if ($setupHash -and $log -match 'Failed to execute Lua code|Lua error') { throw 'Benchmark setup logged a Lua error.' }
    if ($observeHash) {
        $markers = [regex]::Matches($log, '(?m)^\[([0-9]+)\] lua_print: KFX_BENCHMARK_OBSERVE_OK\s+([0-9a-f]{64})\s+(before|after)\s+([0-9]+)\s+([0-9]+)\r?$')
        if ($markers.Count -ne 2 -or $markers[0].Groups[2].Value -ne $observeHash.ToLowerInvariant() -or $markers[1].Groups[2].Value -ne $observeHash.ToLowerInvariant() -or $markers[0].Groups[3].Value -ne 'before' -or $markers[1].Groups[3].Value -ne 'after') { throw 'Printed benchmark observer markers absent or mismatched.' }
    }
    if ($Renderer -eq 'DX12' -and ($log -notmatch 'DX12 submitted first world frame' -or $log -notmatch 'DX12 session: submitted [1-9][0-9]*, rejected 0\b' -or $log -match '(?im)^.*(?:\bError\b[^\r\n]*\bDX12\b|\bDX12\b[^\r\n]*\bError\b|DX12 shutdown:|DX12[^\r\n]*(?:stopped|validation failed)).*$')) { throw 'DX12 benchmark logged a rendering/session failure.' }
    if ($displayGuard.required) {
        $displayGuard.after = Get-BenchmarkDisplayState 'after'
        if (-not $displayGuard.after.passed) { throw "VSync ON benchmark post-run display state refused: $($displayGuard.after.failure)" }
    }
    $passed = $true
} catch { $failure = $_.Exception.Message; throw }
finally {
    $cleanupErrors = @()
    $unexpectedNewSaveFiles = @()
    $saveTreeSafe = $true
    try { if ($client -and -not $client.HasExited) { Stop-Process -Id $client.Id; $null=$client.WaitForExit(5000) } } catch { $cleanupErrors += $_.Exception.Message }
    try { if ($game -and -not $game.HasExited) { $forced=$true;Stop-Process -Id $game.Id;if(-not $game.WaitForExit(5000)){throw 'Game cleanup timed out.'} } } catch { $cleanupErrors += $_.Exception.Message }
    try { if ($game -and (Test-Path -LiteralPath (Join-Path $runtime 'keeperfx.log'))) { Copy-Item -LiteralPath (Join-Path $runtime 'keeperfx.log') -Destination (Join-Path $out 'keeperfx.log') } } catch { $cleanupErrors += $_.Exception.Message }
    foreach ($key in $oldEnvironment.Keys) { [Environment]::SetEnvironmentVariable($key,$oldEnvironment[$key]) }
    if ($Cycles -gt 1) {
        try {
            $currentSaveFiles = @(Get-BenchmarkSaveFiles $saveDirectory | ForEach-Object FullName)
            if ($Reloads -gt 0 -and (Test-Path -LiteralPath $testSave) -and (Test-Path -LiteralPath (Join-Path $out 'soak-save-requested.json'))) { Copy-Item -LiteralPath $testSave -Destination (Join-Path $out 'soak-save-9997.sav') }
            foreach ($path in $currentSaveFiles) {
                if ($path -notin $saveFilesBefore) {
                    if ($path -in $expectedTestSaveFiles -and $null -eq $backups[$path]) { Remove-Item -LiteralPath $path }
                    else { $unexpectedNewSaveFiles += $path; $cleanupErrors += "Unexpected new save file retained: $path" }
                }
            }
        } catch { $saveTreeSafe=$false; $cleanupErrors += $_.Exception.Message }
    }
    foreach ($path in $backups.Keys) {
        if (-not $saveTreeSafe -and $path.StartsWith([IO.Path]::GetFullPath($saveDirectory)+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)) { $cleanupErrors += "Unsafe save-tree restoration skipped: $path"; continue }
        try { if ($null -ne $backups[$path]) { [IO.File]::WriteAllBytes($path,$backups[$path]) } elseif(Test-Path -LiteralPath $path){Remove-Item -LiteralPath $path} } catch { $cleanupErrors += $_.Exception.Message }
    }
    $restored = $cleanupErrors.Count -eq 0
    foreach ($path in $backups.Keys) {
        try { if ($null -eq $backups[$path]) { $restored=$restored -and -not(Test-Path -LiteralPath $path) } else { $restored=$restored -and ([Convert]::ToBase64String([IO.File]::ReadAllBytes($path)) -eq [Convert]::ToBase64String($backups[$path])) } } catch { $restored=$false;$cleanupErrors += $_.Exception.Message }
    }
    if ($Cycles -gt 1) {
        $saveFilesAfter = @(Get-BenchmarkSaveFiles $saveDirectory | ForEach-Object FullName)
        $restored = $restored -and ($saveFilesAfter.Count -eq $saveFilesBefore.Count)
        foreach ($path in $saveFilesAfter) { $restored=$restored -and ($path -in $saveFilesBefore) }
    }
    try { $meta | ConvertTo-Json -Depth 9 | Set-Content -LiteralPath (Join-Path $out 'run-metadata.json') -Encoding utf8 } catch { $restored=$false; $cleanupErrors += "Benchmark metadata write failed: $($_.Exception.Message)" }
    [ordered]@{passed=($passed -and $restored -and -not $forced);renderer=$Renderer;gameProcessId=if($game){$game.Id}else{$null};apiOwnerVerified=$ready;listeners=$observedListeners;exitCode=if($game -and $game.HasExited){$game.ExitCode}else{$null};forced=$forced;byteExactRestoration=$restored;failure=$failure;cleanupErrors=$cleanupErrors;unexpectedNewSaveFiles=$unexpectedNewSaveFiles;displayStateRequired=$displayGuard.required;displayBeforePassed=if($displayGuard.before){$displayGuard.before.passed}else{$null};displayAfterPassed=if($displayGuard.after){$displayGuard.after.passed}else{$null};displayScope=$displayGuard.scope;gate8Accepted=$false} | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $out 'lifecycle.json') -Encoding utf8
    if (-not $restored) { throw "Benchmark restoration failed: $($cleanupErrors -join '; ')" }
}
Write-Output "Benchmark evidence: $out"
