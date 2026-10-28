// SPDX-License-Identifier: GPL-2.0-or-later
/* Selected gameplay diagnostics, separate from saves and replay checksums. */
#include "pre_inc.h"
#include "fidelity_state.h"
#include "game_legacy.h"
#include "thing_list.h"
#include <inttypes.h>
#include <fcntl.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#define O_BINARY 0
#endif
#include "post_inc.h"

/* All values are integers with explicit names and deterministic index order.
 * Pointer values, padding, renderer timers, camera, audio and unsynced effects
 * are deliberately absent. This is selected-domain equivalence, not a VM dump. */
#define VALUE(name, value) fprintf(out, ",\"" name "\":%" PRId64, (int64_t)(value))
#define FIELD(record, name) VALUE(#name, (record)->name)
#define ARRAY(record, name, count) do { \
    fprintf(out, ",\"" #name "\":["); \
    for (int j = 0; j < (count); ++j) fprintf(out, "%s%" PRId64, j ? "," : "", (int64_t)(record)->name[j]); \
    fputc(']', out); \
} while (0)

static void creature(FILE *out, const struct CreatureControl *c)
{
    FIELD(c, exp_level); FIELD(c, exp_points); FIELD(c, hunger_level);
    FIELD(c, hunger_amount); FIELD(c, hunger_loss); FIELD(c, paydays_owed);
    FIELD(c, paydays_advanced); ARRAY(c, annoyance_level, 5); FIELD(c, mood_flags);
    FIELD(c, job_assigned); FIELD(c, work_room_id); FIELD(c, target_room_id);
    FIELD(c, lair_room_id); FIELD(c, turns_at_job); FIELD(c, combat_flags);
    FIELD(c, max_health); FIELD(c, spell_flags); FIELD(c, instance_id);
    FIELD(c, inst_turn); FIELD(c, inst_action_turns); FIELD(c, inst_total_turns);
    ARRAY(c, instance_use_turn, INSTANCE_TYPES_MAX);
    FIELD(c, targtng_idx); FIELD(c, active_state_bkp); FIELD(c, continue_state_bkp);
    FIELD(c, conscious_back_turns); FIELD(c, countdown);
    fprintf(out, ",\"casted_spells\":[");
    for (int i = 0; i < CREATURE_MAX_SPELLS_CASTED_AT; ++i) {
        const struct CastedSpellData *s = &c->casted_spells[i];
        fprintf(out, "%s[%d,%ld,%d,%d]", i ? "," : "", (int)s->spkind,
            (long)s->duration, (int)s->caster_level, (int)s->caster_owner);
    }
    fputc(']', out);
}

static void dungeon(FILE *out, const struct Dungeon *d)
{
    FIELD(d, total_money_owned); FIELD(d, offmap_money_owned);
    FIELD(d, creatures_total_pay); FIELD(d, creatures_total_backpay);
    FIELD(d, creature_tendencies); FIELD(d, num_active_creatrs); FIELD(d, num_active_diggers);
    FIELD(d, num_things_in_hand); ARRAY(d, things_in_hand, MAX_THINGS_IN_HAND);
    FIELD(d, research_progress); FIELD(d, total_research_points);
    FIELD(d, current_research_idx); FIELD(d, research_num);
    FIELD(d, manufacture_progress); FIELD(d, total_manufacture_points);
    FIELD(d, manufacture_kind); FIELD(d, manufacture_class); FIELD(d, manufacture_level);
    FIELD(d, turn_last_manufacture); FIELD(d, manufacture_gold);
    FIELD(d, bodies_rotten_for_vampire); FIELD(d, dead_creatures_count);
    FIELD(d, must_obey_turn); FIELD(d, cta_start_turn); FIELD(d, cta_power_level);
    FIELD(d, cta_stl_x); FIELD(d, cta_stl_y); FIELD(d, max_creatures_attracted);
    ARRAY(d, creature_allowed, CREATURE_TYPES_MAX);
    ARRAY(d, creature_sacrifice, CREATURE_TYPES_MAX);
    ARRAY(d, creature_sacrifice_exp, CREATURE_TYPES_MAX);
    ARRAY(d, magic_level, POWER_TYPES_MAX); ARRAY(d, magic_resrchable, POWER_TYPES_MAX);
    ARRAY(d, room_buildable, TERRAIN_ITEMS_MAX); ARRAY(d, room_resrchable, TERRAIN_ITEMS_MAX);
    ARRAY(d, script_flags, SCRIPT_FLAGS_COUNT);
    ARRAY((&d->mnfct_info), trap_amount_stored, TRAPDOOR_TYPES_MAX);
    ARRAY((&d->mnfct_info), trap_amount_placeable, TRAPDOOR_TYPES_MAX);
    ARRAY((&d->mnfct_info), door_amount_stored, TRAPDOOR_TYPES_MAX);
    ARRAY((&d->mnfct_info), door_amount_placeable, TRAPDOOR_TYPES_MAX);
    fprintf(out, ",\"turn_timers\":[");
    for (int i = 0; i < TURN_TIMERS_COUNT; ++i)
        fprintf(out, "%s[%lu,%u]", i ? "," : "", (unsigned long)d->turn_timers[i].count, d->turn_timers[i].state);
    fprintf(out, "],\"research\":[");
    for (int i = 0; i < DUNGEON_RESEARCH_COUNT; ++i)
        fprintf(out, "%s[%u,%u,%ld]", i ? "," : "", d->research[i].rtyp, d->research[i].rkind, (long)d->research[i].req_amount);
    fputc(']', out);
}

