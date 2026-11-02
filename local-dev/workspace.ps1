# SPDX-License-Identifier: GPL-2.0-or-later
# Shared by the local-dev scripts: finding the workspace and protecting its runtime.

# A checkout's default workspace is its parent, which is wrong for a checkout under .worktrees.
function Resolve-KeeperWorkspace([string]$WorkspacePath) {
  $selected = if ($WorkspacePath) { $WorkspacePath } else { Split-Path -Parent (Split-Path -Parent $PSScriptRoot) }
  $full = [IO.Path]::GetFullPath($selected)
  if (-not (Test-Path -LiteralPath (Join-Path $full 'runtime') -PathType Container)) {
    throw "No runtime folder in $full. Pass -WorkspacePath with the workspace that holds runtime/."
  }
  return $full
}

function Get-TestSessionLock([string]$Workspace) { Join-Path $Workspace 'artifacts/test-session.lock' }

# Refuses to change a runtime while a play session, installation or live test holds it.
function Assert-NoRuntimeSession([Parameter(Mandatory)][string]$Workspace) {
  $locks = @(
    (Join-Path $Workspace 'runtime/keeperfx-x64/remaster/operation.lock'),
    (Join-Path $Workspace 'artifacts/remaster-integration/shared-runtime.lock'),
    (Get-TestSessionLock $Workspace)
  ) | Where-Object { Test-Path -LiteralPath $_ }
  if ($locks) {
    throw "A play session, installation or live test owns this runtime, or ended without restoring it. Finish it or run its recovery first: $($locks -join ', ')"
  }
}

# Keeps a live test's in-memory backups on disk too, so restore-test-session.ps1 can recover a killed run.
function Save-TestBackup([hashtable]$Backups, [string]$Output, [string]$Workspace) {
  Assert-NoRuntimeSession $Workspace
  $directory = Join-Path $Output 'test-backup'
  New-Item -ItemType Directory -Path $directory -Force | Out-Null
  $entries = @(); $index = 0
  foreach ($path in $Backups.Keys) {
    $entry = @{ path = $path; backup = $null }
    if ($null -ne $Backups[$path]) {
      $entry.backup = [string]$index
      [IO.File]::WriteAllBytes((Join-Path $directory $entry.backup), $Backups[$path])
    }
    $entries += $entry; $index++
  }
  ConvertTo-Json -InputObject $entries -Depth 3 | Set-Content -LiteralPath (Join-Path $directory 'manifest.json') -Encoding utf8
  New-Item -ItemType Directory -Path (Split-Path -Parent (Get-TestSessionLock $Workspace)) -Force | Out-Null
  [IO.File]::WriteAllText((Get-TestSessionLock $Workspace), $directory)
}

function Clear-TestBackup([string]$Workspace) {
  $lock = Get-TestSessionLock $Workspace
  if (Test-Path -LiteralPath $lock) { Remove-Item -LiteralPath $lock }
}
