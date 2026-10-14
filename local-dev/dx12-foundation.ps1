# SPDX-License-Identifier: GPL-2.0-or-later
# KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
param(
    [switch]$BuildOnly,
    [switch]$SkipBuild,
    [switch]$Interactive,
    [switch]$GpuValidation,
    [switch]$ResizeWarningTrace,
    [switch]$NoValidation,
    [switch]$Warp,
    [ValidatePattern('^[0-9A-Fa-f]{16}$')][string]$AdapterLuid,
    [ValidateRange(30,600)][int]$TimeoutSeconds = 120,
    [switch]$NoVSync,
    [string]$SceneCapture,
    [string]$IndexedFallback,
    [switch]$IndexedFallbackTest,
    [switch]$RuntimeResourceTest,
    [switch]$RuntimeWorldTest,
    [switch]$RuntimeProcessBudgetTest,
    [string]$RuntimeResourceInput,
    [switch]$Terrain,
    [switch]$World,
    [switch]$Palette,
    [switch]$SpriteReplay,
    [switch]$TerrainReplay,
    [switch]$OutlineReplay,
    [switch]$EnhancedSpriteTest,
    [switch]$EnhancedSpriteReplay,
    [switch]$EnhancedWorldTest,
    [switch]$EnhancedWorld,
    [switch]$EnhancedWorldNativeOrigin,
    [switch]$EnhancedWorldFadeTexelLoad,
    [switch]$LensTest,
    [switch]$WorldOverlayTest,
    [switch]$CompositionTest,
    [switch]$FrozenFrameTest,
    [switch]$FrozenWorld,
    [switch]$FullWalls
)
$ErrorActionPreference = 'Stop'
if ($BuildOnly -and $SkipBuild) { throw 'BuildOnly cannot be combined with SkipBuild.' }
if ($GpuValidation -and $NoValidation) { throw 'GPU validation requires the debug layer.' }
if ($ResizeWarningTrace -and $NoValidation) { throw 'ResizeWarningTrace requires the debug layer.' }
if ($Warp -and $AdapterLuid) { throw 'AdapterLuid cannot be combined with Warp.' }
if ($RuntimeResourceInput -and -not $RuntimeResourceTest) { throw 'RuntimeResourceInput requires RuntimeResourceTest.' }
if ($RuntimeProcessBudgetTest -and ($RuntimeWorldTest -or $RuntimeResourceTest -or $RuntimeResourceInput -or $SceneCapture -or $IndexedFallback -or $IndexedFallbackTest -or $Interactive -or $Terrain -or $World -or $FullWalls -or $Palette -or $SpriteReplay -or $TerrainReplay -or $OutlineReplay -or $FrozenWorld -or $FrozenFrameTest -or $EnhancedSpriteTest -or $EnhancedSpriteReplay -or $EnhancedWorldTest -or $EnhancedWorld -or $EnhancedWorldNativeOrigin -or $EnhancedWorldFadeTexelLoad -or $LensTest -or $WorldOverlayTest -or $CompositionTest)) { throw 'RuntimeProcessBudgetTest cannot be combined with other rendering modes.' }
if ($RuntimeResourceTest -and ($SceneCapture -or $IndexedFallback -or $IndexedFallbackTest -or $Interactive -or $Terrain -or $World -or $FullWalls -or $Palette -or $SpriteReplay -or $TerrainReplay -or $OutlineReplay -or $FrozenWorld -or $FrozenFrameTest -or $EnhancedSpriteTest -or $EnhancedSpriteReplay -or $EnhancedWorldTest -or $EnhancedWorld -or $EnhancedWorldNativeOrigin -or $EnhancedWorldFadeTexelLoad -or $LensTest -or $WorldOverlayTest -or $CompositionTest)) { throw 'RuntimeResourceTest cannot be combined with other rendering modes.' }
if ($RuntimeWorldTest -and ($RuntimeResourceTest -or $RuntimeResourceInput -or $IndexedFallback -or $IndexedFallbackTest -or $Interactive -or $Terrain -or $World -or $FullWalls -or $Palette -or $SpriteReplay -or $TerrainReplay -or $OutlineReplay -or $FrozenWorld -or $FrozenFrameTest -or $EnhancedSpriteTest -or $EnhancedSpriteReplay -or $EnhancedWorldTest -or $EnhancedWorld -or $EnhancedWorldNativeOrigin -or $EnhancedWorldFadeTexelLoad -or $LensTest -or $WorldOverlayTest -or $CompositionTest)) { throw 'RuntimeWorldTest cannot be combined with other rendering modes.' }
if ($Terrain -and -not $SceneCapture) { throw 'Terrain mode requires -SceneCapture.' }
if ($World -and (-not $SceneCapture -or $Terrain)) { throw 'World mode requires -SceneCapture and cannot be combined with -Terrain.' }
if ($Palette -and (-not $SceneCapture -or $Terrain -or $World -or $FullWalls -or $SpriteReplay)) { throw 'Palette mode requires -SceneCapture and cannot be combined with other rendering modes.' }
if ($SpriteReplay -and (-not $SceneCapture -or $Terrain -or $World -or $FullWalls)) { throw 'SpriteReplay requires -SceneCapture and cannot be combined with world/terrain modes.' }
if ($TerrainReplay -and (-not $SceneCapture -or $Terrain -or $World -or $FullWalls -or $Palette -or $SpriteReplay)) { throw 'TerrainReplay requires -SceneCapture and cannot be combined with other rendering modes.' }
if ($OutlineReplay -and (-not $SceneCapture -or $Terrain -or $World -or $FullWalls -or $Palette -or $SpriteReplay -or $TerrainReplay)) { throw 'OutlineReplay requires -SceneCapture and cannot be combined with other rendering modes.' }
if ($FrozenWorld -and (-not $SceneCapture -or $Terrain -or $World -or $FullWalls -or $Palette -or $SpriteReplay -or $TerrainReplay -or $OutlineReplay -or $FrozenFrameTest)) { throw 'FrozenWorld requires a v8 capture and cannot be combined with other rendering modes.' }
if ($FrozenFrameTest -and ($SceneCapture -or $Interactive -or $Terrain -or $World -or $FullWalls -or $Palette -or $SpriteReplay -or $TerrainReplay -or $OutlineReplay -or $FrozenWorld)) { throw 'FrozenFrameTest cannot be combined with other rendering modes.' }
if ($EnhancedSpriteTest -and ($SceneCapture -or $Interactive -or $Terrain -or $World -or $FullWalls -or $Palette -or $SpriteReplay -or $TerrainReplay -or $OutlineReplay -or $FrozenWorld -or $FrozenFrameTest -or $EnhancedSpriteReplay)) { throw 'EnhancedSpriteTest cannot be combined with other rendering modes.' }
if ($EnhancedSpriteReplay -and (-not $SceneCapture -or $Terrain -or $World -or $FullWalls -or $Palette -or $SpriteReplay -or $TerrainReplay -or $OutlineReplay -or $FrozenWorld -or $FrozenFrameTest -or $EnhancedSpriteTest)) { throw 'EnhancedSpriteReplay requires a v9 capture and cannot be combined with other rendering modes.' }
if ($EnhancedWorldTest -and ($SceneCapture -or $Interactive -or $Terrain -or $World -or $FullWalls -or $Palette -or $SpriteReplay -or $TerrainReplay -or $OutlineReplay -or $FrozenWorld -or $FrozenFrameTest -or $EnhancedSpriteTest -or $EnhancedSpriteReplay -or $EnhancedWorld)) { throw 'EnhancedWorldTest cannot be combined with other rendering modes.' }
if ($EnhancedWorld -and (-not $SceneCapture -or $Terrain -or $World -or $FullWalls -or $Palette -or $SpriteReplay -or $TerrainReplay -or $OutlineReplay -or $FrozenWorld -or $FrozenFrameTest -or $EnhancedSpriteTest -or $EnhancedSpriteReplay -or $EnhancedWorldTest)) { throw 'EnhancedWorld requires a v10 or v11 capture and cannot be combined with other rendering modes.' }
if ($EnhancedWorldNativeOrigin -and -not $EnhancedWorld) { throw 'EnhancedWorldNativeOrigin requires -EnhancedWorld.' }
if ($EnhancedWorldFadeTexelLoad -and -not ($EnhancedWorld -or $EnhancedWorldTest)) { throw 'EnhancedWorldFadeTexelLoad requires -EnhancedWorld or -EnhancedWorldTest.' }
if ($LensTest -and ($SceneCapture -or $Interactive -or $Terrain -or $World -or $FullWalls -or $Palette -or $SpriteReplay -or $TerrainReplay -or $OutlineReplay -or $FrozenWorld -or $FrozenFrameTest -or $EnhancedSpriteTest -or $EnhancedSpriteReplay -or $EnhancedWorldTest -or $EnhancedWorld)) { throw 'LensTest cannot be combined with other rendering modes.' }
if ($WorldOverlayTest -and ($SceneCapture -or $Interactive -or $Terrain -or $World -or $FullWalls -or $Palette -or $SpriteReplay -or $TerrainReplay -or $OutlineReplay -or $FrozenWorld -or $FrozenFrameTest -or $EnhancedSpriteTest -or $EnhancedSpriteReplay -or $EnhancedWorldTest -or $EnhancedWorld -or $LensTest)) { throw 'WorldOverlayTest cannot be combined with other rendering modes.' }
if ($CompositionTest -and ($SceneCapture -or $Interactive -or $Terrain -or $World -or $FullWalls -or $Palette -or $SpriteReplay -or $TerrainReplay -or $OutlineReplay -or $FrozenWorld -or $FrozenFrameTest -or $EnhancedSpriteTest -or $EnhancedSpriteReplay -or $EnhancedWorldTest -or $EnhancedWorld -or $LensTest -or $WorldOverlayTest)) { throw 'CompositionTest cannot be combined with other rendering modes.' }
if ($FullWalls -and -not ($Terrain -or $World)) { throw 'FullWalls requires -Terrain or -World.' }
if (($IndexedFallback -or $IndexedFallbackTest) -and ($SceneCapture -or $Terrain -or $World -or $FullWalls -or $Palette -or $SpriteReplay -or $TerrainReplay -or $OutlineReplay -or $FrozenWorld -or $FrozenFrameTest -or $EnhancedSpriteTest -or $EnhancedSpriteReplay -or $EnhancedWorldTest -or $EnhancedWorld -or $EnhancedWorldNativeOrigin -or $LensTest -or $WorldOverlayTest -or $CompositionTest -or ($IndexedFallbackTest -and ($IndexedFallback -or $Interactive)))) { throw 'IndexedFallback requires its own file or IndexedFallbackTest fixture and cannot be combined with other rendering modes.' }
$workspace = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$cache = Join-Path $workspace 'downloads/keeperfx-dx12'
$artifacts = Join-Path $workspace 'artifacts/dx12'
New-Item -ItemType Directory -Path $cache,$artifacts -Force | Out-Null
$dependencies = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'dependencies-dx12.json') -Raw | ConvertFrom-Json
& (Join-Path $PSScriptRoot 'prepare-dx12-dependencies.ps1')
if (-not $SkipBuild) {
    $script = (& wsl.exe -d keeperfx-dev --exec wslpath -a (Join-Path $PSScriptRoot 'dx12-build.sh')).Trim()
    if ($LASTEXITCODE -ne 0) { throw 'The keeperfx-dev build environment is unavailable.' }
    & wsl.exe -d keeperfx-dev --exec bash $script 2>&1 | Tee-Object -FilePath (Join-Path $artifacts 'build.log')
    if ($LASTEXITCODE -ne 0) { throw 'DX12 foundation build failed.' }
}
$bin = Join-Path $artifacts 'bin'
foreach ($dll in @('D3D12Core.dll','d3d12SDKLayers.dll')) {
    $signature = Get-AuthenticodeSignature -LiteralPath (Join-Path $bin "D3D12/$dll")
    if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch 'Microsoft Corporation') {
        throw "Invalid Microsoft SDK signature: $dll ($($signature.Status))"
    }
}
if ($BuildOnly) { Write-Output "Built and verified: $bin"; return }
$run = Join-Path $artifacts ('run-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
New-Item -ItemType Directory -Path $run | Out-Null
$arguments = @('--output', ('"' + $run + '"'))
if ($SceneCapture) {
    $capturePath = (Resolve-Path -LiteralPath $SceneCapture).Path
    $arguments += @('--scene-capture', ('"' + $capturePath + '"'))
}
if ($IndexedFallback) {
    $indexedFallbackPath = (Resolve-Path -LiteralPath $IndexedFallback).Path
    $arguments += @('--indexed-fallback', ('"' + $indexedFallbackPath + '"'))
}
if ($IndexedFallbackTest) { $arguments += '--indexed-fallback-test' }
if ($RuntimeResourceTest) { $arguments += '--runtime-resource-test' }
if ($RuntimeWorldTest) { $arguments += '--runtime-world-test' }
if ($RuntimeProcessBudgetTest) { $arguments += '--runtime-process-budget-test' }
if ($RuntimeResourceInput) {
    $runtimeResourcePath = (Resolve-Path -LiteralPath $RuntimeResourceInput).Path
    $arguments += @('--runtime-resource-input', ('"' + $runtimeResourcePath + '"'))
}
if ($Interactive) { $arguments += '--interactive' }
if ($Terrain) { $arguments += '--terrain' }
if ($World) { $arguments += '--world' }
if ($Palette) { $arguments += '--palette-test' }
if ($SpriteReplay) { $arguments += '--sprite-replay' }
if ($TerrainReplay) { $arguments += '--terrain-replay' }
if ($OutlineReplay) { $arguments += '--outline-replay' }
if ($EnhancedSpriteTest) { $arguments += '--enhanced-sprite-test' }
if ($EnhancedSpriteReplay) { $arguments += '--enhanced-sprite-replay' }
if ($EnhancedWorldTest) { $arguments += '--enhanced-world-test' }
if ($EnhancedWorld) { $arguments += '--enhanced-world' }
if ($EnhancedWorldNativeOrigin) { $arguments += '--enhanced-world-native-origin' }
if ($EnhancedWorldFadeTexelLoad) { $arguments += '--enhanced-world-fade-texel-load' }
if ($LensTest) { $arguments += '--lens-test' }
if ($WorldOverlayTest) { $arguments += '--world-overlay-test' }
if ($CompositionTest) { $arguments += '--composition-test' }
if ($FrozenFrameTest) { $arguments += '--frozen-frame-test' }
if ($FrozenWorld) { $arguments += '--frozen-world' }
if ($FullWalls) { $arguments += '--full-walls' }
if ($GpuValidation) { $arguments += '--gpu-validation' }
if ($ResizeWarningTrace) { $arguments += '--resize-warning-trace' }
if ($NoValidation) { $arguments += '--no-validation' }
if ($Warp) { $arguments += '--warp' }
if ($AdapterLuid) { $arguments += @('--adapter-luid', $AdapterLuid.ToLowerInvariant()) }
if ($NoVSync) { $arguments += '--no-vsync' }
$exe = Join-Path $bin 'keeperfx-dx12.exe'
$process = Start-Process -FilePath $exe -ArgumentList $arguments -WorkingDirectory $bin -WindowStyle Hidden -PassThru `
    -RedirectStandardOutput (Join-Path $run 'stdout.log') -RedirectStandardError (Join-Path $run 'stderr.log')
$manifest = [ordered]@{
    timestamp = (Get-Date).ToUniversalTime().ToString('o')
    executablePath = $exe
    executableSha256 = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash
    reusedBuild = [bool]$SkipBuild
    requestedHardwareAdapterLuid = if ($AdapterLuid) { $AdapterLuid.ToLowerInvariant() } else { $null }
    requestedResizeWarningTrace = [bool]$ResizeWarningTrace
    timeoutSeconds = $TimeoutSeconds
    enhancedWorldFadeAddressing = if ($EnhancedWorld -or $EnhancedWorldTest) { if ($EnhancedWorldFadeTexelLoad) { 'texel-load' } else { 'sampler' } } else { $null }
    dependencies = $dependencies
    arguments = $arguments
    processId = $process.Id
    sourceRevision = (& git -C (Split-Path -Parent $PSScriptRoot) rev-parse HEAD)
    sourceStatus = @(& git -C (Split-Path -Parent $PSScriptRoot) status --short)
    sceneCaptureSha256 = if ($SceneCapture) { (Get-FileHash -LiteralPath $capturePath).Hash } else { $null }
    indexedFallbackPath = if ($IndexedFallback) { $indexedFallbackPath } else { $null }
    indexedFallbackSha256 = if ($IndexedFallback) { (Get-FileHash -LiteralPath $indexedFallbackPath -Algorithm SHA256).Hash } else { $null }
    indexedFallbackSynthetic = [bool]$IndexedFallbackTest
    runtimeWorldTest = [bool]$RuntimeWorldTest
    runtimeProcessBudgetTest = [bool]$RuntimeProcessBudgetTest
    runtimeResourceTest = [bool]$RuntimeResourceTest
    runtimeResourceInputPath = if ($RuntimeResourceInput) { $runtimeResourcePath } else { $null }
    runtimeResourceInputSha256 = if ($RuntimeResourceInput) { (Get-FileHash -LiteralPath $runtimeResourcePath -Algorithm SHA256).Hash } else { $null }
}
$manifest | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $run 'manifest.json') -Encoding utf8
if ($Interactive) { Write-Output "DX12 viewer running. Escape closes it. Evidence: $run"; return }
if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
    Stop-Process -Id $process.Id -ErrorAction SilentlyContinue
    throw "DX12 test timed out. See $run"
}
$process.WaitForExit()
# Failed comparisons need viewable evidence as well as successful ones.
Add-Type -AssemblyName System.Drawing
foreach ($bmp in Get-ChildItem -LiteralPath $run -Filter '*.bmp') {
    $bitmap = [System.Drawing.Bitmap]::new($bmp.FullName)
    try { $bitmap.Save([IO.Path]::ChangeExtension($bmp.FullName, '.png'), [System.Drawing.Imaging.ImageFormat]::Png) }
    finally { $bitmap.Dispose() }
}
if ($process.ExitCode -ne 0) {
    Get-Content -LiteralPath (Join-Path $run 'stderr.log')
    throw "DX12 test failed ($($process.ExitCode)). See $run"
}
$report = Get-Content -LiteralPath (Join-Path $run 'report.json') -Raw | ConvertFrom-Json
if ($report.status -ne 'passed') { throw "DX12 report did not pass: $run" }
$report | ConvertTo-Json -Depth 5
Write-Output "Evidence: $run"
