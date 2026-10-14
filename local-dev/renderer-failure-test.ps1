# SPDX-License-Identifier: GPL-2.0-or-later
# KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
param(
  [ValidateSet('Exception','RemoveDevice')][string]$Mode = 'Exception',
  [ValidateRange(30, 10000)][int]$AfterSubmissions = 120,
  [ValidateRange(15, 180)][int]$TimeoutSeconds = 90
)
$ErrorActionPreference = 'Stop'
$workspace = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$repo = Split-Path -Parent $PSScriptRoot
$runtime = Join-Path $workspace 'runtime/keeperfx-x64'
$executable = Join-Path $runtime 'keeperfx-dx12.exe'
if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) { throw 'Build the x64 DX12 executable first.' }
if (Get-Process -Name keeperfx,keeperfx-local,keeperfx-local-hvlog,keeperfx-dx12,keeperfx-dx12-hvlog -ErrorAction SilentlyContinue) {
  throw 'Close KeeperFX before this check; no existing process will be stopped.'
}
$output = Join-Path $workspace ('artifacts/renderer-failure/DX12-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
New-Item -ItemType Directory -Path $output -ErrorAction Stop | Out-Null
$saveRoot = [IO.Path]::GetFullPath((Join-Path $runtime 'save'))
$savePrefix = $saveRoot.TrimEnd('\') + '\'
$normalConfig = Join-Path $runtime 'keeperfx.cfg'
$testConfig = Join-Path $runtime 'keeperfx-renderer-failure.cfg'
$prefs = Join-Path $env:APPDATA 'keeperfx/keeperfx/renderer_prefs.ini'
$gameLog = Join-Path $runtime 'keeperfx.log'

function Get-SaveFiles {
  if (-not (Test-Path -LiteralPath $saveRoot)) { return }
  if ((Get-Item -LiteralPath $saveRoot).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Refusing a linked save directory.' }
  foreach ($item in Get-ChildItem -LiteralPath $saveRoot -Force -Recurse) {
    if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Refusing linked save entry: $($item.FullName)" }
    $absolute = [IO.Path]::GetFullPath($item.FullName)
    if (-not $absolute.StartsWith($savePrefix, [StringComparison]::OrdinalIgnoreCase)) { throw 'Save path escapes the expected runtime directory.' }
    if (-not $item.PSIsContainer) { $item }
  }
}
function File-State([string]$Path) {
  if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $null }
  return [ordered]@{bytes=(Get-Item -LiteralPath $Path).Length;sha256=(Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash}
}
function Save-Inventory {
  $inventory = [ordered]@{}
  foreach ($file in @(Get-SaveFiles | Sort-Object FullName)) { $inventory[$file.FullName.Substring($savePrefix.Length)] = File-State $file.FullName }
  return $inventory
}
function Same-State($A, $B) {
  if ($null -eq $A -or $null -eq $B) { return $null -eq $A -and $null -eq $B }
  return $A.bytes -eq $B.bytes -and $A.sha256 -eq $B.sha256
}
$beforeSave = Save-Inventory
$originalDirectories = @(if (Test-Path -LiteralPath $saveRoot) { Get-ChildItem -LiteralPath $saveRoot -Force -Recurse -Directory | ForEach-Object FullName })
$backup = @{}
$beforeFiles = [ordered]@{}
$paths = @($normalConfig, $testConfig, $prefs) + @(Get-SaveFiles | ForEach-Object FullName)
$backupIndex = 0
foreach ($path in $paths) {
  if (Test-Path -LiteralPath $path) {
    if ((Get-Item -LiteralPath $path).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Refusing linked protected file: $path" }
    $copy = Join-Path $output ('backup-' + $backupIndex++ + '.bin')
    Copy-Item -LiteralPath $path -Destination $copy
    $backup[$path] = $copy
  } else { $backup[$path] = $null }
  $beforeFiles[$path] = File-State $path
}
$beforeSave | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $output 'save-before.json') -Encoding utf8
$beforeFiles | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $output 'protected-before.json') -Encoding utf8
if (Test-Path -LiteralPath $gameLog) { Copy-Item -LiteralPath $gameLog -Destination (Join-Path $output 'previous-keeperfx.log') }
$sourceHashes = [ordered]@{}
foreach ($relative in @('local-dev/renderer-failure-test.ps1','src/kfx/renderer/RendererDX12.cpp','src/kfx/renderer/RendererManager.cpp','src/kfx/renderer/RendererManager.h','src/kfx/renderer/d3d12/Dx12Device.cpp','src/main.cpp','src/bflib_fmvids.cpp')) {
  $sourceHashes[$relative] = (Get-FileHash -LiteralPath (Join-Path $repo $relative) -Algorithm SHA256).Hash
}
$binaryHash = (Get-FileHash -LiteralPath $executable -Algorithm SHA256).Hash
$config = Get-Content -LiteralPath $normalConfig -Raw
foreach ($pair in @{RENDERER='DX12';API_ENABLED='FALSE';AUTOMATIC_REPLAYS='OFF';STARTUP='';FREEZE_GAME_ON_FOCUS_LOST='OFF';INGAME_RES='1280x720w32';FRONTEND_RES='640x480w32 1280x720w32 1280x720w32';VSYNC='OFF';FRAMES_PER_SECOND='60'}.GetEnumerator()) {
  $pattern = '(?m)^' + $pair.Key + '=.*$'
  if ($config -match $pattern) { $config = $config -replace $pattern, ($pair.Key + '=' + $pair.Value) }
  else { $config += "`r`n$($pair.Key)=$($pair.Value)`r`n" }
}
$oldEnvironment = @{}
foreach ($entry in Get-ChildItem Env: | Where-Object Name -Like 'KFX_*') { $oldEnvironment[$entry.Name] = $entry.Value }
$game = $null
$forced = $false
$failure = $null
$checksPassed = $false
$restored = $false
$progressUnchanged = $false
$elapsed = $null
$exitCode = $null
$diagnostics = $null
$cleanupErrors = @()
$progressChanges = @()
$newSaveFiles = @()
$newProgressFiles = @()
$timer = $null
$arguments = '-config keeperfx-renderer-failure.cfg -nointro -skipheartzoom -campaign keeporig -level 1 -nosound'
try {
  foreach ($name in @($oldEnvironment.Keys)) { [Environment]::SetEnvironmentVariable($name, $null, 'Process') }
  $env:KFX_SCENE_REFERENCE = '1'
  $env:KFX_SCENE_TEST_DIR = $output
  if ($Mode -eq 'RemoveDevice') { $env:KFX_DX12_REMOVE_AFTER_SUBMISSIONS = [string]$AfterSubmissions }
  else { $env:KFX_DX12_FAIL_AFTER_SUBMISSIONS = [string]$AfterSubmissions }
  $env:KFX_DX12_FAILURE_NO_DIALOG = '1'
  $env:KFX_DX12_GPU_VALIDATION = '1'
  [IO.File]::WriteAllText($testConfig, $config)
  Copy-Item -LiteralPath $testConfig -Destination (Join-Path $output 'launch.cfg')
  $timer = [Diagnostics.Stopwatch]::StartNew()
  $game = Start-Process -FilePath $executable -WorkingDirectory $runtime -ArgumentList $arguments -WindowStyle Hidden -PassThru
  if (-not $game.WaitForExit($TimeoutSeconds * 1000)) { throw "Injected failure did not exit within $TimeoutSeconds seconds." }
  $game.WaitForExit()
  $elapsed = $timer.Elapsed.TotalSeconds
  $exitCode = $game.ExitCode
  $log = Get-Content -LiteralPath $gameLog -Raw
  $fatal = [regex]::Matches($log, 'Fatal renderer failure: (?<reason>[^\r\n]+)')
  $sessions = [regex]::Matches($log, 'DX12 session: submitted (?<submitted>[0-9]+), rejected (?<rejected>[0-9]+)')
  $validation = [regex]::Matches($log, 'DX12 validation: errors (?<errors>[0-9]+), warnings (?<warnings>[0-9]+)')
  $expectedReason = 'Diagnostic DX12 renderer failure injection (no device reset)'
  $diagnostics = [ordered]@{
    initialized=$log.Contains('DX12 development backend initialized')
    worldFrameSubmitted=$log.Contains('DX12 submitted first world frame')
    fatalCount=$fatal.Count
    firstReason=if($fatal.Count){$fatal[0].Groups['reason'].Value.Trim()}else{$null}
    sessionCount=$sessions.Count
    submitted=if($sessions.Count){[uint64]$sessions[-1].Groups['submitted'].Value}else{0}
    rejected=if($sessions.Count){[uint64]$sessions[-1].Groups['rejected'].Value}else{0}
    validationCount=$validation.Count
    gpuErrors=if($validation.Count){[uint64]$validation[-1].Groups['errors'].Value}else{$null}
    gpuWarnings=if($validation.Count){[uint64]$validation[-1].Groups['warnings'].Value}else{$null}
    dialogSuppressed=$log.Contains('Diagnostic renderer failure dialog suppressed:')
    misleadingSuccessfulExit=($log -match 'finished properly')
    cleanupFailure=($log -match 'DX12 shutdown:|Could not release diagnostic GPU readers during shutdown')
  }
  if ($exitCode -ne 1) { throw "Expected fatal exit1, observed $exitCode." }
  if (-not $diagnostics.initialized -or -not $diagnostics.worldFrameSubmitted) { throw 'Runtime world startup was not recorded before failure.' }
  if ($Mode -eq 'RemoveDevice') {
    $removed = [regex]::Matches($log, 'DX12 diagnostic RemoveDevice invoked: HRESULT 0x(?<hr>[0-9a-fA-F]{8})')
    $stopped = [regex]::Matches($log, 'DX12 stopped at epoch [0-9]+ after (?<submitted>[0-9]+) submitted frames;')
    $failureValidation = [regex]::Matches($log, 'DX12 failure validation: errors (?<errors>[0-9]+), warnings (?<warnings>[0-9]+)')
    $messages = @([regex]::Matches($log, 'DX12 validation at failure: (?<message>[^\r\n]+)') | ForEach-Object { $_.Groups['message'].Value })
    # Only explicit API-requested removal messages are expected. Retain every message;
    # unknown driver wording fails closed and needs review, never a blanket error waiver.
    $unexpected = @($messages | Where-Object { $_ -notmatch '(?i)\bRemoveDevice\b' })
    $diagnostics.removeDeviceCalls = $removed.Count
    $diagnostics.removalHRESULT = if ($removed.Count) { $removed[0].Groups['hr'].Value } else { $null }
    $diagnostics.validationMessages = $messages
    $diagnostics.unexpectedValidationMessages = $unexpected
    if ($removed.Count -ne 1 -or $diagnostics.removalHRESULT -notmatch '^[89a-fA-F]') { throw 'Actual failing device-removal HRESULT missing; unsupported does not pass.' }
    if ($fatal.Count -ne 1 -or $diagnostics.firstReason -ne 'Runtime GPU fence returned the device-removal sentinel') { throw 'Ordinary fence removal detection did not produce the first fatal reason.' }
    if ($stopped.Count -ne 1 -or [uint64]$stopped[0].Groups['submitted'].Value -ne $AfterSubmissions) { throw 'Removal submission boundary mismatch.' }
    if ($failureValidation.Count -ne 1 -or $unexpected.Count -ne 0 -or $messages.Count -ne ([uint64]$failureValidation[0].Groups['errors'].Value + [uint64]$failureValidation[0].Groups['warnings'].Value)) { throw 'Unexpected or missing removal validation diagnostics.' }
    $shutdown = [regex]::Matches($log, 'DX12 shutdown: (?<reason>[^\r\n]+)')
    if ($shutdown.Count -ne 1 -or $shutdown[0].Groups['reason'].Value -notmatch '^GPU fence/device removed failed \(0x[89a-fA-F][0-9a-fA-F]{7}\)$') { throw 'Expected bounded removed-device teardown was not recorded.' }
    if (-not $diagnostics.dialogSuppressed -or $diagnostics.misleadingSuccessfulExit -or $log.Contains('Could not release diagnostic GPU readers during shutdown')) { throw 'Removal exit/dialog cleanup failed.' }
  } else {
  if ($fatal.Count -ne 1 -or $diagnostics.firstReason -ne $expectedReason) { throw 'Expected exactly the injected first fatal reason.' }
  if ($sessions.Count -ne 1 -or $diagnostics.submitted -ne $AfterSubmissions -or $diagnostics.rejected -ne 0) { throw 'Normal shutdown submission summary disagrees with injection.' }
  if ($validation.Count -ne 1 -or $diagnostics.gpuErrors -ne 0 -or $diagnostics.gpuWarnings -ne 0) { throw 'Clean GPU validation shutdown summary is missing.' }
  if (-not $diagnostics.dialogSuppressed -or $diagnostics.misleadingSuccessfulExit -or $diagnostics.cleanupFailure) { throw 'Fatal shutdown/dialog diagnostics are incorrect.' }
  }
  $checksPassed = $true
} catch { $failure = $_.Exception.Message }
finally {
  foreach ($entry in @(Get-ChildItem Env: | Where-Object Name -Like 'KFX_*')) { [Environment]::SetEnvironmentVariable($entry.Name, $null, 'Process') }
  foreach ($name in $oldEnvironment.Keys) { [Environment]::SetEnvironmentVariable($name, $oldEnvironment[$name], 'Process') }
  try {
    if ($game -and -not $game.HasExited) {
      $forced = $true
      # Kill only the process object launched above; never look up a process by name.
      $game.Kill()
      if (-not $game.WaitForExit(5000)) { throw 'Owned process did not stop; protected files will not be restored while it is alive.' }
    }
    if ($game) { $exitCode = $game.ExitCode; if ($timer) { $elapsed = $timer.Elapsed.TotalSeconds } }
  } catch { $cleanupErrors += $_.Exception.Message }
  $safeToRestore = -not $game -or $game.HasExited
  if ($safeToRestore) {
    try {
      if ($game -and (Test-Path -LiteralPath $gameLog)) { Copy-Item -LiteralPath $gameLog -Destination (Join-Path $output 'keeperfx.log') }
      $afterSave = Save-Inventory
      $afterSave | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $output 'save-after-exit-before-restore.json') -Encoding utf8
      foreach ($name in $beforeSave.Keys) {
        if ($name -ieq 'settings.toml') { continue }
        if (-not (Same-State $beforeSave[$name] $afterSave[$name])) { $progressChanges += $name }
      }
      $newSaveFiles = @($afterSave.Keys | Where-Object { -not $beforeSave.Contains($_) })
      $newProgressFiles = @($newSaveFiles | Where-Object { $_ -ine 'settings.toml' })
      $progressUnchanged = $progressChanges.Count -eq 0
      foreach ($file in @(Get-SaveFiles)) {
        $relative = $file.FullName.Substring($savePrefix.Length)
        if (-not $beforeSave.Contains($relative) -or -not (Same-State $beforeSave[$relative] $afterSave[$relative])) {
          $evidence = Join-Path $output ('after-save/' + $relative)
          New-Item -ItemType Directory -Path (Split-Path -Parent $evidence) -Force | Out-Null
          Copy-Item -LiteralPath $file.FullName -Destination $evidence
        }
        if (-not $beforeSave.Contains($relative)) { Remove-Item -LiteralPath $file.FullName }
      }
    } catch { $cleanupErrors += $_.Exception.Message }
    foreach ($path in $backup.Keys) {
      try {
        if ($null -eq $backup[$path]) { if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path } }
        else {
          New-Item -ItemType Directory -Path (Split-Path -Parent $path) -Force | Out-Null
          Copy-Item -LiteralPath $backup[$path] -Destination $path -Force
        }
      } catch { $cleanupErrors += $_.Exception.Message }
    }
    try {
      # Remove only new empty subdirectories, with verified in-root absolute paths.
      $null = @(Get-SaveFiles)
      foreach ($directory in @(Get-ChildItem -LiteralPath $saveRoot -Directory -Force -Recurse | Sort-Object { $_.FullName.Length } -Descending)) {
        if ($directory.FullName -notin $originalDirectories -and @(Get-ChildItem -LiteralPath $directory.FullName -Force).Count -eq 0) { Remove-Item -LiteralPath $directory.FullName }
      }
      $restored = $cleanupErrors.Count -eq 0
      foreach ($path in $beforeFiles.Keys) { if (-not (Same-State $beforeFiles[$path] (File-State $path))) { $restored = $false } }
      $restoredSave = Save-Inventory
      if (($restoredSave | ConvertTo-Json -Compress -Depth 5) -ne ($beforeSave | ConvertTo-Json -Compress -Depth 5)) { $restored = $false }
    } catch { $restored = $false; $cleanupErrors += $_.Exception.Message }
  }
  $passed = $checksPassed -and $progressUnchanged -and $newProgressFiles.Count -eq 0 -and $restored -and -not $forced
  [ordered]@{
    schema=1;passed=$passed;mode=$Mode;scope='Guarded exception or application-requested device removal; not physical TDR, global GPU reset or automatic recovery qualification.'
    executable=$executable;binarySha256=$binaryHash;sourceSha256=$sourceHashes;arguments=$arguments
    afterSubmissions=$AfterSubmissions;timeoutSeconds=$TimeoutSeconds;elapsedSeconds=$elapsed
    processId=if($game){$game.Id}else{$null};exitCode=$exitCode;forced=$forced;apiEnabled=$false
    preexistingProgressFiles=@($beforeSave.Keys | Where-Object { $_ -ine 'settings.toml' })
    preexistingProgressUnchangedBeforeRestoration=$progressUnchanged;changedProgressFiles=$progressChanges
    newSaveFilesBeforeRestoration=@($newSaveFiles);newProgressFilesBeforeRestoration=@($newProgressFiles);byteExactRestoration=$restored
    strippedDiagnosticNames=@($oldEnvironment.Keys | Sort-Object);diagnostics=$diagnostics;failure=$failure;cleanupErrors=$cleanupErrors
  } | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $output 'verification.json') -Encoding utf8
}
if (-not $passed) { throw "Renderer failure check failed: $failure; cleanup: $($cleanupErrors -join '; '); evidence: $output" }
Write-Output "Guarded renderer failure check passed: $output"