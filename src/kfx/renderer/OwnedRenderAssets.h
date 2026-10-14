// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace kfx::render {
enum class OwnedAssetFormat { R8, RG8, RGBA8, BGRA8, R16LE, RG16SIntLE };
// Domain and generation are producer-owned bank identity, not raw wrapping engine counters.
// Epoch, generation and revision are nonzero. Shape/format stay fixed within a generation.
struct OwnedAssetKey {
    uint64_t epoch = 0, domain = 0, logical_id = 0, generation = 0, content_revision = 0;
    bool operator==(const OwnedAssetKey&) const = default;
};
struct OwnedAssetDesc {
    OwnedAssetFormat format = OwnedAssetFormat::R8;
    uint32_t width = 0, height = 0;
    bool operator==(const OwnedAssetDesc&) const = default;
};
struct OwnedAssetView {
    OwnedAssetDesc desc;
    std::span<const uint8_t> bytes;
    size_t row_stride = 0;
};
namespace owned_detail { struct State; }
// Historical leases remain valid across publication, bank/epoch reset and catalog
// destruction. Bytes are canonical tight rows and never refer to engine storage.
class OwnedAssetLease {
public:
    OwnedAssetLease() noexcept = default;
    bool Valid() const noexcept { return bool(state_); }
    OwnedAssetKey Key() const noexcept;
    OwnedAssetDesc Desc() const noexcept;
    std::span<const uint8_t> Bytes() const noexcept;
    size_t RowStride() const noexcept;
private:
    std::shared_ptr<const owned_detail::State> state_;
    explicit OwnedAssetLease(std::shared_ptr<const owned_detail::State> state) noexcept;
    friend class OwnedRenderAssets;
};
enum class OwnedAssetStatus {
    Published, Reused, Rehydrated, WouldBlock, InvalidSource, StaleKey,
    WrongEpoch, GenerationRequired, ContentConflict, AllocationFailed,
};
enum class OwnedAssetPressure { None, Bytes, Versions, Contents, Identities, Banks };
struct OwnedAssetResult {
    OwnedAssetStatus status = OwnedAssetStatus::InvalidSource;
    OwnedAssetPressure pressure = OwnedAssetPressure::None;
    OwnedAssetLease lease;
    explicit operator bool() const noexcept { return lease.Valid(); }
};
struct OwnedAssetLimits {
    size_t bytes = 128u * 1024u * 1024u;
    size_t identities = 16384, banks = 128, versions = 32768, contents = 32768;
};
struct OwnedAssetStats {
    size_t payload_bytes = 0, temporary_bytes = 0, peak_owned_bytes = 0;
    size_t live_versions = 0, live_contents = 0, identities = 0, banks = 0;
    size_t version_records = 0, table_storage_bytes = 0;
    uint64_t publications = 0, reuses = 0, rehydrations = 0, evictions = 0;
};
// Mutate on one producer thread. Only independent lease ownership/destruction
// crosses threads. Source views must remain stable for the duration of Publish.
// Payload + conversion scratch share the byte cap; fixed table storage is reported
// separately. General allocator/control-block overhead is not a payload byte.
class OwnedRenderAssets {
public:
    explicit OwnedRenderAssets(uint64_t epoch, OwnedAssetLimits limits = {});
    ~OwnedRenderAssets();
    OwnedRenderAssets(const OwnedRenderAssets&) = delete;
    OwnedRenderAssets& operator=(const OwnedRenderAssets&) = delete;
    OwnedAssetResult Publish(OwnedAssetKey key, OwnedAssetView source) noexcept;
    // Decode initialized encoded bytes, preserve opaque palette index zero, and
    // initialize the entire target including padding to RGBA(index,coverage,0,0).
    OwnedAssetResult PublishKeeperSprite(OwnedAssetKey key, std::span<const uint8_t> encoded,
        uint32_t width, uint32_t height, uint32_t target_width, uint32_t target_height) noexcept;
    // Normal UI INDEX8 semantics: zero is transparent; an encoded opaque zero
    // becomes index one, matching LbSpriteDecode. Conversion scratch is charged.
    OwnedAssetResult PublishUISprite(OwnedAssetKey key,std::span<const uint8_t> encoded,
        uint32_t width,uint32_t height) noexcept;
    // MSB-first packed font bitmap; expanded R8 uses 255 for covered pixels.
    // Temporary expansion and final payload are both charged to the byte cap.
    OwnedAssetResult PublishBitMask(OwnedAssetKey key,std::span<const uint8_t> packed,
        size_t row_stride,uint32_t width,uint32_t height) noexcept;
    // Empty means absent. Callers must reject an absent required frame asset.
    OwnedAssetLease Find(OwnedAssetKey key) const noexcept;
    // Strictly increasing reset watermarks; held historical payloads stay charged.
    bool ResetBank(uint64_t domain, uint64_t new_generation) noexcept;
    bool AdvanceEpoch(uint64_t new_epoch) noexcept;
    // Normal collection prunes expired versions. Pressure also evicts cache-only
    // current payloads; their latest identity/shape watermarks remain bounded.
    void Collect(bool pressure = false) noexcept;
    OwnedAssetStats Stats() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    OwnedAssetResult PublishImpl(OwnedAssetKey key, OwnedAssetView source, bool expand_rg) noexcept;
};
// Eviction retains the latest key/shape watermark. Restoring that same evicted key
// trusts the producer's immutable source identity and is reported as Rehydrated.
// Exact bytes are checked whenever a lease still owns that key. No byte-conflict
// authentication is claimed after the final owner releases the previous payload.
}
