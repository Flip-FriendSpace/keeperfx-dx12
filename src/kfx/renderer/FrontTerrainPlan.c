// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#include "FrontTerrainPlan.h"

#define FTP_CAPACITY 16384u
#define FTP_BUCKETS 32768u
#define FTP_NONE UINT16_MAX
struct FrontTerrainCacheEntry {
    uint32_t hash;
    uint16_t next;
    struct FrontTerrainInputs inputs;
    struct FrontTerrainPlan plan;
};
static struct FrontTerrainCacheEntry entries[FTP_CAPACITY];
static uint16_t heads[FTP_BUCKETS];
static unsigned count, victim;
static int initialized;
static struct FrontTerrainPlanStats stats;
static uint64_t current_epoch;

static int equal_inputs(const struct FrontTerrainInputs* a, const struct FrontTerrainInputs* b)
{
    unsigned i;
    for (i = 0; i < 8; ++i)
        if (a->present[i] != b->present[i] || a->side_texture[i] != b->side_texture[i]
            || a->top_texture[i] != b->top_texture[i]) return 0;
    return a->solidmask == b->solidmask && a->ceiling_filled == b->ceiling_filled;
}
static void append(struct FrontTerrainPlan* plan, unsigned kind, unsigned height, uint16_t texture, unsigned lower)
{
    struct FrontTerrainOperation* op = &plan->operations[plan->count++];
    op->kind = (uint8_t)kind; op->height = (uint8_t)height;
    op->texture = texture; op->lower_run = (uint8_t)lower;
}
static void build(struct FrontTerrainPlan* plan, const struct FrontTerrainInputs* in)
{
    unsigned height = 0;
    int last_cube = -1;
    plan->count = 0;
    while (height < 8 && in->present[height]) {
        last_cube = (int)height;
        append(plan, FTP_Side, height + 1, in->side_texture[height], 1);
        ++height;
    }
    if (last_cube >= 0) append(plan, FTP_Top, height, in->top_texture[last_cube], 1);
    if (in->ceiling_filled && in->solidmask > (1u << height)) {
        while (height < 8 && !in->present[height]) ++height;
        while (height < 8 && in->present[height]) {
            last_cube = (int)height;
            append(plan, FTP_Side, height + 1, in->side_texture[height], 0);
            ++height;
        }
        /* The original walk retains the lower cube's material if an inconsistent
         * ceiling mask has no upper cube. Preserve its final top at height8. */
        if (last_cube >= 0) append(plan, FTP_Top, height, in->top_texture[last_cube], 0);
    }
}
const struct FrontTerrainPlan* FrontTerrainPlanResolve(uint64_t epoch, uint16_t x, uint16_t y,
    const struct FrontTerrainInputs* inputs)
{
    (void)x; (void)y;
    ++stats.requests;
    if (!initialized || epoch != current_epoch) {
        for (unsigned i = 0; i < FTP_BUCKETS; ++i) heads[i] = FTP_NONE;
        count = victim = 0; initialized = 1; current_epoch = epoch; ++stats.epoch_resets;
    }
    // Hash explicit values, never structure padding. Hash equality is only a hint.
    uint32_t hash = 2166136261u;
    for (unsigned i = 0; i < 8; ++i) {
        hash = (hash ^ inputs->present[i]) * 16777619u;
        hash = (hash ^ inputs->side_texture[i]) * 16777619u;
        hash = (hash ^ inputs->top_texture[i]) * 16777619u;
    }
    hash = (hash ^ inputs->solidmask) * 16777619u;
    hash = (hash ^ inputs->ceiling_filled) * 16777619u;
    const unsigned bucket = hash & (FTP_BUCKETS - 1);
    for (uint16_t i = heads[bucket]; i != FTP_NONE; i = entries[i].next) {
        struct FrontTerrainCacheEntry* entry = &entries[i];
        if (entry->hash == hash && equal_inputs(&entry->inputs, inputs)) {
            ++stats.hits; return &entry->plan;
        }
    }
    unsigned chosen;
    if (count < FTP_CAPACITY) chosen = count++;
    else {
        chosen = victim; victim = (victim + 1) % FTP_CAPACITY; ++stats.evictions;
        uint16_t* link = &heads[entries[chosen].hash & (FTP_BUCKETS - 1)];
        while (*link != chosen) link = &entries[*link].next;
        *link = entries[chosen].next;
    }
    struct FrontTerrainCacheEntry* entry = &entries[chosen];
    entry->hash = hash; entry->next = heads[bucket]; heads[bucket] = (uint16_t)chosen;
    entry->inputs = *inputs; build(&entry->plan, inputs); ++stats.builds;
    return &entry->plan;
}
struct FrontTerrainPlanStats FrontTerrainPlanGetStats(void)
{
    struct FrontTerrainPlanStats result = stats;
    result.capacity_bytes = sizeof(entries) + sizeof(heads) + sizeof(count) + sizeof(victim) + sizeof(initialized) + sizeof(stats) + sizeof(current_epoch);
    result.capacity_entries = FTP_CAPACITY;
    return result;
}
