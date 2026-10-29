# SPDX-License-Identifier: GPL-2.0-or-later
param(
  [string]$WorkspacePath,[string]$RuntimePath,
  [switch]$Original,[switch]$NativeAudio,[switch]$QuickStart,[switch]$OpenGLFallback,[switch]$Hidden,
  [switch]$GpuValidation,[switch]$VerifyOnly,[switch]$Recover
)
$ErrorActionPreference='Stop'
. (Join-Path (Split-Path -Parent $PSScriptRoot) 'audio/session-audio.ps1')
if(-not $RuntimePath){if(-not $WorkspacePath){throw 'Select -WorkspacePath or -RuntimePath.'};$RuntimePath=Join-Path $WorkspacePath 'runtime/keeperfx-x64'}
$runtime=[IO.Path]::GetFullPath($RuntimePath).TrimEnd('\','/')
$control=Join-Path $runtime 'remaster'
function Hash([string]$Path) { (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() }
function Path-InRuntime([string]$Relative) {
  if($Relative -match '(^/|\\|:|(^|/)\.\.?(/|$))'){throw 'Invalid remaster relative path.'}
  $path=[IO.Path]::GetFullPath((Join-Path $runtime $Relative))
  if(-not $path.StartsWith($runtime+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)){throw 'Path leaves runtime.'}
  # Runtime data/mod top-level junctions are explicitly selected by the user.
  $parts=$Relative.Split('/');$current=Join-Path $runtime $parts[0]
  if($parts[0] -eq 'remaster' -and (Test-Path -LiteralPath $current) -and ((Get-Item -LiteralPath $current -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)){throw 'Linked remaster control directory.'}
  for($i=1;$i -lt $parts.Count;$i++){$current=Join-Path $current $parts[$i];if((Test-Path -LiteralPath $current) -and ((Get-Item -LiteralPath $current -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)){throw "Linked remaster descendant: $Relative"}}
  return $path
}
function Get-SessionModOrder([string]$Order,[bool]$UseOriginal,[string]$Mod,[bool]$UseNativeAudio=$false) {
  if(-not $Order.EndsWith("`n")){$Order+="`r`n"}
  $known='(?:keeper_remaster_default|keeper_enhanced_default|keeper_hd_terrain|enhanced_creatures|imp_art_prototype|heroes_art_prototype|hero_[a-z_]+_art_prototype|remaster_core_[a-f0-9]{12}(?:_original)?|remaster_heart_001_(?:original|hd))'
  $order=[regex]::Replace($order,'(?im)^[\t ]*'+$known+'[\t ]*(?=\r?$)\r?\n?','')
  if(-not $UseOriginal){
    if($order -notmatch '(?im)^\[after_map\]\s*$'){$order+="`r`n[after_map]`r`n"}
    $order=[regex]::Replace($order,'(?ims)(^\[after_map\][\t ]*\r?\n)(.*?)(?=^\[|\z)',('$1$2'+"`r`n"+$Mod+"`r`n"))
  }
  if($UseNativeAudio){
    $order=Remove-ModernAudioFromOrder $order
  }elseif($order -notmatch '(?im)^\s*dk_modern_audio\s*$'){
    if($order -notmatch '(?im)^\[after_base\]\s*$'){$order="[after_base]`r`n"+$order}
    $order=[regex]::Replace($order,'(?im)^(\[after_base\][\t ]*\r?$)',('$1'+"`r`ndk_modern_audio"))
  }
  return $order
}
function Assert-Closed {
  if(Get-Process -Name keeperfx,keeperfx-local,keeperfx-local-hvlog,keeperfx-dx12,keeperfx-dx12-hvlog -ErrorAction SilentlyContinue){throw 'Close KeeperFX before switching profiles; no game was stopped.'}
}
function Get-ExpectedUiBanks($Profile) {
  $legacy='dec01d5ddb2414ffccb98d38c1712996694b00aa639a049c8a776add7533bdbf'
  $current='9a285bacf69a60ea22bd0974c915ed37f1d5ab1b93bf20b7a0bc93390b0065de'
  if($Profile.selection -notin @($legacy,$current) -or $Profile.ui_directory -ne ('remaster/packages/'+$Profile.selection+'/ui') -or $Profile.schema -ne 1 -or $Profile.id -ne 'keeperfx-remaster-v13' -or $Profile.mod -ne 'keeper_remaster_default' -or $Profile.text_percent -ne 80 -or $Profile.api_enabled){throw 'Unsupported remaster profile.'}
  $banks=@('font1-64','font2-64','gui1-64','gui2-64','frontbit','frontft1','frontft2','frontft3','frontft4','netfont','pointer64','maphand')
  if($Profile.selection -eq $current){$banks+=@('lndflag_ens','lndflag_pin')}
  return @($banks|ForEach-Object {$_+'.kfui'}|Sort-Object)
}
function Replace-OwnedFile([string]$Source,[string]$Target,[string]$Expected) {
  $temporary=$Target+'.remaster-'+[Guid]::NewGuid().ToString('N')+'.tmp';$created=$false
  try{
    $output=[IO.File]::Open($temporary,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None)
    $created=$true
    try{$input=[IO.File]::OpenRead($Source);try{$input.CopyTo($output);$output.Flush($true)}finally{$input.Dispose()}}finally{$output.Dispose()}
    if((Hash $temporary) -ne $Expected){throw 'Staged session file hash mismatch.'}
    if(Test-Path -LiteralPath $Target){[IO.File]::Replace($temporary,$Target,[NullString]::Value)}
    else{[IO.File]::Move($temporary,$Target)}
  }finally{if($created -and (Test-Path -LiteralPath $temporary)){Remove-Item -LiteralPath $temporary}}
}
function Write-Journal($Journal) {
  $path=Join-Path $Journal.directory 'journal.json'
  $temporary=$path+'.'+[Guid]::NewGuid().ToString('N')+'.tmp';$created=$false
  try{
    $bytes=[Text.Encoding]::UTF8.GetBytes(($Journal|ConvertTo-Json -Depth 8))
    $stream=[IO.File]::Open($temporary,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None)
    $created=$true
    try{$stream.Write($bytes,0,$bytes.Length);$stream.Flush($true)}finally{$stream.Dispose()}
    if(Test-Path -LiteralPath $path){[IO.File]::Replace($temporary,$path,[NullString]::Value)}else{[IO.File]::Move($temporary,$path)}
  }finally{if($created -and (Test-Path -LiteralPath $temporary)){Remove-Item -LiteralPath $temporary}}
}
function Restore-Journal($Journal) {
  Assert-Closed
  foreach($entry in @($Journal.entries)){
    $path=Path-InRuntime $entry.path
    $actual=if(Test-Path -LiteralPath $path){Hash $path}else{$null}
    if($actual -eq $entry.before){continue}
    if($actual -ne $entry.after){throw "A session file changed; recover from the retained journal: $($entry.path)"}
    if($entry.before){
      $backup=Join-Path $Journal.directory $entry.backup
      if((Hash $backup) -ne $entry.before){throw 'Recovery backup hash mismatch.'}
      Replace-OwnedFile $backup $path $entry.before
      if((Hash $path) -ne $entry.before){throw 'Recovery hash mismatch.'}
    }else{Remove-Item -LiteralPath $path}
  }
  $Journal.state='restored'
  Write-Journal $Journal
}
$control=Path-InRuntime 'remaster'
$lockPath=Path-InRuntime 'remaster/operation.lock'
Assert-Closed
if($Recover){
  if(-not(Test-Path -LiteralPath $lockPath)){return}
  $recoveryLock=[IO.File]::Open($lockPath,[IO.FileMode]::Open,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None)
  $recovered=$false
  try{
    $reader=[IO.StreamReader]::new($recoveryLock,[Text.Encoding]::UTF8,$true,1024,$true)
    try{$journalPath=$reader.ReadToEnd().Trim()}finally{$reader.Dispose()}
    $sessions=[IO.Path]::GetFullPath((Join-Path $control 'sessions'))+[IO.Path]::DirectorySeparatorChar
    if(-not [IO.Path]::GetFullPath($journalPath).StartsWith($sessions,[StringComparison]::OrdinalIgnoreCase)){throw 'This is not a play-session recovery journal; inspect the retained installation backup.'}
    $relativeJournal=[IO.Path]::GetFullPath($journalPath).Substring($runtime.Length+1).Replace('\','/')
    $journalPath=Path-InRuntime $relativeJournal
    $journal=Get-Content -LiteralPath $journalPath -Raw|ConvertFrom-Json
    if([IO.Path]::GetFullPath($journal.directory) -ine [IO.Path]::GetDirectoryName([IO.Path]::GetFullPath($journalPath))){throw 'Journal directory mismatch.'}
    foreach($entry in $journal.entries){if($entry.backup -notmatch '^\d+$'){throw 'Invalid recovery backup name.'}}
    Restore-Journal $journal
    $recovered=$true
  }finally{$recoveryLock.Dispose();if($recovered){Remove-Item -LiteralPath $lockPath}}
  return
}
$profile=Get-Content -LiteralPath (Path-InRuntime 'remaster/default.json') -Raw|ConvertFrom-Json
$selectedUi=@(Get-ExpectedUiBanks $profile)
foreach($entry in $profile.files){$path=Path-InRuntime $entry.path;if(-not(Test-Path -LiteralPath $path) -or (Hash $path) -ne $entry.sha256){throw "Installed remaster file mismatch: $($entry.path)"}}
$uiPath=Path-InRuntime $profile.ui_directory
$expectedUi=@($profile.files|Where-Object {$_.path.StartsWith($profile.ui_directory+'/')}|ForEach-Object {$_.path.Substring($profile.ui_directory.Length+1)}|Sort-Object)
$actualUi=@(Get-ChildItem -LiteralPath $uiPath -File -Filter '*.kfui'|ForEach-Object {$_.Name}|Sort-Object)
if(@(Compare-Object $selectedUi $expectedUi).Count -or @(Compare-Object $expectedUi $actualUi).Count){throw 'The installed UI sidecar set differs from the selected profile.'}
$modPrefix='mods/'+$profile.mod+'/'
$expectedMod=@($profile.files|Where-Object {$_.path.StartsWith($modPrefix)}|ForEach-Object {$_.path.Substring($modPrefix.Length)}|Sort-Object)
$modPath=Path-InRuntime ('mods/'+$profile.mod)
$actualMod=@(Get-ChildItem -LiteralPath $modPath -File -Recurse|ForEach-Object {$_.FullName.Substring($modPath.Length+1).Replace('\','/')}|Sort-Object)
if(@(Compare-Object $expectedMod $actualMod).Count){throw 'The installed remaster mod has unexpected or missing files.'}
foreach($entry in $profile.overlays){if((Hash (Path-InRuntime $entry.file)) -ne $entry.original_sha256){throw "Original frontend source changed: $($entry.file)"}}
if($VerifyOnly){Write-Output "Verified installed remaster $($profile.selection).";return}
$session=Path-InRuntime ('remaster/sessions/'+[DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss-fff')+'-'+[Guid]::NewGuid().ToString('N').Substring(0,8))
$journal=@{schema=1;state='prepared';directory=$session;entries=@();selection=$profile.selection;original=[bool]$Original;native_audio=[bool]$NativeAudio}
$game=$null;$restored=$false
$lock=[IO.File]::Open($lockPath,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::Read)
try{
  New-Item -ItemType Directory -Path $session|Out-Null
  $journalPath=Join-Path $session 'journal.json'
  $bytes=[Text.Encoding]::UTF8.GetBytes($journalPath);$lock.Write($bytes,0,$bytes.Length);$lock.Flush()
  $orderPath=Path-InRuntime 'mods/load_order.cfg';$order=[IO.File]::ReadAllText($orderPath)
  $order=Get-SessionModOrder $order ([bool]$Original) $profile.mod ([bool]$NativeAudio)
  $orderStage=Join-Path $session 'load_order.cfg';[IO.File]::WriteAllText($orderStage,$order,[Text.UTF8Encoding]::new($false))
  $changes=@(@{file='mods/load_order.cfg';source=$orderStage;sha256=(Hash $orderStage)})
  if(-not $Original){foreach($e in $profile.overlays){$changes+=@{file=$e.file;source=(Path-InRuntime $e.source);sha256=$e.sha256}}}
  foreach($change in $changes){
    $path=Path-InRuntime $change.file;$backup=[string]$journal.entries.Count
    $before=if(Test-Path -LiteralPath $path){Hash $path}else{$null}
    if($before){[IO.File]::Copy($path,(Join-Path $session $backup),$false)}
    $journal.entries+=@{path=$change.file;backup=$backup;before=$before;after=$change.sha256}
  }
  Write-Journal $journal
  for($i=0;$i -lt $changes.Count;$i++){
    $change=$changes[$i];$entry=$journal.entries[$i];$path=Path-InRuntime $entry.path
    if((Hash $path) -ne $entry.before -or (Hash $change.source) -ne $entry.after){throw 'Profile input changed during activation.'}
    Replace-OwnedFile $change.source $path $entry.after
    if((Hash $path) -ne $entry.after){throw 'Profile activation hash mismatch.'}
  }
  $options=@{WorkspacePath=$WorkspacePath;RuntimePath=$runtime;NativeProfile=$true;DX12Preview=$true;QuickStart=$QuickStart;OpenGLFallback=$OpenGLFallback;Hidden=$Hidden;PassThru=$true;GpuValidation=$GpuValidation;NativeTerrain=$Original;UiTextPercent=$(if($Original){100}else{80})}
  if(-not $Original){$options.HdUiOverrideDir=Path-InRuntime $profile.ui_directory;if(-not $OpenGLFallback){$options.RemasterLighting=$true;$options.RemasterEmissive=$true}}
  $game=& (Join-Path (Split-Path -Parent $PSScriptRoot) 'play.ps1') @options
  if(-not $game -or -not $game.Id){throw 'Launcher did not return the owned game process.'}
  $null=$game.Handle
  $journal.state='running';$journal.pid=$game.Id;$journal.executable_sha256=Hash (Join-Path $runtime 'keeperfx-dx12.exe')
  Write-Journal $journal
  while(-not $game.WaitForExit(250)){}
  if($game.ExitCode -ne 0){throw "KeeperFX exited with code $($game.ExitCode)."}
}finally{
  try{if($null -eq $game -or $game.HasExited){Restore-Journal $journal;$restored=$true}}finally{
    if($game){$game.Dispose()};$lock.Dispose()
    if($restored){Remove-Item -LiteralPath $lockPath}else{Write-Warning 'The session is still active or needs recovery. After closing the game, run play-remaster.ps1 -WorkspacePath <workspace> -Recover.'}
  }
}
