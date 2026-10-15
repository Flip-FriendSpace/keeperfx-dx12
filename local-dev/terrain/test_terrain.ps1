# SPDX-License-Identifier: GPL-2.0-or-later
param([ValidateSet('Original','HD','Default')][string]$Variant='Default',[switch]$FullTerrain,[string]$RuntimePath,[string]$ArtifactRoot)
$ErrorActionPreference='Stop'
$workspace=Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $PSScriptRoot))
$runtime=if($RuntimePath){[IO.Path]::GetFullPath($RuntimePath)}else{Join-Path $workspace 'runtime/keeperfx-x64'}
$art=if($ArtifactRoot){[IO.Path]::GetFullPath($ArtifactRoot)}else{Join-Path $workspace 'artifacts/terrain'}
$pack=Join-Path $runtime 'fxdata/standard-hd.kfht'
$manifestPath=Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'assets/terrain/standard-hd/manifest.json'
$manifest=Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
$tiles=@($manifest.tiles | ForEach-Object { [int]$_.tile })
if ($tiles.Count -gt 64 -and -not $FullTerrain) {throw 'Use -FullTerrain to validate the complete pack and all animation aliases.'}
if (-not (Test-Path -LiteralPath $pack)) {throw 'Install the enhanced terrain bundle before running the fixture.'}
if (Get-Process -Name keeperfx,keeperfx-local,keeperfx-local-hvlog,keeperfx-dx12 -ErrorAction SilentlyContinue) {throw 'Close KeeperFX before running the terrain pilot.'}
if (@(Get-NetTCPConnection -State Listen -ErrorAction Stop | Where-Object LocalPort -EQ 5599).Count) {throw 'API port 5599 is occupied.'}
$output=Join-Path $art ('Validation-'+$Variant+'-'+(Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
New-Item -ItemType Directory -Path $output | Out-Null
$testConfig=Join-Path $runtime 'keeperfx-terrain-art.cfg';$loadOrder=Join-Path $runtime 'mods/load_order.cfg'
$backups=@{}
foreach($path in @($testConfig,$loadOrder,(Join-Path $runtime 'keeperfx.cfg'),(Join-Path $runtime 'save/settings.toml'),(Join-Path $runtime 'save/fx1lastf.sav'),(Join-Path $runtime 'keeperfx.log'),(Join-Path $env:APPDATA 'keeperfx/keeperfx/renderer_prefs.ini'))){$backups[$path]=if(Test-Path -LiteralPath $path){[IO.File]::ReadAllBytes($path)}else{$null}}
$envNames=@('KFX_HD_TERRAIN_PACK','KFX_SCENE_TEST_DIR','KFX_SCENE_REFERENCE','KFX_SCENE_LIVE_SURFACES','KFX_DX12_PERFORMANCE','KFX_RENDER_BENCHMARK_DIR','KFX_RENDER_BENCHMARK_WARMUP_SECONDS','KFX_RENDER_BENCHMARK_SAMPLE_SECONDS')
$oldEnv=@{};foreach($key in $envNames){$oldEnv[$key]=[Environment]::GetEnvironmentVariable($key)}
$game=$null;$client=$null;$passed=$false;$apiOwnerVerified=$false;$failure=$null;$validation=$null
try{
 $order=[IO.File]::ReadAllText($loadOrder);[IO.File]::WriteAllText($loadOrder,($order -replace '(?m)^keeper_enhanced_default\s*$', '; Texture pilot temporarily uses original themes'))
 $config=[IO.File]::ReadAllText((Join-Path $runtime 'keeperfx.cfg'))
 $settings=@{API_ENABLED='TRUE';API_PORT='5599';AUTOMATIC_REPLAYS='OFF';FREEZE_GAME_ON_FOCUS_LOST='OFF';RENDERER='DX12';CAPTURE_CURSOR='OFF';CURSOR_EDGE_CAMERA_PANNING='OFF';VSYNC='OFF';INGAME_RES='1920x1080w32'}
 foreach($key in $settings.Keys){$pattern='(?m)^'+$key+'=.*$';if($config -match $pattern){$config=$config -replace $pattern,($key+'='+$settings[$key])}else{$config+="`r`n$key=$($settings[$key])`r`n"}}
 [IO.File]::WriteAllText($testConfig,$config)
 foreach($key in $envNames){[Environment]::SetEnvironmentVariable($key,$null)}
 if($Variant -eq 'Original'){$env:KFX_HD_TERRAIN_PACK='off'}elseif($Variant -eq 'HD'){$env:KFX_HD_TERRAIN_PACK=$pack}
 $env:KFX_SCENE_TEST_DIR=$output;$env:KFX_SCENE_REFERENCE='1';$env:KFX_SCENE_LIVE_SURFACES='1'
 $executable=Join-Path $runtime 'keeperfx-dx12.exe'
 $game=Start-Process -FilePath $executable -WorkingDirectory $runtime -WindowStyle Hidden -PassThru -ArgumentList '-config keeperfx-terrain-art.cfg -nointro -skipheartzoom -campaign keeporig -level 1 -nosound -alex'
 $timer=[Diagnostics.Stopwatch]::StartNew()
 while($timer.Elapsed.TotalSeconds -lt 30){if($game.HasExited){throw 'Game exited before API readiness.'};foreach($listener in @(Get-NetTCPConnection -State Listen -ErrorAction Stop | Where-Object LocalPort -EQ 5599)){if($listener.OwningProcess -ne $game.Id -or $listener.LocalAddress -ne '127.0.0.1'){throw 'API listener ownership or loopback check failed.'};$apiOwnerVerified=$true};if($apiOwnerVerified){break};Start-Sleep -Milliseconds 250}
 if(-not $apiOwnerVerified){throw 'No owned loopback API listener.'}
 $node=(Get-Command node -ErrorAction Stop).Source
 $client=Start-Process -FilePath $node -WindowStyle Hidden -PassThru -ArgumentList @(('"'+(Join-Path $PSScriptRoot 'test_terrain.mjs')+'"'),('"'+$output+'"'),($Variant),([string]$tiles.Count),([string][int][bool]$FullTerrain)) -RedirectStandardOutput (Join-Path $output 'client.stdout.log') -RedirectStandardError (Join-Path $output 'client.stderr.log')
 $timer.Restart();while(-not $client.WaitForExit(100)){if($timer.Elapsed.TotalSeconds -gt 300){throw 'Terrain fixture exceeded its time bound.'};if($game.HasExited -and -not $client.WaitForExit(1000)){throw 'Game exited before the fixture finished.'}}
 $client.WaitForExit();if($client.ExitCode -ne 0){throw 'Terrain fixture failed; inspect client.stderr.log.'}
 if(-not $game.WaitForExit(10000) -or $game.ExitCode -ne 0){throw 'Game did not exit cleanly.'}
 $log=[IO.File]::ReadAllText((Join-Path $runtime 'keeperfx.log'))
 $sessions=[regex]::Matches($log,'DX12 session: submitted (?<submitted>[0-9]+), rejected (?<rejected>[0-9]+)\b')
 $validation=@{submitted=0;rejected=0;matchedTiles=@([regex]::Matches($log,'HD terrain matched tile ([0-9]+):') | ForEach-Object {[int]$_.Groups[1].Value})}
 foreach($s in $sessions){$validation.submitted+=[int]$s.Groups['submitted'].Value;$validation.rejected+=[int]$s.Groups['rejected'].Value}
 if($validation.submitted -le 0 -or $validation.rejected -ne 0){throw 'DX12 submission validation failed.'}
 if($Variant -ne 'Original'){foreach($tile in $tiles){if($validation.matchedTiles -notcontains $tile){throw "HD tile $tile was not used by the live fixture."}}}
 elseif($validation.matchedTiles.Count){throw 'Original control unexpectedly used HD textures.'}
 $passed=$true
}catch{$failure=$_.Exception.Message;throw}
finally{
 if($client -and -not $client.HasExited){Stop-Process -Id $client.Id -ErrorAction SilentlyContinue}
 if($game -and -not $game.HasExited){Stop-Process -Id $game.Id -ErrorAction SilentlyContinue}
 if($game){$game.WaitForExit(10000)|Out-Null}
 if(Test-Path -LiteralPath (Join-Path $runtime 'keeperfx.log')){Copy-Item -LiteralPath (Join-Path $runtime 'keeperfx.log') -Destination (Join-Path $output 'keeperfx.log') -Force}
 foreach($key in $envNames){[Environment]::SetEnvironmentVariable($key,$oldEnv[$key])}
 foreach($path in $backups.Keys){if($null -ne $backups[$path]){[IO.File]::WriteAllBytes($path,$backups[$path])}elseif(Test-Path -LiteralPath $path){Remove-Item -LiteralPath $path}}
 $restored=$true;foreach($path in $backups.Keys){if($null -eq $backups[$path]){$restored=$restored -and -not(Test-Path -LiteralPath $path)}else{$restored=$restored -and ([Convert]::ToBase64String([IO.File]::ReadAllBytes($path)) -eq [Convert]::ToBase64String($backups[$path]))}}
 @{passed=$passed;error=$failure;variant=$Variant;apiOwnerVerified=$apiOwnerVerified;settingsRestored=$restored;dx12=$validation;executableSHA256=(Get-FileHash (Join-Path $runtime 'keeperfx-dx12.exe')).Hash;packageSHA256=(Get-FileHash $pack).Hash}|ConvertTo-Json -Depth 5|Set-Content -LiteralPath (Join-Path $output 'run-report.json') -Encoding utf8
 Write-Output "Terrain pilot evidence: $output"
}
