# SPDX-License-Identifier: GPL-2.0-or-later
param([Parameter(Mandatory=$true)][string]$OutputPath)
$ErrorActionPreference='Stop'
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
