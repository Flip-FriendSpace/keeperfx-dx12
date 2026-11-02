# SPDX-License-Identifier: GPL-2.0-or-later
# Checks the promises test.ps1 makes: a failure fails the run, and a check that did not run never passes.
$ErrorActionPreference = 'Stop'
$root = Join-Path ([IO.Path]::GetTempPath()) ('kfx-test-runner-' + [guid]::NewGuid().ToString('N'))
$checks = 0
function Check([bool]$Condition, [string]$What) {
  if (-not $Condition) { throw "Failed: $What" }
  $script:checks++
}
function Invoke-Runner([object[]]$Tests, [switch]$Strict) {
  $manifest = Join-Path $root 'tests.json'
  (@{ tests = $Tests; notTests = @() } | ConvertTo-Json -Depth 5) | Set-Content -LiteralPath $manifest -Encoding utf8
  $arguments = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $PSScriptRoot 'test.ps1'),
    '-Tier', 'unit', '-WorkspacePath', $root, '-Manifest', $manifest)
  if ($Strict) { $arguments += '-Strict' }
  $null = & powershell.exe @arguments 2>&1
  $code = $LASTEXITCODE
  $folder = Get-ChildItem -LiteralPath (Join-Path $root 'artifacts/test-runner') -Directory | Sort-Object LastWriteTime | Select-Object -Last 1
  $report = Get-Content -LiteralPath (Join-Path $folder.FullName 'report.json') -Raw | ConvertFrom-Json
  Remove-Item -LiteralPath $folder.FullName -Recurse -Force
  return [pscustomobject]@{ Code = $code; Report = $report }
}
function New-Script([string]$Name, [string]$Body) {
  $path = Join-Path $root $Name
  Set-Content -LiteralPath $path -Value $Body
  return $path
}
function New-Entry([string]$Id, [string]$Script, [int]$Timeout = 60) {
  return @{ id = $Id; tier = 'unit'; timeoutSeconds = $Timeout; run = @('powershell', $Script) }
}
New-Item -ItemType Directory -Path (Join-Path $root 'runtime') -Force | Out-Null
try {
  $pass = New-Script 'pass.ps1' 'exit 0'
  $fail = New-Script 'fail.ps1' 'exit 3'
  $hang = New-Script 'hang.ps1' 'Start-Sleep -Seconds 60'
  $skipped = @{ id = 'skipped'; tier = 'unit'; notRun = 'No hardware.' }

  $run = Invoke-Runner @((New-Entry 'pass' $pass))
  Check ($run.Code -eq 0) 'A passing run exits 0'
  Check ($run.Report.counts.passed -eq 1) 'A passing check is counted'

  $run = Invoke-Runner @((New-Entry 'pass' $pass), (New-Entry 'fail' $fail))
  Check ($run.Code -eq 1) 'A failing check fails the run'
  Check ($run.Report.counts.failed -eq 1) 'A failing check is counted'
  Check (@($run.Report.results | Where-Object id -eq 'fail')[0].exitCode -eq 3) 'The exit code is reported'

  $run = Invoke-Runner @((New-Entry 'pass' $pass), $skipped)
  Check ($run.Code -eq 0) 'A check that did not run does not fail a normal run'
  Check ($run.Report.counts.passed -eq 1 -and $run.Report.counts.notRun -eq 1) 'A check that did not run is not counted as passed'
  Check (@($run.Report.results | Where-Object id -eq 'skipped')[0].reason -eq 'No hardware.') 'The reason is reported'

  $run = Invoke-Runner @((New-Entry 'pass' $pass), $skipped) -Strict
  Check ($run.Code -eq 1) 'A check that did not run fails a strict run'

  $run = Invoke-Runner @((New-Entry 'hang' $hang 2))
  Check ($run.Code -eq 1) 'A timeout fails the run'
  Check (@($run.Report.results)[0].reason -match 'Timed out') 'The timeout is reported'
}
finally {
  Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
}
Write-Output "PASS test runner: $checks checks"
