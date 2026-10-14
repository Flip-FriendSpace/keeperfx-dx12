// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#include "TerrainColumnPlan.h"

#define TCP_WAYS 4u
#define TCP_SETS 4096u
struct TerrainColumnCacheEntry {
    uint64_t epoch;
    uint16_t x, y;
    uint8_t valid;
    struct TerrainColumnInputs inputs;
    struct TerrainColumnPlan plan;
};
static struct TerrainColumnCacheEntry entries[TCP_SETS][TCP_WAYS];
static uint8_t victims[TCP_SETS];
static struct TerrainColumnPlanStats stats;
static uint64_t current_epoch;

static int equal_inputs(const struct TerrainColumnInputs* a, const struct TerrainColumnInputs* b)
{
    unsigned i, j;
    for (i=0;i<5;++i) if (a->masks[i]!=b->masks[i]) return 0;
    for (i=0;i<8;++i) for (j=0;j<4;++j) if (a->sides[i][j]!=b->sides[i][j]) return 0;
    return a->mode==b->mode && a->raw_floor_height==b->raw_floor_height
        && a->lintel_bottom_height==b->lintel_bottom_height && a->ceiling_height==b->ceiling_height
        && a->draw_ceiling==b->draw_ceiling && a->lintel_bottom_texture==b->lintel_bottom_texture
        && a->ceiling_texture==b->ceiling_texture && a->floor_texture==b->floor_texture && a->top_texture==b->top_texture
        && a->lintel_texture==b->lintel_texture && a->marked_land==b->marked_land
        && a->marked_gold==b->marked_gold && a->floor_height==b->floor_height
        && a->lintel_height==b->lintel_height && a->floor_abyss==b->floor_abyss
        && a->unexplored==b->unexplored && a->tagged_valuable==b->tagged_valuable && a->valuable==b->valuable;
}
static void append(struct TerrainColumnPlan* plan, unsigned kind, unsigned height,
    uint16_t texture, unsigned unlit, unsigned top_remap)
{
    struct TerrainColumnOperation* operation=&plan->operations[plan->count++];
    operation->kind=(uint8_t)kind; operation->height=(uint8_t)height; operation->texture=texture;
    operation->unlit=(uint8_t)unlit; operation->top_remap=(uint8_t)top_remap;
}
static void build(struct TerrainColumnPlan* plan, const struct TerrainColumnInputs* in)
{
    unsigned height, side;
    plan->count=0;
    for (height=0;height<8;++height) if (in->masks[0] & (1u<<height))
        for (side=0;side<4;++side) if (!(in->masks[side+1] & (1u<<height)))
            append(plan,side,height,in->sides[height][side],0,0);
    append(plan,TCP_Abyss,0,0,0,0);
    if (in->mode==TCP_Perspective) {
        if (in->floor_height) append(plan,TCP_Top,in->floor_height,in->top_texture,0,1);
        else if (!in->floor_abyss) append(plan,TCP_Top,0,in->floor_texture,0,1);
    } else if (in->floor_height && (in->mode!=TCP_Cluedo || in->floor_height<=8)) {
        if (in->unexplored) append(plan,TCP_Top,in->floor_height,in->marked_land,1,0);
        else if (in->mode==TCP_Cluedo) {
            if (in->tagged_valuable) append(plan,TCP_Top,in->floor_height,in->marked_gold,1,0);
            else if (in->raw_floor_height && in->raw_floor_height<=8)
                append(plan,TCP_Top,in->floor_height,in->top_texture,0,1);
        } else if (!in->tagged_valuable) append(plan,TCP_Top,in->floor_height,in->top_texture,0,1);
        else if (in->valuable) append(plan,TCP_Top,in->floor_height,in->marked_gold,1,0);
    } else if (!in->floor_abyss) {
        append(plan,TCP_Top,0,in->unexplored?in->marked_land:in->floor_texture,in->unexplored,!in->unexplored);
    }
    if (in->lintel_height && (in->mode!=TCP_Cluedo || in->lintel_height<=8)) {
        append(plan,TCP_Top,in->lintel_height,in->lintel_texture,0,1);
        if (in->mode==TCP_Perspective)
            append(plan,TCP_Bottom,in->lintel_bottom_height,in->lintel_bottom_texture,0,0);
    }
    if (in->mode==TCP_Perspective && in->draw_ceiling)
        append(plan,TCP_Bottom,in->ceiling_height,in->ceiling_texture,0,0);
}
const struct TerrainColumnPlan* TerrainColumnPlanResolve(uint64_t epoch, uint16_t x, uint16_t y,
    const struct TerrainColumnInputs* inputs)
{
    uint32_t hash=((uint32_t)y<<16)|x;
    unsigned way, chosen=TCP_WAYS;
    hash^=hash>>16; hash*=0x7feb352du; hash^=hash>>15;
    const unsigned set=hash&(TCP_SETS-1);
    ++stats.requests;++stats.mode_requests[inputs->mode];
    if (epoch!=current_epoch) {current_epoch=epoch;++stats.epoch_resets;}
    for (way=0;way<TCP_WAYS;++way) {
        struct TerrainColumnCacheEntry* entry=&entries[set][way];
        if (entry->valid && entry->epoch==epoch && entry->x==x && entry->y==y) {
            if (equal_inputs(&entry->inputs,inputs)) {++stats.hits;++stats.mode_hits[inputs->mode];return &entry->plan;}
            chosen=way;break;
        }
        if (chosen==TCP_WAYS && (!entry->valid || entry->epoch!=epoch)) chosen=way;
    }
    if (chosen==TCP_WAYS) {chosen=victims[set];victims[set]=(uint8_t)((chosen+1)%TCP_WAYS);++stats.evictions;}
    struct TerrainColumnCacheEntry* entry=&entries[set][chosen];
    entry->valid=1;entry->epoch=epoch;entry->x=x;entry->y=y;entry->inputs=*inputs;
    build(&entry->plan,inputs);++stats.builds;++stats.mode_builds[inputs->mode];
    return &entry->plan;
}
struct TerrainColumnPlanStats TerrainColumnPlanGetStats(void)
{
    struct TerrainColumnPlanStats result=stats;
    result.capacity_bytes=sizeof(entries)+sizeof(victims)+sizeof(stats)+sizeof(current_epoch);
    result.capacity_entries=TCP_SETS*TCP_WAYS;
    return result;
}
