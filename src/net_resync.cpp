// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file net_resync.cpp
 *     Network resynchronization for Dungeon Keeper multiplayer.
 * @par Purpose:
 *     Network resynchronization routines for multiplayer games.
 * @par Comment:
 *     None.
 * @author   KeeperFX Team
 * @date     31 Oct 2025
 * @par  Copying and copyrights:
 *     This program is free software; you can redistribute it and/or modify
 *     it under the terms of the GNU General Public License as published by
 *     the Free Software Foundation; either version 2 of the License, or
 *     (at your option) any later version.
 */
/******************************************************************************/
#include "pre_inc.h"
#include "kfx/renderer/WorldSceneCapture.h"
#include "net_resync.h"
#include "state_codec.h"
#include "state_navigation.h"
#include "bflib_datetm.h"
#include "bflib_enet.h"
#include "net_exchange_gameplay.h"
#include "net_exchange_common.h"
#include "net_main.h"
#include <zlib.h>
#include "globals.h"
#include "frontend.h"
#include "frontmenu_ingame_map.h"
#include "player_data.h"
#include "net_game.h"
#include "net_lobby.h"
#include "game_legacy.h"
#include "lens_api.h"
#include "lua_base.h"
#include "net_input_lag.h"
#include "net_checksums.h"
#include "net_spectator.h"
#include "ariadne_update.h"
#include "keeperfx.hpp"
#include "post_inc.h"

