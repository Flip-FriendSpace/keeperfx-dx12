// SPDX-License-Identifier: GPL-2.0-or-later
// Optional INDEX8 terrain replacements authenticated against native pixels.
#pragma once
#include "TileSource.h"
#include "TerrainTextureResolution.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <span>
#include <vector>

namespace kfx::render {
class HdTerrainPack {
public:
    static constexpr size_t LegacyMaxFileBytes = 8u * 1024u * 1024u;
    static constexpr size_t MaxFileBytes = 80u * 1024u * 1024u;
    static constexpr uint32_t StaticATiles = 544, StaticBTiles = 544;
    static constexpr uint32_t StaticTileCount = StaticATiles + StaticBTiles;
    static constexpr size_t PhysicalBankBytes = size_t(StaticTileCount) * 1024;
    static constexpr uint32_t InvalidTile = UINT32_MAX;
    struct Record {
        uint32_t tile = 0, dimension = 0;
        std::array<uint8_t,1024> original{};
        std::vector<uint8_t> pixels;
        struct Match { uint64_t serial = 0; size_t offset = 0; bool value = false; };
        mutable std::array<Match,TileSourceVariationCount> matches{};
    };

    // KFHT v1 retains its logical-ID/64-record/8-MiB contract. Version 2 uses
    // physical A/B source IDs so animated aliases select their actual frame.
    // Both versions use LE header words, native32 reference and tight HD pixels.
    bool Parse(std::span<const uint8_t> bytes) {
        if (bytes.size() < 16 || bytes.size() > MaxFileBytes) return false;
        size_t at = 0;
        const auto word = [&]() {
            uint32_t n = 0;
            for (unsigned i = 0; i < 4; ++i) n |= uint32_t(bytes[at++]) << (8*i);
            return n;
        };
        if (word() != 0x5448464b) return false;
        const uint32_t version = word(), count = word(), reserved = word();
        if ((version != 1 && version != 2) || !count || reserved
            || count > (version == 1 ? 64u : StaticTileCount)
            || (version == 1 && bytes.size() > LegacyMaxFileBytes)) return false;
        std::vector<Record> next;
        next.reserve(count);
        std::array<int16_t,TileSourceTileCount> lookup;
        lookup.fill(-1);
        for (uint32_t i = 0; i < count; ++i) {
            if (bytes.size() - at < 12) return false;
            Record r;
            r.tile = word(); r.dimension = word();
            if (word() || r.tile >= TileSourceTileCount
                || (version == 2 && !PhysicalTile(r.tile))
                || !TerrainTextureDimension(r.dimension) || r.dimension == 32
                || lookup[r.tile] >= 0) return false;
            const size_t length = size_t(r.dimension) * r.dimension;
            if (bytes.size() - at < r.original.size() + length) return false;
            std::copy_n(bytes.data() + at, r.original.size(), r.original.data());
            at += r.original.size();
            r.pixels.assign(bytes.data() + at, bytes.data() + at + length);
            at += length;
            lookup[r.tile] = static_cast<int16_t>(next.size());
            next.push_back(std::move(r));
        }
        if (at != bytes.size()) return false;
        records_ = std::move(next); lookup_ = lookup; version_ = version;
        return true;
    }
    size_t Count() const noexcept { return records_.size(); }
    uint32_t Version() const noexcept { return version_; }
    static constexpr bool PhysicalTile(uint32_t tile) noexcept {
        return tile < StaticATiles || (tile >= 1000 && tile < 1000 + StaticBTiles);
    }
    static uint32_t PhysicalSourceTile(const TileSourceView& source) noexcept {
        if (source.row_stride != 256) return InvalidTile;
        const size_t offset = source.bank_offset % PhysicalBankBytes;
        const size_t x = offset % 256, y = offset / 256;
        if (x % 32 || y % 32) return InvalidTile;
        const uint32_t index = uint32_t(y / 32 * 8 + x / 32);
        return index < StaticATiles ? index : 1000 + index - StaticATiles;
    }
    const Record* Match(uint32_t tile,const TileSourceView& source) const noexcept {
        if (records_.empty() || tile >= TileSourceTileCount
            || source.width != 32 || source.height != 32 || source.row_stride < 32
            || source.variation >= TileSourceVariationCount || !source.mutation_serial
            || source.mutation_serial == UINT64_MAX
            || source.bytes.size() < uint64_t(source.row_stride)*31 + 32) return nullptr;
        const uint32_t id = version_ == 2 ? PhysicalSourceTile(source) : tile;
        if (id >= TileSourceTileCount || lookup_[id] < 0) return nullptr;
        const auto& r = records_[size_t(lookup_[id])];
        auto& cached = r.matches[source.variation];
        if (cached.serial != source.mutation_serial || cached.offset != source.bank_offset) {
            cached = {source.mutation_serial,source.bank_offset,true};
            for (size_t y = 0; y < 32 && cached.value; ++y)
                cached.value = std::memcmp(source.bytes.data() + y*source.row_stride,
                    r.original.data() + y*32,32) == 0;
        }
        return cached.value ? &r : nullptr;
    }
private:
    std::vector<Record> records_;
    std::array<int16_t,TileSourceTileCount> lookup_{};
    uint32_t version_ = 0;
};
// Loaded once per process on the producer; native banks and gameplay stay intact.
const HdTerrainPack::Record* ResolveHdTerrain(uint32_t tile,const TileSourceView& source) noexcept;
}
