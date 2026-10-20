// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX renderer performance changes: 2026-10-19.
#pragma once
#include "OwnedRenderAssets.h"
#include <array>

namespace kfx::render {
// Classification belongs to the immutable source, not each draw. Keep only
// metadata so this cache cannot pin decoded images against the asset budget.
class KeeperSpriteDarkenCache {
    struct Entry {
        OwnedAssetKey key{};
        uint32_t width=0,height=0;
        bool darkens=false;
    };
    std::array<Entry,1024> entries_{};
public:
    bool Darkens(const OwnedAssetLease& source,uint32_t width,uint32_t height) noexcept {
        const auto desc=source.Desc();
        if(!source.Valid() || (desc.format!=OwnedAssetFormat::RGBA8 && desc.format!=OwnedAssetFormat::RG8) || !width || !height
            || width>desc.width || height>desc.height) return false;
        const auto key=source.Key();
        auto& entry=entries_[key.logical_id%entries_.size()];
        if(entry.key==key && entry.width==width && entry.height==height) return entry.darkens;
        const auto pixels=source.Bytes();
        const size_t stride=desc.format==OwnedAssetFormat::RG8?2:4;
        bool darkens=false;
        for(uint32_t row=0;!darkens && row<height;++row) for(uint32_t col=0;col<width;++col) {
            const size_t at=(size_t(row)*desc.width+col)*stride;
            if(pixels[at+1] && pixels[at]>=50 && pixels[at]<=56) { darkens=true; break; }
        }
        entry={key,width,height,darkens};
        return darkens;
    }
};
}
