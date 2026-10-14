// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#include "OwnedRenderAssets.h"
#include "KeeperSpriteDecode.h"
#include <atomic>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>
#include <vector>

namespace kfx::render {
namespace {
size_t PixelBytes(OwnedAssetFormat format) noexcept
{
    switch (format) {
    case OwnedAssetFormat::R8: return 1;
    case OwnedAssetFormat::RG8: case OwnedAssetFormat::R16LE: return 2;
    case OwnedAssetFormat::RGBA8: case OwnedAssetFormat::BGRA8:
    case OwnedAssetFormat::RG16SIntLE: return 4;
    }
    return 0;
}
bool Shape(OwnedAssetDesc desc, size_t& row, size_t& bytes) noexcept
{
    const size_t pixel = PixelBytes(desc.format);
    if (!pixel || !desc.width || !desc.height || desc.width > SIZE_MAX / pixel) return false;
    row = size_t(desc.width) * pixel;
    if (desc.height > SIZE_MAX / row) return false;
    bytes = row * desc.height;
    return true;
}
uint64_t Mix(uint64_t n) noexcept
{
    n ^= n >> 30; n *= UINT64_C(0xbf58476d1ce4e5b9);
    n ^= n >> 27; n *= UINT64_C(0x94d049bb133111eb);
    return n ^ (n >> 31);
}
size_t Hash(uint64_t n) noexcept { return size_t(Mix(n)); }
size_t Hash(OwnedAssetKey key) noexcept
{
    return size_t(Mix(key.epoch) ^ Mix(key.domain + 11) ^ Mix(key.logical_id + 23)
        ^ Mix(key.generation + 37) ^ Mix(key.content_revision + 53));
}
struct Identity {
    uint64_t domain, id;
    bool operator==(const Identity&) const = default;
};
size_t Hash(Identity key) noexcept { return size_t(Mix(key.domain) ^ Mix(key.id + 17)); }
// Fixed storage, linear probing. Erasure leaves a tombstone; insertion reuses it.
// Sweeps/reset are bounded maintenance, never dynamic hash-node allocation.
template<class Key, class Value> struct Table {
    struct Slot { unsigned char mark = 0; Key key{}; Value value{}; };
    std::vector<Slot> slots;
    size_t count = 0, limit;
    explicit Table(size_t maximum) : limit(maximum)
    {
        if (!maximum || maximum > SIZE_MAX / 4) throw std::invalid_argument("Invalid catalog capacity");
        size_t n = 2;
        while (n / 2 < maximum) {
            if (n > SIZE_MAX / 2) throw std::invalid_argument("Catalog capacity overflow");
            n *= 2;
        }
        slots.resize(n);
    }
    size_t Locate(Key key) const noexcept
    {
        size_t at = Hash(key) & (slots.size() - 1), tombstone = slots.size();
        for (size_t probes = 0; probes < slots.size(); ++probes) {
            const auto& slot = slots[at];
            if (!slot.mark) return tombstone == slots.size() ? at : tombstone;
            if (slot.mark == 1 && slot.key == key) return at;
            if (slot.mark == 2 && tombstone == slots.size()) tombstone = at;
            at = (at + 1) & (slots.size() - 1);
        }
        return tombstone;
    }
    Value* Find(Key key) noexcept {
        const size_t at = Locate(key);
        return at < slots.size() && slots[at].mark == 1 ? &slots[at].value : nullptr;
    }
    const Value* Find(Key key) const noexcept { return const_cast<Table*>(this)->Find(key); }
    void Put(Key key, Value value) noexcept {
        auto& slot = slots[Locate(key)];
        if (slot.mark != 1) ++count;
        slot.key = key; slot.value = std::move(value); slot.mark = 1;
    }
    void Erase(Slot& slot) noexcept { slot.value = {}; slot.mark = 2; --count; }
    void Clear() noexcept { for (auto& slot : slots) slot = {}; count = 0; }
};
}
namespace owned_detail {
struct Accounting {
    std::atomic<size_t> bytes{0}, temporary{0}, peak{0}, versions{0}, contents{0};
    void Peak() noexcept {
        const size_t actual = bytes.load() + temporary.load();
        size_t previous = peak.load();
        while (actual > previous && !peak.compare_exchange_weak(previous, actual)) {}
    }
};
struct Blob {
    std::shared_ptr<Accounting> accounting;
    std::unique_ptr<uint8_t[]> data;
    size_t size;
    Blob(std::shared_ptr<Accounting> owner, size_t bytes)
        : accounting(std::move(owner)), data(new uint8_t[bytes]), size(bytes)
    {
        accounting->bytes.fetch_add(size); accounting->contents.fetch_add(1); accounting->Peak();
    }
    ~Blob() { accounting->bytes.fetch_sub(size); accounting->contents.fetch_sub(1); }
};
struct State {
    OwnedAssetKey key;
    OwnedAssetDesc desc;
    std::shared_ptr<const Blob> blob;
    State(OwnedAssetKey k, OwnedAssetDesc d, std::shared_ptr<const Blob> b)
        : key(k), desc(d), blob(std::move(b)) { blob->accounting->versions.fetch_add(1); }
    ~State() { blob->accounting->versions.fetch_sub(1); }
};
struct Scratch {
    std::shared_ptr<Accounting> accounting;
    std::unique_ptr<uint8_t[]> data;
    size_t size;
    Scratch(std::shared_ptr<Accounting> owner, size_t bytes)
        : accounting(std::move(owner)), data(new uint8_t[bytes]), size(bytes)
    { accounting->temporary.fetch_add(size); accounting->Peak(); }
    ~Scratch() { accounting->temporary.fetch_sub(size); }
};
}
OwnedAssetLease::OwnedAssetLease(std::shared_ptr<const owned_detail::State> state) noexcept : state_(std::move(state)) {}
OwnedAssetKey OwnedAssetLease::Key() const noexcept { return state_ ? state_->key : OwnedAssetKey{}; }
OwnedAssetDesc OwnedAssetLease::Desc() const noexcept { return state_ ? state_->desc : OwnedAssetDesc{}; }
std::span<const uint8_t> OwnedAssetLease::Bytes() const noexcept
{ return state_ ? std::span<const uint8_t>(state_->blob->data.get(), state_->blob->size) : std::span<const uint8_t>{}; }
size_t OwnedAssetLease::RowStride() const noexcept { return size_t(Desc().width) * PixelBytes(Desc().format); }
struct OwnedRenderAssets::Impl {
    struct Current {
        OwnedAssetKey key;
        OwnedAssetDesc desc;
        std::shared_ptr<const owned_detail::State> cached;
    };
    uint64_t epoch;
    OwnedAssetLimits limits;
    std::shared_ptr<owned_detail::Accounting> accounting = std::make_shared<owned_detail::Accounting>();
    Table<uint64_t, uint64_t> banks;
    Table<Identity, Current> identities;
    Table<OwnedAssetKey, std::weak_ptr<const owned_detail::State>> versions;
    uint64_t publications = 0, reuses = 0, rehydrations = 0, evictions = 0;
    Impl(uint64_t e, OwnedAssetLimits l) : epoch(e), limits(l), banks(l.banks), identities(l.identities), versions(l.versions)
    { if (!epoch || !limits.bytes || !limits.contents) throw std::invalid_argument("Invalid catalog limits/epoch"); }
    bool Fits(size_t additional) const noexcept {
        const size_t bytes = accounting->bytes.load(), temporary = accounting->temporary.load();
        return bytes <= limits.bytes && temporary <= limits.bytes - bytes
            && additional <= limits.bytes - bytes - temporary;
    }
};
OwnedRenderAssets::OwnedRenderAssets(uint64_t epoch, OwnedAssetLimits limits) : impl_(std::make_unique<Impl>(epoch, limits)) {}
OwnedRenderAssets::~OwnedRenderAssets() = default;
OwnedAssetLease OwnedRenderAssets::Find(OwnedAssetKey key) const noexcept
{
    const auto* found = impl_->versions.Find(key);
    return OwnedAssetLease(found ? found->lock() : nullptr);
}
void OwnedRenderAssets::Collect(bool pressure) noexcept
{
    if (pressure) for (auto& slot : impl_->identities.slots) {
        if (slot.mark == 1 && slot.value.cached && slot.value.cached.use_count() == 1) {
            slot.value.cached.reset(); ++impl_->evictions;
        }
    }
    for (auto& slot : impl_->versions.slots)
        if (slot.mark == 1 && slot.value.expired()) impl_->versions.Erase(slot);
}
bool OwnedRenderAssets::AdvanceEpoch(uint64_t epoch) noexcept
{
    if (epoch <= impl_->epoch) return false;
    impl_->identities.Clear(); impl_->banks.Clear(); impl_->epoch = epoch;
    Collect(); return true;
}
bool OwnedRenderAssets::ResetBank(uint64_t domain, uint64_t generation) noexcept
{
    if (!generation) return false;
    const auto* old = impl_->banks.Find(domain);
    if (old && generation <= *old) return false;
    if (!old && impl_->banks.count == impl_->limits.banks) return false;
    for (auto& slot : impl_->identities.slots)
        if (slot.mark == 1 && slot.key.domain == domain) impl_->identities.Erase(slot);
    impl_->banks.Put(domain, generation); Collect(); return true;
}
OwnedAssetStats OwnedRenderAssets::Stats() const noexcept
{
    const auto& a = *impl_->accounting;
    return {a.bytes.load(), a.temporary.load(), a.peak.load(), a.versions.load(), a.contents.load(),
        impl_->identities.count, impl_->banks.count, impl_->versions.count,
        impl_->banks.slots.capacity() * sizeof(decltype(impl_->banks)::Slot)
            + impl_->identities.slots.capacity() * sizeof(decltype(impl_->identities)::Slot)
            + impl_->versions.slots.capacity() * sizeof(decltype(impl_->versions)::Slot),
        impl_->publications, impl_->reuses, impl_->rehydrations, impl_->evictions};
}
OwnedAssetResult OwnedRenderAssets::Publish(OwnedAssetKey key, OwnedAssetView source) noexcept
{ return PublishImpl(key, source, false); }
OwnedAssetResult OwnedRenderAssets::PublishImpl(OwnedAssetKey key, OwnedAssetView source, bool expand_rg) noexcept
{
    using Status = OwnedAssetStatus;
    auto reject = [](Status status, OwnedAssetPressure pressure = OwnedAssetPressure::None) {
        return OwnedAssetResult{status, pressure, {}};
    };
    size_t row = 0, bytes = 0;
    if (!Shape(source.desc, row, bytes) || !source.bytes.data() || !key.generation || !key.content_revision)
        return reject(Status::InvalidSource);
    const size_t source_row = expand_rg ? row / 2 : row;
    if (source.row_stride < source_row || source.bytes.size() < source_row
        || source.desc.height - 1 > (source.bytes.size() - source_row) / source.row_stride)
        return reject(Status::InvalidSource);
    if (key.epoch != impl_->epoch) return reject(Status::WrongEpoch);
    const auto* generation = impl_->banks.Find(key.domain);
    if (generation && key.generation != *generation)
        return reject(key.generation < *generation ? Status::StaleKey : Status::GenerationRequired);
    const Identity identity{key.domain, key.logical_id};
    auto* current = impl_->identities.Find(identity);
    if (current && key.content_revision < current->key.content_revision) return reject(Status::StaleKey);
    if (current && source.desc != current->desc) return reject(Status::ContentConflict);
    // Pin the previous publication before any pressure collection. A failed
    // replacement must preserve both its watermark and its resident payload.
    auto prior = current ? Find(current->key) : OwnedAssetLease{};
    const bool same_key = current && key == current->key;
    bool equal = prior.Valid();
    if (equal) {
        const auto previous = prior.Bytes();
        for (uint32_t y = 0; equal && y < source.desc.height; ++y) {
            const uint8_t* src = source.bytes.data() + size_t(y) * source.row_stride;
            const uint8_t* dst = previous.data() + size_t(y) * row;
            if (!expand_rg) equal = std::memcmp(src, dst, row) == 0;
            else for (uint32_t x = 0; equal && x < source.desc.width; ++x)
                equal = dst[size_t(x)*4] == src[size_t(x)*2]
                    && dst[size_t(x)*4+1] == src[size_t(x)*2+1]
                    && dst[size_t(x)*4+2] == 0 && dst[size_t(x)*4+3] == 0;
        }
    }
    if (same_key && prior.Valid()) {
        if (!equal) return reject(Status::ContentConflict);
        ++impl_->reuses; return {Status::Reused, OwnedAssetPressure::None, std::move(prior)};
    }
    if (!generation && impl_->banks.count == impl_->limits.banks)
        return reject(Status::WouldBlock, OwnedAssetPressure::Banks);
    if (!current && impl_->identities.count == impl_->limits.identities)
        return reject(Status::WouldBlock, OwnedAssetPressure::Identities);
    auto pressure = [&]() {
        if (impl_->accounting->versions.load() >= impl_->limits.versions
            || impl_->versions.count >= impl_->limits.versions) return OwnedAssetPressure::Versions;
        if (!equal && impl_->accounting->contents.load() >= impl_->limits.contents) return OwnedAssetPressure::Contents;
        if (!equal && !impl_->Fits(bytes)) return OwnedAssetPressure::Bytes;
        return OwnedAssetPressure::None;
    };
    auto reason = pressure();
    if (reason != OwnedAssetPressure::None) { Collect(true); reason = pressure(); }
    if (reason != OwnedAssetPressure::None) return reject(Status::WouldBlock, reason);
    try {
        std::shared_ptr<const owned_detail::Blob> blob;
        if (equal) blob = prior.state_->blob;
        else {
            auto fresh = std::make_shared<owned_detail::Blob>(impl_->accounting, bytes);
            for (uint32_t y = 0; y < source.desc.height; ++y) {
                const uint8_t* src = source.bytes.data() + size_t(y) * source.row_stride;
                uint8_t* dst = fresh->data.get() + size_t(y) * row;
                if (!expand_rg) std::memcpy(dst, src, row);
                else for (uint32_t x = 0; x < source.desc.width; ++x) {
                    dst[size_t(x)*4] = src[size_t(x)*2]; dst[size_t(x)*4+1] = src[size_t(x)*2+1];
                    dst[size_t(x)*4+2] = 0; dst[size_t(x)*4+3] = 0;
                }
            }
            blob = std::move(fresh);
        }
        auto state = std::make_shared<owned_detail::State>(key, source.desc, std::move(blob));
        // No throwing operations after this point. Metadata storage was reserved
        // at construction; all ownership/identity publication commits together.
        impl_->versions.Put(key, state);
        impl_->banks.Put(key.domain, key.generation);
        impl_->identities.Put(identity, {key, source.desc, state});
        ++impl_->publications;
        if (same_key) ++impl_->rehydrations;
        return {same_key ? Status::Rehydrated : Status::Published, OwnedAssetPressure::None, OwnedAssetLease(std::move(state))};
    } catch (const std::bad_alloc&) { return reject(Status::AllocationFailed); }
}
OwnedAssetResult OwnedRenderAssets::PublishKeeperSprite(OwnedAssetKey key, std::span<const uint8_t> encoded,
    uint32_t width, uint32_t height, uint32_t target_width, uint32_t target_height) noexcept
{
    size_t row = 0, bytes = 0;
    const OwnedAssetDesc desc{OwnedAssetFormat::RGBA8, target_width, target_height};
    if (!width || !height || !encoded.data() || encoded.empty()
        || width > target_width || height > target_height || !Shape(desc, row, bytes)
        || !key.generation || !key.content_revision)
        return {OwnedAssetStatus::InvalidSource, OwnedAssetPressure::None, {}};
    row /= 2; bytes /= 2; // RG decode scratch; final RGBA size was checked above.
    if (key.epoch != impl_->epoch) return {OwnedAssetStatus::WrongEpoch, OwnedAssetPressure::None, {}};
    const auto* generation = impl_->banks.Find(key.domain);
    if (generation && key.generation != *generation)
        return {key.generation < *generation ? OwnedAssetStatus::StaleKey : OwnedAssetStatus::GenerationRequired,
            OwnedAssetPressure::None, {}};
    // Keep the target alive while scratch admission may evict other cached data.
    const auto* current = impl_->identities.Find({key.domain, key.logical_id});
    if (current && key.content_revision < current->key.content_revision)
        return {OwnedAssetStatus::StaleKey, OwnedAssetPressure::None, {}};
    if (current && desc != current->desc)
        return {OwnedAssetStatus::ContentConflict, OwnedAssetPressure::None, {}};
    const auto previous = current ? Find(current->key) : OwnedAssetLease{};
    if (!impl_->Fits(bytes)) Collect(true);
    if (!impl_->Fits(bytes)) return {OwnedAssetStatus::WouldBlock, OwnedAssetPressure::Bytes, {}};
    try {
        owned_detail::Scratch scratch(impl_->accounting, bytes);
        if (!DecodeKeeperSpriteRG({{scratch.data.get(), bytes}, target_width, target_height, row}, encoded, width, height))
            return {OwnedAssetStatus::InvalidSource, OwnedAssetPressure::None, {}};
        return PublishImpl(key, {{OwnedAssetFormat::RGBA8, target_width, target_height},
            {scratch.data.get(), bytes}, row}, true);
    } catch (const std::bad_alloc&) { return {OwnedAssetStatus::AllocationFailed, OwnedAssetPressure::None, {}}; }
}
OwnedAssetResult OwnedRenderAssets::PublishUISprite(OwnedAssetKey key,std::span<const uint8_t> encoded,
    uint32_t width,uint32_t height) noexcept
{
    size_t row=0,bytes=0;
    const OwnedAssetDesc desc{OwnedAssetFormat::R8,width,height};
    if(!Shape({OwnedAssetFormat::RG8,width,height},row,bytes) || !encoded.data() || encoded.empty()
        || !key.generation || !key.content_revision) return {OwnedAssetStatus::InvalidSource,OwnedAssetPressure::None,{}};
    if(key.epoch!=impl_->epoch) return {OwnedAssetStatus::WrongEpoch,OwnedAssetPressure::None,{}};
    const auto* generation=impl_->banks.Find(key.domain);
    if(generation && key.generation!=*generation)
        return {key.generation<*generation?OwnedAssetStatus::StaleKey:OwnedAssetStatus::GenerationRequired,OwnedAssetPressure::None,{}};
    const auto* current=impl_->identities.Find({key.domain,key.logical_id});
    if(current && key.content_revision<current->key.content_revision) return {OwnedAssetStatus::StaleKey,OwnedAssetPressure::None,{}};
    if(current && desc!=current->desc) return {OwnedAssetStatus::ContentConflict,OwnedAssetPressure::None,{}};
    const auto previous=current?Find(current->key):OwnedAssetLease{};
    if(!impl_->Fits(bytes)) Collect(true);
    if(!impl_->Fits(bytes)) return {OwnedAssetStatus::WouldBlock,OwnedAssetPressure::Bytes,{}};
    try {
        owned_detail::Scratch scratch(impl_->accounting,bytes);
        if(!DecodeKeeperSpriteRG({{scratch.data.get(),bytes},width,height,row},encoded,width,height))
            return {OwnedAssetStatus::InvalidSource,OwnedAssetPressure::None,{}};
        // Compact forwards in place; unread RG pairs always follow their output.
        for(size_t i=0;i<bytes/2;++i) scratch.data[i]=scratch.data[i*2+1]?std::max(uint8_t(1),scratch.data[i*2]):0;
        return Publish(key,{desc,{scratch.data.get(),bytes/2},width});
    } catch(const std::bad_alloc&) { return {OwnedAssetStatus::AllocationFailed,OwnedAssetPressure::None,{}}; }
}

OwnedAssetResult OwnedRenderAssets::PublishBitMask(OwnedAssetKey key,std::span<const uint8_t> packed,
    size_t row_stride,uint32_t width,uint32_t height) noexcept
{
    size_t row=0,bytes=0;
    const OwnedAssetDesc desc{OwnedAssetFormat::R8,width,height};
    const size_t packed_row=size_t(width)/8+(width%8!=0);
    if(!Shape(desc,row,bytes) || !packed.data() || row_stride<packed_row || packed.size()<packed_row
        || size_t(height-1)>(packed.size()-packed_row)/row_stride || !key.generation || !key.content_revision)
        return {OwnedAssetStatus::InvalidSource,OwnedAssetPressure::None,{}};
    if(key.epoch!=impl_->epoch) return {OwnedAssetStatus::WrongEpoch,OwnedAssetPressure::None,{}};
    const auto* generation=impl_->banks.Find(key.domain);
    if(generation && key.generation!=*generation)
        return {key.generation<*generation?OwnedAssetStatus::StaleKey:OwnedAssetStatus::GenerationRequired,OwnedAssetPressure::None,{}};
    const auto* current=impl_->identities.Find({key.domain,key.logical_id});
    if(current && key.content_revision<current->key.content_revision) return {OwnedAssetStatus::StaleKey,OwnedAssetPressure::None,{}};
    if(current && desc!=current->desc) return {OwnedAssetStatus::ContentConflict,OwnedAssetPressure::None,{}};
    const auto previous=current?Find(current->key):OwnedAssetLease{};
    if(!impl_->Fits(bytes)) Collect(true);
    if(!impl_->Fits(bytes)) return {OwnedAssetStatus::WouldBlock,OwnedAssetPressure::Bytes,{}};
    try {
        owned_detail::Scratch scratch(impl_->accounting,bytes);
        for(uint32_t y=0;y<height;++y) for(uint32_t x=0;x<width;++x)
            scratch.data[size_t(y)*width+x]=(packed[size_t(y)*row_stride+x/8]&(0x80u>>(x%8)))?255:0;
        return Publish(key,{desc,{scratch.data.get(),bytes},width});
    } catch(const std::bad_alloc&) { return {OwnedAssetStatus::AllocationFailed,OwnedAssetPressure::None,{}}; }
}

}
