# SPDX-License-Identifier: GPL-2.0-or-later
param(
    [Parameter(Mandatory = $true)][string]$OutputDirectory,
    [ValidateSet('x64', 'x86', 'both')][string]$Architecture = 'x64',
    [ValidateRange(3, 30)][int]$TimeoutSeconds = 5,
    [switch]$ExpectFailure
)
$ErrorActionPreference = 'Stop'
$output = [IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $output) { throw 'Choose a new output directory.' }
New-Item -ItemType Directory -Path $output | Out-Null
$repo = Split-Path -Parent $PSScriptRoot
$sourceHashes = [ordered]@{}
foreach ($relative in @('src/kfx/renderer/RenderThreadManager.cpp', 'src/kfx/renderer/RenderThreadManager.h',
    'src/kfx/renderer/RendererThread.cpp', 'src/kfx/renderer/RendererThread.h', 'local-dev/render-thread-test.cpp')) {
    $sourceHashes[$relative] = (Get-FileHash -LiteralPath (Join-Path $repo $relative) -Algorithm SHA256).Hash.ToLowerInvariant()
}
$buildScript = (& wsl.exe -d keeperfx-dev --exec wslpath -a (Join-Path $PSScriptRoot 'render-thread-test.sh')).Trim()
if ($LASTEXITCODE -ne 0) { throw 'keeperfx-dev is unavailable.' }
$binaryDirectory = Join-Path $output 'bin'
$buildOutput = (& wsl.exe -d keeperfx-dev --exec wslpath -a $binaryDirectory).Trim()
if ($LASTEXITCODE -ne 0) { throw 'Could not translate the output path.' }
& wsl.exe -d keeperfx-dev --exec bash $buildScript $buildOutput $Architecture
if ($LASTEXITCODE -ne 0) { throw 'Render-thread test compilation failed.' }
foreach ($relative in $sourceHashes.Keys) {
    if ((Get-FileHash -LiteralPath (Join-Path $repo $relative) -Algorithm SHA256).Hash.ToLowerInvariant() -ne $sourceHashes[$relative]) {
        throw "Source changed during compilation: $relative"
    }
}
$architectures = switch ($Architecture) {
    'x64' { @('x86_64') }
    'x86' { @('i686') }
    'both' { @('i686', 'x86_64') }
}
$results = @()
foreach ($arch in $architectures) {
    foreach ($scenario in @('init', 'work', 'present', 'present-stop', 'cleanup', 'restart')) {
        $stdout = Join-Path $output "$arch-$scenario.stdout.log"
        $stderr = Join-Path $output "$arch-$scenario.stderr.log"
        $process = $null
        try {
            $process = Start-Process -FilePath (Join-Path $binaryDirectory "render-thread-test-$arch.exe") `
                -ArgumentList $scenario -WindowStyle Hidden -PassThru `
                -RedirectStandardOutput $stdout -RedirectStandardError $stderr
            # Keep a process handle before a short-lived child exits; Windows
            # PowerShell 5 otherwise can return a null ExitCode after waiting.
            $processHandle = $process.Handle
            $timedOut = -not $process.WaitForExit($TimeoutSeconds * 1000)
            if ($timedOut) {
                $process.Kill()
                if (-not $process.WaitForExit(2000)) { throw 'Owned regression process did not terminate.' }
            }
            $process.Refresh()
            $exitCode = $process.ExitCode
            $outputText = [IO.File]::ReadAllText($stdout)
            $errorText = [IO.File]::ReadAllText($stderr)
            $passed = -not $timedOut -and $exitCode -eq 0 -and $outputText.Contains('"passed":true')
            # Baseline mode accepts only the expected message-delivery assertion,
            # not an unrelated crash, compiler failure or unexplained timeout.
            $expectedFailure = -not $timedOut -and $exitCode -eq 1 `
                -and $errorText.Contains('sent message timed out or failed:') `
                -and $errorText.Contains('sent message did not complete during the manager wait')
            $results += [pscustomobject][ordered]@{
                architecture = $arch; scenario = $scenario; passed = $passed
                timedOut = $timedOut; exitCode = $exitCode; expectedFailure = $expectedFailure
                stdout = [IO.Path]::GetFileName($stdout); stderr = [IO.Path]::GetFileName($stderr)
            }
        } finally {
            if ($null -ne $process) {
                if (-not $process.HasExited) { $process.Kill(); $process.WaitForExit() }
                $process.Dispose()
            }
        }
    }
}
$success = @($results | Where-Object { if ($ExpectFailure) { -not $_.expectedFailure } else { -not $_.passed } }).Count -eq 0
$report = [ordered]@{
    schema = 1; passed = $success; baselineExpectedFailure = [bool]$ExpectFailure
    sources = $sourceHashes; results = $results
}
$report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $output 'result.json') -Encoding UTF8
$results | Format-Table architecture, scenario, passed, expectedFailure, timedOut, exitCode
if (-not $success) { throw "Render-thread regression failed. Evidence: $output" }
Write-Output "Render-thread regression evidence: $output"
