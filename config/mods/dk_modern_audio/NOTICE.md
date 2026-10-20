# Modern original English audio v2.1

This directory contains remastered recordings derived from the original English audio in Dungeon Keeper / Dungeon Keeper Gold. It retains the original performances, timing and channel layout. It is not a newly recorded voice cast or a music replacement.

The 734 playback files cover 125 in-game voices, 569 original effect entries and 40 original campaign narrations. Repeated effect IDs intentionally share identical recordings. Original empty placeholders and KeeperFX-only additions are excluded. Effect IDs 134 and 135 use the restored heart variants from the matching KeeperFX runtime rather than the duplicate original-bank entry. Source-bank fingerprints and per-file SHA256 hashes are recorded in local-audio-mod.json.

Treatment: gentle spectral noise reduction and 48 kHz conversion, with source-constrained upper-band detail. Reconstructed detail is capped at 15% of the cleaned original's local spectral magnitude and fades out at the source bandwidth. The lower-band source signal is retained, and no DC or sub-audio filtering alters the start or end of a recording. Loudness follows the current game recordings with -1 dB true-peak headroom. Four distinct noisy recordings retain cleanup only after reconstruction failed alignment checks. The manifest records each file's processing and measurements.

Copyright in the original recordings remains with the respective rights holders. KeeperFX's GPL source-code license does not relicense these recordings. This notice records provenance and does not grant rights to the underlying Dungeon Keeper material. A legally obtained copy of the game is still needed for the other original game data.

The original sound banks, music, other commercial game data, trained model and development caches are not included in this directory.
