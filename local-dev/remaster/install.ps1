# SPDX-License-Identifier: GPL-2.0-or-later
param([Parameter(Mandatory=$true)][string]$RuntimePath,[string]$Python='python',[switch]$VerifyOnly)
$ErrorActionPreference='Stop'
$arguments=@((Join-Path $PSScriptRoot 'install.py'),'--runtime',$RuntimePath)
if($VerifyOnly){$arguments+='--verify-only'}
& $Python @arguments
if($LASTEXITCODE -ne 0){throw 'Remaster installation/verification failed; see diagnostics above.'}
