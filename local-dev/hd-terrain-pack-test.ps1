# SPDX-License-Identifier: GPL-2.0-or-later
param()
$ErrorActionPreference='Stop'
$repository=Split-Path -Parent $PSScriptRoot
$workspace=Split-Path -Parent $repository
$output=Join-Path $workspace ('artifacts/texture-prototype/native-'+(Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
New-Item -ItemType Directory -Path $output | Out-Null
$linuxRepo=(& wsl.exe -d keeperfx-dev --exec wslpath -a $repository).Trim()
if($LASTEXITCODE -ne 0){throw 'The dedicated keeperfx-dev distribution is unavailable.'}
$linuxOut=(& wsl.exe -d keeperfx-dev --exec wslpath -a $output).Trim()
if($LASTEXITCODE -ne 0){throw 'Native test output path conversion failed.'}
$cases=[ordered]@{
 'hd-terrain-pack-test'=@('local-dev/hd-terrain-pack-test.cpp')
 'owned-render-frame-test'=@('local-dev/owned-render-frame-test.cpp','src/kfx/renderer/OwnedRenderFrame.cpp','src/kfx/renderer/OwnedRenderAssets.cpp','src/kfx/renderer/PaletteIndexTable.cpp')
}
$report=[ordered]@{passed=$false;distribution='keeperfx-dev';sanitizers=@('address','undefined');cases=@()}
try{
 foreach($name in $cases.Keys){
  $sources=@($cases[$name] | ForEach-Object {"$linuxRepo/$_"})
  $executable="$linuxOut/$name"
  $arguments=@('-std=c++20','-g','-O1','-fsanitize=address,undefined','-fno-omit-frame-pointer','-pthread','-I',"$linuxRepo/src")+$sources+@('-o',$executable)
  & wsl.exe -d keeperfx-dev --exec g++ @arguments 2>&1 | Tee-Object -FilePath (Join-Path $output ($name+'-build.log'))
  if($LASTEXITCODE -ne 0){throw "$name compilation failed."}
  & wsl.exe -d keeperfx-dev --exec $executable 2>&1 | Tee-Object -FilePath (Join-Path $output ($name+'.log'))
  if($LASTEXITCODE -ne 0){throw "$name failed."}
  $report.cases+=@{name=$name;passed=$true;executableSHA256=(Get-FileHash (Join-Path $output $name)).Hash}
 }
 $report.passed=$true
}catch{$report.error=$_.Exception.Message;throw}
finally{$report|ConvertTo-Json -Depth 5|Set-Content -LiteralPath (Join-Path $output 'report.json') -Encoding utf8;Write-Output "Native terrain evidence: $output"}
