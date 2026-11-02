# SPDX-License-Identifier: GPL-2.0-or-later
# Builds the lens callback test in keeperfx-dev and runs the Windows program it produces.
param([Parameter(Mandatory = $true)][string]$Output)
$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force -Path $Output | Out-Null
$linuxOutput = (& wsl.exe -d keeperfx-dev --exec wslpath -a $Output).Trim()
$linuxScript = (& wsl.exe -d keeperfx-dev --exec wslpath -a (Join-Path $PSScriptRoot 'test-custom-lens-callbacks.py')).Trim()
if ($LASTEXITCODE -ne 0) { throw 'keeperfx-dev is unavailable.' }
& wsl.exe -d keeperfx-dev --cd / --exec python3 $linuxScript --output $linuxOutput
if ($LASTEXITCODE -ne 0) { throw 'Lens callback test build failed.' }
& (Join-Path $Output 'custom-lens-callback-test.exe')
if ($LASTEXITCODE -ne 0) { throw "Lens callback test failed with exit code $LASTEXITCODE." }
