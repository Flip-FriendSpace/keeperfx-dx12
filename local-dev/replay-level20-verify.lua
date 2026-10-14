-- SPDX-License-Identifier: GPL-2.0-or-later
-- KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
-- Read-only original level 20 script assertions for record and playback.
-- map00020.txt unlocks BRIDGE after TIMER0>150 and sets FLAG2 at TIMER0>=1133.
assert(PLAYER_GOOD.TIMER0>=1134, "Level 20 timer did not reach the script window")
assert(PLAYER_GOOD.FLAG2==1, "Level 20 second-wave script flag was not set")
assert(PLAYER0:available("BRIDGE")==1, "Level 20 bridge was not unlocked")
print("G7 SCRIPT20 PASS",PLAYER0.GAME_TURN,PLAYER_GOOD.TIMER0,PLAYER_GOOD.FLAG2,PLAYER0:available("BRIDGE"))
