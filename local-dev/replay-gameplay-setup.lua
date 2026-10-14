-- SPDX-License-Identifier: GPL-2.0-or-later
-- KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
-- Disposable level-one fixture. Each non-comment line is one paused setup command.
-- All mutations precede the replay snapshot; normal creature updates follow it.
assert(GetSlab(46,43).kind=="GOLD", "Original gold target missing"); assert(Game.G7==nil, "Fixture already exists"); Game.G7={}
for x=36,38 do for y=42,44 do ChangeSlabType(x,y,"TREASURY_AREA"); ChangeSlabOwner(x,y,PLAYER0) end end; RevealMapRect(PLAYER0,124,133,42,36)
local p=PLAYER0.heart.pos; assert(not(p.val_x>=35*768 and p.val_x<44*768 and p.val_y>=46*768 and p.val_y<51*768),"Arena overlaps heart"); for x=35,43 do for y=46,50 do ChangeSlabType(x,y,"DIRT") end end
for _,a in ipairs({36,40}) do for x=a,a+2 do for y=47,49 do ChangeSlabType(x,y,11); ChangeSlabOwner(x,y,PLAYER0) end end end
local t=AddCreatureToLevel(PLAYER0,"TROLL",{stl_x=112,stl_y=145},3,0); assert(t); t.health=math.floor(t.max_health/2); Game.G7.friend={id=t.ThingIndex,born=t.creation_turn,hp=t.health}
local t=AddCreatureToLevel("PLAYER_GOOD","BUG",{stl_x=124,stl_y=145},1,0); assert(t); t.health=math.max(1,math.floor(t.max_health/4)); Game.G7.enemy={id=t.ThingIndex,born=t.creation_turn,hp=t.health}
MagicAvailable(PLAYER0,"POWER_HEAL_CREATURE",true,true); MagicAvailable(PLAYER0,"POWER_POSSESS",true,true); RoomAvailable(PLAYER0,"TREASURE",1,true); PLAYER0:add_gold(5000); Game.G7.money=PLAYER0.MONEY; Game.G7.mined=PLAYER0.TOTAL_GOLD_MINED
