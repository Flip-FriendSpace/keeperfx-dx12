# SPDX-License-Identifier: GPL-2.0-or-later
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'session-audio.ps1')
$cases = @(
    @("[after_base]`r`ndk_modern_audio`r`nPlayer_texture_pack`r`n", "[after_base]`r`nPlayer_texture_pack`r`n"),
    @("; dk_modern_audio`n  DK_MODERN_AUDIO ; comment`nother_audio`n", "; dk_modern_audio`nother_audio`n"),
    @("dk_modern_audio_extra`ndk_modern_audio", "dk_modern_audio_extra`n"),
    @("[after_map]`ndk_modern_audio # test`n", "[after_map]`n"),
    @("[after_base]`n; keep native`n", "[after_base]`n; keep native`n"),
    @("[after_base]`rdk_modern_audio`rother_audio`r", "[after_base]`rother_audio`r"),
    @("[after_base]`r`nDK_MODERN_AUDIO # mixed`rkeep_me`n dk_modern_audio`r`n", "[after_base]`r`nkeep_me`n")
)
foreach ($case in $cases) {
    if ((Remove-ModernAudioFromOrder $case[0]) -cne $case[1]) { throw 'Native-audio mod-order regression.' }
}
Write-Output 'PASS: native-audio selection preserves other mods, comments and line endings.'
