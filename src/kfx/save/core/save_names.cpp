/******************************************************************************/
/** @file save_names.cpp
 *     NAMS chunk: writes each name domain's config-table code names, and
 *     checks a decoded Game's name_domain-tagged fields against them
 *     (it refuses a mismatch; it doesn't remap). Sprite animations are the
 *     exception: they are moved to the index their name has now.
 */
/******************************************************************************/
#include "pre_inc.h"
#include "kfx/save/SaveTypes.h"
#include "game_legacy.h"
#include "game_merge.h"
#include "packets.h"
#include "config_campaigns.h"
#include "front_network.h"
#include "creature_control.h"
#include "thing_list.h"
#include "post_inc.h"

#include <algorithm>
#include <array>
#include <iterator>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "kfx/save/core/save_bytes.h"
#include "kfx/save/core/save_names.h"
#include "kfx/save/core/save_schema.h"
#include "kfx/save/core/schema/save_tables_decl.h"
/******************************************************************************/
using savebytes::ByteReader;

#define GOFF(path) ((uint32_t)offsetof(struct Game, path))
constexpr uint32_t NO_COUNT_FIELD = 0xFFFFFFFFU;

struct SaveNameDomainRow {
    uint16_t domain;
    uint32_t array_offset;   /* offsetof(struct Game, ...), first element */
    uint32_t elem_stride;
    uint32_t name_offset;    /* offset of the code name within one element */
    uint32_t name_maxlen;
    uint32_t count_offset;   /* offsetof(struct Game, ..._count); NO_COUNT_FIELD if none */
    uint32_t fixed_count;    /* array capacity; also the count when count_offset is absent */
};

#define ROW(dom, arrpath, countpath, ElemT, namefield, maxcount) \
    { (dom), GOFF(arrpath), (uint32_t)sizeof(ElemT), (uint32_t)offsetof(ElemT, namefield), \
      (uint32_t)sizeof(((ElemT *)0)->namefield), GOFF(countpath), (maxcount) }
#define ROW_FIXED(dom, arrpath, ElemT, namefield, maxcount) \
    { (dom), GOFF(arrpath), (uint32_t)sizeof(ElemT), (uint32_t)offsetof(ElemT, namefield), \
      (uint32_t)sizeof(((ElemT *)0)->namefield), NO_COUNT_FIELD, (maxcount) }

/* Lens is a real domain (config_lenses.h's LensConfig.code_name) but its table
   (lenses_conf) is a standalone global, not reachable through struct Game, so it
   can't be read from a decoded record the way every other domain here can, so it
   is left untagged. Job is a bitmask
   (CreatureJob), not a plain index - config_creature.c's get_config_for_job()
   converts a flag to a bit position before indexing - so it needs its own
   resolution strategy and is also left untagged. */
