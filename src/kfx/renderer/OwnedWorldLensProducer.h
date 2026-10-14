// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#pragma once
#include "OwnedRenderFrame.h"
#include "PaletteIndexTable.h"
#include <array>
#include <vector>

struct IRWorldLensCmd;
namespace kfx::render {
struct OwnedWorldLensInputs {
    uint64_t epoch=0;
    uint32_t native_width=0,native_height=0;
    const IRWorldLensCmd* evaluated=nullptr;
    OwnedAssetLease display_palette,fade;
    std::span<const uint8_t> base_palette_rgb6;
    std::array<float,4> clear_rgba{0,0,0,1};
};
struct OwnedWorldLensResult {
    OwnedAssetStatus status=OwnedAssetStatus::InvalidSource;
    OwnedAssetPressure pressure=OwnedAssetPressure::None;
    OwnedFrameLens lens;
    bool ready=false;
    explicit operator bool() const noexcept {return ready;}
};
// Synchronous producer-thread consumption of one already evaluated command.
// Never calls LensManager or replays callbacks. Copies are catalog-owned; raw
// wrapping IR version counters are not content identities. Domain100 is reserved
// by the normal producer for this bank's five auxiliary IDs. A shape change
// advances the entire bank generation and clears every old cache entry.
// The caller advances the shared catalog epoch before supplying a newer epoch.
// Two fixed262144-byte RGB6 work tables are separate from catalog payload caps.
class OwnedWorldLensProducer {
public:
    explicit OwnedWorldLensProducer(OwnedRenderAssets&,uint64_t domain=100);
    OwnedWorldLensResult Update(const OwnedWorldLensInputs&) noexcept;
    // Release prior-level auxiliary leases even when the new level has no lens.
    void ResetEpoch(uint64_t epoch) noexcept;
private:
    OwnedRenderAssets& assets_;
    uint64_t domain_,epoch_=0,generation_=1;
    std::array<OwnedAssetLease,5> current_;
    std::array<uint64_t,5> revisions_{};
    std::array<OwnedAssetDesc,5> shapes_{};
    std::array<bool,5> have_shape_{};
    std::vector<uint8_t> base_,table_,remap_bytes_;
    std::array<uint8_t,768> base_palette_{};
    std::array<uint8_t,1024> display_palette_{};
    std::array<uint32_t,256> palette_keys_{};
    bool have_base_=false,have_display_=false;
    OwnedAssetResult Publish(size_t slot,OwnedAssetDesc,std::span<const uint8_t>) noexcept;
};
}
