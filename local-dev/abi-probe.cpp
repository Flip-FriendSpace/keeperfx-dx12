// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#include <cstddef>
#include "game_legacy.h"
#include "game_merge.h"
#include "game_saves.h"
#include "engine_buckets.h"
#include "kfx/renderer/WorldVertex.h"

// Emit compile-time constants that can be inspected without executing
// either target architecture, using the compiler's assembly output.
extern "C" {
#define ABI_SIZE(T) unsigned int kfx_size_##T = sizeof(T)
ABI_SIZE(Game); ABI_SIZE(Thing); ABI_SIZE(CreatureControl); ABI_SIZE(Dungeon); ABI_SIZE(Computer2);
ABI_SIZE(PlayerInfo); ABI_SIZE(Packet); ABI_SIZE(Map); ABI_SIZE(SlabMap); ABI_SIZE(Room); ABI_SIZE(LightsShadows);
ABI_SIZE(IntralevelData); ABI_SIZE(CatalogueEntry); ABI_SIZE(FileChunkHeader); ABI_SIZE(Camera);
ABI_SIZE(EngineCoord); ABI_SIZE(BucketKindPolygonStandard); ABI_SIZE(WorldVertex);
ABI_SIZE(Configs); ABI_SIZE(SoundSettings); ABI_SIZE(LightSystemState); ABI_SIZE(LevelScript);
unsigned int kfx_pointer_bytes = sizeof(void*);
unsigned int kfx_long_bytes = sizeof(long);
unsigned int kfx_long_double_bytes = sizeof(long double);
#define ABI_OFFSET(F) unsigned int kfx_offset_##F = offsetof(Game, F)
ABI_OFFSET(lish); ABI_OFFSET(things_data); ABI_OFFSET(cctrl_data); ABI_OFFSET(map); ABI_OFFSET(computer);
ABI_OFFSET(dungeon); ABI_OFFSET(play_gameturn); ABI_OFFSET(action_random_seed); ABI_OFFSET(packets);
ABI_OFFSET(conf); ABI_OFFSET(process_turn_time);
}