// NOLINTNEXTLINE(modernize-avoid-c-arrays)
static const struct SaveNameDomainRow name_domain_rows[] = {
    ROW(SND_CreatureModel,   conf.crtr_conf.model,                    conf.crtr_conf.model_count,                    struct CreatureModelConfig,        name,      CREATURE_TYPES_MAX),
    ROW(SND_RoomKind,        conf.slab_conf.room_cfgstats,            conf.slab_conf.room_types_count,               struct RoomConfigStats,            code_name, TERRAIN_ITEMS_MAX),
    ROW(SND_Trap,            conf.trapdoor_conf.trap_cfgstats,        conf.trapdoor_conf.trap_types_count,           struct TrapConfigStats,            code_name, TRAPDOOR_TYPES_MAX),
    ROW(SND_Door,            conf.trapdoor_conf.door_cfgstats,        conf.trapdoor_conf.door_types_count,           struct DoorConfigStats,            code_name, TRAPDOOR_TYPES_MAX),
    ROW(SND_Object,          conf.object_conf.object_cfgstats,        conf.object_conf.object_types_count,           struct ObjectConfigStats,          code_name, OBJECT_TYPES_MAX),
    ROW(SND_Spell,           conf.magic_conf.spell_cfgstats,          conf.magic_conf.spell_types_count,             struct SpellConfigStats,           code_name, MAGIC_ITEMS_MAX),
    ROW(SND_Power,           conf.magic_conf.power_cfgstats,          conf.magic_conf.power_types_count,             struct PowerConfigStats,           code_name, MAGIC_ITEMS_MAX),
    ROW(SND_Shot,            conf.magic_conf.shot_cfgstats,           conf.magic_conf.shot_types_count,              struct ShotConfigStats,            code_name, MAGIC_ITEMS_MAX),
    ROW_FIXED(SND_Effect,          conf.effects_conf.effect_cfgstats,                                                struct EffectConfigStats,          code_name, EFFECTS_TYPES_MAX),
    ROW_FIXED(SND_EffectElement,   conf.effects_conf.effectelement_cfgstats,                                         struct EffectElementConfigStats,   code_name, EFFECTSELLEMENTS_TYPES_MAX),
    ROW(SND_EffectGenerator, conf.effects_conf.effectgen_cfgstats,    conf.effects_conf.effectgen_cfgstats_count,    struct EffectGeneratorConfigStats, code_name, EFFECTSGEN_TYPES_MAX),
    ROW(SND_Instance,        conf.crtr_conf.instances,                conf.crtr_conf.instances_count,                struct CreatureInstanceConfig,     name,      INSTANCE_TYPES_MAX),
};
#define NAME_DOMAIN_ROW_COUNT ((uint32_t)(sizeof(name_domain_rows) / sizeof(name_domain_rows[0])))

static const struct SaveNameDomainRow *find_row(uint16_t domain)
{
    const auto *const row = std::find_if(std::begin(name_domain_rows), std::end(name_domain_rows),
        [domain](const SaveNameDomainRow &candidate) { return candidate.domain == domain; });
    return (row != std::end(name_domain_rows)) ? row : nullptr;
}

/* Thing.model's domain depends on Thing.class_id (mirrors the class_id groups the
   Thing_u_valuable union table already encodes in schema_things.c). SND_None means
   "this class's model isn't a config-table index" (skip the check). */
static uint16_t resolve_thing_model_domain(uint8_t class_id)
{
    switch (class_id)
    {
    case TCls_Object:        return SND_Object;
    case TCls_Shot:          return SND_Shot;
    case TCls_EffectElem:    return SND_EffectElement;
    case TCls_DeadCreature:
    case TCls_Creature:      return SND_CreatureModel;
    case TCls_Effect:        return SND_Effect;
    case TCls_EffectGen:     return SND_EffectGenerator;
    case TCls_Trap:          return SND_Trap;
    case TCls_Door:          return SND_Door;
    default:                 return SND_None; /* Empty, AmbientSnd (raw sample number), CaveIn (model always 0) */
    }
}

/* Dungeon.manufacture_kind's domain depends on the sibling Dungeon.manufacture_class. */
static uint16_t resolve_manufacture_domain(uint8_t class_id)
{
    switch (class_id)
    {
    case TCls_Trap: return SND_Trap;
    case TCls_Door: return SND_Door;
    default:        return SND_None;
    }
}
/******************************************************************************/
static int put_str(struct SaveBuffer *buffer, const char *text, uint32_t maxlen)
{
    const size_t length = strnlen(text, maxlen);
    return save_buf_u16(buffer, static_cast<uint16_t>(length)) | save_buf_append(buffer, text, static_cast<uint32_t>(length));
}

/* One past the last entry that has a name. Some loaders never keep the table's count field, so the
   names themselves say how far the table goes. */
static uint32_t named_extent(const uint8_t *base, const struct SaveNameDomainRow *row)
{
    for (uint32_t index = row->fixed_count; index > 0; index--)
    {
        const char *name = reinterpret_cast<const char *>(base + row->array_offset + (static_cast<size_t>(index - 1) * row->elem_stride) + row->name_offset);
        if (name[0] != '\0')
            return index;
    }
    return 0;
}

/* The sprite domain lists every sprite index from 0 to the end of the custom ones, so an entry's index is
   the sprite index. Only the first frame of a custom animation has a name; the rest are empty. */
