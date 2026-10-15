# SPDX-License-Identifier: GPL-2.0-or-later
param(
  [ValidateSet('Diagnostic')][string]$Mode='Diagnostic',
  [ValidateSet('Original','HD')][string]$Variant='HD'
)
$ErrorActionPreference='Stop'
$workspace=Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $PSScriptRoot))
$runtime=Join-Path $workspace 'runtime/keeperfx-x64'
$art=Join-Path $workspace 'artifacts/art-prototype/heroes'
$executable=Join-Path $runtime 'keeperfx-dx12.exe'
if (Get-Process -Name keeperfx,keeperfx-local,keeperfx-dx12 -ErrorAction SilentlyContinue) { throw 'Close KeeperFX before running the hero roster test.' }
if (@(Get-NetTCPConnection -State Listen -ErrorAction Stop | Where-Object LocalPort -EQ 5599).Count) { throw 'API port 5599 is occupied.' }
$nodeExecutable=(Get-Command node -ErrorAction Stop).Source
$output=Join-Path $art ($Mode+'-'+$Variant+'-'+(Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
New-Item -ItemType Directory -Path $output | Out-Null
$testConfig=Join-Path $runtime 'keeperfx-heroes-diagnostic.cfg'
$loadOrder=Join-Path $runtime 'mods/load_order.cfg'
$modTarget=Join-Path $runtime 'mods/heroes_art_prototype'
if ((Test-Path -LiteralPath $modTarget) -and -not (Test-Path -LiteralPath (Join-Path $modTarget 'local-prototype.json'))) { throw 'Existing mod directory has no local prototype marker; refusing to overwrite it.' }
$backups=@{}
foreach ($path in @($testConfig,$loadOrder,(Join-Path $runtime 'keeperfx.cfg'),(Join-Path $runtime 'save/settings.toml'),(Join-Path $runtime 'save/fx1lastf.sav'),(Join-Path $env:APPDATA 'keeperfx/keeperfx/renderer_prefs.ini'))) {
  $backups[$path]=if (Test-Path -LiteralPath $path) { [IO.File]::ReadAllBytes($path) } else { $null }
}
$oldScene=$env:KFX_SCENE_TEST_DIR
$oldReference=$env:KFX_SCENE_REFERENCE
$oldLive=$env:KFX_SCENE_LIVE_SURFACES
$benchNames=@('KFX_RENDER_BENCHMARK_DIR','KFX_RENDER_BENCHMARK_WARMUP_SECONDS','KFX_RENDER_BENCHMARK_SAMPLE_SECONDS','KFX_DX12_PERFORMANCE','KFX_DX12_GPU_VALIDATION')
$oldBench=@{}
foreach ($key in $benchNames) {$oldBench[$key]=[Environment]::GetEnvironmentVariable($key)}
$game=$null;$client=$null;$passed=$false;$apiOwnerVerified=$false;$failure=$null;$validation=$null
try {
  $order=Get-Content -LiteralPath $loadOrder -Raw
  if ($order -match '(?m)^heroes_art_prototype\s*$') { throw 'Heroes prototype is already enabled; leave the current configuration untouched.' }
  New-Item -ItemType Directory -Path $modTarget -Force | Out-Null
  Copy-Item -Path (Join-Path $art 'mod/heroes_art_prototype/*') -Destination $modTarget -Recurse -Force
  $roster=@('wizard','barbarian','archer','monk','dwarfa','knight','avatar','tunneller','witch','giant','fairy','thief','samurai','imp')
  $creatureBackups=@{}
  foreach ($slug in $roster) {
    $target=Join-Path $modTarget ('creatrs/'+$slug+'.cfg')
    if (-not (Test-Path -LiteralPath $target)) {throw "Combined package missing creature config: $slug"}
    $creatureBackups[$target]=[IO.File]::ReadAllBytes($target)
    if ($Variant -eq 'Original') {
      $source=Get-Content -LiteralPath (Join-Path $workspace ('keeperfx/config/creatrs/'+$slug+'.cfg')) -Raw
      $section=[regex]::Match($source,'(?ms)^\[sprites\]\s*$(.*?)(?=^\[|\z)')
      if (-not $section.Success) {throw "Missing original sprite section: $slug"}
      [IO.File]::WriteAllText($target,$section.Value)
    }
  }
  [IO.File]::WriteAllText($loadOrder,($order.TrimEnd()+"`r`nheroes_art_prototype`r`n"))
  $config=Get-Content -LiteralPath (Join-Path $runtime 'keeperfx.cfg') -Raw
  $settings=@{API_ENABLED='TRUE';API_PORT='5599';AUTOMATIC_REPLAYS='OFF';FREEZE_GAME_ON_FOCUS_LOST='OFF';RENDERER='DX12';CAPTURE_CURSOR='OFF';CURSOR_EDGE_CAMERA_PANNING='OFF';VSYNC='OFF';INGAME_RES='1920x1080w32'}
  foreach ($key in $settings.Keys) {
    $pattern='(?m)^'+$key+'=.*$'
    if ($config -match $pattern) {$config=$config -replace $pattern,($key+'='+$settings[$key])}
    else {$config+="`r`n$key=$($settings[$key])`r`n"}
  }
  [IO.File]::WriteAllText($testConfig,$config)
  $env:KFX_SCENE_TEST_DIR=$output;$env:KFX_SCENE_REFERENCE='1';$env:KFX_SCENE_LIVE_SURFACES='1'
  foreach ($key in $benchNames) {[Environment]::SetEnvironmentVariable($key,$null)}
  if ($Mode -eq 'Benchmark') {
    $env:KFX_RENDER_BENCHMARK_DIR=$output;$env:KFX_RENDER_BENCHMARK_WARMUP_SECONDS='5';$env:KFX_RENDER_BENCHMARK_SAMPLE_SECONDS='20';$env:KFX_DX12_PERFORMANCE='1'
  }
  $env:KFX_DX12_GPU_VALIDATION='1'
  $game=Start-Process -FilePath $executable -WorkingDirectory $runtime -WindowStyle Hidden -PassThru -ArgumentList '-config keeperfx-heroes-diagnostic.cfg -nointro -skipheartzoom -campaign keeporig -level 1 -nosound -alex'
  $timer=[Diagnostics.Stopwatch]::StartNew()
  while ($timer.Elapsed.TotalSeconds -lt 30) {
    if ($game.HasExited) {throw 'Game exited before API readiness.'}
    foreach ($listener in @(Get-NetTCPConnection -State Listen -ErrorAction Stop | Where-Object LocalPort -EQ 5599)) {
      if ($listener.OwningProcess -ne $game.Id -or $listener.LocalAddress -ne '127.0.0.1') {throw 'API listener ownership or loopback check failed.'}
      $apiOwnerVerified=$true
    }
    if ($apiOwnerVerified) {break}
    Start-Sleep -Milliseconds 250
  }
  if (-not $apiOwnerVerified) {throw 'No owned loopback API listener within 30 seconds.'}
  $clientScript='test_heroes_diagnostic.mjs'
  $client=Start-Process -FilePath $nodeExecutable -WindowStyle Hidden -PassThru -ArgumentList @(('"'+(Join-Path $PSScriptRoot $clientScript)+'"'),('"'+$output+'"'),$Mode,$Variant) -RedirectStandardOutput (Join-Path $output 'client.stdout.log') -RedirectStandardError (Join-Path $output 'client.stderr.log')
  $timer.Restart()
  while (-not $client.WaitForExit(100)) {
    if ($timer.Elapsed.TotalSeconds -gt 300) {throw 'Sprite fixture exceeded its time bound.'}
    if ($game.HasExited -and -not $client.WaitForExit(1000)) {throw 'Game exited before the sprite fixture finished.'}
  }
  $client.WaitForExit()
  if ($client.ExitCode -ne 0) {throw 'Sprite fixture failed; inspect client.stderr.log.'}
  if (-not $game.WaitForExit(10000) -or $game.ExitCode -ne 0) {throw 'Game did not exit cleanly.'}
  $log=Get-Content -LiteralPath (Join-Path $runtime 'keeperfx.log') -Raw
  $sessions=[regex]::Matches($log,'DX12 session: submitted (?<submitted>[0-9]+), rejected (?<rejected>[0-9]+)\b')
  $validation=@{initialized=$log.Contains('DX12 development backend initialized');worldSubmitted=$log.Contains('DX12 submitted first world frame');submitted=0;rejected=0}
  foreach ($s in $sessions) {$validation.submitted+=[int]$s.Groups['submitted'].Value;$validation.rejected+=[int]$s.Groups['rejected'].Value}
  if (-not $validation.initialized -or -not $validation.worldSubmitted -or $validation.submitted -le 0 -or $validation.rejected -ne 0) {throw 'DX12 submission validation failed.'}
  $passed=$true
} catch {$failure=$_.Exception.Message;throw}
finally {
  $env:KFX_SCENE_TEST_DIR=$oldScene;$env:KFX_SCENE_REFERENCE=$oldReference;$env:KFX_SCENE_LIVE_SURFACES=$oldLive
  foreach ($key in $benchNames) {[Environment]::SetEnvironmentVariable($key,$oldBench[$key])}
  if ($client -and -not $client.HasExited) {Stop-Process -Id $client.Id -ErrorAction SilentlyContinue}
  if ($game -and -not $game.HasExited) {Stop-Process -Id $game.Id -ErrorAction SilentlyContinue}
  if ($game) {$game.WaitForExit(10000) | Out-Null}
  if ($creatureBackups) {foreach ($path in $creatureBackups.Keys) {[IO.File]::WriteAllBytes($path,$creatureBackups[$path])}}
  foreach ($path in $backups.Keys) {
    if ($null -ne $backups[$path]) {[IO.File]::WriteAllBytes($path,$backups[$path])}
    elseif (Test-Path -LiteralPath $path) {Remove-Item -LiteralPath $path}
  }
  $restored=$true
  foreach ($path in $backups.Keys) {
    if ($null -eq $backups[$path]) {$restored=$restored -and -not (Test-Path -LiteralPath $path)}
    else {$restored=$restored -and ([Convert]::ToBase64String([IO.File]::ReadAllBytes($path)) -eq [Convert]::ToBase64String($backups[$path]))}
  }
  if (Test-Path -LiteralPath (Join-Path $runtime 'keeperfx.log')) {Copy-Item -LiteralPath (Join-Path $runtime 'keeperfx.log') -Destination (Join-Path $output 'keeperfx.log') -Force}
  @{passed=$passed;error=$failure;apiOwnerVerified=$apiOwnerVerified;settingsRestored=$restored;modEnabledAfterTest=$false;dx12=$validation;executable=$executable;packageHashes=@(Get-ChildItem -LiteralPath (Join-Path $art 'mod/heroes_art_prototype/fxdata') -Filter '*.zip' | ForEach-Object {@{file=$_.Name;sha256=(Get-FileHash -LiteralPath $_.FullName).Hash}});comparison='Both variants keep HD archives loaded; original overrides only sprite mappings, not archive residency.'} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $output 'run-report.json') -Encoding utf8
  Write-Output "Heroes sprite evidence: $output"
}
