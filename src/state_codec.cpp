// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
// Fixed-width state encoding shared by saves, replays and network snapshots.
#include "state_codec.h"
#include "game_legacy.h"
#include "game_saves.h"
#include "replay.h"
#include "state_navigation.h"

#include <array>
#include <bit>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <type_traits>
#include <utility>

namespace {
// Frozen legacy union subrecords currently target little-endian machines.
static_assert(std::endian::native == std::endian::little);
static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559);
static_assert(sizeof(double) == 8 && std::numeric_limits<double>::is_iec559);

uint32_t checksum(const unsigned char* data, size_t length)
{
    static constexpr auto table = [] {
        std::array<uint32_t, 256> result{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t value = i;
            for (int bit = 0; bit < 8; ++bit)
                value = (value >> 1) ^ ((value & 1) ? UINT32_C(0xedb88320) : 0);
            result[i] = value;
        }
        return result;
    }();
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < length; ++i)
        crc = (crc >> 8) ^ table[(crc ^ data[i]) & 255];
    return crc ^ UINT32_MAX;
}

struct Codec {
    unsigned char* position;
    unsigned char* end;
    Game* owner;
    bool reading;
    bool ok = true;

    void bytes(void* value, size_t count)
    {
        if (!ok || count > static_cast<size_t>(end - position)) { ok = false; return; }
        if (reading) std::memcpy(value, position, count);
        else std::memcpy(position, value, count);
        position += count;
    }

    void scalars(void* value, size_t native, size_t count, size_t width, bool is_signed)
    {
        if (!ok || !width || width > 8 || native > 8 || native < width || count > static_cast<size_t>(end - position) / width) {
            ok = false;
            return;
        }
        // Explicitly sized scalar arrays have identical bytes on supported
        // little-endian hosts. No enclosing record padding is copied here.
        if (native == width) { bytes(value, count * width); return; }
        auto* field = static_cast<unsigned char*>(value);
        for (size_t i = 0; i < count; ++i, field += native) {
            uint64_t bits = 0;
            if (reading) {
                std::memcpy(&bits, position, width);
                if (is_signed && width < 8 && (bits & (UINT64_C(1) << (width * 8 - 1))))
                    bits |= UINT64_MAX << (width * 8);
                std::memcpy(field, &bits, native);
            } else {
                std::memcpy(&bits, field, native);
                uint64_t narrowed = bits & (UINT64_MAX >> ((8 - width) * 8));
                uint64_t expanded = narrowed;
                if (is_signed && width < 8 && (narrowed & (UINT64_C(1) << (width * 8 - 1))))
                    expanded |= UINT64_MAX << (width * 8);
                if (expanded != bits) { ok = false; return; }
                std::memcpy(position, &narrowed, width);
            }
            position += width;
        }
    }

    void dungeon(Dungeon** field)
    {
        uint32_t index = UINT32_MAX;
        Dungeon* pointer = nullptr;
        std::memcpy(&pointer, field, sizeof(pointer));
        if (!reading && pointer == INVALID_DUNGEON) {
            index = UINT32_MAX - 1;
        } else if (!reading && pointer) {
            const uintptr_t address = reinterpret_cast<uintptr_t>(pointer);
            const uintptr_t first = reinterpret_cast<uintptr_t>(&owner->dungeon[0]);
            const uintptr_t last = reinterpret_cast<uintptr_t>(&owner->dungeon[DUNGEONS_COUNT]);
            if (address < first || address >= last || (address - first) % sizeof(Dungeon)) { ok = false; return; }
            index = static_cast<uint32_t>((address - first) / sizeof(Dungeon));
        }
        scalars(&index, sizeof(index), 1, 4, false);
        if (reading && ok) {
            if (index < UINT32_MAX - 1 && index >= DUNGEONS_COUNT) { ok = false; return; }
            pointer = index == UINT32_MAX ? nullptr : index == UINT32_MAX - 1 ? INVALID_DUNGEON : &owner->dungeon[index];
            std::memcpy(field, &pointer, sizeof(pointer));
        }
    }
};

struct Schema {
    size_t payload;
    size_t native;
    void (*visit)(Codec&, void*);
};

#include "state_schema.inc"

const Schema* schema(KfxStateKind kind)
{
    return kind > 0 && static_cast<size_t>(kind) < std::size(schemas) ? &schemas[kind] : nullptr;
}

void preserve_local_fields(Game* destination, const Game* source)
{
    destination->sound_settings.sound_data_path = source->sound_settings.sound_data_path;
    destination->sound_settings.music_data_path = source->sound_settings.music_data_path;
    destination->sound_settings.dir3 = source->sound_settings.dir3;
    destination->process_turn_time = 0;
}
}

