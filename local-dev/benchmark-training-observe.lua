-- SPDX-License-Identifier: GPL-2.0-or-later
-- KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
-- Read-only: execute once after warm-up and once after timing; do not pass this as SetupScript.
-- Output: turn, surviving fixture identities, actively training/moving-to-training, level sum, XP sum, gold.
-- Before/after proves retained active actors and progress; it does not prove every actor was visible every frame.
assert(Game.BenchmarkTraining and #Game.BenchmarkTraining==64); local alive,working,levels,xp=0,0,0,0; for _,t in ipairs(GetThingsOfClass("CREATURE")) do for _,r in ipairs(Game.BenchmarkTraining) do if t.ThingIndex==r.id and t.creation_turn==r.born then alive=alive+1; levels=levels+t.level; xp=xp+t.exp_points; if t.state_besides_interruptions=="Training" then working=working+1 end end end end; print("BENCH_TRAINING_OBSERVED",PLAYER0.GAME_TURN,alive,working,levels,xp,PLAYER0.MONEY); assert(alive==64,"Training workload lost actors"); assert(working>=48,"Training workload is no longer densely active")
