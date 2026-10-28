// SPDX-License-Identifier: GPL-2.0-or-later
#include "fidelity_state.h"
#include "game_legacy.h"
#include "thing_list.h"
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <string>

static std::string scratch_path;
static FILE *scratch() { return fopen(scratch_path.c_str(), "w+b"); }

static std::string snapshot(const Game *g)
{
    FILE *file = scratch(); assert(file);
    assert(fidelity_write_snapshot(file, g));
    long size = ftell(file); assert(size > 0); rewind(file);
    std::string output(size, '\0');
    assert(fread(output.data(), 1, output.size(), file) == output.size());
    fclose(file); return output;
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    scratch_path = std::string(argv[1]) + ".scratch";
    auto *g = static_cast<Game *>(calloc(1, sizeof(Game)));
    auto *before = static_cast<Game *>(malloc(sizeof(Game))); assert(g && before);
    g->play_gameturn = 123; g->loaded_level_number = 1;
    auto &thing = g->things_data[1]; thing.alloc_flags = TAlF_Exists;
    thing.class_id = TCls_Creature; thing.ccontrol_idx = 1; thing.health = 100;
    g->script.conditions_num = 1; g->script.values_num = 1;
    std::memcpy(before, g, sizeof(Game));
    const auto baseline = snapshot(g);
    assert(std::memcmp(before, g, sizeof(Game)) == 0);
    int cases = 0;
#define CHECK_CHANGE(field) do { ++(field); assert(snapshot(g) != baseline); --(field); ++cases; } while (0)
    CHECK_CHANGE(thing.health);
    CHECK_CHANGE(g->cctrl_data[1].hunger_level);
    CHECK_CHANGE(g->cctrl_data[1].annoyance_level[1]);
    CHECK_CHANGE(g->cctrl_data[1].exp_points);
    CHECK_CHANGE(g->cctrl_data[1].instance_use_turn[2]);
    CHECK_CHANGE(g->dungeon[0].total_money_owned);
    CHECK_CHANGE(g->dungeon[0].research_progress);
    CHECK_CHANGE(g->dungeon[0].manufacture_progress);
    CHECK_CHANGE(g->dungeon[0].script_flags[1]);
    CHECK_CHANGE(g->dungeon[0].turn_timers[0].count);
    CHECK_CHANGE(g->script.conditions[0].status);
    CHECK_CHANGE(g->script.values[0].bytes[0]);
    CHECK_CHANGE(g->action_random_seed);
    CHECK_CHANGE(g->ai_random_seed);
    CHECK_CHANGE(g->player_random_seed);
    CHECK_CHANGE(g->players[0].victory_state);
    CHECK_CHANGE(g->slabmap[1].health);
    ++thing.last_turn_drawn; ++thing.creature.health_bar_turns;
    ++g->cctrl_data[1].thought_bubble_last_turn_drawn;
    ++g->unsync_random_seed; ++g->sound_random_seed;
    assert(snapshot(g) == baseline);
    assert(fidelity_write_snapshot_path(argv[1], g));
    assert(!fidelity_write_snapshot_path(argv[1], g));
    g->script.conditions_num = CONDITIONS_COUNT + 1;
    FILE *bad = scratch(); assert(bad && !fidelity_write_snapshot(bad, g)); fclose(bad);
    assert(remove(scratch_path.c_str()) == 0);
    printf("{\"passed\":true,\"mutationCases\":%d,\"purity\":true,\"presentationExclusions\":true,\"exclusiveOutput\":true}\n", cases);
    free(before); free(g);
}
