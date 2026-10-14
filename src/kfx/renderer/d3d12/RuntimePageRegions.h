// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#pragma once
#include <algorithm>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace kfx::render::dx12 {
struct RuntimePageRegion {
    uint32_t x = 0, y = 0, width = 0, height = 0;
    uint64_t id = 0; // Nonzero for live allocations; zero for free rectangles.
    bool operator==(const RuntimePageRegion&) const = default;
};

// CPU-only page occupancy, with no ownership or GPU-fence policy. The caller
// releases a region only after its last lease and queued GPU reader retire.
// Allocation is deterministic first-fit in (y,x,height,width) order, without
// rotation. A horizontal guillotine split produces disjoint free rectangles;
// release repeatedly merges rectangles sharing a complete edge. Some layouts
// remain fragmented even when total free area is sufficient. Live regions never
// move. Releasing the last allocation restores the entire page as one rectangle.
//
// Copy to plan reservations; Swap commits without throwing. TryAllocate/Release
// have the strong logical exception guarantee (vector capacity may grow). A
// failed allocation leaves its output unchanged. Handles belong to this page
// and committed plan only: pair them with the page owner, and never publish a
// handle from a discarded plan. IDs are not reused within an allocator, even
// after it becomes empty. Returned spans expire on any mutating operation.
class RuntimePageRegions {
public:
    RuntimePageRegions() = default; // Inert, valid empty state for page setup.
    RuntimePageRegions(uint32_t width, uint32_t height) : width_(width), height_(height)
    {
        if (!width || !height || width > 16384 || height > 16384)
            throw std::invalid_argument("Invalid runtime page dimensions");
        free_.push_back({0, 0, width, height, 0});
    }
    RuntimePageRegions(const RuntimePageRegions&) = default;
    RuntimePageRegions(RuntimePageRegions&& other) noexcept { Swap(other); }
    RuntimePageRegions& operator=(RuntimePageRegions other) noexcept
    {
        Swap(other); return *this;
    }
    void Swap(RuntimePageRegions& other) noexcept
    {
        using std::swap;
        swap(width_, other.width_); swap(height_, other.height_);
        swap(next_id_, other.next_id_); swap(used_area_, other.used_area_);
        free_.swap(other.free_); used_.swap(other.used_);
    }
    friend void swap(RuntimePageRegions& a, RuntimePageRegions& b) noexcept { a.Swap(b); }

    uint32_t Width() const noexcept { return width_; }
    uint32_t Height() const noexcept { return height_; }
    uint64_t FreeArea() const noexcept { return uint64_t(width_) * height_ - used_area_; }
    size_t AllocationCount() const noexcept { return used_.size(); }
    size_t MetadataCapacityBytes() const noexcept
    {
        return (free_.capacity() + used_.capacity()) * sizeof(RuntimePageRegion);
    }
    std::span<const RuntimePageRegion> FreeRectangles() const noexcept { return free_; }
    std::span<const RuntimePageRegion> AllocatedRegions() const noexcept { return used_; }

    bool TryAllocate(uint32_t width, uint32_t height, RuntimePageRegion& output)
    {
        if (!width || !height || width > width_ || height > height_) return false;
        size_t selected = 0;
        while (selected < free_.size()
            && (free_[selected].width < width || free_[selected].height < height)) ++selected;
        if (selected == free_.size()) return false;
        if (next_id_ == std::numeric_limits<uint64_t>::max())
            throw std::overflow_error("Runtime page allocation IDs exhausted");
        // Reserve both vectors before changing occupancy or the ID sequence.
        ReserveOne(free_); // One rectangle becomes at most two.
        ReserveOne(used_);
        const auto space = free_[selected];
        const RuntimePageRegion allocated{space.x, space.y, width, height, next_id_};
        free_.erase(free_.begin() + selected);
        if (width < space.width)
            free_.push_back({space.x + width, space.y, space.width - width, height, 0});
        if (height < space.height)
            free_.push_back({space.x, space.y + height, space.width, space.height - height, 0});
        used_.push_back(allocated);
        ++next_id_; used_area_ += Area(allocated);
        SortFree(); output = allocated; return true;
    }