int fidelity_write_snapshot(FILE *out, const struct Game *g)
{
    if (!out || !g || g->script.conditions_num > CONDITIONS_COUNT
        || g->script.values_num > SCRIPT_VALUES_COUNT
        || g->script.party_triggers_num > PARTY_TRIGGERS_COUNT
        || g->script.tunneller_triggers_num > TUNNELLER_TRIGGERS_COUNT)
        return 0;
    fprintf(out, "{\"schema\":1,\"scope\":\"selected-gameplay-v1\",\"turn\":%lu,\"level\":%d,\"state\":{\"rng\":{\"action\":%lu,\"ai\":%lu,\"player\":%lu},\"things\":[",
        (unsigned long)g->play_gameturn, (int)g->loaded_level_number,
        (unsigned long)g->action_random_seed, (unsigned long)g->ai_random_seed, (unsigned long)g->player_random_seed);
    int comma = 0;
    for (int i = 1; i < THINGS_COUNT; ++i) {
        const struct Thing *t = &g->things_data[i];
        /* Match is_non_synchronized_thing_class without invoking game code. */
        if (!(t->alloc_flags & TAlF_Exists) || t->class_id == TCls_EffectElem
            || t->class_id == TCls_AmbientSnd || t->class_id == TCls_Effect)
            continue;
        fprintf(out, "%s{\"id\":%d", comma++ ? "," : "", i);
        FIELD(t, class_id); FIELD(t, model); FIELD(t, owner); FIELD(t, creation_turn);
        FIELD(t, active_state); FIELD(t, continue_state); FIELD(t, health);
        VALUE("x", t->mappos.x.val); VALUE("y", t->mappos.y.val); VALUE("z", t->mappos.z.val);
        FIELD(t, move_angle_xy); FIELD(t, random_seed); FIELD(t, holding_player);
        if (t->class_id == TCls_Creature) {
            if (t->ccontrol_idx <= 0 || t->ccontrol_idx >= CREATURES_COUNT) return 0;
            VALUE("gold_carried", t->creature.gold_carried);
            creature(out, &g->cctrl_data[t->ccontrol_idx]);
        } else if (t->class_id == TCls_Trap) {
            VALUE("shots", t->trap.num_shots); VALUE("rearm_turn", t->trap.rearm_turn);
            VALUE("wait_for_rearm", t->trap.wait_for_rearm);
        } else if (t->class_id == TCls_Door) {
            VALUE("locked", t->door.is_locked); VALUE("opening", t->door.opening_counter);
            VALUE("closing", t->door.closing_counter);
        } else if (t->class_id == TCls_Shot) {
            VALUE("damage", t->shot.damage); VALUE("target", t->shot.target_idx);
        }
        fputc('}', out);
    }
    fprintf(out, "],\"dungeons\":[");
    for (int i = 0; i < DUNGEONS_COUNT; ++i) {
        fprintf(out, "%s{\"id\":%d", i ? "," : "", i);
        dungeon(out, &g->dungeon[i]);
        fputc('}', out);
    }
    fprintf(out, "],\"players\":[");
    for (int i = 0; i < PLAYERS_COUNT; ++i) {
        const struct PlayerInfo *p = &g->players[i];
        fprintf(out, "%s{\"id\":%d", i ? "," : "", i);
        FIELD(p, victory_state); VALUE("pay_day_progress", g->pay_day_progress[i]);
        fputc('}', out);
    }
    fprintf(out, "],\"rooms\":["); comma = 0;
    for (int i = 1; i < ROOMS_COUNT; ++i) {
        const struct Room *r = &g->rooms[i];
        if (!r->alloc_flags) continue;
        fprintf(out, "%s{\"id\":%d", comma++ ? "," : "", i);
        FIELD(r, kind); FIELD(r, owner); FIELD(r, health); FIELD(r, slabs_count);
        FIELD(r, total_capacity); FIELD(r, used_capacity); FIELD(r, capacity_used_for_storage);
        FIELD(r, efficiency); FIELD(r, hatch_gameturn);
        fputc('}', out);
    }
    fprintf(out, "],\"slabs\":[");
    for (int i = 0; i < MAX_TILES_X * MAX_TILES_Y; ++i) {
        const struct SlabMap *s = &g->slabmap[i];
        fprintf(out, "%s[%d,%d,%ld,%d]", i ? "," : "", (int)s->kind, (int)s->owner, (long)s->health, (int)s->room_index);
    }
    fprintf(out, "],\"scripts\":{\"conditions\":[");
    for (unsigned long i = 0; i < g->script.conditions_num; ++i) {
        const struct Condition *c = &g->script.conditions[i];
        fprintf(out, "%s[%d,%u,%u,%u,%u,%u,%lu,%u,%u,%u,%d]", i ? "," : "", c->condit_idx,
            c->status, c->plyr_range, c->variabl_type, c->variabl_idx, c->operation, c->rvalue,
            c->plyr_range_right, c->variabl_type_right, c->variabl_idx_right, c->use_second_variable);
    }
    fprintf(out, "],\"values\":[");
    for (unsigned long i = 0; i < g->script.values_num; ++i) {
        const struct ScriptValue *v = &g->script.values[i];
        fprintf(out, "%s[%u,%u,%u,%u", i ? "," : "", v->flags, v->condit_idx, v->valtype, v->plyr_range);
        for (int j = 0; j < 32; ++j) fprintf(out, ",%u", v->bytes[j]);
        fputc(']', out);
    }
    fprintf(out, "],\"party_triggers\":[");
    for (unsigned long i = 0; i < g->script.party_triggers_num; ++i) {
        const struct PartyTrigger *p = &g->script.party_triggers[i];
        fprintf(out, "%s[%u,%u,%d,%u,%lu,%d,%u,%u,%u]", i ? "," : "", p->flags, p->condit_idx,
            p->creatr_id, p->plyr_idx, (unsigned long)p->location, p->spawn_type, p->exp_level, p->carried_gold, p->ncopies);
    }
    fprintf(out, "],\"tunneller_triggers\":[");
    for (unsigned long i = 0; i < g->script.tunneller_triggers_num; ++i) {
        const struct TunnellerTrigger *t = &g->script.tunneller_triggers[i];
        fprintf(out, "%s[%u,%u,%u,%lu,%lu,%ld,%u,%d]", i ? "," : "", t->flags, t->condit_idx,
            t->plyr_idx, t->location, t->heading, t->carried_gold, t->exp_level, t->party_id);
    }
    fprintf(out, "]},\"armageddon\":[%lu,%lu,%d]}}\n", (unsigned long)g->armageddon_cast_turn,
        (unsigned long)g->armageddon_over_turn, (int)g->armageddon_caster_idx);
    return !ferror(out);
}

int fidelity_write_snapshot_path(const char *path, const struct Game *state)
{
    int descriptor = open(path, O_WRONLY | O_CREAT | O_EXCL | O_BINARY, S_IREAD | S_IWRITE);
    if (descriptor < 0) return 0;
    FILE *out = fdopen(descriptor, "wb");
    if (!out) { close(descriptor); remove(path); return 0; }
    int result = fidelity_write_snapshot(out, state);
    if (fclose(out) != 0) result = 0;
    if (!result) remove(path);
    return result;
}
