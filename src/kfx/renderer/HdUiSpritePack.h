// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX remaster changes: 2026-10-16.
#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>
#include <utility>
#include <set>

namespace kfx::render {
struct HdUiRecord {
    uint32_t index = 0;
    uint16_t logical_width = 0, logical_height = 0;
    uint16_t width = 0, height = 0;
    uint8_t scale = 1;
    uint64_t source_fingerprint = 0;
    std::vector<uint8_t> rle;
};
// UI RLE uses signed8 commands: positive literals, negative transparent skips,
// zero end-of-row. Zero palette indices in literals remain opaque data.
inline bool HdUiRleExtent(const uint8_t* p, size_t size, uint32_t w, uint32_t h, size_t& extent)
{
    extent = 0;
    if (!p || !w || !h || w > 1024 || h > 1024) return false;
    size_t pos = 0;
    for (uint32_t y = 0; y < h; ++y) {
        uint32_t x = 0;
        for (;;) {
            if (pos >= size) return false;
            const int value = p[pos++];
            const int cmd = value < 128 ? value : value - 256;
            if (!cmd) break;
            const uint32_t run = uint32_t(cmd < 0 ? -cmd : cmd);
            if (run > w - x) return false;
            x += run;
            if (cmd > 0) {
                if (run > size - pos) return false;
                pos += run;
            }
        }
    }
    extent = pos;
    return true;
}
inline uint64_t HdUiFingerprint(const uint8_t* p, size_t size)
{
    uint64_t hash = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < size; ++i) { hash ^= p[i]; hash *= UINT64_C(1099511628211); }
    return hash;
}
// Parse transactionally. Callers validate every record against the loaded
// native sheet before publishing any record; failed loads retain all native art.
inline bool ParseHdUiPack(const std::vector<uint8_t>& bytes, std::vector<HdUiRecord>& output)
{
    constexpr size_t MaxBytes = 64u * 1024u * 1024u;
    if (bytes.size() < 12 || bytes.size() > MaxBytes) return false;
    auto u16 = [&](size_t p) { return uint16_t(uint16_t(bytes[p]) | uint16_t(bytes[p+1]) << 8); };
    auto u32 = [&](size_t p) { return uint32_t(u16(p)) | uint32_t(u16(p+2)) << 16; };
    auto u64 = [&](size_t p) { return uint64_t(u32(p)) | uint64_t(u32(p+4)) << 32; };
    if (bytes[0]!='K' || bytes[1]!='F' || bytes[2]!='U' || bytes[3]!='I' || u16(4)!=1 || u16(6)!=32) return false;
    const uint32_t count = u32(8);
    if (count > 4096 || size_t(count) > (bytes.size()-12)/32) return false;
    std::vector<HdUiRecord> parsed;
    std::set<uint32_t> indices;
    parsed.reserve(count);
    size_t pos = 12;
    for (uint32_t i = 0; i < count; ++i) {
        if (bytes.size()-pos < 32) return false;
        HdUiRecord r;
        r.index=u32(pos);r.logical_width=u16(pos+4);r.logical_height=u16(pos+6);
        r.width=u16(pos+8);r.height=u16(pos+10);r.scale=bytes[pos+12];
        r.source_fingerprint=u64(pos+16);
        const uint32_t size=u32(pos+24);
        if (bytes[pos+13] || u16(pos+14) || u32(pos+28) || !indices.insert(r.index).second) return false;
        if (!(r.scale==1 || r.scale==2 || r.scale==4 || r.scale==8) ||
            !r.logical_width || !r.logical_height || r.width>1024 || r.height>1024 ||
            uint32_t(r.logical_width)*r.scale!=r.width || uint32_t(r.logical_height)*r.scale!=r.height) return false;
        pos += 32;
        if (size > bytes.size()-pos) return false;
        size_t used = 0;
        if (!HdUiRleExtent(bytes.data()+pos,size,r.width,r.height,used) || used!=size) return false;
        r.rle.assign(bytes.begin()+pos,bytes.begin()+pos+size);
        parsed.push_back(std::move(r));pos += size;
    }
    if (pos != bytes.size()) return false;
    output = std::move(parsed);
    return true;
}
} // namespace kfx::render
