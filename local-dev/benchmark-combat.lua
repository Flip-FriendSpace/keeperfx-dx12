-- SPDX-License-Identifier: GPL-2.0-or-later
-- KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
-- KFX_BENCHMARK_COMBAT_V1
-- Disposable keeporig level-one arena; every mutation is performed while paused before measurement.
assert(Game.BenchmarkCombat==nil); local p=PLAYER0.heart.pos; assert(p.val_x<60*768 or p.val_x>=71*768 or p.val_y<60*768 or p.val_y>=71*768,"Arena overlaps heart"); Game.BenchmarkCombat={}
for _,kind in ipairs({"CREATURE","TRAP","OBJECT","EFFECTELEM","EFFECT","EFFECTGEN","DEADCREATURE","SHOT","DOOR"}) do for _,t in ipairs(GetThingsOfClass(kind)) do local p=t.pos; if p.val_x>=60*768 and p.val_x<71*768 and p.val_y>=60*768 and p.val_y<71*768 then t:delete() end end end
for x=60,70 do for y=60,70 do ChangeSlabType(x,y,11); ChangeSlabOwner(x,y,PLAYER0) end end; RevealMapRect(PLAYER0,195,195,42,42); AllyPlayers(PLAYER0,PLAYER_GOOD,0)
for i=0,23 do local x=182+(i%12)*2; local row=math.floor(i/12); local a=AddCreatureToLevel(PLAYER0,i%2==0 and "TROLL" or "SORCEROR",{stl_x=x,stl_y=184+row*3},5,0); local b=AddCreatureToLevel(PLAYER_GOOD,i%2==0 and "KNIGHT" or "WIZARD",{stl_x=x,stl_y=197+row*3},5,0); assert(a and b); a.force_health_flower_displayed=true; b.force_health_flower_displayed=true; table.insert(Game.BenchmarkCombat,{id=a.ThingIndex,born=a.creation_turn}); table.insert(Game.BenchmarkCombat,{id=b.ThingIndex,born=b.creation_turn}) end; assert(#Game.BenchmarkCombat==48)
for _,x in ipairs({182,190,198,206}) do assert(AddObjectToLevelAtPos("CANDLESTCK",x,181,0,PLAYER0,0)); assert(AddObjectToLevelAtPos("TORCH",x,207,0,PLAYER0,0)) end
