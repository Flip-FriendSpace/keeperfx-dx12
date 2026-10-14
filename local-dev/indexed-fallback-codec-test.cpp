// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#include "kfx/renderer/IndexedWorldFallbackCodec.h"
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <new>
#include <tuple>

namespace {
bool watch_allocations = false;
size_t large_allocations = 0;
}
// Only output-sized allocations are monitored; exception messages may allocate.
[[gnu::noinline]] void* operator new(std::size_t length)
{
    if (watch_allocations && length >= 1024) ++large_allocations;
    if (void* memory = std::malloc(length ? length : 1)) return memory;
    throw std::bad_alloc();
}
[[gnu::noinline]] void operator delete(void* memory) noexcept { std::free(memory); }
[[gnu::noinline]] void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void* operator new[](std::size_t length) { return ::operator new(length); }
void operator delete[](void* memory) noexcept { ::operator delete(memory); }
void operator delete[](void* memory, std::size_t) noexcept { ::operator delete(memory); }

using namespace kfx::render;
namespace {
size_t rejections = 0;
IndexedWorldFallbackFrame Fixture()
{
    IndexedWorldFallbackFrame frame;
    frame.sequence = 0x0123456789abcdefull;
    frame.custom_draw_attempts_before = 0x1020304050607080ull;
    frame.custom_draw_attempts_after = frame.custom_draw_attempts_before + 1;
    frame.native_width = 127; frame.native_height = 93;
    frame.x = 13; frame.y = 17; frame.width = 7; frame.height = 5;
    frame.destination_clear_index = 144; frame.renderer_state_restored = true;
    for (unsigned i = 0; i < 256; ++i) {
        frame.display_palette_rgba[i*4] = uint8_t(i);
        frame.display_palette_rgba[i*4+1] = uint8_t(255-i);
        frame.display_palette_rgba[i*4+2] = uint8_t(i*37);
        frame.display_palette_rgba[i*4+3] = 255;
    }
    for (unsigned i = 0; i < 35; ++i) frame.indices.push_back(uint8_t(i*17));
    frame.indices[1] = 255; frame.indices.back() = 0;
    return frame;
}
bool Same(const IndexedWorldFallbackFrame& a, const IndexedWorldFallbackFrame& b)
{
    return a.reason == b.reason && a.sequence == b.sequence
        && a.custom_draw_attempts_before == b.custom_draw_attempts_before && a.custom_draw_attempts_after == b.custom_draw_attempts_after
        && a.native_width == b.native_width && a.native_height == b.native_height
        && a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height
        && a.destination_clear_index == b.destination_clear_index && a.display_palette_rgba == b.display_palette_rgba
        && a.indices == b.indices && a.renderer_state_restored == b.renderer_state_restored;
}
void Set(std::vector<uint8_t>& bytes, size_t offset, uint64_t value, unsigned length)
{
    for (unsigned i = 0; i < length; ++i) bytes[offset+i] = uint8_t(value >> (8*i));
}
void Reject(std::span<const uint8_t> bytes, bool allocation_probe = false)
{
    bool rejected = false; large_allocations = 0; watch_allocations = allocation_probe;
    try { (void)DecodeIndexedWorldFallback(bytes); } catch (const std::invalid_argument&) { rejected = true; }
    watch_allocations = false;
    assert(rejected && (!allocation_probe || large_allocations == 0)); ++rejections;
}
void RejectFrame(const IndexedWorldFallbackFrame& frame)
{
    assert(!ValidateIndexedWorldFallbackFrame(frame)); bool rejected = false;
    try { (void)EncodeIndexedWorldFallback(frame); } catch (const std::invalid_argument&) { rejected = true; }
    assert(rejected); ++rejections;
}
void Write(const std::filesystem::path& path, const std::vector<uint8_t>& bytes)
{
    std::ofstream file(path,std::ios::binary); file.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());
    if (!file) throw std::runtime_error("Cannot write codec fixture");
}
void Inspect(const std::filesystem::path& path)
{
    std::ifstream file(path,std::ios::binary); if (!file) throw std::runtime_error("Cannot read codec fixture");
    const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(file),{}};
    const auto frame = DecodeIndexedWorldFallback(bytes);
    assert(EncodeIndexedWorldFallback(frame) == bytes);
    std::cout << "{\"canonical\":true,\"native\":[" << frame.native_width << ',' << frame.native_height
        << "],\"viewport\":[" << frame.x << ',' << frame.y << ',' << frame.width << ',' << frame.height
        << "],\"clearIndex\":" << unsigned(frame.destination_clear_index) << ",\"sequence\":\"" << frame.sequence
        << "\",\"before\":\"" << frame.custom_draw_attempts_before << "\",\"after\":\"" << frame.custom_draw_attempts_after
        << "\",\"indexBytes\":" << frame.indices.size() << ",\"restored\":true}\n";
}
}
int main(int argc, char** argv)
{
    try {
        if (argc == 3 && std::string(argv[1]) == "--inspect") { Inspect(argv[2]); return 0; }
        if (argc != 2) throw std::invalid_argument("Usage: indexed-fallback-codec-test output | --inspect snapshot");
        auto original = Fixture(); assert(ValidateIndexedWorldFallbackFrame(original));
        const auto bytes = EncodeIndexedWorldFallback(original);
        // Hand-written header checks the ABI/wire independently of the codec's
        // scalar helper and includes values that lose precision as JS numbers.
        constexpr std::array<uint8_t,72> header{
            0x4b,0x46,0x58,0x49, 1,0,0,0, 127,0,0,0, 93,0,0,0,
            13,0,0,0, 17,0,0,0, 7,0,0,0, 5,0,0,0,
            0,144,1,0, 0,4,0,0, 35,0,0,0, 0,0,0,0,
            0xef,0xcd,0xab,0x89,0x67,0x45,0x23,0x01,
            0x80,0x70,0x60,0x50,0x40,0x30,0x20,0x10,
            0x81,0x70,0x60,0x50,0x40,0x30,0x20,0x10};
        assert(bytes.size() == 1131 && std::equal(header.begin(),header.end(),bytes.begin()));
        assert(bytes[72] == 0 && bytes[73] == 255 && bytes[74] == 0 && bytes[75] == 255);
        assert(bytes[1092] == 255 && bytes[1093] == 0 && bytes[1094] == 219 && bytes[1095] == 255);
        assert(bytes[1096] == 0 && bytes[1097] == 255 && bytes.back() == 0);
        const auto decoded = DecodeIndexedWorldFallback(bytes); assert(Same(original,decoded));
        original.indices[0] = 77; original.display_palette_rgba[0] = 99;
        assert(EncodeIndexedWorldFallback(decoded) == bytes); // Snapshot storage is owned.
        auto reject = [&](auto change) { auto frame = Fixture(); change(frame); RejectFrame(frame); };
        reject([](auto& f) { f.reason = IndexedWorldFallbackReason(1); });
        reject([](auto& f) { f.sequence = 0; });
        reject([](auto& f) { f.custom_draw_attempts_before = UINT64_MAX; f.custom_draw_attempts_after = 0; });
        reject([](auto& f) { f.custom_draw_attempts_after--; });
        reject([](auto& f) { f.custom_draw_attempts_after++; });
        reject([](auto& f) { f.renderer_state_restored = false; });
        for (auto member : {&IndexedWorldFallbackFrame::native_width,&IndexedWorldFallbackFrame::native_height,
            &IndexedWorldFallbackFrame::width,&IndexedWorldFallbackFrame::height}) {
            reject([&](auto& f) { f.*member = 0; }); reject([&](auto& f) { f.*member = -1; });
        }
        reject([](auto& f) { f.native_width = 16385; }); reject([](auto& f) { f.native_height = 16385; });
        reject([](auto& f) { f.native_width = f.native_height = 4097; });
        reject([](auto& f) { f.x = -1; }); reject([](auto& f) { f.y = -1; });
        reject([](auto& f) { f.x = 121; }); reject([](auto& f) { f.y = 89; });
        reject([](auto& f) { f.width = 128; }); reject([](auto& f) { f.height = 94; });
        reject([](auto& f) { f.indices.pop_back(); }); reject([](auto& f) { f.indices.push_back(0); });
        for (size_t i = 3; i < 1024; i += 4) reject([&](auto& f) { f.display_palette_rgba[i] = 254; });
        const std::vector<std::tuple<size_t,uint64_t,unsigned>> mutations{
            {0,0,4},{4,0,4},{4,2,4},{8,0,4},{12,0,4},{8,16385,4},{12,16385,4},{16,UINT32_MAX,4},{20,UINT32_MAX,4},
            {16,121,4},{20,89,4},{24,0,4},{28,0,4},{24,128,4},{28,94,4},{32,1,1},{34,0,1},{34,3,1},{35,1,1},
            {36,1023,4},{36,1025,4},{40,34,4},{40,36,4},{40,UINT32_MAX,4},{44,1,4},{48,0,8},
            {56,UINT64_MAX,8},{64,0,8},{64,0x1020304050607080ull,8},{64,0x1020304050607082ull,8}};
        for (auto [offset,value,length] : mutations) { auto bad = bytes; Set(bad,offset,value,length); Reject(bad); }
        for (size_t cut = 0; cut < 1096; ++cut) Reject(std::span(bytes).first(cut));
        for (size_t cut : {size_t(1096),size_t(1100),bytes.size()-1}) Reject(std::span(bytes).first(cut));
        auto trailing = bytes; trailing.push_back(0); Reject(trailing);
        auto changed = bytes; changed[1096] = 123; const auto altered = DecodeIndexedWorldFallback(changed);
        assert(altered.indices[0] == 123 && EncodeIndexedWorldFallback(altered) == changed); // Valid INDEX8 data is not a transparent-code filter.
        auto maximum = Fixture(); maximum.native_width = maximum.width = 16384; maximum.native_height = maximum.height = 1024;
        maximum.x = maximum.y = 0; maximum.sequence = UINT64_MAX;
        maximum.custom_draw_attempts_before = UINT64_MAX-1; maximum.custom_draw_attempts_after = UINT64_MAX;
        maximum.indices.assign(16*1024*1024,255); maximum.indices.front() = 0;
        assert(ValidateIndexedWorldFallbackFrame(maximum)); const auto max_bytes = EncodeIndexedWorldFallback(maximum);
        assert(max_bytes.size() == IndexedWorldFallbackMaxSnapshotBytes && Same(maximum,DecodeIndexedWorldFallback(max_bytes)));
        maximum.native_height++; RejectFrame(maximum); auto too_big = max_bytes; too_big.push_back(0); Reject(too_big);
        auto minimum = Fixture(); minimum.native_width = minimum.native_height = minimum.width = minimum.height = 1;
        minimum.x = minimum.y = 0; minimum.sequence = 1; minimum.custom_draw_attempts_before = 0; minimum.custom_draw_attempts_after = 1;
        minimum.indices = {0}; minimum.destination_clear_index = 255; const auto min_bytes = EncodeIndexedWorldFallback(minimum);
        assert(min_bytes.size() == 1097 && Same(minimum,DecodeIndexedWorldFallback(min_bytes)));
        auto probe = minimum; probe.native_width = probe.width = 256; probe.native_height = probe.height = 256; probe.indices.assign(65536,0);
        const auto probe_bytes = EncodeIndexedWorldFallback(probe);
        watch_allocations = true; large_allocations = 0; const auto probe_decoded = DecodeIndexedWorldFallback(probe_bytes); watch_allocations = false;
        assert(large_allocations > 0 && probe_decoded.indices.size() == 65536); // The watch observes real output allocations.
        for (auto [offset,value,length] : std::vector<std::tuple<size_t,uint64_t,unsigned>>{{8,0,4},{34,3,1},{40,UINT32_MAX,4},{56,UINT64_MAX,8},{64,0,8},{75,0,1}}) {
            auto bad = probe_bytes; Set(bad,offset,value,length); Reject(bad,true);
        }
        Write(argv[1],bytes); Write(std::string(argv[1])+".max",max_bytes); Write(std::string(argv[1])+".min",min_bytes);
        std::cout << "{\"passed\":true,\"rejections\":" << rejections << ",\"allocationRejections\":6,\"fixtureBytes\":" << bytes.size() << "}\n";
        return 0;
    } catch (const std::exception& e) { watch_allocations = false; std::cerr << e.what() << '\n'; return 1; }
}
