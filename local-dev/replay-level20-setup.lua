-- SPDX-License-Identifier: GPL-2.0-or-later
-- KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
-- Disposable original level 20 timer fixture. Each line is one paused setup command.
-- Mutations occur before the mid-game recording snapshot.
assert(PLAYER_GOOD.TIMER0==0 and PLAYER_GOOD.FLAG2==0 and PLAYER0:available("BRIDGE")==0)
Game.G7Script={timerBase=PLAYER0.GAME_TURN}; SetTimer(PLAYER_GOOD,"TIMER0")
