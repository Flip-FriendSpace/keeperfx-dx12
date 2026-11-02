# SPDX-License-Identifier: GPL-2.0-or-later
# Runs the checks registered in tests.json. A check that did not run is never counted as passed.
param(
  [ValidateSet('meta', 'unit', 'native', 'data', 'live', 'all')][string]$Tier = 'unit',
  [string]$WorkspacePath,
  [string]$Filter,
  [switch]$List,
  [switch]$Strict,
  [string]$Manifest
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'workspace.ps1')
$repo = Split-Path -Parent $PSScriptRoot
$isDefaultManifest = -not $Manifest
if (-not $Manifest) { $Manifest = Join-Path $PSScriptRoot 'tests.json' }
$registry = Get-Content -LiteralPath $Manifest -Raw | ConvertFrom-Json
$tiers = @('unit', 'native', 'data', 'live')

function Get-EntryFile($entry) {
  if ($entry.file) { return $entry.file }
  if ($entry.sources) { return $entry.sources[0] }
  if ($entry.run) { return @($entry.run | Where-Object { $_ -like 'local-dev/*' -or $_ -like 'tests/*' })[0] }
}

function Get-RegistryProblems {
  $problems = New-Object System.Collections.Generic.List[string]
  $listed = @{}
  $ids = @{}
  foreach ($entry in $registry.tests) {
    if ($ids.ContainsKey($entry.id)) { $problems.Add("Duplicate id: $($entry.id)") }
    $ids[$entry.id] = $true
    if ($tiers -notcontains $entry.tier) { $problems.Add("$($entry.id): unknown tier $($entry.tier)") }
    if (-not ($entry.run -or $entry.sources -or $entry.notRun)) { $problems.Add("$($entry.id): no run, sources or notRun") }
    $paths = @(Get-EntryFile $entry) + @($entry.sources)
    foreach ($relative in @($paths | Where-Object { $_ })) {
      if (-not (Test-Path -LiteralPath (Join-Path $repo $relative) -PathType Leaf)) { $problems.Add("$($entry.id): missing $relative") }
      $listed[$relative.Replace('\', '/')] = $true
    }
    foreach ($pattern in @($entry.covers)) {
      if (-not $pattern) { continue }
      $found = @(Get-ChildItem -Path (Join-Path $repo $pattern) -File -ErrorAction SilentlyContinue)
      if (-not $found) { $problems.Add("$($entry.id): covers nothing at $pattern") }
      foreach ($file in $found) { $listed[$file.FullName.Substring($repo.Length + 1).Replace('\', '/')] = $true }
    }
  }
  foreach ($file in $registry.notTests) {
    if (-not (Test-Path -LiteralPath (Join-Path $repo $file.path) -PathType Leaf)) { $problems.Add("notTests: missing $($file.path)") }
    $listed[$file.path] = $true
  }
  # A new check has to be registered, or it would silently never run.
  $pattern = '(?i)(^|[/_.-])tests?([/_.-]|$)|Tests?\.(cpp|h)$|(^|[/_-])(verify|audit)[_.-]'
  foreach ($root in @('local-dev', 'tests')) {
    foreach ($file in Get-ChildItem -LiteralPath (Join-Path $repo $root) -Recurse -File) {
      $relative = $file.FullName.Substring($repo.Length + 1).Replace('\', '/')
      if ($relative -match '\.md$' -or $relative -match '__pycache__') { continue }
      $name = $relative.Substring($root.Length + 1)
      if ($name -match $pattern -and -not $listed.ContainsKey($relative)) { $problems.Add("Not registered in tests.json: $relative") }
    }
  }
  return $problems
}

function Resolve-TestPython([string]$Workspace) {
  $candidates = @($env:KFX_TEST_PYTHON,
    (Join-Path $HOME '.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe'),
    (Join-Path $Workspace 'downloads/audio-tools/venv312/Scripts/python.exe')) | Where-Object { $_ -and (Test-Path -LiteralPath $_) }
  foreach ($candidate in $candidates) {
    & $candidate -c 'import PIL, numpy' 2>$null
    if ($LASTEXITCODE -eq 0) { return , @($candidate) }
  }
  return , @('py', '-3')
}

function Join-Arguments([string[]]$Values) {
  return (@($Values | ForEach-Object { if ($_ -match '[\s"]') { '"' + ($_ -replace '"', '\"') + '"' } else { $_ } }) -join ' ')
}

function Invoke-TestEntry($entry, [hashtable]$Context) {
  $safeId = $entry.id
  $started = Get-Date
  $result = [ordered]@{ id = $entry.id; tier = $entry.tier; status = 'failed'; seconds = 0; exitCode = $null; log = $null; reason = $null }
  if ($entry.notRun) { $result.status = 'not-run'; $result.reason = $entry.notRun; return $result }
  $out = Join-Path $Context.Output $safeId
  $wslOut = $Context.WslOutput + '/' + $safeId
  $tempOut = "/opt/keeperfx-build/test-runner/$($Context.CheckoutName)/$safeId"
  if ($entry.sources) { $command = @('wsl', 'bash', 'local-dev/test-native.sh', $safeId) + @($entry.sources) }
  else { $command = @($entry.run) }
  $command = @($command | ForEach-Object {
    $_.Replace('{workspace}', $Context.Workspace).Replace('{out}', $out).Replace('{wslOut}', $wslOut).Replace('{wslRepo}', $Context.WslRepo).Replace('{wslTemp}', $tempOut).Replace('{wslGitDir}', $Context.WslGitDir)
  })
  $program = $command[0]
  $rest = @($command | Select-Object -Skip 1)
  switch ($program) {
    'python' { $file = $Context.Python[0]; $arguments = @($Context.Python | Select-Object -Skip 1) + $rest }
    'powershell' { $file = 'powershell.exe'; $arguments = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File') + $rest }
    'wsl' { $file = 'wsl.exe'; $arguments = @('-d', 'keeperfx-dev', '--cd', $Context.WslRepo, '--exec') + $rest }
    'exe' { $file = $rest[0]; $arguments = @($rest | Select-Object -Skip 1) }
    default { $file = $program; $arguments = $rest }
  }
  if ($entry.requires -and -not (Get-Command $entry.requires -ErrorAction SilentlyContinue)) {
    $result.status = 'not-run'; $result.reason = "$($entry.requires) is not installed."; return $result
  }
  New-Item -ItemType Directory -Force -Path $out | Out-Null
  $stdout = Join-Path $out 'stdout.log'
  $stderr = Join-Path $out 'stderr.log'
  $result.log = $stdout
  $limit = if ($entry.timeoutSeconds) { [int]$entry.timeoutSeconds } else { 300 }
  try {
    $process = Start-Process -FilePath $file -ArgumentList (Join-Arguments $arguments) -WorkingDirectory $repo -PassThru -WindowStyle Hidden -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    $null = $process.Handle
    if ($process.WaitForExit($limit * 1000)) {
      $process.WaitForExit()
      $result.exitCode = $process.ExitCode
      if ($process.ExitCode -eq 0) { $result.status = 'passed' }
      else { $result.reason = (@(Get-Content -LiteralPath $stderr -Tail 2) + @(Get-Content -LiteralPath $stdout -Tail 2)) -join ' | ' }
    } else {
      & taskkill.exe /T /F /PID $process.Id *> $null
      $result.reason = "Timed out after $limit seconds."
    }
  } catch { $result.reason = $_.Exception.Message }
  $result.seconds = [math]::Round(((Get-Date) - $started).TotalSeconds, 1)
  return $result
}

if ($isDefaultManifest) {
  $problems = @(Get-RegistryProblems)
  if ($problems) { $problems | ForEach-Object { Write-Output "REGISTRY $_" }; throw "$($problems.Count) registry problems in tests.json." }
}
$selected = @($registry.tests | Where-Object {
  (($Tier -eq 'all' -and $_.tier -ne 'live') -or $_.tier -eq $Tier) -and (-not $Filter -or $_.id -like "*$Filter*")
})
if ($List) { $selected | ForEach-Object { Write-Output ("{0,-7} {1}{2}" -f $_.tier, $_.id, $(if ($_.notRun) { '  [not run]' })) }; return }
if ($Tier -eq 'meta') { Write-Output "Registry OK: $(@($registry.tests).Count) checks."; return }
if ($Tier -eq 'live' -and -not $WorkspacePath) { throw 'The live tier changes a runtime, so it needs an explicit -WorkspacePath.' }
$workspace = Resolve-KeeperWorkspace $WorkspacePath
if ($Tier -eq 'live') { Assert-NoRuntimeSession $workspace }
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss-fff'
$output = Join-Path $workspace "artifacts/test-runner/$Tier-$stamp"
New-Item -ItemType Directory -Force -Path $output | Out-Null
$wslRepo = (& wsl.exe -d keeperfx-dev --exec wslpath -a $repo).Trim()
$wslOutput = (& wsl.exe -d keeperfx-dev --exec wslpath -a $output).Trim()
$wslGitDir = (& wsl.exe -d keeperfx-dev --exec wslpath -a (& git -C $repo rev-parse --path-format=absolute --git-common-dir)).Trim()
$context = @{ Workspace = $workspace; Output = $output; WslOutput = $wslOutput; WslRepo = $wslRepo; WslGitDir = $wslGitDir
  CheckoutName = (Split-Path -Leaf $repo); Python = (Resolve-TestPython $workspace) }
$env:PYTHONDONTWRITEBYTECODE = '1'
$results = foreach ($entry in $selected) {
  $result = Invoke-TestEntry $entry $context
  Write-Output ("{0,-8} {1,-38} {2}" -f $result.status, $result.id, $(if ($result.reason) { $result.reason } else { "$($result.seconds)s" })) | Out-Host
  [pscustomobject]$result
}
$results = @($results)
$counts = [ordered]@{
  passed = @($results | Where-Object status -eq 'passed').Count
  failed = @($results | Where-Object status -eq 'failed').Count
  notRun = @($results | Where-Object status -eq 'not-run').Count
}
$commit = (& git -C $repo rev-parse HEAD).Trim()
$dirty = [bool](& git -C $repo status --porcelain)
[ordered]@{ tier = $Tier; commit = $commit; dirty = $dirty; strict = [bool]$Strict; counts = $counts; results = $results } |
  ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $output 'report.json') -Encoding utf8
Write-Output "Passed $($counts.passed), failed $($counts.failed), not run $($counts.notRun). Report: $output\report.json"
if ($counts.failed -gt 0 -or ($Strict -and $counts.notRun -gt 0)) { exit 1 }
