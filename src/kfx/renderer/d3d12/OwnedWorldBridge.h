// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
// KeeperFX renderer capacity changes: 2026-10-20.
#pragma once
#include "kfx/renderer/OwnedRenderFrame.h"
#include "RuntimeWorld.h"
#include <memory>

namespace kfx::render::dx12 {
class Device;
enum class OwnedWorldPrepareStatus : uint8_t { Ready, WouldBlock, PendingPacket, Unsupported, InvalidInput };
struct OwnedWorldBridgeLimits {
    // At most 2048 distinct (owned key, usage) pairs per packet; the default is
    // also the maximum. A 4K Armageddon crowd needs more than 512. Slots can change
    // kind; eight runtime kinds bound lifetime tombstones to 8*2048=16384 per epoch.
    // Same-lineage revisions reuse their slot; distinct identities use empty
    // slots first, then deterministic clock eviction of unselected entries.
    uint32_t mapping_slots = 2048;
    // Exclusive namespace for this bridge on its Device. Do not publish other
    // runtime assets into this range, or create a second bridge using it.
    uint64_t first_logical_id = 0x100000000ull;
};
struct OwnedWorldBridgeStats {
    uint64_t publications=0, cache_hits=0, publication_blocks=0;
    uint32_t occupied_slots=0, selected_slots=0;
    // Most distinct sources one packet gathered; equal to the limit after overflow.
    uint32_t source_high_water=0, source_limit=0;
    bool pending=false, prepared=false;
};
// Serial render-thread adapter. Device must outlive it. This deliberately admits
// world fragments separately from complete supported frames. PiP and
// other unsupported nonempty phases are rejected before any publication.
class OwnedWorldBridge {
public:
    explicit OwnedWorldBridge(Device&, OwnedWorldBridgeLimits = {});
    ~OwnedWorldBridge();
    OwnedWorldBridge(const OwnedWorldBridge&)=delete;
    OwnedWorldBridge& operator=(const OwnedWorldBridge&)=delete;
    OwnedWorldPrepareStatus PrepareWorldFragment(const OwnedFrameLease&);
    const RuntimeWorldFrame* PreparedWorldFragment() const noexcept;
    RuntimeSubmitResult TrySubmitPreparedWorldFragment(bool diagnostic_readback=false);
    OwnedWorldPrepareStatus PrepareFrame(const OwnedFrameLease&);
    const RuntimeWorldFrame* PreparedFrame() const noexcept;
    RuntimeSubmitResult TrySubmitPreparedFrame(bool diagnostic_readback=false);
    void Abandon() noexcept;
    void TrimUnselected();
    OwnedWorldBridgeStats Stats() const noexcept;
private:
    OwnedWorldPrepareStatus Prepare(const OwnedFrameLease&,bool full_frame);
    RuntimeSubmitResult Submit(bool diagnostic_readback,bool full_frame);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
