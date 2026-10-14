-- SPDX-License-Identifier: GPL-2.0-or-later
-- KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
-- Disposable keeporig level-one sustained training workload; all mutations are paused setup only.
-- Intended first qualification: 20 simulation turns/s, 10s warm-up + 120s sample. Not a combat benchmark.
-- No health, hunger, speed, experience or creature configuration overrides. Ordinary training owns subsequent state.
-- ORC/SKELETON both have PrimaryJobs=TRAIN, so normal job selection resumes training after level-up celebrations.
assert(Game.BenchmarkTraining==nil); local p=PLAYER0.heart.pos; assert(p.val_x<58*768 or p.val_x>=73*768 or p.val_y<58*768 or p.val_y>=73*768,"Training fixture overlaps heart"); Game.BenchmarkTraining={}
for _,kind in ipairs({"CREATURE","TRAP","OBJECT","EFFECTELEM","EFFECT","EFFECTGEN","DEADCREATURE","SHOT","DOOR"}) do for _,t in ipairs(GetThingsOfClass(kind)) do local p=t.pos; if p.val_x>=58*768 and p.val_x<73*768 and p.val_y>=58*768 and p.val_y<73*768 then t:delete() end end end
for x=58,72 do for y=58,72 do ChangeSlabType(x,y,"HARD") end end
for x=59,71 do for y=59,71 do ChangeSlabType(x,y,"PRETTY_PATH"); ChangeSlabOwner(x,y,PLAYER0) end end
for x=60,70 do for y=60,70 do ChangeSlabType(x,y,"TRAINING_AREA") end end; assert(GetSlab(65,65).room.type=="TRAINING"); PLAYER0:add_gold(1000000)
for x=60,70 do ChangeSlabType(x,59,"WATER"); ChangeSlabType(x,71,"LAVA") end; for _,x in ipairs({181,190,199,208}) do assert(AddObjectToLevelAtPos("CANDLESTCK",x,181,0,PLAYER0,0)); assert(AddObjectToLevelAtPos("TORCH",x,211,0,PLAYER0,0)) end
for i=0,63 do local kinds={"ORC","SKELETON"}; local t=AddCreatureToLevel(PLAYER0,kinds[(i%2)+1],{stl_x=183+(i%8)*3,stl_y=183+math.floor(i/8)*3},1,0); assert(t); t.force_health_flower_displayed=true; t.state="AtTrainingRoom"; table.insert(Game.BenchmarkTraining,{id=t.ThingIndex,born=t.creation_turn}) end; assert(#Game.BenchmarkTraining==64)
RevealMapRect(PLAYER0,195,195,48,48); ZoomToLocation(PLAYER0,{stl_x=195,stl_y=195}); print("BENCH_TRAINING_PREPARED",PLAYER0.GAME_TURN,#Game.BenchmarkTraining)
