# SPDX-License-Identifier: GPL-2.0-or-later
# Pure transformation; the combined launcher owns backup, activation and restore.
function Remove-ModernAudioFromOrder([string]$Text) {
    # The engine accepts CR, LF and CRLF line separators. Keep unrelated bytes.
    return [regex]::Replace($Text,
        '(?i)(?<![^\r\n])[\t ]*dk_modern_audio[\t ]*(?:[;#][^\r\n]*)?(?:\r\n|\r|\n|\z)', '')
}