#ifdef __cplusplus
extern "C" {
#endif

struct Boing {
  unsigned char active_panel_menu_index;
  unsigned char comp_player_aggressive;
  unsigned char comp_player_defensive;
  unsigned char comp_player_construct;
  unsigned char comp_player_creatrsonly;
  unsigned char creatures_tend_imprison;
  unsigned char creatures_tend_flee;
  unsigned short hand_over_subtile_x;
  unsigned short hand_over_subtile_y;
  unsigned long chosen_room_kind;
  unsigned long chosen_room_spridx;
  unsigned long chosen_room_tooltip;
  unsigned long chosen_spell_type;
  unsigned long chosen_spell_spridx;
  unsigned long chosen_spell_tooltip;
  unsigned long manufactr_element;
  unsigned long manufactr_spridx;
  unsigned long manufactr_tooltip;
  struct GuiMessage messages[GUI_MESSAGES_COUNT];
  unsigned char active_messages_count;
};

static struct Boing boing;

TbBool detailed_multiplayer_logging = false;

#define RESYNC_RECEIVE_TIMEOUT_MS 30000
#define RESYNC_HEADER_SIZE 21
#define RESYNC_STATE_HEADER_SIZE 20

struct ResyncHeader {
    unsigned char message_type;
    uint32_t protocol;
    uint32_t compressed_length;
    uint32_t original_length;
    uint32_t data_checksum;
    uint32_t gameplay_generation;
};

static void read_resync_header(const char *data, struct ResyncHeader *header)
{
    header->message_type = (unsigned char)data[0];
    header->protocol = kfx_state_read_u32(data + 1);
    header->compressed_length = kfx_state_read_u32(data + 5);
    header->original_length = kfx_state_read_u32(data + 9);
    header->data_checksum = kfx_state_read_u32(data + 13);
    header->gameplay_generation = kfx_state_read_u32(data + 17);
}

// function to intentionally desync the game state for testing purposes
void intentional_desync()
{
    if (!network_is_active() || network_is_host()) {
        return;
    }
    struct Room* start_rooms = &game.rooms[1];
    struct Room* end_rooms = &game.rooms[ROOMS_COUNT];

    for (struct Room* room = start_rooms; room < end_rooms; room += 1) {
        if (room_exists(room)) {
            room->slabs_count += 1;
            break;
        }
    }
    int i = game.thing_lists[TngList_Creatures].index;
    if (i != 0) {
        struct Thing* thing = thing_get(i);
        if (!thing_is_invalid(thing)) {
            thing->health += 1;
        }
    }
    get_player(0)->instance_remain_turns += 1;
}

void animate_resync_progress_bar(int current_phase, int total_phases)
{
    if (get_gameturn() == 0) {
        return;
    }
    if ((game.operation_flags & GOF_Paused) != 0) {
        return;
    }
    const int32_t max_progress = 32 * units_per_pixel / 16;
    const int32_t progress_pixels = (int32_t)((double)max_progress * current_phase / total_phases);
    draw_out_of_sync_box(progress_pixels, max_progress, status_panel_width);
}

void store_localised_game_structure(void)
{
    boing.active_panel_menu_index = game.active_panel_mnu_idx;
    boing.comp_player_aggressive = game.comp_player_aggressive;
    boing.comp_player_defensive = game.comp_player_defensive;
    boing.comp_player_construct = game.comp_player_construct;
    boing.comp_player_creatrsonly = game.comp_player_creatrsonly;
    boing.creatures_tend_imprison = game.creatures_tend_imprison;
    boing.creatures_tend_flee = game.creatures_tend_flee;
    boing.hand_over_subtile_x = game.hand_over_subtile_x;
    boing.hand_over_subtile_y = game.hand_over_subtile_y;
    boing.chosen_room_kind = game.chosen_room_kind;
    boing.chosen_room_spridx = game.chosen_room_spridx;
    boing.chosen_room_tooltip = game.chosen_room_tooltip;
    boing.chosen_spell_type = game.chosen_spell_type;
    boing.chosen_spell_spridx = game.chosen_spell_spridx;
    boing.chosen_spell_tooltip = game.chosen_spell_tooltip;
    boing.manufactr_element = game.manufactr_element;
    boing.manufactr_spridx = game.manufactr_spridx;
    boing.manufactr_tooltip = game.manufactr_tooltip;
    memcpy(boing.messages, game.messages, sizeof(boing.messages));
    boing.active_messages_count = game.active_messages_count;
}

void recall_localised_game_structure(void)
{
    game.active_panel_mnu_idx = boing.active_panel_menu_index;
    game.comp_player_aggressive = boing.comp_player_aggressive;
    game.comp_player_defensive = boing.comp_player_defensive;
    game.comp_player_construct = boing.comp_player_construct;
    game.comp_player_creatrsonly = boing.comp_player_creatrsonly;
    game.creatures_tend_imprison = boing.creatures_tend_imprison;
    game.creatures_tend_flee = boing.creatures_tend_flee;
    game.hand_over_subtile_x = boing.hand_over_subtile_x;
    game.hand_over_subtile_y = boing.hand_over_subtile_y;
    game.chosen_room_kind = boing.chosen_room_kind;
    game.chosen_room_spridx = boing.chosen_room_spridx;
    game.chosen_room_tooltip = boing.chosen_room_tooltip;
    game.chosen_spell_type = boing.chosen_spell_type;
    game.chosen_spell_spridx = boing.chosen_spell_spridx;
    game.chosen_spell_tooltip = boing.chosen_spell_tooltip;
    game.manufactr_element = boing.manufactr_element;
    game.manufactr_spridx = boing.manufactr_spridx;
    game.manufactr_tooltip = boing.manufactr_tooltip;
    memcpy(game.messages, boing.messages, sizeof(game.messages));
    game.active_messages_count = boing.active_messages_count;
}

// resync message: ResyncHeader, then the zlib-compressed data
static char *encode_resync_message(const void * buffer, size_t total_length, size_t *message_size)
{
    TbClockMSec start_time = LbTimerClock();
    if (total_length > KFX_STATE_MAX_SIZE) {
        ERRORLOG("Resync data too large");
        return NULL;
    }

    uLongf compressed_size = compressBound(total_length);
    if (compressed_size > UINT32_MAX - RESYNC_HEADER_SIZE) {
        ERRORLOG("Compressed resync data too large");
        return NULL;
    }

    char * message_buffer = (char *) malloc(RESYNC_HEADER_SIZE + compressed_size);
    if (message_buffer == NULL) {
        ERRORLOG("Failed to allocate message buffer");
        return NULL;
    }

    int compress_result = compress2((Bytef *)(message_buffer + RESYNC_HEADER_SIZE), &compressed_size, (const Bytef *)buffer, total_length, Z_BEST_SPEED);
    if (compress_result != Z_OK) {
        ERRORLOG("Compression failed: zlib error %d", compress_result);
        free(message_buffer);
        return NULL;
    }

    uLong data_crc = crc32(0L, Z_NULL, 0);
    data_crc = crc32(data_crc, (const Bytef *)buffer, total_length);
    NETLOG("Compression successful: %u -> %u bytes in %u ms", (uint32_t)total_length, (uint32_t)compressed_size, (uint32_t)(LbTimerClock() - start_time));

    message_buffer[0] = NETMSG_RESYNC_DATA;
    kfx_state_write_u32(message_buffer + 1, KFX_STATE_PROTOCOL);
    kfx_state_write_u32(message_buffer + 5, (uint32_t)compressed_size);
    kfx_state_write_u32(message_buffer + 9, (uint32_t)total_length);
    kfx_state_write_u32(message_buffer + 13, (uint32_t)data_crc);
    kfx_state_write_u32(message_buffer + 17, netstate.gameplay_generation);
    *message_size = RESYNC_HEADER_SIZE + compressed_size;
    return message_buffer;
}

static void send_resync_message(const char * message_buffer, size_t message_size)
{
    NETLOG("Host: Sending resync data to all clients");
    for (NetUserId user_index = 0; user_index < MAX_NET_CONNECTIONS; ++user_index) {
        if (user_index == SERVER_ID || netstate.users[user_index].progress != USER_LOGGEDIN || (user_index >= MAX_NET_USERS && netstate.users[user_index].spectator_state != NetSpectator_Streaming)) {
            continue;
        }
        netstate.sp->sendmsg_single(netstate.users[user_index].id, message_buffer, message_size);
    }
}

static TbBool send_resync_data(const void * buffer, size_t total_length)
{
    size_t message_size;
    char * message_buffer = encode_resync_message(buffer, total_length, &message_size);
    if (message_buffer == NULL) {
        return false;
    }
    send_resync_message(message_buffer, message_size);
    free(message_buffer);
    return true;
}

static TbBool decode_resync_message(const char * message_buffer, size_t message_size, size_t expected_length, char ** data_buffer, size_t * data_length)
{
    if (message_size < RESYNC_HEADER_SIZE) {
        ERRORLOG("Resync message too small: %u bytes", (uint32_t)message_size);
        return false;
    }
    ResyncHeader header;
    read_resync_header(message_buffer, &header);
    if (header.message_type != NETMSG_RESYNC_DATA || header.protocol != KFX_STATE_PROTOCOL) {
        ERRORLOG("Resync message has wrong type: %d", header.message_type);
        return false;
    }
    if (header.compressed_length != message_size - RESYNC_HEADER_SIZE) {
        ERRORLOG("Resync message size mismatch: %u != %u",
            (uint32_t)(message_size - RESYNC_HEADER_SIZE), header.compressed_length);
        return false;
    }
    if (expected_length != 0 && header.original_length != expected_length) {
        ERRORLOG("Resync data with wrong size: %u != %u", header.original_length, (uint32_t)expected_length);
        return false;
    }

    size_t output_size = header.original_length;
    if (output_size > KFX_STATE_MAX_SIZE) {
        return false;
    }
    if (output_size == 0) {
        output_size = 1;
    }
    char * output_buffer = (char *) malloc(output_size);
    if (output_buffer == NULL) {
        ERRORLOG("Failed to allocate resync destination buffer");
        return false;
    }

    uLongf dest_len = output_size;
    int uncompress_result = uncompress((Bytef *)output_buffer, &dest_len,
        (const Bytef *)(message_buffer + RESYNC_HEADER_SIZE), header.compressed_length);
    if (uncompress_result != Z_OK || dest_len != header.original_length) {
        ERRORLOG("Decompression failed: zlib error %d, expected %u bytes, got %u bytes",
            uncompress_result, header.original_length, (uint32_t)dest_len);
        free(output_buffer);
        return false;
    }

    uLong verify_crc = crc32(0L, Z_NULL, 0);
    verify_crc = crc32(verify_crc, (const Bytef *)output_buffer, header.original_length);
    if ((uint32_t)verify_crc != header.data_checksum) {
        ERRORLOG("Resync data checksum mismatch");
        free(output_buffer);
        return false;
    }

    *data_buffer = output_buffer;
    *data_length = header.original_length;
    return true;
}

static TbBool receive_resync_message(char **message_out, size_t *message_size_out)
{
    netstate.resync_pending = false;
    NETLOG("Starting to receive resync data");

    TbClockMSec start_time = LbTimerClock();
    TbClockMSec last_progress_time = start_time;
    int32_t last_progress = 0;
    while (LbTimerClock() - start_time < RESYNC_RECEIVE_TIMEOUT_MS) {
        netstate.sp->update(OnNewUser);
        int32_t progress = GetResyncProgress() * 80 / 100;
        if (progress > last_progress && LbTimerClock() - last_progress_time >= 50) {
            animate_resync_progress_bar(progress, 100);
            last_progress = progress;
            last_progress_time = LbTimerClock();
        }
        size_t received_size = netstate.sp->msgready(SERVER_ID, 10);
        if (received_size == 0) {
            continue;
        }

        if (received_size > 16 * 1024 * 1024) {
            return false;
        }
        size_t expected_size = received_size;
        char *message_buffer = (char *)malloc(received_size);
        if (message_buffer == NULL) {
            ERRORLOG("Failed to allocate message buffer");
            return false;
        }

        received_size = netstate.sp->readmsg(SERVER_ID, message_buffer, received_size);
        if (received_size != expected_size) {
            free(message_buffer);
            return false;
        }
        if (received_size > 0 && received_size <= sizeof(netstate.msg_buffer) && message_buffer[0] == NETMSG_USERUPDATE) {
            memcpy(netstate.msg_buffer, message_buffer, received_size);
            netstate.msg_buffer_null = '\0';
            if (received_size < sizeof(netstate.msg_buffer)) {
                netstate.msg_buffer[received_size] = '\0';
            }
            process_user_update_message(SERVER_ID, netstate.msg_buffer + 1, netstate.msg_buffer + received_size);
            free(message_buffer);
            continue;
        }
        if (received_size < RESYNC_HEADER_SIZE) {
            if (received_size > 0 && message_buffer[0] == NETMSG_RESYNC_DATA) {
                ERRORLOG("Received incomplete resync data: %u bytes", (uint32_t)received_size);
            } else {
                ERRORLOG("Ignoring message too small for resync data: %u bytes", (uint32_t)received_size);
            }
            free(message_buffer);
            continue;
        }
        if (message_buffer[0] != NETMSG_RESYNC_DATA) {
            MULTIPLAYER_LOG("Received wrong message type: %d", message_buffer[0]);
            free(message_buffer);
            continue;
        }
        MULTIPLAYER_LOG("Client: Received resync message, %u bytes", (uint32_t)received_size);
        NETLOG("Resync transfer received: %u bytes in %u ms", (uint32_t)received_size, (uint32_t)(LbTimerClock() - start_time));
        animate_resync_progress_bar(80, 100);
        *message_out = message_buffer;
        *message_size_out = received_size;
        return true;
    }

    ERRORLOG("Client: Timeout waiting for resync data after %dms", RESYNC_RECEIVE_TIMEOUT_MS);
    return false;
}

static TbBool receive_resync_data(char ** data_buffer, size_t * data_length)
{
    char * message_buffer = NULL;
    size_t message_size = 0;
    if (!receive_resync_message(&message_buffer, &message_size)) {
        return false;
    }
    TbBool result = decode_resync_message(message_buffer, message_size, *data_length, data_buffer, data_length);
    free(message_buffer);
    if (result) {
        NETLOG("Client: Resync data received successfully");
    }
    return result;
}

TbBool LbNetwork_Resync(void * data_buffer, size_t buffer_length)
{
    MULTIPLAYER_LOG("Starting resync, my_id=%d, buffer_length=%u", netstate.my_id, (uint32_t)buffer_length);

    TbBool result;
    if (network_is_host()) {
        MULTIPLAYER_LOG("Resync: I am the server");
        result = send_resync_data(data_buffer, buffer_length);
    } else {
        MULTIPLAYER_LOG("Resync: I am a client, receiving");
        char * received_data = NULL;
        size_t received_length = buffer_length;
        result = receive_resync_data(&received_data, &received_length);
        if (result) {
            memcpy(data_buffer, received_data, buffer_length);
        }
        free(received_data);
    }

    if (!result) {
        ERRORLOG("Resync FAILED");
        return false;
    }

    MULTIPLAYER_LOG("Resync data transfer completed successfully");
    return true;
}

static char *build_resync_game_data(size_t *full_resync_len)
{
    *full_resync_len = 0;
    const char *lua_data = "";
    size_t lua_size = 0;
    if (Lvl_script != NULL)
        lua_data = lua_get_serialised_data(&lua_size);
    unsigned char *game_data = NULL;
    unsigned char *nav_data = NULL;
    size_t game_size = 0, nav_size = 0;
    struct NavigationSnapshot *nav = (struct NavigationSnapshot *)malloc(sizeof(*nav));
    char *result = NULL;
    if (nav != NULL && lua_data != NULL && lua_size <= 16u * 1024u * 1024u) {
        capture_navigation_snapshot(nav);
        if (kfx_state_encode(KfxState_Game, &game, &game_data, &game_size)
            && kfx_state_encode(KfxState_Navigation, nav, &nav_data, &nav_size)) {
            size_t total = RESYNC_STATE_HEADER_SIZE + game_size + lua_size + nav_size;
            if (total <= KFX_STATE_MAX_SIZE)
                result = (char *)malloc(total);
            if (result != NULL) {
                memcpy(result, "KFXR", 4);
                kfx_state_write_u32(result + 4, KFX_STATE_PROTOCOL);
                kfx_state_write_u32(result + 8, (uint32_t)game_size);
                kfx_state_write_u32(result + 12, (uint32_t)lua_size);
                kfx_state_write_u32(result + 16, (uint32_t)nav_size);
                memcpy(result + RESYNC_STATE_HEADER_SIZE, game_data, game_size);
                memcpy(result + RESYNC_STATE_HEADER_SIZE + game_size, lua_data, lua_size);
                memcpy(result + RESYNC_STATE_HEADER_SIZE + game_size + lua_size, nav_data, nav_size);
                *full_resync_len = total;
            }
        }
    }
    free(nav);
    free(nav_data);
    free(game_data);
    cleanup_serialized_data();
    return result;
}

static TbBool apply_resync_game_data(const char *data, size_t length)
{
    if (length < RESYNC_STATE_HEADER_SIZE || length > KFX_STATE_MAX_SIZE
        || memcmp(data, "KFXR", 4) != 0 || kfx_state_read_u32(data + 4) != KFX_STATE_PROTOCOL)
        return false;
    size_t game_size = kfx_state_read_u32(data + 8);
    size_t lua_size = kfx_state_read_u32(data + 12);
    size_t nav_size = kfx_state_read_u32(data + 16);
    if (game_size != kfx_state_size(KfxState_Game) || nav_size != kfx_state_size(KfxState_Navigation)
        || lua_size > 16u * 1024u * 1024u
        || length != RESYNC_STATE_HEADER_SIZE + game_size + lua_size + nav_size
        || (Lvl_script == NULL && lua_size != 0))
        return false;
    const char *game_data = data + RESYNC_STATE_HEADER_SIZE;
    const char *lua_data = game_data + game_size;
    const char *nav_data = lua_data + lua_size;
    // Validate and stage both snapshots before touching live simulation or Lua.
    struct Game *staged = (struct Game *)calloc(1, sizeof(*staged));
    struct NavigationSnapshot *nav = (struct NavigationSnapshot *)calloc(1, sizeof(*nav));
    TbBool result = false;
    if (staged != NULL && nav != NULL
        && kfx_state_decode(KfxState_Game, staged, game_data, game_size)
        && kfx_state_decode(KfxState_Navigation, nav, nav_data, nav_size)
        && (Lvl_script == NULL || lua_set_serialised_data(lua_data, lua_size))) {
        kfx_state_copy_game(&game, staged);
        WorldSceneReset();
        apply_navigation_snapshot(nav);
        result = true;
    }
    free(nav);
    free(staged);
    return result;
}

static TbBool apply_resync_game_message(const char * message_buffer, size_t message_size)
{
    TbClockMSec start_time = LbTimerClock();
    char * full_resync_data = NULL;
    size_t full_resync_len = 0;
    if (!decode_resync_message(message_buffer, message_size, 0, &full_resync_data, &full_resync_len)) {
        return false;
    }
    TbBool result = apply_resync_game_data(full_resync_data, full_resync_len);
    free(full_resync_data);
    if (result) {
        ResyncHeader header;
        read_resync_header(message_buffer, &header);
        netstate.gameplay_generation = header.gameplay_generation;
    }
    NETLOG("Resync decompression and state application took %u ms", (uint32_t)(LbTimerClock() - start_time));
    return result;
}

static char *create_resync_game_message(size_t *message_size)
{
    size_t full_resync_len = 0;
    char * full_resync_data = build_resync_game_data(&full_resync_len);
    if (full_resync_data == NULL) {
        return NULL;
    }
    char *message_buffer = encode_resync_message(full_resync_data, full_resync_len, message_size);
    free(full_resync_data);
    return message_buffer;
}

TbBool state_resync_self_test(void)
{
    const char *enabled = getenv("KFX_STATE_TEST");
    if (enabled == NULL || strcmp(enabled, "1") != 0 || network_is_active())
        return false;
    size_t before_size = 0, message_size = 0;
    char *before = build_resync_game_data(&before_size);
    if (before == NULL)
        return false;
    char *message = encode_resync_message(before, before_size, &message_size);
    TbBool ok = message != NULL;
    if (ok) {
        // Framing, protocol and compressed-data failures must be harmless.
        ok = !apply_resync_game_message(message, RESYNC_HEADER_SIZE - 1);
        message[1] ^= 1;
        ok = !apply_resync_game_message(message, message_size) && ok;
        message[1] ^= 1;
        message[message_size - 1] ^= 1;
        ok = !apply_resync_game_message(message, message_size) && ok;
        message[message_size - 1] ^= 1;
        // Exercise a checksum-valid envelope with a corrupt navigation block.
        before[before_size - 1] ^= 1;
        ok = !apply_resync_game_data(before, before_size) && ok;
        before[before_size - 1] ^= 1;
        ok = apply_resync_game_message(message, message_size) && ok;
    }
    size_t after_size = 0;
    char *after = build_resync_game_data(&after_size);
    // Lua tables are serialized with pairs(), whose key order may change after
    // re-creation. Compare the two canonical C snapshots independently of that
    // byte ordering; Lua application itself must have succeeded above.
    const size_t game_size = kfx_state_size(KfxState_Game);
    const size_t nav_size = kfx_state_size(KfxState_Navigation);
    const TbBool game_equal = after != NULL && after_size >= RESYNC_STATE_HEADER_SIZE + game_size + nav_size
        && memcmp(before + RESYNC_STATE_HEADER_SIZE, after + RESYNC_STATE_HEADER_SIZE, game_size) == 0;
    const TbBool nav_equal = after != NULL && after_size >= nav_size
        && memcmp(before + before_size - nav_size, after + after_size - nav_size, nav_size) == 0;
    ok = ok && game_equal && nav_equal;
    SYNCLOG("State resync canonical comparison: Game=%d Navigation=%d", game_equal, nav_equal);
    SYNCLOG("State resync self-test: %s, snapshot %u bytes, compressed message %u bytes",
        ok ? "PASS" : "FAIL", (unsigned)before_size, (unsigned)message_size);
    free(after);
    free(message);
    free(before);
    return ok;
}

TbBool send_spectator_resync(uint64_t user_mask)
{
    size_t message_size = 0;
    char *message_buffer = create_resync_game_message(&message_size);
    if (message_buffer == NULL) {
        return false;
    }
    send_to_active_peers(1, NetSend_Reliable, message_buffer, message_size, user_mask);
    free(message_buffer);
    return true;
}

TbBool send_resync_game(void)
{
    pack_desync_history_for_resync();
    clear_flag(game.operation_flags, GOF_Paused);
    animate_resync_progress_bar(0, 100);
    NETLOG("Initiating re-synchronization of network game");
    take_game_timestamp();

    size_t message_size = 0;
    netstate.gameplay_generation += 1;
    char *message_buffer = create_resync_game_message(&message_size);
    if (message_buffer == NULL) {
        netstate.gameplay_generation -= 1;
        return false;
    }
    send_resync_message(message_buffer, message_size);
    replay_record_resync(message_buffer, message_size);
    free(message_buffer);
    animate_resync_progress_bar(90, 100);
    NETLOG("Host: Resync data queued");
    return true;
}

TbBool receive_resync_game(void)
{
    clear_flag(game.operation_flags, GOF_Paused);
    animate_resync_progress_bar(0, 100);
    NETLOG("Initiating re-synchronization of network game");
    char * message_buffer = NULL;
    size_t message_size = 0;
    if (!receive_resync_message(&message_buffer, &message_size)) {
        return false;
    }
    TbBool result = apply_resync_game_message(message_buffer, message_size);
    if (result) {
        replay_record_resync(message_buffer, message_size);
    }
    free(message_buffer);
    if (!result) {
        return false;
    }

    animate_resync_progress_bar(90, 100);

    compare_desync_history_from_host();

    return true;
}

static void finish_resync(const struct Packet *saved_packets)
{
    if (Lvl_script != NULL) {
        lua_set_random_seed(game.action_random_seed);
    }
    recall_localised_game_structure();
    rebuild_net_user_player_numbers();
    reinit_level_after_load();
    if (network_is_active() && net_join_role == NetRole_Spectator) {
        reinitialise_eye_lens(game.applied_lens_type);
    }
    memcpy(game.packets, saved_packets, sizeof(game.packets));

    if (!network_is_active() || !network_user_is_spectator(netstate.my_id)) {
        game.skip_initial_input_turns = calculate_skip_input();
    }
    initialize_packet_history();
    NETLOG("Input lag after resync: %d turns", game.input_lag_turns);

    clear_flag(local_system_flags, GSF_NetGameNoSync);
    clear_flag(local_system_flags, GSF_NetSeedNoSync);
    panel_map_update(0, 0, game.map_subtiles_x + 1, game.map_subtiles_y + 1);
}

void resync_game(void)
{
    TbClockMSec start_time = LbTimerClock();
    SYNCDBG(2,"Starting");
    draw_out_of_sync_box(0, 32*units_per_pixel/16, local_state.engine_window_x);
    reset_eye_lenses();
    store_localised_game_structure();
    struct Packet saved_packets[PACKETS_COUNT];
    memcpy(saved_packets, game.packets, sizeof(saved_packets));
    TbBool result;
    if (network_is_host()) {
        result = send_resync_game();
    } else {
        result = receive_resync_game();
    }
    if (!result) {
        recall_localised_game_structure();
        return;
    }
    finish_resync(saved_packets);
    wait_for_all_players();
    animate_resync_progress_bar(100, 100);
    NETLOG("Resync finished locally in %u ms", (uint32_t)(LbTimerClock() - start_time));
}

TbBool apply_recorded_resync(const char * message_buffer, size_t message_size)
{
    reset_eye_lenses();
    store_localised_game_structure();
    struct Packet saved_packets[PACKETS_COUNT];
    memcpy(saved_packets, game.packets, sizeof(saved_packets));
    if (!apply_resync_game_message(message_buffer, message_size)) {
        recall_localised_game_structure();
        return false;
    }
    finish_resync(saved_packets);
    if (network_is_active() && network_user_is_spectator(netstate.my_id)) {
        network_spectator_reset();
        network_spectator_resync_pending = true;
    }
    return true;
}

#ifdef __cplusplus
}
#endif
