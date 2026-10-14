// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#include "IndexedFallbackReplay.h"
#include "kfx/renderer/IndexedWorldFallbackCodec.h"
#include <algorithm>
#include <fstream>
#include <stdexcept>

namespace kfx::render::dx12 {
namespace {
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void Bytes(const std::filesystem::path& path, const std::vector<uint8_t>& bytes)
{
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    Require(bool(file), "Cannot retain indexed fallback bytes");
}
void Bitmap(const std::filesystem::path& path, const Image& image)
{
    std::ofstream file(path, std::ios::binary);
    auto word = [&](uint16_t n) { file.put(char(n)); file.put(char(n >> 8)); };
    auto dword = [&](uint32_t n) { word(uint16_t(n)); word(uint16_t(n >> 16)); };
    file.put('B'); file.put('M'); dword(uint32_t(54 + image.rgba.size()));
    word(0); word(0); dword(54); dword(40); dword(image.width); dword(image.height);
    word(1); word(32); dword(0); dword(uint32_t(image.rgba.size())); dword(2835); dword(2835); dword(0); dword(0);
    for (uint32_t y = image.height; y-- > 0;) for (uint32_t x = 0; x < image.width; ++x) {
        const auto p = (size_t(y) * image.width + x) * 4;
        file.put(char(image.rgba[p + 2])); file.put(char(image.rgba[p + 1]));
        file.put(char(image.rgba[p])); file.put(char(image.rgba[p + 3]));
    }
    Require(bool(file), "Cannot retain indexed fallback bitmap");
}
IndexedWorldFallbackFrame Load(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    Require(bool(file), "Cannot open indexed fallback file");
    const auto size = file.tellg();
    Require(size >= std::streamoff(IndexedWorldFallbackHeaderBytes + IndexedWorldFallbackPaletteBytes)
        && size <= std::streamoff(IndexedWorldFallbackMaxSnapshotBytes), "Indexed fallback file exceeds bounded format size");
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size()));
    Require(bool(file) && file.peek() == std::char_traits<char>::eof(), "Indexed fallback file changed or was truncated while reading");
    return DecodeIndexedWorldFallback(bytes);
}
uint64_t Differences(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b)
{
    Require(a.size() == b.size(), "Indexed fallback comparison size differs");
    uint64_t count = 0;
    for (size_t i = 0; i < a.size(); ++i) count += a[i] != b[i];
    return count;
}
}

std::filesystem::path WriteIndexedFallbackFixture(const std::filesystem::path& output)
{
    IndexedWorldFallbackFrame fixture;
    fixture.sequence = 1; fixture.custom_draw_attempts_before = 7; fixture.custom_draw_attempts_after = 8;
    fixture.native_width = 33; fixture.native_height = 21;
    fixture.x = 7; fixture.y = 5; fixture.width = 19; fixture.height = 11;
    fixture.destination_clear_index = 233; fixture.renderer_state_restored = true;
    for (size_t i = 0; i < 256; ++i) {
        fixture.display_palette_rgba[i * 4] = uint8_t(i * 73 + 1);
        fixture.display_palette_rgba[i * 4 + 1] = uint8_t(i * 29 + 2);
        fixture.display_palette_rgba[i * 4 + 2] = uint8_t(i * 151 + 3);
        fixture.display_palette_rgba[i * 4 + 3] = 255;
    }
    fixture.indices.resize(size_t(fixture.width) * fixture.height);
    for (int y = 0; y < fixture.height; ++y) for (int x = 0; x < fixture.width; ++x)
        fixture.indices[size_t(y) * fixture.width + x] = (x + y) % 5 == 0 ? 0
            : (x + y) % 7 == 0 ? 255 : uint8_t(x * 17 + y * 31);
    const auto path = output / "indexed-fallback-synthetic.kfxidx";
    Bytes(path, EncodeIndexedWorldFallback(fixture));
    return path;
}

