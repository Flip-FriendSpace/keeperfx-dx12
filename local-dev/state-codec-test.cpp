// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#include "state_codec.h"
#include "game_legacy.h"
#include "game_saves.h"
#include "replay.h"
#include "state_navigation.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

extern "C" { struct Dungeon bad_dungeon; }

static std::vector<unsigned char> encode(KfxStateKind kind, const void* value)
{
    unsigned char* bytes = nullptr;
    size_t size = 0;
    assert(kfx_state_encode(kind, value, &bytes, &size));
    std::vector<unsigned char> result(bytes, bytes + size);
    free(bytes);
    return result;
}

static uint32_t crc32(const unsigned char* data, size_t length)
{
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (int j = 0; j < 8; ++j) crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0);
    }
    return ~crc;
}

int main(int argc, char** argv)
{
    assert(argc == 3);
    assert(kfx_state_checksum("123456789", 9) == UINT32_C(0xcbf43926));
    assert(kfx_state_checksum(nullptr, 0) == 0);
    static_assert(sizeof(GameVersionPacket) == 20);
    auto peer = net_current_version;
    assert(net_versions_match(&peer, &net_current_version));
    peer.state_protocol = 0;
    assert(!net_versions_match(&peer, &net_current_version));
    auto* source = static_cast<Game*>(calloc(1, sizeof(Game)));
    auto* target = static_cast<Game*>(calloc(1, sizeof(Game)));
    assert(source && target);
    memset(source, 0x5a, sizeof(Game));
    for (int i = 0; i < PLAYERS_COUNT; ++i) source->computer[i].dungeon = nullptr;
    source->sound_settings.music_data_path = nullptr;
    source->sound_settings.dir3 = nullptr;
    // Named fields and union variants produce the same fixture on both ABIs.
    source->process_turn_time = 123456.75L;
    source->sound_settings.sound_data_path = const_cast<char*>("source-local");
    source->computer[0].dungeon = &source->dungeon[DUNGEONS_COUNT - 1];
    source->computer[1].dungeon = INVALID_DUNGEON;
    source->computer[2].dungeon = nullptr;
    source->loaded_level_number = 7;
    memset(source->things_data, 0x5a, sizeof(source->things_data));
    memset(source->cctrl_data, 0xa5, sizeof(source->cctrl_data));
    memset(source->computer_task, 0x33, sizeof(source->computer_task));
    char local_path[] = "destination-local";
    target->sound_settings.sound_data_path = local_path;
    auto bytes = encode(KfxState_Game, source);
    assert(kfx_state_decode(KfxState_Game, target, bytes.data(), bytes.size()));
    assert(target->loaded_level_number == 7);
    assert(target->computer[0].dungeon == &target->dungeon[DUNGEONS_COUNT - 1]);
    assert(target->computer[1].dungeon == INVALID_DUNGEON);
    assert(target->computer[2].dungeon == nullptr);
    assert(target->sound_settings.sound_data_path == local_path && target->process_turn_time == 0);
    assert(bytes == encode(KfxState_Game, target));
    assert(memcmp(source->things_data, target->things_data, sizeof(source->things_data)) == 0);
    assert(memcmp(source->cctrl_data, target->cctrl_data, sizeof(source->cctrl_data)) == 0);
    assert(memcmp(source->computer_task, target->computer_task, sizeof(source->computer_task)) == 0);
    // Every rejected frame must leave destination bytes and pointers unchanged.
    std::vector<unsigned char> before(reinterpret_cast<unsigned char*>(target), reinterpret_cast<unsigned char*>(target) + sizeof(Game));
    for (size_t offset : {size_t(0), size_t(4), size_t(8), size_t(12), size_t(16), size_t(20), size_t(24), size_t(28), bytes.size() - 1}) {
        bytes[offset] ^= 1;
        assert(!kfx_state_decode(KfxState_Game, target, bytes.data(), bytes.size()));
        assert(memcmp(before.data(), target, sizeof(Game)) == 0);
        bytes[offset] ^= 1;
    }
    for (size_t size : {size_t(0), size_t(31), bytes.size() - 1, bytes.size() + 1})
        assert(!kfx_state_decode(KfxState_Game, target, bytes.data(), size));
    // Locate the encoded reference using a one-field change, then give it a
    // valid checksum but invalid object index to exercise semantic validation.
    source->computer[0].dungeon = &source->dungeon[0];
    auto other = encode(KfxState_Game, source);
    size_t ref_offset = KFX_STATE_HEADER_SIZE;
    while (ref_offset < bytes.size() && other[ref_offset] == bytes[ref_offset]) ++ref_offset;
    assert(ref_offset < bytes.size());
    kfx_state_write_u32(other.data() + ref_offset, DUNGEONS_COUNT + 1);
    kfx_state_write_u32(other.data() + 24, crc32(other.data() + KFX_STATE_HEADER_SIZE, other.size() - KFX_STATE_HEADER_SIZE));
    assert(kfx_state_validate(KfxState_Game, other.data(), other.size()));
    assert(!kfx_state_decode(KfxState_Game, target, other.data(), other.size()));
    assert(memcmp(before.data(), target, sizeof(Game)) == 0);
    source->computer[0].dungeon = reinterpret_cast<Dungeon*>(uintptr_t(1));
    unsigned char* failed = nullptr;
    size_t failed_size = 0;
    assert(!kfx_state_encode(KfxState_Game, source, &failed, &failed_size));
    assert(!failed && !failed_size);

    struct Fixture { KfxStateKind kind; size_t native; } fixtures[] = {
        {KfxState_Intralevel, sizeof(IntralevelData)}, {KfxState_Catalogue, sizeof(CatalogueEntry)},
        {KfxState_PacketHeader, sizeof(PacketSaveHead)}, {KfxState_Continue, sizeof(ContinueData)},
        {KfxState_Navigation, sizeof(NavigationSnapshot)},
    };
    FILE* output = fopen(argv[1], "wb");
    assert(output && fwrite(bytes.data(), 1, bytes.size(), output) == bytes.size());
    for (const auto& f : fixtures) {
        void* a = calloc(1, f.native);
        void* b = calloc(1, f.native);
        assert(a && b);
        memset(a, 0x36, f.native);
        auto data = encode(f.kind, a);
        assert(kfx_state_decode(f.kind, b, data.data(), data.size()));
        assert(data == encode(f.kind, b));
        assert(fwrite(data.data(), 1, data.size(), output) == data.size());
        free(a); free(b);
    }
    assert(fclose(output) == 0);
    // Decode a fixture created by the other architecture when supplied.
    if (strcmp(argv[2], "-") != 0) {
        FILE* input = fopen(argv[2], "rb");
        assert(input);
        std::vector<unsigned char> imported(bytes.size());
        assert(fread(imported.data(), 1, imported.size(), input) == imported.size());
        assert(kfx_state_decode(KfxState_Game, target, imported.data(), imported.size()));
        assert(bytes == encode(KfxState_Game, target));
        for (const auto& f : fixtures) {
            std::vector<unsigned char> data(kfx_state_size(f.kind));
            void* value = calloc(1, f.native);
            assert(value && fread(data.data(), 1, data.size(), input) == data.size());
            assert(kfx_state_decode(f.kind, value, data.data(), data.size()));
            assert(data == encode(f.kind, value));
            free(value);
        }
        assert(fgetc(input) == EOF);
        fclose(input);
    }
    printf("PASS: %u-bit codec; Game native=%zu wire=%zu; six schemas, rejected corruption, atomic decode, references and cross-ABI fixtures\n", unsigned(sizeof(void*) * 8), sizeof(Game), bytes.size());
    free(source); free(target);
}
