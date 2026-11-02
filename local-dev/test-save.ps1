# SPDX-License-Identifier: GPL-2.0-or-later
# Builds the save tests in keeperfx-dev and runs them as the Windows programs they target.
param([Parameter(Mandatory = $true)][string]$Output)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
New-Item -ItemType Directory -Force -Path $Output | Out-Null
$linuxOutput = (& wsl.exe -d keeperfx-dev --exec wslpath -a $Output).Trim()
$linuxScript = (& wsl.exe -d keeperfx-dev --exec wslpath -a (Join-Path $PSScriptRoot 'test-save.sh')).Trim()
if ($LASTEXITCODE -ne 0) { throw 'keeperfx-dev is unavailable.' }
& wsl.exe -d keeperfx-dev --cd / --exec bash $linuxScript build $linuxOutput
if ($LASTEXITCODE -ne 0) { throw 'Save test build failed.' }
$programs = @(Get-ChildItem -LiteralPath $Output -Filter '*.exe')
if ($programs.Count -lt 2) { throw "Expected two save test programs, found $($programs.Count)." }
foreach ($program in $programs) {
  & $program.FullName
  if ($LASTEXITCODE -ne 0) { throw "$($program.Name) failed with exit code $LASTEXITCODE." }
}
