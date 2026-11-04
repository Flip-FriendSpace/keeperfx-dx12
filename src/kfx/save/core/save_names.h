/******************************************************************************/
/** @file save_names.h
 *     NAMS chunk: per-domain code names of config-table entries, and the
 *     refuse-only check that a save's name_domain-tagged fields still resolve
 *     to the same entry in this build's config tables. Sprite animations are
 *     named too, and are moved by name instead of refused.
 */
/******************************************************************************/
#ifndef KFX_SAVE_NAMES_H
#define KFX_SAVE_NAMES_H

#include <stdint.h>

#include "kfx/save/core/save_codec.h"

struct Game;

#ifdef __cplusplus
extern "C" {
#endif
/******************************************************************************/
/** One custom sprite animation of the running session: its name, and the sprite index of its first frame. */
struct SaveSpriteName {
    const char *name;
    uint32_t sprite;
};

/** The custom sprite animations of the running session. Sprite indices below first are the original game's
 *  and mean the same in every session; custom ones are given out in the order the mods load. */
struct SaveSpriteNames {
    /** The first custom sprite index. */
    uint32_t first;
    /** One past the last custom sprite index in use. */
    uint32_t end;
    uint32_t count;
    const struct SaveSpriteName *names;
};

/** Where the custom sprite animations of a save are in the running session. A save's sprite index below
 *  count becomes to[index], or 0 where to[index] is -1: an animation this session doesn't have, which shows
 *  sprite 0 as a config file's unknown animation name does. Any other index stays as it is. count is 0 when
 *  nothing moves: the save has no sprite names, or every animation it names is where it was. */
struct SaveSpriteMap {
    uint32_t count;
    int32_t *to;
    /** Animations named by the save that are at another index now. */
    uint32_t moved;
    /** Animations named by the save that this session doesn't have. */
    uint32_t missing;
    /** The first of the missing animations, for the log. */
    char missing_name[64];
};

/** Writes the NAMS chunk body (not yet wrapped in a SaveChunk) from this
 *  build's config tables, reached through game_state, and the session's
 *  sprite animations (sprites may be NULL: no sprite names are written).
 *  Does not touch any global. */
enum SaveResult save_names_write(const struct Game *game_state, const struct SaveSpriteNames *sprites,
    struct SaveBuffer *out, struct SaveError *err);

/** Checks every name_domain-tagged field of a just-decoded Game against the
 *  NAMS chunk body the file carried (nams_data/nams_len, as returned by
 *  save_chunk_unpack for SCID_Names). Refuses (does not remap) on the first
 *  used index whose file name doesn't match this build's name at that same
 *  position in its domain. Sprite fields aren't checked: see
 *  save_sprite_map_build. */
enum SaveResult save_names_check(const uint8_t *nams_data, uint32_t nams_len,
    const struct Game *game_state, struct SaveError *err);

/** Works out where the sprite animations named in a NAMS chunk body are among the session's (now). The map
 *  is empty (count 0) when the file names no sprites. Free it with save_sprite_map_free. */
enum SaveResult save_sprite_map_build(const uint8_t *nams_data, uint32_t nams_len,
    const struct SaveSpriteNames *now, struct SaveSpriteMap *map, struct SaveError *err);
void save_sprite_map_free(struct SaveSpriteMap *map);

/** The sprite index the running session uses for a save's sprite index. */
uint64_t save_sprite_map_index(const struct SaveSpriteMap *map, uint64_t sprite);

/** Moves every SND_Sprite field of a just-decoded Game through map. Returns how many values changed, and in
 *  *lost (may be NULL) how many of them used an animation this session doesn't have. */
uint32_t save_sprite_map_apply(const struct SaveSpriteMap *map, struct Game *game_state, uint32_t *lost);
/******************************************************************************/
#ifdef __cplusplus
}
#endif
#endif