extern "C" uint32_t kfx_state_read_u32(const void* data)
{
    const auto* p = static_cast<const unsigned char*>(data);
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

extern "C" void kfx_state_write_u32(void* data, uint32_t value)
{
    auto* p = static_cast<unsigned char*>(data);
    for (int i = 0; i < 4; ++i) p[i] = static_cast<unsigned char>(value >> (i * 8));
}

extern "C" size_t kfx_state_size(KfxStateKind kind)
{
    const auto* s = schema(kind);
    return s ? KFX_STATE_HEADER_SIZE + s->payload : 0;
}

extern "C" uint32_t kfx_state_checksum(const void* data, size_t length)
{
    return checksum(static_cast<const unsigned char*>(data), length);
}

extern "C" int kfx_state_encode(KfxStateKind kind, const void* state, unsigned char** data, size_t* length)
{
    if (!data || !length) return 0;
    *data = nullptr;
    *length = 0;
    const auto* s = schema(kind);
    if (!s || !state || kfx_state_size(kind) > KFX_STATE_MAX_SIZE) return 0;
    auto* buffer = static_cast<unsigned char*>(std::calloc(1, kfx_state_size(kind)));
    if (!buffer) return 0;
    Codec c{buffer + KFX_STATE_HEADER_SIZE, buffer + kfx_state_size(kind),
        kind == KfxState_Game ? static_cast<Game*>(const_cast<void*>(state)) : nullptr, false};
    s->visit(c, const_cast<void*>(state));
    if (!c.ok || c.position != c.end) { std::free(buffer); return 0; }
    std::memcpy(buffer, "KFXS", 4);
    kfx_state_write_u32(buffer + 4, KFX_STATE_VERSION);
    kfx_state_write_u32(buffer + 8, kind);
    kfx_state_write_u32(buffer + 12, static_cast<uint32_t>(s->payload));
    kfx_state_write_u32(buffer + 16, static_cast<uint32_t>(schemaFingerprint));
    kfx_state_write_u32(buffer + 20, static_cast<uint32_t>(schemaFingerprint >> 32));
    kfx_state_write_u32(buffer + 24, checksum(buffer + KFX_STATE_HEADER_SIZE, s->payload));
    *data = buffer;
    *length = kfx_state_size(kind);
    return 1;
}

extern "C" int kfx_state_validate(KfxStateKind kind, const void* data, size_t length)
{
    const auto* s = schema(kind);
    if (!s || !data || length != kfx_state_size(kind) || length > KFX_STATE_MAX_SIZE) return 0;
    const auto* p = static_cast<const unsigned char*>(data);
    return std::memcmp(p, "KFXS", 4) == 0 && kfx_state_read_u32(p + 4) == KFX_STATE_VERSION &&
        kfx_state_read_u32(p + 8) == static_cast<uint32_t>(kind) && kfx_state_read_u32(p + 12) == s->payload &&
        kfx_state_read_u32(p + 16) == static_cast<uint32_t>(schemaFingerprint) &&
        kfx_state_read_u32(p + 20) == static_cast<uint32_t>(schemaFingerprint >> 32) &&
        kfx_state_read_u32(p + 28) == 0 && kfx_state_read_u32(p + 24) == checksum(p + KFX_STATE_HEADER_SIZE, s->payload);
}

extern "C" void kfx_state_copy_game(Game* destination, const Game* source)
{
    if (destination == source) return;
    // Source must be a decoded state: its dungeon references were validated.
    char* sound_path = destination->sound_settings.sound_data_path;
    char* music_path = destination->sound_settings.music_data_path;
    char* dir3 = destination->sound_settings.dir3;
    std::memcpy(destination, source, sizeof(Game));
    destination->sound_settings.sound_data_path = sound_path;
    destination->sound_settings.music_data_path = music_path;
    destination->sound_settings.dir3 = dir3;
    for (int i = 0; i < PLAYERS_COUNT; ++i) {
        const auto* dungeon = source->computer[i].dungeon;
        destination->computer[i].dungeon = dungeon == INVALID_DUNGEON ? INVALID_DUNGEON :
            dungeon ? &destination->dungeon[dungeon - source->dungeon] : nullptr;
    }
    destination->process_turn_time = 0;
}

extern "C" int kfx_state_decode(KfxStateKind kind, void* state, const void* data, size_t length)
{
    if (!state || !kfx_state_validate(kind, data, length)) return 0;
    const auto* s = schema(kind);
    void* staged = std::calloc(1, s->native);
    if (!staged) return 0;
    if (kind == KfxState_Game) preserve_local_fields(static_cast<Game*>(staged), static_cast<const Game*>(state));
    auto* payload = static_cast<unsigned char*>(const_cast<void*>(data)) + KFX_STATE_HEADER_SIZE;
    Codec c{payload, payload + s->payload, kind == KfxState_Game ? static_cast<Game*>(staged) : nullptr, true};
    s->visit(c, staged);
    const bool success = c.ok && c.position == c.end;
    if (success) {
        if (kind == KfxState_Game) kfx_state_copy_game(static_cast<Game*>(state), static_cast<const Game*>(staged));
        else std::memcpy(state, staged, s->native);
    }
    std::free(staged);
    return success;
}