FrozenGpuFrame PrepareIndexedFallbackReplay(Device& device, const std::filesystem::path& path,
    const std::filesystem::path& output, bool synthetic)
{
    const auto owned = Load(path);
    const uint32_t width = uint32_t(owned.native_width), height = uint32_t(owned.native_height);
    Image atlas;
    atlas.width = std::max(256u, uint32_t(owned.width));
    atlas.height = std::max(256u, uint32_t(owned.height));
    Require(uint64_t(atlas.width) * atlas.height * 4 <= 64 * 1024 * 1024, "Indexed fallback atlas exceeds 64 MiB");
    atlas.rgba.resize(size_t(atlas.width) * atlas.height * 4);
    for (int y = 0; y < owned.height; ++y) for (int x = 0; x < owned.width; ++x) {
        const size_t p = (size_t(y) * atlas.width + x) * 4;
        atlas.rgba[p] = owned.indices[size_t(y) * owned.width + x];
        atlas.rgba[p + 3] = 255;
    }
    PalettePrimitive draw;
    draw.sprite = {owned.x, owned.y, uint32_t(owned.width), uint32_t(owned.height),
        0, 0, uint32_t(owned.width), uint32_t(owned.height), PaletteBlend::Opaque};
    const auto before = device.GetDiagnostics();
    auto frame = device.PrepareFrozenIndexedFrame(width, height, owned.destination_clear_index, atlas,
        {draw}, owned.display_palette_rgba, false);
    const auto prepared = device.GetDiagnostics();
    Require(prepared.frozen_preparations == before.frozen_preparations + 1
        && prepared.frozen_image_readbacks == before.frozen_image_readbacks,
        "Indexed fallback preparation rebuilt or read back an image");
    const auto indices = device.ReadbackFrozenIndices(frame);
    const auto rgba = device.ReadbackFrozenRGBA(frame);
    const auto after = device.GetDiagnostics();
    Require(after.frozen_image_readbacks == before.frozen_image_readbacks + 2,
        "Indexed fallback comparison did not perform exactly two explicit readbacks");
    std::vector<uint8_t> expected(size_t(width) * height, owned.destination_clear_index);
    for (int y = 0; y < owned.height; ++y)
        std::copy_n(owned.indices.data() + size_t(y) * owned.width, owned.width,
            expected.data() + size_t(owned.y + y) * width + owned.x);
    Image expected_rgba{width, height, std::vector<uint8_t>(expected.size() * 4)};
    for (size_t p = 0; p < expected.size(); ++p)
        std::copy_n(owned.display_palette_rgba.data() + size_t(expected[p]) * 4, 4, expected_rgba.rgba.data() + p * 4);
    Require(rgba.width == width && rgba.height == height, "Indexed fallback RGBA extent differs");
    const uint64_t index_differences = Differences(indices, expected);
    const uint64_t rgba_differences = Differences(rgba.rgba, expected_rgba.rgba);
    Image difference{width, height, std::vector<uint8_t>(expected.size() * 4)};
    uint64_t differing_pixels = 0;
    for (size_t p = 0; p < expected.size(); ++p) {
        bool mismatch = indices[p] != expected[p];
        for (size_t c = 0; c < 4; ++c) mismatch |= rgba.rgba[p * 4 + c] != expected_rgba.rgba[p * 4 + c];
        differing_pixels += mismatch;
        difference.rgba[p * 4] = difference.rgba[p * 4 + 2] = mismatch ? 255 : 0;
        difference.rgba[p * 4 + 3] = 255;
    }
    Bytes(output / "indexed-fallback-gpu.idx", indices); Bytes(output / "indexed-fallback-expected.idx", expected);
    Bytes(output / "indexed-fallback-gpu.rgba", rgba.rgba); Bytes(output / "indexed-fallback-expected.rgba", expected_rgba.rgba);
    Bytes(output / "indexed-fallback-difference.rgba", difference.rgba);
    Bitmap(output / "indexed-fallback-gpu.bmp", rgba); Bitmap(output / "indexed-fallback-expected.bmp", expected_rgba);
    Bitmap(output / "indexed-fallback-difference.bmp", difference);
    const bool exact = !index_differences && !rgba_differences;
    std::ofstream report(output / "indexed-fallback-replay.json");
    report << "{\"schema\":1,\"scope\":\"Owned post-callback indexed viewport upload and exact native composition; callback execution, asynchronous runtime caching and gameplay are not tested\""
        << ",\"synthetic\":" << (synthetic ? "true" : "false") << ",\"exact\":" << (exact ? "true" : "false")
        << ",\"gate2Accepted\":false,\"visualAccepted\":false,\"callbackExecuted\":false,\"residentGpuFrame\":true"
        << ",\"formatVersion\":" << IndexedWorldFallbackVersion << ",\"reason\":\"custom-lens-software-fallback\""
        << ",\"sequence\":" << owned.sequence << ",\"producerCustomDrawAttemptsBefore\":" << owned.custom_draw_attempts_before
        << ",\"producerCustomDrawAttemptsAfter\":" << owned.custom_draw_attempts_after << ",\"producerStateRestored\":true"
        << ",\"nativeWidth\":" << width << ",\"nativeHeight\":" << height
        << ",\"viewport\":[" << owned.x << ',' << owned.y << ',' << owned.width << ',' << owned.height << ']'
        << ",\"clearIndex\":" << unsigned(owned.destination_clear_index) << ",\"paletteBytes\":1024,\"ownedIndices\":" << owned.indices.size()
        << ",\"indexChecks\":" << expected.size() << ",\"rgbaByteChecks\":" << expected_rgba.rgba.size()
        << ",\"differingIndices\":" << index_differences << ",\"differingRgbaBytes\":" << rgba_differences
        << ",\"differingPixels\":" << differing_pixels << ",\"preparations\":1,\"preparationImageReadbacks\":0,\"diagnosticImageReadbacks\":2}\n";
    Require(bool(report), "Cannot retain indexed fallback report");
    Require(exact, "Indexed fallback native indices or exact display palette expansion differ");
    Require(after.validation_errors == before.validation_errors && after.validation_warnings == before.validation_warnings,
        "Indexed fallback emitted GPU validation diagnostics");
    return frame;
}
}
