# SPDX-License-Identifier: GPL-2.0-or-later
# KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
param(
  [ValidateSet('OPENGL','SOFTWARE')][string]$Renderer = 'OPENGL',
  [ValidateSet('x86','x64')][string]$Architecture = 'x64',
  [ValidateSet(1,5,20)][int[]]$Levels = @(1,5,20),
  [ValidateSet('baseline','details','lenses','lens-script','world','settings','effects','doors','cameras','keys','selection')][string]$Suite = 'baseline',
  [ValidateSet('3840x2160','1920x1080','640x480')][string]$Resolution = '3840x2160',
  [string[]]$Cases = @()
)
$ErrorActionPreference = 'Stop'
$catalog = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'reference-scenes.json') -Raw | ConvertFrom-Json
if ($Suite -eq 'selection' -and $Renderer -ne 'OPENGL') { throw 'The selection suite requires owned OpenGL composition.' }
if ($Cases.Count -gt 0 -and $Suite -eq 'baseline') { throw 'Baseline cases have ordered dependencies; run the whole baseline suite for a level.' }
$selected = @($catalog.scenes | Where-Object { $Levels -contains $_.level -and (($_.suite -eq $Suite) -or (-not $_.suite -and $Suite -eq 'baseline')) -and ($_.resolution -eq $Resolution -or (-not $_.resolution -and $Resolution -eq '3840x2160')) -and ($Cases.Count -eq 0 -or $Cases -contains $_.id) })
if ($selected.Count -eq 0) { throw 'No reference cases match the requested levels and suite.' }
foreach ($id in $Cases) { if ($selected.id -notcontains $id) { throw "Unknown reference case for the requested levels and suite: $id" } }
foreach ($scene in $selected) { if ($scene.continuationOf -and $selected.id -notcontains $scene.continuationOf) { throw "Frozen variant $($scene.id) requires $($scene.continuationOf)." } }
$workspace = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$runtime = Join-Path $workspace $(if ($Architecture -eq 'x64') { 'runtime/keeperfx-x64' } else { 'runtime/keeperfx' })
if (Get-Process -Name keeperfx,keeperfx-local,keeperfx-local-hvlog -ErrorAction SilentlyContinue) {
  throw 'Close KeeperFX before capturing reference scenes.'
}
$output = Join-Path $workspace ('artifacts/reference/' + $Renderer + '-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
New-Item -ItemType Directory -Path $output -Force | Out-Null
$backups = @{}
$configPath = Join-Path $runtime 'keeperfx.cfg'
$prefsPath = Join-Path $env:APPDATA 'keeperfx/keeperfx/renderer_prefs.ini'
$testConfigPath = Join-Path $runtime 'keeperfx-reference-test.cfg'
$lensFixture = @($selected.id | Where-Object { $_ -in @('lens-config-overlay','lens-config-mist-overlay') }).Count -gt 0
$lensConfigPath = Join-Path $runtime 'fxdata/lenses.cfg'
foreach ($path in @($configPath,$prefsPath,$testConfigPath,(Join-Path $runtime 'save/settings.toml'),(Join-Path $runtime 'save/fx1lastf.sav'))) {
  $backups[$path] = if (Test-Path -LiteralPath $path) { [IO.File]::ReadAllBytes($path) } else { $null }
}
$oldScene = $env:KFX_SCENE_TEST_DIR
$oldReference = $env:KFX_SCENE_REFERENCE
$process = $null
try {
  if ($lensFixture) {
    $backups[$lensConfigPath] = [IO.File]::ReadAllBytes($lensConfigPath)
    $lensConfig = Get-Content -LiteralPath $lensConfigPath -Raw
    if ($lensConfig -match '(?im)^\s*\[lens(15|16)\]') { throw 'Configured lens reference slots 15/16 are already occupied.' }
    $lensAsset = Join-Path $runtime 'data/frac00.raw'
    if ((Get-Item -LiteralPath $lensAsset).Length -ne 65536) { throw 'Configured lens fixture requires the existing 256x256 frac00.raw asset.' }
    $lensConfig += "`r`n[lens15]`r`nName = REFERENCE_OVERLAY`r`nOverlay = frac00.raw 128`r`n`r`n[lens16]`r`nName = REFERENCE_MIST_OVERLAY`r`nMist = frac00.raw 0 0 0 0 0 0`r`nOverlay = frac00.raw 128`r`n"
    [IO.File]::WriteAllText($lensConfigPath,$lensConfig)
    Copy-Item -LiteralPath $lensConfigPath -Destination (Join-Path $output 'lenses-fixture.cfg')
    @{configSha256=(Get-FileHash $lensConfigPath).Hash;asset='data/frac00.raw';assetSha256=(Get-FileHash $lensAsset).Hash;slots=@(15,16);scope='Temporary configured lens references; runtime config restored in finally'} | ConvertTo-Json | Set-Content (Join-Path $output 'lens-fixture.json') -Encoding utf8
  }
  $config = Get-Content -LiteralPath $configPath -Raw
  $settings = @{ API_ENABLED='TRUE'; API_PORT='5599'; AUTOMATIC_REPLAYS='OFF'; FREEZE_GAME_ON_FOCUS_LOST='OFF'; RENDERER=$Renderer; CAPTURE_CURSOR='OFF'; CURSOR_EDGE_CAMERA_PANNING='OFF'; VSYNC='OFF'; INGAME_RES=($Resolution+'w32') }
  foreach ($key in $settings.Keys) { $config = $config -replace ('(?m)^' + $key + '=.*$'), ($key + '=' + $settings[$key]) }
  [IO.File]::WriteAllText($testConfigPath,$config)
  $source = git -C (Split-Path -Parent $PSScriptRoot) rev-parse HEAD
  $status = git -C (Split-Path -Parent $PSScriptRoot) status --porcelain=v1
  git -C (Split-Path -Parent $PSScriptRoot) diff HEAD --binary | Set-Content (Join-Path $output 'source.patch') -Encoding utf8
  # A diff cannot retain newly created source files. Archive their exact bytes
  # alongside it so a dirty-tree reference can be reproduced later.
  $repo = Split-Path -Parent $PSScriptRoot
  $untracked = git -C $repo ls-files --others --exclude-standard
  foreach ($relative in $untracked) {
    if ($relative -notmatch '^(src|local-dev)/' -or $relative -match '(^|/)\.\.(/|$)') { throw "Unexpected untracked input: $relative" }
    $destination = Join-Path (Join-Path $output 'untracked-source') $relative
    New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $repo $relative) -Destination $destination
  }
  Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'reference-scenes.json') -Destination $output
  Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'reference-test.mjs') -Destination $output
  Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'reference-fingerprint.mjs') -Destination $output
  Copy-Item -LiteralPath $PSCommandPath -Destination $output
  @{revision=$source;status=@($status);renderer=$Renderer;architecture=$Architecture;suite=$Suite;resolution=$Resolution;levels=$Levels;cases=@($selected.id);executableHash=(Get-FileHash (Join-Path $runtime 'keeperfx-local.exe')).Hash;normalConfigHash=(Get-FileHash $configPath).Hash;preferencesHash=(Get-FileHash $prefsPath).Hash} | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $output 'run.json') -Encoding utf8
  foreach ($level in $Levels) {
    $levelCases = @($selected | Where-Object { $_.level -eq $level })
    if ($levelCases.Count -eq 0) { continue }
    $levelOutput = Join-Path $output "level-$level"
    New-Item -ItemType Directory -Path $levelOutput -Force | Out-Null
    $env:KFX_SCENE_TEST_DIR = $levelOutput
    $env:KFX_SCENE_REFERENCE = '1'
    $process = Start-Process -FilePath (Join-Path $runtime 'keeperfx-local.exe') -WorkingDirectory $runtime -WindowStyle Hidden -PassThru -ArgumentList "-config keeperfx-reference-test.cfg -nointro -skipheartzoom -campaign keeporig -level $level -nosound -alex"
    try {
      $ready = $false
      for ($attempt=0; $attempt -lt 60; $attempt++) {
        if ($process.HasExited) { throw "KeeperFX exited before reference level $level was ready." }
        # Inspect listener ownership without opening a disposable connection
        # that could occupy the one-slot API backlog while the level loads.
        $listeners = @(Get-NetTCPConnection -State Listen -ErrorAction Stop | Where-Object LocalPort -EQ 5599)
        foreach ($listener in $listeners) {
          if ($listener.OwningProcess -ne $process.Id) { throw "Reference API port 5599 belongs to process $($listener.OwningProcess), expected KeeperFX $($process.Id)." }
          if ($listener.LocalAddress -ne '127.0.0.1') { throw "Reference API listener is not loopback-only: $($listener.LocalAddress)." }
          $ready = $true
        }
        if ($ready) { break }
        Start-Sleep -Milliseconds 500
      }
      if (-not $ready) { throw 'Reference API unavailable.' }
      & node (Join-Path $PSScriptRoot 'reference-test.mjs') $levelOutput $level $Renderer $Suite ($levelCases.id -join ',')
      if ($LASTEXITCODE -ne 0) { throw "Reference capture failed on level $level." }
      if (-not $process.WaitForExit(10000) -or $process.ExitCode -ne 0) { throw 'KeeperFX did not exit cleanly.' }
    } finally {
      if ($process -and -not $process.HasExited) { Stop-Process -Id $process.Id }
      Copy-Item -LiteralPath (Join-Path $runtime 'keeperfx.log') -Destination (Join-Path $levelOutput 'keeperfx.log')
    }
  }
} finally {
  $env:KFX_SCENE_TEST_DIR = $oldScene
  $env:KFX_SCENE_REFERENCE = $oldReference
  foreach ($path in $backups.Keys) {
    if ($null -ne $backups[$path]) { [IO.File]::WriteAllBytes($path,$backups[$path]) }
    elseif (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path }
  }
  Write-Output "Reference evidence: $output"
}
