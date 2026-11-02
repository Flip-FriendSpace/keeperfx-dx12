# SPDX-License-Identifier: GPL-2.0-or-later
# Restores the files a live test changed, after the test was killed before its own cleanup.
param([string]$WorkspacePath)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'workspace.ps1')
$workspace = Resolve-KeeperWorkspace $WorkspacePath
$lock = Get-TestSessionLock $workspace
if (-not (Test-Path -LiteralPath $lock)) { Write-Output 'No interrupted test session.'; return }
if (Get-Process -Name keeperfx,keeperfx-local,keeperfx-local-hvlog,keeperfx-dx12,keeperfx-dx12-hvlog -ErrorAction SilentlyContinue) {
  throw 'Close KeeperFX before restoring.'
}
$directory = [IO.File]::ReadAllText($lock).Trim()
# On Windows PowerShell, @() around ConvertFrom-Json would nest the array.
$entries = Get-Content -LiteralPath (Join-Path $directory 'manifest.json') -Raw | ConvertFrom-Json
foreach ($entry in $entries) {
  if ($entry.backup) { [IO.File]::WriteAllBytes($entry.path, [IO.File]::ReadAllBytes((Join-Path $directory $entry.backup))) }
  elseif (Test-Path -LiteralPath $entry.path) { Remove-Item -LiteralPath $entry.path }
}
Remove-Item -LiteralPath $lock
Write-Output "Restored $($entries.Count) files from $directory"
