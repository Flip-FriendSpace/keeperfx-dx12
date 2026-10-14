# SPDX-License-Identifier: GPL-2.0-or-later
# KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
param(
  [ValidateSet('OPENGL','SOFTWARE')][string]$Renderer = 'OPENGL',
  [ValidateSet('ENG','FRE','GER','JPN')][string]$Language = 'ENG',
  [ValidateSet('menu','level','movie')][string]$Start = 'menu'
)
$ErrorActionPreference = 'Stop'
$workspace = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$repo = Split-Path -Parent $PSScriptRoot
$runtime = Join-Path $workspace 'runtime/keeperfx-x64'
if (Get-Process -Name keeperfx,keeperfx-local,keeperfx-local-hvlog -ErrorAction SilentlyContinue) {
  throw 'Close KeeperFX before starting a disposable UI reference session.'
}
$output = Join-Path $workspace ('artifacts/reference/UI-' + $Renderer + '-' + $Language + '-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
New-Item -ItemType Directory -Path $output -Force | Out-Null
$configPath = Join-Path $runtime 'keeperfx.cfg'
$testConfigPath = Join-Path $runtime 'keeperfx-ui-reference.cfg'
$backups = @{}
foreach ($path in @($configPath,$testConfigPath,(Join-Path $env:APPDATA 'keeperfx/keeperfx/renderer_prefs.ini'),(Join-Path $runtime 'save/settings.toml'),(Join-Path $runtime 'save/fx1lastf.sav'))) {
  $backups[$path] = if (Test-Path -LiteralPath $path) { [IO.File]::ReadAllBytes($path) } else { $null }
}
$oldScene = $env:KFX_SCENE_TEST_DIR
$oldReference = $env:KFX_SCENE_REFERENCE
$process = $null
try {
  $config = Get-Content -LiteralPath $configPath -Raw
  $settings = @{ LANGUAGE=$Language; API_ENABLED='TRUE'; API_PORT='5599'; AUTOMATIC_REPLAYS='OFF'; FREEZE_GAME_ON_FOCUS_LOST='OFF'; RENDERER=$Renderer; CAPTURE_CURSOR='OFF'; CURSOR_EDGE_CAMERA_PANNING='OFF'; VSYNC='OFF'; INGAME_RES='1920x1080w32'; FRONTEND_RES='640x480w32 1920x1080w32 1920x1080w32' }
  foreach ($key in $settings.Keys) { $config = $config -replace ('(?m)^' + $key + '=.*$'), ($key + '=' + $settings[$key]) }
  [IO.File]::WriteAllText($testConfigPath,$config)
  Copy-Item -LiteralPath $PSCommandPath -Destination $output
  git -C $repo diff HEAD --binary | Set-Content (Join-Path $output 'source.patch') -Encoding utf8
  foreach ($relative in (git -C $repo ls-files --others --exclude-standard)) {
    if ($relative -notmatch '^(src|local-dev)/' -or $relative -match '(^|/)\.\.(/|$)') { throw "Unexpected untracked input: $relative" }
    $destination = Join-Path (Join-Path $output 'untracked-source') $relative
    New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $repo $relative) -Destination $destination
  }
  @{version=1;revision=(git -C $repo rev-parse HEAD);status=@(git -C $repo status --porcelain=v1);renderer=$Renderer;language=$Language;start=$Start;resolution=@(1920,1080);executableHash=(Get-FileHash (Join-Path $runtime 'keeperfx-local.exe')).Hash;configurationHash=(Get-FileHash $testConfigPath).Hash;scope='Disposable UI reference session. Screenshots require explicit visual review; this does not qualify DX12 UI or gameplay.'} | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $output 'run.json') -Encoding utf8
  $env:KFX_SCENE_TEST_DIR = $output
  $env:KFX_SCENE_REFERENCE = '1'
  $arguments = @('-config','keeperfx-ui-reference.cfg','-nosound','-alex')
  if ($Start -ne 'movie') { $arguments += @('-nointro','-skipheartzoom') }
  if ($Start -eq 'level') { $arguments += @('-campaign','keeporig','-level','1') }
  $process = Start-Process -FilePath (Join-Path $runtime 'keeperfx-local.exe') -WorkingDirectory $runtime -ArgumentList $arguments -WindowStyle Hidden -PassThru
  Write-Output "UI reference session: $output"
  Write-Output "Process: $($process.Id)"
  $process.WaitForExit()
  if ($process.ExitCode -ne 0) { throw "UI reference process exited with $($process.ExitCode)." }
} finally {
  if ($process -and -not $process.HasExited) { Stop-Process -Id $process.Id }
  if (Test-Path -LiteralPath (Join-Path $runtime 'keeperfx.log')) { Copy-Item -LiteralPath (Join-Path $runtime 'keeperfx.log') -Destination $output }
  $env:KFX_SCENE_TEST_DIR = $oldScene
  $env:KFX_SCENE_REFERENCE = $oldReference
  foreach ($path in $backups.Keys) {
    if ($null -ne $backups[$path]) { [IO.File]::WriteAllBytes($path,$backups[$path]) }
    elseif (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path }
  }
  Write-Output "Restored normal settings; UI evidence retained at $output"
}
