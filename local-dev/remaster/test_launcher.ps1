# SPDX-License-Identifier: GPL-2.0-or-later
param([Parameter(Mandatory=$true)][string]$OutputPath)
$ErrorActionPreference='Stop'
. (Join-Path (Split-Path -Parent $PSScriptRoot) 'audio/session-audio.ps1')
$source=Join-Path $PSScriptRoot 'play-remaster.ps1'
$tokens=$null;$errors=$null;$ast=[Management.Automation.Language.Parser]::ParseFile($source,[ref]$tokens,[ref]$errors)
if($errors){throw ($errors|Out-String)}
foreach($function in $ast.FindAll({param($n) $n -is [Management.Automation.Language.FunctionDefinitionAst]},$false)){Invoke-Expression $function.Extent.Text}
# These are pure transaction fixtures, not a game/runtime test.
function Assert-Closed {}
$runtime=[IO.Path]::GetFullPath((Join-Path $OutputPath ('fixture-'+[Guid]::NewGuid().ToString('N'))))
New-Item -ItemType Directory -Path (Join-Path $runtime 'mods'),(Join-Path $runtime 'data'),(Join-Path $runtime 'remaster/sessions/test') -Force|Out-Null
$checks=0
function Check([bool]$Condition,[string]$Message){$script:checks++;if(-not $Condition){throw $Message}}
$legacySelection='dec01d5ddb2414ffccb98d38c1712996694b00aa639a049c8a776add7533bdbf'
$currentSelection='9a285bacf69a60ea22bd0974c915ed37f1d5ab1b93bf20b7a0bc93390b0065de'
foreach($selection in @($legacySelection,$currentSelection)){
 $profile=@{schema=1;id='keeperfx-remaster-v13';selection=$selection;mod='keeper_remaster_default';ui_directory=('remaster/packages/'+$selection+'/ui');text_percent=80;api_enabled=$false}
 $banks=@(Get-ExpectedUiBanks $profile)
 $expectedCount=if($selection -eq $currentSelection){14}else{12}
 Check ($banks.Count -eq $expectedCount) 'Installed V13/V23 profile selected the wrong bank count.'
 Check (($banks -contains 'lndflag_ens.kfui') -eq ($selection -eq $currentSelection)) 'Campaign marker bank selected for the wrong publication.'
 Check (($banks -contains 'lndflag_pin.kfui') -eq ($selection -eq $currentSelection)) 'Pin marker bank selected for the wrong publication.'
 foreach($bad in @(@{selection=('0'*64)},@{ui_directory='remaster/packages/other/ui'},@{api_enabled=$true})){
  $changed=$profile.Clone();foreach($key in $bad.Keys){$changed[$key]=$bad[$key]}
  $rejected=$false;try{$null=Get-ExpectedUiBanks $changed}catch{$rejected=$true}
  Check $rejected 'Unsupported publication, UI path or API setting accepted.'
 }
}
foreach($input in @('[after_map]',"[after_map]`n", "[after_base]`r`nkeeper_enhanced_default`r`n[after_map]`r`nkeeper_hd_terrain`r`nenhanced_creatures_custom`r`nkeeper_remaster_default_extra")){
 $hd=Get-SessionModOrder $input $false 'keeper_remaster_default'
 Check ($hd -match '(?m)^keeper_remaster_default\r?$') 'Default must be inserted, including EOF header.'
 Check ($hd -match '(?m)^dk_modern_audio\r?$') 'Audio default missing.'
 $original=Get-SessionModOrder $hd $true 'keeper_remaster_default'
 Check ($original -notmatch '(?m)^(keeper_remaster_default|keeper_enhanced_default|keeper_hd_terrain)\r?$') 'Known legacy default survived Original.'
 if($input -match 'enhanced_creatures_custom'){
  Check ($original.Contains('enhanced_creatures_custom')) 'Unrelated prefix mod altered.'
  Check ($original.Contains('keeper_remaster_default_extra')) 'Unrelated default prefix altered.'
 }
}
$audioOrder="[after_base]`r`n; dk_modern_audio is optional`r`nDK_MODERN_AUDIO ; enabled`r`nother_audio`r`ndk_modern_audio_extra`r`n[after_map]`r`ndk_modern_audio`r`n"
foreach($useOriginal in @($false,$true)){
 $native=Get-SessionModOrder $audioOrder $useOriginal 'keeper_remaster_default' $true
 Check ($native -notmatch '(?im)^\s*dk_modern_audio[\t ]*(?:[;#].*)?$') 'Native session retained Modern audio.'
 Check ($native.Contains('; dk_modern_audio is optional') -and $native.Contains('other_audio') -and $native.Contains('dk_modern_audio_extra')) 'Native session altered unrelated mods/comments.'
 Check ($useOriginal -eq ($native -notmatch '(?m)^keeper_remaster_default\r?$')) 'Audio selection changed graphics selection.'
}
$nativeCr=Get-SessionModOrder "[after_base]`rdk_modern_audio`rother_audio`r" $true 'keeper_remaster_default' $true
Check (-not ($nativeCr -split '[\r\n]+' | Where-Object { $_ -eq 'dk_modern_audio' })) 'Native session retained CR-only Modern entry.'
Check ($nativeCr.Contains("other_audio`r")) 'Native session changed unrelated CR-only entry.'
$path=Join-Path $runtime 'data/native.dat';$replacement=Join-Path $runtime 'data/replacement.dat'
[IO.File]::WriteAllBytes($path,[byte[]](1,2,3,4));[IO.File]::WriteAllBytes($replacement,[byte[]](5,6,7,8))
$session=Join-Path $runtime 'remaster/sessions/test';Copy-Item -LiteralPath $path -Destination (Join-Path $session '0')
$before=Hash $path;$after=Hash $replacement
$journal=@{schema=1;state='prepared';directory=$session;entries=@(@{path='data/native.dat';backup='0';before=$before;after=$after})}
Write-Journal $journal
Replace-OwnedFile $replacement $path $after
Check ((Hash $path) -eq $after) 'Activation failed.'
Restore-Journal $journal
Check ((Hash $path) -eq $before) 'Restoration failed.'
$badRejected=$false
try{Replace-OwnedFile $replacement $path ('0'*64)}catch{$badRejected=$true}
Check $badRejected 'Bad replacement hash accepted.'
Check ((Hash $path) -eq $before) 'Bad replacement changed native file.'
Check (@(Get-ChildItem -LiteralPath (Join-Path $runtime 'data') -Filter '*.tmp').Count -eq 0) 'Owned temporary leaked.'
$parsed=Get-Content -LiteralPath (Join-Path $session 'journal.json') -Raw|ConvertFrom-Json
Check ($parsed.state -eq 'restored' -and $parsed.entries.Count -eq 1) 'Atomic journal lost recovery map.'
foreach($relative in @('../escape','data/../../escape','C:/escape','data\escape')){
 $rejected=$false;try{$null=Path-InRuntime $relative}catch{$rejected=$true};Check $rejected ('Unsafe path accepted: '+$relative)
}
@{passed=$true;checks=$checks;fixture=$runtime;scope='Production mod-order and atomic session/recovery functions; no game launched.'}|ConvertTo-Json|Set-Content -LiteralPath (Join-Path $OutputPath 'launcher-contracts.json') -Encoding UTF8
Write-Output "PASS $checks launcher transaction checks."
