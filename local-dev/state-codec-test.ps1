# SPDX-License-Identifier: GPL-2.0-or-later
# KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$output = Join-Path (Split-Path -Parent $repo) 'artifacts/state-codec'
$linuxScript = (& wsl.exe -d keeperfx-dev --exec wslpath -a (Join-Path $PSScriptRoot 'state-codec-test.sh')).Trim()
if ($LASTEXITCODE -ne 0) { throw 'keeperfx-dev is unavailable.' }
& wsl.exe -d keeperfx-dev --exec bash $linuxScript
if ($LASTEXITCODE -ne 0) { throw 'State codec tests failed to compile.' }
$x86 = Join-Path $output 'fixture-i686.bin'
$x64 = Join-Path $output 'fixture-x86_64.bin'
& (Join-Path $output 'state-codec-i686.exe') $x86 '-'
if ($LASTEXITCODE -ne 0) { throw '32-bit codec regression failed.' }
& (Join-Path $output 'state-codec-x86_64.exe') $x64 $x86
if ($LASTEXITCODE -ne 0) { throw '64-bit codec or reading 32-bit data failed.' }
& (Join-Path $output 'state-codec-i686.exe') $x86 $x64
if ($LASTEXITCODE -ne 0) { throw 'Reading 64-bit data in the 32-bit codec failed.' }
$hash32 = (Get-FileHash -LiteralPath $x86 -Algorithm SHA256).Hash
$hash64 = (Get-FileHash -LiteralPath $x64 -Algorithm SHA256).Hash
if ($hash32 -ne $hash64) { throw 'The architectures produced different wire bytes.' }
$result = [ordered]@{ passed = $true; sha256 = $hash32; fixtureBytes = (Get-Item -LiteralPath $x86).Length; architectures = @('i686', 'x86_64'); crossRead = 'both directions' }
$result | ConvertTo-Json | Tee-Object -FilePath (Join-Path $output 'codec-test.json')