static int write_sprite_names(const struct SaveSpriteNames *sprites, struct SaveBuffer *out)
{
    const uint32_t count = (sprites->end > sprites->first) ? sprites->end : 0;
    std::vector<const char *> names(count, nullptr);
    for (uint32_t index = 0; index < sprites->count; index++)
    {
        const struct SaveSpriteName &sprite_name = sprites->names[index];
        if ((sprite_name.name != nullptr) && (sprite_name.sprite >= sprites->first) && (sprite_name.sprite < count))
            names[sprite_name.sprite] = sprite_name.name;
    }
    int failed = save_buf_u16(out, SND_Sprite) | save_buf_u32(out, count);
    for (const char *name : names)
        failed |= put_str(out, (name != nullptr) ? name : "", UINT16_MAX);
    return failed;
}

enum SaveResult save_names_write(const struct Game *game_state, const struct SaveSpriteNames *sprites,
    struct SaveBuffer *out, struct SaveError *err)
{
    const auto *base = reinterpret_cast<const uint8_t *>(game_state);
    const uint32_t domain_count = NAME_DOMAIN_ROW_COUNT + ((sprites != nullptr) ? 1U : 0U);
    int failed = save_buf_u16(out, static_cast<uint16_t>(domain_count));
    for (const auto &name_domain_row : name_domain_rows)
    {
        const struct SaveNameDomainRow *row = &name_domain_row;
        uint32_t count = row->fixed_count;
        if (row->count_offset != NO_COUNT_FIELD)
        {
            int32_t live;
            memcpy(&live, base + row->count_offset, sizeof(live)); /* Game is packed: no aligned load */
            if ((live >= 0) && (static_cast<uint32_t>(live) < row->fixed_count))
                count = static_cast<uint32_t>(live);
            const uint32_t named = named_extent(base, row);
            count = std::max(count, named);
        }
        failed |= save_buf_u16(out, row->domain);
        failed |= save_buf_u32(out, count);
        for (uint32_t index = 0; index < count; index++)
        {
            const char *name = reinterpret_cast<const char *>(base + row->array_offset + (static_cast<size_t>(index) * row->elem_stride) + row->name_offset);
            failed |= put_str(out, name, row->name_maxlen);
        }
    }
    if (sprites != nullptr)
        failed |= write_sprite_names(sprites, out);
    return (failed != 0) ? save_fail(err, SVR_NoMemory, "out of memory writing the name table") : SVR_Ok;
}
/******************************************************************************/
/* The NAMS payload: for each domain the file has a table for, its names as views into the caller's buffer. */
struct ParsedNames {
    std::array<bool, SND_COUNT> present;
    std::array<std::vector<std::string_view>, SND_COUNT> names;
};

static enum SaveResult parse_nams(const uint8_t *data, uint32_t len, ParsedNames &parsed_names, SaveError *err)
{
    parsed_names.present.fill(false);
    if ((data == nullptr) || (len == 0))
        return SVR_Ok; /* no NAMS chunk in the file: nothing to check against */
    ByteReader reader(data, len);
    const uint32_t domain_count = reader.Get<uint16_t>();
    for (uint32_t domain_index = 0; (domain_index < domain_count) && reader.ok(); domain_index++)
    {
        const auto domain = reader.Get<uint16_t>();
        const auto count = reader.Get<uint32_t>();
        if (count > reader.left() / 2) /* every name takes at least its two length bytes */
            reader.Fail();
        if (!reader.ok())
            break;
        std::vector<std::string_view> names;
        names.reserve(count);
        for (uint32_t index = 0; (index < count) && reader.ok(); index++)
        {
            uint16_t name_length;
            const uint8_t *bytes = reader.TakeString(&name_length);
            if (bytes != nullptr)
                names.emplace_back(reinterpret_cast<const char *>(bytes), name_length);
        }
        if (!reader.ok())
            break;
        if (domain < SND_COUNT) /* a domain this build doesn't know is from a future format; ignore it */
        {
            parsed_names.names[domain] = std::move(names);
            parsed_names.present[domain] = true;
        }
    }
    if (!reader.ok())
        return save_fail(err, SVR_Damaged, "the name table is damaged");
    return SVR_Ok;
}
/******************************************************************************/
static enum SaveResult check_index(const ParsedNames &parsed_names, const struct Game *game_state,
    uint16_t domain, uint32_t index, const char *field_name, struct SaveError *err)
{
    const struct SaveNameDomainRow *row = find_row(domain);
    if ((row == nullptr) || (index >= row->fixed_count))
        return SVR_Ok; /* not a plausible table index (e.g. a "none" sentinel); nothing to check */
    if (!parsed_names.present[domain])
        return SVR_Ok; /* file carries no name table for this domain (no NAMS chunk) */
    const std::vector<std::string_view> &names = parsed_names.names[domain];
    if (index >= names.size())
        return save_fail(err, SVR_Unsupported,
            "this save uses %s index %u, which its own name table doesn't cover", field_name, index);
    const auto *base = reinterpret_cast<const uint8_t *>(game_state);
    const char *cur_name = reinterpret_cast<const char *>(base + row->array_offset + (static_cast<size_t>(index) * row->elem_stride) + row->name_offset);
    const size_t cur_len = strnlen(cur_name, row->name_maxlen);
    const std::string_view &saved = names[index];
    if (saved != std::string_view(cur_name, cur_len))
        return save_fail(err, SVR_Unsupported,
            "this save uses %s '%.*s', which this build/mod set doesn't have at that position",
            field_name, static_cast<int>(saved.size()), saved.data());
    return SVR_Ok;
}