    // Reject a double release, stale ID, or modified rectangle without mutation.
    bool Release(const RuntimePageRegion& region)
    {
        const auto found = std::find(used_.begin(), used_.end(), region);
        if (!region.id || found == used_.end()) return false;
        ReserveOne(free_); // The only potentially throwing step.
        const auto released = *found;
        used_.erase(found); used_area_ -= Area(released);
        if (used_.empty()) {
            free_.clear(); free_.push_back({0, 0, width_, height_, 0});
        } else {
            free_.push_back({released.x, released.y, released.width, released.height, 0});
            Coalesce();
        }
        return true;
    }

    // Diagnostic only: quadratic exhaustive bounds, identity, overlap and area
    // checks. No allocation; do not put this scan on the normal rendering path.
    bool Valid() const noexcept
    {
        if (!width_ || !height_)
            return !width_ && !height_ && free_.empty() && used_.empty() && !used_area_ && next_id_ == 1;
        if (width_ > 16384 || height_ > 16384 || !next_id_) return false;
        uint64_t free_area = 0, used_area = 0;
        for (size_t i = 0; i < free_.size(); ++i) {
            const auto& a = free_[i];
            if (!Contains(a) || a.id || (i && Before(a, free_[i - 1]))) return false;
            free_area += Area(a);
            for (size_t j = i + 1; j < free_.size(); ++j) if (Overlaps(a, free_[j])) return false;
            for (const auto& b : used_) if (Overlaps(a, b)) return false;
        }
        for (size_t i = 0; i < used_.size(); ++i) {
            const auto& a = used_[i];
            if (!Contains(a) || !a.id || a.id >= next_id_) return false;
            used_area += Area(a);
            for (size_t j = i + 1; j < used_.size(); ++j)
                if (a.id == used_[j].id || Overlaps(a, used_[j])) return false;
        }
        return used_area == used_area_ && free_area + used_area == uint64_t(width_) * height_;
    }
private:
    static void ReserveOne(std::vector<RuntimePageRegion>& regions)
    {
        if (regions.size() == regions.capacity())
            regions.reserve(std::max<size_t>(regions.size() + 1, regions.capacity() * 2));
    }
    static uint64_t Area(const RuntimePageRegion& r) noexcept { return uint64_t(r.width) * r.height; }
    bool Contains(const RuntimePageRegion& r) const noexcept
    {
        return r.width && r.height && uint64_t(r.x) + r.width <= width_ && uint64_t(r.y) + r.height <= height_;
    }
    static bool Overlaps(const RuntimePageRegion& a, const RuntimePageRegion& b) noexcept
    {
        return uint64_t(a.x) < uint64_t(b.x) + b.width && uint64_t(b.x) < uint64_t(a.x) + a.width
            && uint64_t(a.y) < uint64_t(b.y) + b.height && uint64_t(b.y) < uint64_t(a.y) + a.height;
    }
    static bool Before(const RuntimePageRegion& a, const RuntimePageRegion& b) noexcept
    {
        if (a.y != b.y) return a.y < b.y;
        if (a.x != b.x) return a.x < b.x;
        if (a.height != b.height) return a.height < b.height;
        return a.width < b.width;
    }
    void SortFree() noexcept { std::sort(free_.begin(), free_.end(), Before); }
    void Coalesce() noexcept
    {
        SortFree();
        for (;;) {
            bool merged = false;
            for (size_t i = 0; i < free_.size() && !merged; ++i) {
                for (size_t j = i + 1; j < free_.size(); ++j) {
                    auto& a = free_[i]; const auto b = free_[j];
                    if (a.y == b.y && a.height == b.height
                        && (a.x + a.width == b.x || b.x + b.width == a.x)) {
                        a.x = std::min(a.x, b.x); a.width += b.width;
                    } else if (a.x == b.x && a.width == b.width
                        && (a.y + a.height == b.y || b.y + b.height == a.y)) {
                        a.y = std::min(a.y, b.y); a.height += b.height;
                    } else continue;
                    free_.erase(free_.begin() + j); merged = true; break;
                }
            }
            if (!merged) break;
            SortFree();
        }
    }
    uint32_t width_ = 0, height_ = 0;
    uint64_t next_id_ = 1, used_area_ = 0;
    std::vector<RuntimePageRegion> free_, used_;
};
}
