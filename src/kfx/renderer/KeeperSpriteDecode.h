// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
// Bounded normal keeper-sprite RLE decoding, independent of backend resources.
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace kfx::render {
enum class KeeperSpriteDecodeStatus {
    Success, InvalidDimensions, InvalidTarget, OverlappingStorage,
    TruncatedCommand, TruncatedRun,
};
struct KeeperSpriteDecodeResult {
    KeeperSpriteDecodeStatus status;
    size_t bytes_consumed;
    explicit operator bool() const noexcept { return status == KeeperSpriteDecodeStatus::Success; }
};
struct KeeperSpriteRGTarget {
    std::span<uint8_t> bytes;
    uint32_t width, height;
    size_t row_stride;
};

// Commands are signed bytes: negative skips, positive literal runs, zero ends
// one row (including an empty/short row). Literal index zero remains opaque.
// Preserve the normal renderer's clipped over-wide runs, consuming their whole
// payload. Trailing allocation bytes are not part of the image and are ignored.
// Input must name initialized readable bytes. Source and target must not alias.
// Validate first: failure leaves target untouched; success initializes every
// declared target row, including right/bottom padding and row-stride padding.
// Allocation-free, noexcept, and safe to use before publishing an owned image.
inline KeeperSpriteDecodeResult DecodeKeeperSpriteRG(KeeperSpriteRGTarget target,
    std::span<const uint8_t> source, uint32_t width, uint32_t height) noexcept
{
    using Status = KeeperSpriteDecodeStatus;
    if (!width || !height || width > target.width || height > target.height)
        return {Status::InvalidDimensions, 0};
    if (!target.bytes.data() || !target.row_stride || target.width > target.row_stride / 2
        || target.height > target.bytes.size() / target.row_stride)
        return {Status::InvalidTarget, 0};
    const size_t target_bytes = target.row_stride * target.height;
    if (!source.empty()) {
        const auto dst = reinterpret_cast<uintptr_t>(target.bytes.data());
        const auto src = reinterpret_cast<uintptr_t>(source.data());
        if ((dst <= src && src - dst < target_bytes)
            || (src < dst && dst - src < source.size()))
            return {Status::OverlappingStorage, 0};
    }
    size_t pos = 0;
    for (uint32_t y = 0; y < height; ++y) {
        for (;;) {
            if (pos == source.size()) return {Status::TruncatedCommand, pos};
            const unsigned command = source[pos++];
            if (!command) break;
            if (command < 128) {
                if (command > source.size() - pos) return {Status::TruncatedRun, pos};
                pos += command;
            }
        }
    }
    const size_t consumed = pos;
    std::memset(target.bytes.data(), 0, target_bytes);
    pos = 0;
    for (uint32_t y = 0; y < height; ++y) {
        uint8_t* const row = target.bytes.data() + size_t(y) * target.row_stride;
        uint32_t x = 0;
        for (;;) {
            const unsigned command = source[pos++];
            if (!command) break;
            const unsigned count = command < 128 ? command : 256 - command;
            const unsigned visible = std::min(count, width - x);
            if (command < 128) {
                for (unsigned i = 0; i < visible; ++i) {
                    row[size_t(x + i) * 2] = source[pos + i];
                    row[size_t(x + i) * 2 + 1] = 255;
                }
                pos += count;
            }
            x += visible; // Saturate at logical width; no overflow on long rows.
        }
    }
    return {Status::Success, consumed};
}
}
