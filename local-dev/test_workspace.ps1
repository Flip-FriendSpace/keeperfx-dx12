# SPDX-License-Identifier: GPL-2.0-or-later
# Runs only against a temporary folder.
param([string]$OutputPath)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'workspace.ps1')
$root = Join-Path ([IO.Path]::GetTempPath()) ('keeperfx-workspace-test-' + [Guid]::NewGuid().ToString('N'))
$checks = 0
function Check([bool]$Condition, [string]$Message) { $script:checks++; if (-not $Condition) { throw $Message } }
function Refuses([scriptblock]$Action) { try { & $Action; return $false } catch { return $true } }
try {
  New-Item -ItemType Directory -Path (Join-Path $root 'runtime/keeperfx-x64/remaster'), (Join-Path $root 'artifacts/run') -Force | Out-Null
  Check ((Resolve-KeeperWorkspace $root) -eq [IO.Path]::GetFullPath($root)) 'A workspace with a runtime was refused.'
  Check (Refuses { Resolve-KeeperWorkspace (Join-Path $root 'artifacts') }) 'A folder without a runtime was accepted.'
  Assert-NoRuntimeSession $root
  foreach ($relative in 'runtime/keeperfx-x64/remaster/operation.lock', 'artifacts/remaster-integration/shared-runtime.lock') {
    $lock = Join-Path $root $relative
    New-Item -ItemType Directory -Path (Split-Path -Parent $lock) -Force | Out-Null
    [IO.File]::WriteAllText($lock, 'held')
    Check (Refuses { Assert-NoRuntimeSession $root }) "A held $relative was ignored."
    Remove-Item -LiteralPath $lock
  }
  $kept = Join-Path $root 'kept.cfg'; $created = Join-Path $root 'created.cfg'
  [IO.File]::WriteAllText($kept, 'before')
  $backups = @{ $kept = [IO.File]::ReadAllBytes($kept); $created = $null }
  Save-TestBackup $backups (Join-Path $root 'artifacts/run') $root
  Check (Refuses { Save-TestBackup $backups (Join-Path $root 'artifacts/run') $root }) 'A second snapshot would have saved test state as the backup.'
  Check (Refuses { Assert-NoRuntimeSession $root }) 'A test session did not hold the runtime.'
  [IO.File]::WriteAllText($kept, 'test state'); [IO.File]::WriteAllText($created, 'test state')
  & (Join-Path $PSScriptRoot 'restore-test-session.ps1') -WorkspacePath $root | Out-Null
  Check ([IO.File]::ReadAllText($kept) -ceq 'before') 'A changed file was not restored.'
  Check (-not (Test-Path -LiteralPath $created)) 'A created file was not removed.'
  Assert-NoRuntimeSession $root
  Save-TestBackup $backups (Join-Path $root 'artifacts/run') $root
  Clear-TestBackup $root
  Assert-NoRuntimeSession $root
  Write-Output "PASS $checks workspace checks."
} finally {
  if (Test-Path -LiteralPath $root) { Remove-Item -LiteralPath $root -Recurse -Force }
}
