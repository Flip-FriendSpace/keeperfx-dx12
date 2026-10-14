// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#include "OwnedWorldSources.h"
#include "PaletteTransform.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace kfx::render {
OwnedWorldSources::OwnedWorldSources(OwnedRenderAssets& assets, uint64_t domain)
    : assets_(assets), domain_(domain)
{
    if (!domain || domain == UINT64_MAX) throw std::invalid_argument("Invalid owned world source domain");
}

OwnedAssetResult OwnedWorldSources::Publish(size_t slot, OwnedAssetDesc desc,
    std::span<const uint8_t> bytes) noexcept
{
    auto& previous = current_[slot];
    if (previous.Valid() && previous.Desc() == desc
        && std::equal(bytes.begin(), bytes.end(), previous.Bytes().begin(), previous.Bytes().end()))
        return {OwnedAssetStatus::Reused, OwnedAssetPressure::None, previous};
    if (revisions_[slot] >= UINT64_MAX - 1) return {};
    const OwnedAssetKey key{epoch_, domain_, slot + 1, 1, revisions_[slot] + 1};
    const size_t pixel_bytes = desc.format == OwnedAssetFormat::RGBA8 ? 4
        : desc.format == OwnedAssetFormat::R16LE ? 2 : 1;
    auto result = assets_.Publish(key, {desc, bytes, size_t(desc.width) * pixel_bytes});
    if (result) { previous = result.lease; revisions_[slot] = key.content_revision; }
    return result;
}

OwnedWorldSourceResult OwnedWorldSources::Update(const OwnedWorldSourceInputs& input) noexcept
{
    OwnedWorldSourceResult result;
    if (!input.epoch || input.epoch == UINT64_MAX || input.epoch < epoch_
        || input.display_palette_rgba.size() != 1024 || input.sprite_palette_rgb6.size() != 768
        || input.base_palette_rgb6.size() != 768 || input.fade.size() != 256 * 64
        || !input.lightmap_width || !input.lightmap_height
        || input.lightmap_width > 511 || input.lightmap_height > 511
        || input.lightmap.size() < uint64_t(input.lightmap_width) * input.lightmap_height) return result;
    for (size_t i = 3; i < 1024; i += 4)
        if (input.display_palette_rgba[i] != 255) return result;
    for (auto value : input.sprite_palette_rgb6) if (value > 63) return result;
    for (auto value : input.base_palette_rgb6) if (value > 63) return result;
    const auto shade = WorldShadeSettings(input.settings);
    for (float value : shade) if (!std::isfinite(value)) return result;
    if (input.epoch != epoch_) {
        epoch_ = input.epoch;
        current_ = {};
        revisions_ = {};
    }

    // Match snapshot_lightmap's edge replication, with explicit little-endian
    // storage for the asset contract. Nothing reads simulation data after this.
    for (uint32_t y = 0; y < 511; ++y) {
        const size_t row = size_t(std::min(y, input.lightmap_height - 1)) * input.lightmap_width;
        for (uint32_t x = 0; x < 511; ++x) {
            const uint16_t value = input.lightmap[row + std::min(x, input.lightmap_width - 1)];
            const size_t target = (size_t(y) * 511 + x) * 2;
            lightmap_bytes_[target] = uint8_t(value);
            lightmap_bytes_[target + 1] = uint8_t(value >> 8);
        }
    }
    std::array<uint8_t, 1024> sprite_palette;
    for (size_t i = 0; i < 256; ++i) {
        for (size_t channel = 0; channel < 3; ++channel)
            sprite_palette[i * 4 + channel] = uint8_t((input.sprite_palette_rgb6[i * 3 + channel] * 255) / 63);
        sprite_palette[i * 4 + 3] = 255;
    }
    const std::array<OwnedAssetDesc, 4> descriptions{{
        {OwnedAssetFormat::RGBA8,256,1}, {OwnedAssetFormat::R8,256,64},
        {OwnedAssetFormat::R16LE,511,511}, {OwnedAssetFormat::RGBA8,256,1}}};
    const std::array<std::span<const uint8_t>, 4> sources{
        input.display_palette_rgba, input.fade, lightmap_bytes_, sprite_palette};
    result.status = OwnedAssetStatus::Reused;
    for (size_t i = 0; i < sources.size(); ++i) {
        const auto published = Publish(i, descriptions[i], sources[i]);
        if (!published) {
            result.status = published.status; result.pressure = published.pressure;
            return result;
        }
        if (published.status != OwnedAssetStatus::Reused) result.status = published.status;
    }
    result.state.palette = current_[0]; result.state.fade = current_[1]; result.state.lightmap = current_[2];
    result.state.sprite_palette = current_[3];
    result.state.settings = input.settings; result.state.shade_settings = shade;
    std::copy(input.sprite_palette_rgb6.begin(), input.sprite_palette_rgb6.end(), result.state.sprite_palette_rgb6.begin());
    const auto transform = PaletteTransformFit(input.base_palette_rgb6.data(), input.sprite_palette_rgb6.data());
    for (size_t row = 0; row < 3; ++row)
        for (size_t column = 0; column < 3; ++column)
            result.state.palette_transform[row * 3 + column] = transform.m[row][column];
    return result;
}
}
