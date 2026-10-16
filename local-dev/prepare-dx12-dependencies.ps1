# SPDX-License-Identifier: GPL-2.0-or-later
# KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
param([string]$WorkspacePath)
$ErrorActionPreference = 'Stop'
if (-not $WorkspacePath) { $WorkspacePath = Split-Path -Parent (Split-Path -Parent $PSScriptRoot) }
$workspace = (Resolve-Path -LiteralPath $WorkspacePath).Path
$cache = Join-Path $workspace 'downloads/keeperfx-dx12'
$artifacts = Join-Path $workspace 'artifacts/dx12'
New-Item -ItemType Directory -Path $cache,$artifacts -Force | Out-Null
$dependencies = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'dependencies-dx12.json') -Raw | ConvertFrom-Json
foreach ($dependency in $dependencies) {
    $path = Join-Path $cache $dependency.file
    if (-not (Test-Path -LiteralPath $path)) {
        & curl.exe -fL --retry 3 --silent --show-error $dependency.url -o ($path + '.partial')
        if ($LASTEXITCODE -ne 0) { throw "Download failed: $($dependency.name)" }
        if ((Get-FileHash -LiteralPath ($path + '.partial') -Algorithm SHA256).Hash -ne $dependency.sha256) {
            throw "SHA256 mismatch: $($dependency.name)"
        }
        Move-Item -LiteralPath ($path + '.partial') -Destination $path
    }
    if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $dependency.sha256) {
        throw "SHA256 mismatch: $($dependency.name)"
    }
}
$script = (& wsl.exe -d keeperfx-dev --exec wslpath -a (Join-Path $PSScriptRoot 'dx12-prepare.sh')).Trim()
if ($LASTEXITCODE -ne 0) { throw 'The keeperfx-dev build environment is unavailable.' }
$linuxWorkspace = (& wsl.exe -d keeperfx-dev --exec wslpath -a $workspace).Trim()
if ($LASTEXITCODE -ne 0) { throw 'Cannot resolve the dependency workspace in keeperfx-dev.' }
& wsl.exe -d keeperfx-dev --exec bash $script $linuxWorkspace
if ($LASTEXITCODE -ne 0) { throw 'Pinned DX12 dependency preparation failed.' }
foreach ($dll in @('D3D12Core.dll', 'd3d12SDKLayers.dll')) {
    $signature = Get-AuthenticodeSignature -LiteralPath (Join-Path $artifacts "bin/D3D12/$dll")
    if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch 'Microsoft Corporation') {
        throw "Invalid Microsoft SDK signature: $dll ($($signature.Status))"
    }
}
