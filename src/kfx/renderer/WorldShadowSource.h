// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
// Normal keeper-shadow geometry and synchronous source resolution.
#pragma once
#include <cstdint>
#include <span>

struct BucketKindCreatureShadow;

namespace kfx::render {
inline constexpr int WorldShadowDecodeDimension = 256;

// Pointer-free geometry, shared by renderer backends. UVs address the complete
// padded keeper-sprite decode image, rather than its content dimensions.
struct WorldShadowGeometry {
    float x[4]{}, y[4]{}, u[4]{}, v[4]{};
    float z_ndc = 0;
    float darken = 0;
};

// Producer-thread borrowed view only. Consume/copy/decode bytes synchronously
// before any sprite-heap operation can dispose of the source. Neither this view
// nor its byte span may survive in a sealed owned frame.
struct WorldShadowSourceView {
    WorldShadowGeometry geometry;
    int32_t draw_idx = -1;
    int src_w = 0, src_h = 0;
    std::span<const uint8_t> bytes;
    // Zero means no owned publication identity is available. Legacy rendering
    // can still consume the bounded source; owned publication must reject it.
    uint64_t publication_stamp = 0;
};

enum class WorldShadowSourceStatus { Success, ResolutionFailed, ExtentUnavailable };

WorldShadowGeometry GetWorldShadowGeometry(const BucketKindCreatureShadow& shadow) noexcept;
WorldShadowSourceStatus ResolveWorldShadowSource(const BucketKindCreatureShadow& shadow,
    WorldShadowSourceView& source) noexcept;
}
