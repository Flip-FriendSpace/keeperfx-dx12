// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
// Owned custom-lens INDEX8 diagnostics, independent of KFXC scene captures.
#pragma once
#include "kfx/renderer/IndexedWorldFallback.h"
#include <algorithm>
#include <limits>
#include <span>
#include <stdexcept>

namespace kfx::render {
inline constexpr uint32_t IndexedWorldFallbackMagic = 0x4958464b; // KFXI
inline constexpr uint32_t IndexedWorldFallbackVersion = 1;
inline constexpr size_t IndexedWorldFallbackHeaderBytes = 72;
inline constexpr size_t IndexedWorldFallbackPaletteBytes = 1024;
inline constexpr uint64_t IndexedWorldFallbackMaxRgbaBytes = 64ull * 1024 * 1024;
inline constexpr size_t IndexedWorldFallbackMaxSnapshotBytes =
    IndexedWorldFallbackHeaderBytes + IndexedWorldFallbackPaletteBytes + IndexedWorldFallbackMaxRgbaBytes / 4;

namespace indexed_fallback_codec_detail {
inline bool Rectangle(uint32_t native_width, uint32_t native_height,
    uint32_t x, uint32_t y, uint32_t width, uint32_t height) noexcept
{
    return native_width && native_height && native_width <= 16384 && native_height <= 16384
        && uint64_t(native_width) * native_height <= IndexedWorldFallbackMaxRgbaBytes / 4
        && width && height && uint64_t(x) + width <= native_width && uint64_t(y) + height <= native_height;
}
inline void Require(bool valid, const char* reason)
{
    if (!valid) throw std::invalid_argument(reason);
}
inline void Write(std::vector<uint8_t>& bytes, uint64_t value, unsigned length)
{
    for (unsigned i = 0; i < length; ++i) bytes.push_back(uint8_t(value >> (i * 8)));
}
struct Reader {
    std::span<const uint8_t> bytes;
    size_t position = 0;
    uint64_t Read(unsigned length)
    {
        Require(length <= bytes.size() - position, "Truncated indexed fallback header");
        uint64_t value = 0;
        for (unsigned i = 0; i < length; ++i) value |= uint64_t(bytes[position++]) << (i * 8);
        return value;
    }
};
inline bool Opaque(const std::array<uint8_t, 1024>& palette) noexcept
{
    for (size_t i = 3; i < palette.size(); i += 4) if (palette[i] != 255) return false;
    return true;
}
}

inline bool ValidateIndexedWorldFallbackFrame(const IndexedWorldFallbackFrame& frame) noexcept
{
    using namespace indexed_fallback_codec_detail;
    return frame.reason == IndexedWorldFallbackReason::CustomLens && frame.sequence != 0
        && frame.custom_draw_attempts_before != std::numeric_limits<uint64_t>::max()
        && frame.custom_draw_attempts_after == frame.custom_draw_attempts_before + 1
        && frame.renderer_state_restored
        && Rectangle(uint32_t(frame.native_width), uint32_t(frame.native_height), uint32_t(frame.x), uint32_t(frame.y),
            uint32_t(frame.width), uint32_t(frame.height))
        && frame.indices.size() == uint64_t(frame.width) * frame.height && Opaque(frame.display_palette_rgba);
}

// Header offsets: magic/version0/4, native W/H8/12, x/y/W/H16/20/24/28,
// reason/clear/restored flags/reserved32/33/34/35, palette/index lengths36/40,
// reserved flags44, sequence/before/after48/56/64. Palette then tight indices.
inline std::vector<uint8_t> EncodeIndexedWorldFallback(const IndexedWorldFallbackFrame& frame)
{
    using namespace indexed_fallback_codec_detail;
    Require(ValidateIndexedWorldFallbackFrame(frame), "Invalid indexed fallback frame");
    std::vector<uint8_t> bytes;
    bytes.reserve(IndexedWorldFallbackHeaderBytes + IndexedWorldFallbackPaletteBytes + frame.indices.size());
    Write(bytes, IndexedWorldFallbackMagic, 4); Write(bytes, IndexedWorldFallbackVersion, 4);
    for (int value : {frame.native_width, frame.native_height, frame.x, frame.y, frame.width, frame.height})
        Write(bytes, uint32_t(value), 4);
    Write(bytes, uint8_t(frame.reason), 1); Write(bytes, frame.destination_clear_index, 1);
    Write(bytes, 1, 1); Write(bytes, 0, 1); // Only restored-state bit0 is defined.
    Write(bytes, IndexedWorldFallbackPaletteBytes, 4); Write(bytes, frame.indices.size(), 4); Write(bytes, 0, 4);
    Write(bytes, frame.sequence, 8); Write(bytes, frame.custom_draw_attempts_before, 8); Write(bytes, frame.custom_draw_attempts_after, 8);
    bytes.insert(bytes.end(), frame.display_palette_rgba.begin(), frame.display_palette_rgba.end());
    bytes.insert(bytes.end(), frame.indices.begin(), frame.indices.end());
    return bytes;
}

inline IndexedWorldFallbackFrame DecodeIndexedWorldFallback(std::span<const uint8_t> bytes)
{
    using namespace indexed_fallback_codec_detail;
    Require(bytes.size() >= IndexedWorldFallbackHeaderBytes + IndexedWorldFallbackPaletteBytes
        && bytes.size() <= IndexedWorldFallbackMaxSnapshotBytes, "Invalid indexed fallback file size");
    Reader reader{bytes};
    Require(reader.Read(4) == IndexedWorldFallbackMagic && reader.Read(4) == IndexedWorldFallbackVersion,
        "Unknown indexed fallback magic or version");
    const auto native_width = uint32_t(reader.Read(4)), native_height = uint32_t(reader.Read(4));
    const auto x = uint32_t(reader.Read(4)), y = uint32_t(reader.Read(4));
    const auto width = uint32_t(reader.Read(4)), height = uint32_t(reader.Read(4));
    const auto reason = uint8_t(reader.Read(1)), clear = uint8_t(reader.Read(1));
    const auto flags = uint8_t(reader.Read(1)), reserved = uint8_t(reader.Read(1));
    const auto palette_bytes = uint32_t(reader.Read(4)), index_bytes = uint32_t(reader.Read(4));
    const auto reserved_flags = uint32_t(reader.Read(4));
    const auto sequence = reader.Read(8), before = reader.Read(8), after = reader.Read(8);
    Require(Rectangle(native_width, native_height, x, y, width, height), "Invalid indexed fallback rectangle");
    Require(reason == uint8_t(IndexedWorldFallbackReason::CustomLens) && flags == 1 && reserved == 0 && reserved_flags == 0,
        "Unknown indexed fallback reason or flags");
    Require(sequence != 0 && before != std::numeric_limits<uint64_t>::max() && after == before + 1,
        "Invalid indexed fallback sequence or attempt counters");
    Require(palette_bytes == IndexedWorldFallbackPaletteBytes && index_bytes == uint64_t(width) * height
        && bytes.size() == IndexedWorldFallbackHeaderBytes + IndexedWorldFallbackPaletteBytes + index_bytes,
        "Invalid indexed fallback payload lengths or trailing bytes");
    IndexedWorldFallbackFrame frame;
    frame.native_width = int(native_width); frame.native_height = int(native_height);
    frame.x = int(x); frame.y = int(y); frame.width = int(width); frame.height = int(height);
    frame.reason = IndexedWorldFallbackReason(reason); frame.destination_clear_index = clear;
    frame.sequence = sequence; frame.custom_draw_attempts_before = before; frame.custom_draw_attempts_after = after;
    frame.renderer_state_restored = true;
    std::copy_n(bytes.begin() + IndexedWorldFallbackHeaderBytes, IndexedWorldFallbackPaletteBytes, frame.display_palette_rgba.begin());
    Require(Opaque(frame.display_palette_rgba), "Nonopaque indexed fallback display palette");
    // The first output allocation occurs only after every header, length and
    // palette check. No untrusted length reaches vector allocation earlier.
    frame.indices.assign(bytes.begin() + IndexedWorldFallbackHeaderBytes + IndexedWorldFallbackPaletteBytes, bytes.end());
    return frame;
}
}