static enum SaveResult check_struct(const struct SaveStructDesc *desc, const uint8_t *base,
    const ParsedNames &parsed_names, const struct Game *game_state, struct SaveError *err)
{
    for (uint32_t field_index = 0; field_index < desc->field_count; field_index++)
    {
        const struct SaveFieldDesc *field = &desc->fields[field_index];
        if (!save_field_included(field, SVM_Save))
            continue;
        const uint32_t count = save_field_count(field);
        const uint32_t elem = field->mem_size / count;
        for (uint32_t index = 0; index < count; index++)
        {
            const uint8_t *bytes = base + field->offset + (static_cast<size_t>(index) * elem);
            if (field->stored_type == SV_STRUCT)
            {
                const enum SaveResult result = check_struct(field->sub, bytes, parsed_names, game_state, err);
                if (result != SVR_Ok)
                    return result;
                continue;
            }
            if ((field->stored_type == SV_UNION) || (field->name_domain == SND_None))
                continue;
            uint16_t domain = field->name_domain;
            if (domain == SND_ByThingClass)
                domain = resolve_thing_model_domain(base[offsetof(struct Thing, class_id)]);
            else if (domain == SND_ByManufactureClass)
                domain = resolve_manufacture_domain(base[offsetof(struct Dungeon, manufacture_class)]);
            if ((domain == SND_None) || (domain == SND_Sprite)) /* sprites are moved, not checked */
                continue;
            struct SaveValue value;
            save_value_load_mem(bytes, elem, field->stored_type, &value);
            if (value.is_float || value.neg)
                continue; /* index fields are non-negative; anything else isn't a real index right now */
            const enum SaveResult result = check_index(parsed_names, game_state, domain, static_cast<uint32_t>(value.mag), field->name, err);
            if (result != SVR_Ok)
                return result;
        }
    }
    return SVR_Ok;
}

