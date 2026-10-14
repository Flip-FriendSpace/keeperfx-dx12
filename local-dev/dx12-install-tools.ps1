# SPDX-License-Identifier: GPL-2.0-or-later
# KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#Requires -RunAsAdministrator
# Run only when explicitly installing the development validation prerequisite.
$ErrorActionPreference = 'Stop'
$out = Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'artifacts/dx12'
New-Item -ItemType Directory -Path $out -Force | Out-Null
$resultPath = Join-Path $out 'graphics-tools-install.json'
try {
    $name = 'Tools.Graphics.DirectX~~~~0.0.1.0'
    $before = Get-WindowsCapability -Online -Name $name
    $restartNeeded = $false
    if ($before.State -ne 'Installed') {
        $installed = Add-WindowsCapability -Online -Name $name -LogPath (Join-Path $out 'graphics-tools-dism.log')
        $restartNeeded = $installed.RestartNeeded
    }
    $after = Get-WindowsCapability -Online -Name $name
    [ordered]@{ status = 'complete'; name = $name; state = [string]$after.State; restartNeeded = $restartNeeded } |
        ConvertTo-Json | Set-Content -LiteralPath $resultPath -Encoding utf8
    if ($after.State -ne 'Installed') { throw 'Graphics Tools installation did not complete.' }
} catch {
    [ordered]@{ status = 'failed'; error = $_.Exception.Message } |
        ConvertTo-Json | Set-Content -LiteralPath $resultPath -Encoding utf8
    exit 1
}