enum SaveResult save_names_check(const uint8_t *nams_data, uint32_t nams_len,
    const struct Game *game_state, struct SaveError *err)
{
    ParsedNames parsed_names;
    const enum SaveResult result = parse_nams(nams_data, nams_len, parsed_names, err);
    if (result != SVR_Ok)
        return result;
    return check_struct(&save_desc_Game, reinterpret_cast<const uint8_t *>(game_state), parsed_names, game_state, err);
}
/******************************************************************************/
enum SaveResult save_sprite_map_build(const uint8_t *nams_data, uint32_t nams_len,
    const struct SaveSpriteNames *now, struct SaveSpriteMap *map, struct SaveError *err)
{
    memset(map, 0, sizeof(*map));
    ParsedNames parsed_names;
    const enum SaveResult result = parse_nams(nams_data, nams_len, parsed_names, err);
    if ((result != SVR_Ok) || !parsed_names.present[SND_Sprite])
        return result;
    const std::vector<std::string_view> &saved = parsed_names.names[SND_Sprite];
    std::vector<std::string_view> now_at(std::max(now->end, now->first));
    std::unordered_map<std::string_view, uint32_t> now_by_name;
    for (uint32_t index = 0; index < now->count; index++)
    {
        const struct SaveSpriteName &sprite_name = now->names[index];
        if ((sprite_name.name == nullptr) || (sprite_name.name[0] == '\0'))
            continue;
        const std::string_view name(sprite_name.name);
        now_by_name[name] = sprite_name.sprite;
        if (sprite_name.sprite < now_at.size())
            now_at[sprite_name.sprite] = name;
    }
    std::vector<int32_t> to(saved.size());
    for (uint32_t index = 0; index < saved.size(); index++)
    {
        to[index] = static_cast<int32_t>(index);
        const std::string_view &name = saved[index];
        /* The original sprites never move, and a frame that isn't the first of an animation has no name. */
        if ((index < now->first) || name.empty() || ((index < now_at.size()) && (now_at[index] == name)))
            continue;
        const auto found = now_by_name.find(name);
        if (found != now_by_name.end())
        {
            to[index] = static_cast<int32_t>(found->second);
            map->moved++;
            continue;
        }
        to[index] = -1;
        if (map->missing++ == 0)
            snprintf(map->missing_name, sizeof(map->missing_name), "%.*s", static_cast<int>(name.size()), name.data());
    }
    if ((map->moved == 0) && (map->missing == 0))
        return SVR_Ok;
    map->to = static_cast<int32_t *>(malloc(to.size() * sizeof(int32_t)));
    if (map->to == nullptr)
        return save_fail(err, SVR_NoMemory, "out of memory reading the sprite names");
    memcpy(map->to, to.data(), to.size() * sizeof(int32_t));
    map->count = static_cast<uint32_t>(to.size());
    return SVR_Ok;
}

void save_sprite_map_free(struct SaveSpriteMap *map)
{
    free(map->to);
    memset(map, 0, sizeof(*map));
}

uint64_t save_sprite_map_index(const struct SaveSpriteMap *map, uint64_t sprite)
{
    if ((map == nullptr) || (map->to == nullptr) || (sprite >= map->count))
        return sprite;
    return (map->to[sprite] < 0) ? 0 : static_cast<uint64_t>(map->to[sprite]);
}

static uint32_t apply_struct(const struct SaveSpriteMap *map, const struct SaveStructDesc *desc, uint8_t *base,
    uint32_t *lost)
{
    uint32_t changed = 0;
    for (uint32_t field_index = 0; field_index < desc->field_count; field_index++)
    {
        const struct SaveFieldDesc *field = &desc->fields[field_index];
        if (!save_field_included(field, SVM_Save))
            continue;
        if ((field->stored_type != SV_STRUCT) && (field->name_domain != SND_Sprite))
            continue;
        const uint32_t count = save_field_count(field);
        const uint32_t elem = field->mem_size / count;
        for (uint32_t index = 0; index < count; index++)
        {
            uint8_t *bytes = base + field->offset + (static_cast<size_t>(index) * elem);
            if (field->stored_type == SV_STRUCT)
            {
                changed += apply_struct(map, field->sub, bytes, lost);
                continue;
            }
            struct SaveValue value;
            save_value_load_mem(bytes, elem, field->stored_type, &value);
            if (value.is_float || value.neg)
                continue;
            const uint64_t sprite = save_sprite_map_index(map, value.mag);
            if (sprite == value.mag)
                continue;
            if (map->to[value.mag] < 0)
                (*lost)++;
            value.mag = sprite;
            save_value_store_mem(bytes, elem, field->stored_type, &value, 1);
            changed++;
        }
    }
    return changed;
}

uint32_t save_sprite_map_apply(const struct SaveSpriteMap *map, struct Game *game_state, uint32_t *lost)
{
    uint32_t lost_values = 0;
    const uint32_t changed = ((map == nullptr) || (map->to == nullptr)) ? 0
        : apply_struct(map, &save_desc_Game, reinterpret_cast<uint8_t *>(game_state), &lost_values);
    if (lost != nullptr)
        *lost = lost_values;
    return changed;
}
