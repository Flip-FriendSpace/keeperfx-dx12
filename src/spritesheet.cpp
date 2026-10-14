// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#include "pre_inc.h"
#include "bflib_sprite.h"
#include "bflib_filelst.h"
#include "bflib_dernc.h"
#include "globals.h"        // SYNCLOG
#include "kfx/renderer/RendererBridge_UI.h" // RendererForgetSprites
#include <vector>
#include <memory>
#include <map>
#include <algorithm>
#include "post_inc.h"

static TbSpriteSheet* live_sprite_sheets = nullptr;
static uint64_t ui_sprite_publication_serial = 0;
static uint64_t next_ui_sprite_publication()
{
    if (ui_sprite_publication_serial >= UINT64_MAX - 1) {
        ui_sprite_publication_serial = UINT64_MAX;
        return 0;
    }
    return ++ui_sprite_publication_serial;
}

struct TbSpriteSheet {
    std::vector<TbSprite> sprites; // needs to be contiguous to avoid crashing some code
    std::vector<std::vector<unsigned char>> data;
    std::vector<uint64_t> source_ids;
    TbSpriteSheet *previous = nullptr, *next = nullptr;
    TbSpriteSheet() : next(live_sprite_sheets) {
        if (next) next->previous = this;
        live_sprite_sheets = this;
    }
    ~TbSpriteSheet() {
        if (previous) previous->next = next;
        else live_sprite_sheets = next;
        if (next) next->previous = previous;
    }
    TbSpriteSheet(const TbSpriteSheet&) = delete;
    TbSpriteSheet& operator=(const TbSpriteSheet&) = delete;
};

namespace {

#pragma pack(1)
#ifdef SPRITE_FORMAT_V2
    struct sprite_entry {
        uint32_t offset;
        uint16_t width;
        uint16_t height;
    };
#else
    struct sprite_entry {
        uint32_t offset;
        uint8_t width;
        uint8_t height;
    };
#endif
#pragma pack(0)

using offset_list = std::vector<std::pair<uint32_t, size_t>>;

bool load_index_file(TbSpriteSheet & sheet, offset_list & offsets, const char * filename)
{
    const auto fname = modify_data_load_filename_function(filename);
    const size_t index_size = LbFileLengthRnc(fname);
    if (index_size <= 0 || index_size % sizeof(sprite_entry) != 0) return false;
    const size_t num_sprites = index_size / sizeof(sprite_entry);
    if (num_sprites == 0) return false;
    std::vector<sprite_entry> entries(num_sprites);
    if (LbFileLoadAt(fname, entries.data()) != index_size) return false;
    sheet.sprites.reserve(num_sprites);
    sheet.data.resize(num_sprites);
    sheet.source_ids.resize(num_sprites);
    offsets.reserve(num_sprites + 1);
    for (size_t i = 0; i < num_sprites; ++i) {
        const auto & entry = entries[i];
        sheet.sprites.emplace_back(TbSprite{nullptr, entry.width, entry.height});
        offsets.emplace_back(entry.offset, i);
    }
    return true;
}

bool load_data_file(TbSpriteSheet & sheet, offset_list & offsets, const char * filename)
{
    const auto num_sprites = sheet.sprites.size();
    const auto fname = modify_data_load_filename_function(filename);
    const size_t data_size = LbFileLengthRnc(fname);
    if (data_size <= 0 || data_size > UINT32_MAX) return false;
    // figure out the size of each sprite by sorting offsets
    offsets.emplace_back(data_size, num_sprites);
    std::sort(offsets.begin(), offsets.end(), [] (const auto & a, const auto & b) -> auto {
        return a.first < b.first || (a.first == b.first && a.second < b.second);
    });
    std::vector<unsigned char> buffer(data_size);
    if (LbFileLoadAt(fname, buffer.data()) != data_size) return false;
    // Validate every actual allocation range before constructing iterators.
    for (const auto& item : offsets) if (item.first > data_size) return false;
    // populate sprite data
    for (size_t i = 0; i < num_sprites; ++i) {
        const auto first = buffer.begin() + offsets[i].first;
        const auto last = buffer.begin() + offsets[i + 1].first;
        const auto sprite_idx = offsets[i].second;
        auto & sprite = sheet.sprites[sprite_idx];
        auto & data = sheet.data[sprite_idx];
        data = std::move(std::vector<unsigned char>(first, last));
        sprite.Data = data.data();
        sheet.source_ids[sprite_idx] = next_ui_sprite_publication();
    }
    return true;
}

} // local

extern "C" TbSpriteSheet * create_spritesheet()
{
    try {
        return new TbSpriteSheet();
    } catch (const std::exception & e) {
        ERRORLOG("Failed to create sprite sheet: %s", e.what());
    }
    return nullptr;
}

extern "C" TbSpriteSheet * load_spritesheet(const char * data_fname, const char * index_fname)
{
    try {
        auto sheet = std::make_unique<TbSpriteSheet>();
        offset_list offsets;
        if (!load_index_file(*sheet, offsets, index_fname)) return nullptr;
        if (!load_data_file(*sheet, offsets, data_fname)) return nullptr;
        return sheet.release();
    } catch (const std::exception & e) {
        ERRORLOG("Failed to load sprite sheet from %s %s: %s", data_fname, index_fname, e.what());
    }
    return nullptr;
}

extern "C" void free_spritesheet(TbSpriteSheet ** sheet)
{
    if (sheet && *sheet && !(*sheet)->sprites.empty()) {
        // IUIRenderer::ResolveSprite() caches handles keyed by TbSprite*; the
        // next sheet loaded can reuse this memory, so drop this sheet's entries.
        int32_t forgotten = RendererForgetSprites((*sheet)->sprites.data(), (long)(*sheet)->sprites.size());
        if (forgotten > 0) {
            SYNCLOG("forgot %d cached UI sprite handle(s) before freeing a sprite sheet", (int)forgotten);
        }
    }
    if (sheet) {
        delete *sheet;
        *sheet = NULL;
    }
}

extern "C" const TbSprite * get_sprite(const TbSpriteSheet * sheet, const long index)
{
    if (!sheet) {
        return NULL;
    } else if (index >= sheet->sprites.size()) {
        return NULL;
    }
    return &sheet->sprites[index];
}

#ifdef SPRITE_FORMAT_V2
extern "C" TbBool add_sprite(TbSpriteSheet * sheet, unsigned short width, unsigned short height, int size, const void * data)
#else
extern "C" TbBool add_sprite(TbSpriteSheet * sheet, unsigned char width, unsigned char height, int size, const void * data)
#endif
{
    if (!sheet || size < 0 || (size && !data)) return false;
    try {
        sheet->source_ids.reserve(sheet->sprites.size() + 1);
        std::vector<unsigned char> owned;
        if (size) owned.assign(static_cast<const unsigned char*>(data), static_cast<const unsigned char*>(data) + size);
        sheet->data.emplace_back(std::move(owned));
        const TbSprite * old_first = sheet->sprites.data();
        const size_t old_count = sheet->sprites.size();
        try {
            sheet->sprites.emplace_back(TbSprite{sheet->data.back().data(), width, height});
        } catch (...) {
            sheet->data.pop_back();
            throw;
        }
        sheet->source_ids.push_back(next_ui_sprite_publication());
        if (old_count > 0 && sheet->sprites.data() != old_first) {
            // The sprites moved; their old addresses may be reused.
            RendererForgetSprites(old_first, (long)old_count);
        }
        return true;
    } catch (const std::exception & e) {
        ERRORLOG("Failed to add sprite: %s", e.what());
    }
    return false;
}

extern "C" long num_sprites(const TbSpriteSheet * sheet)
{
    if (!sheet) {
        return 0;
    }
    return sheet->sprites.size();
}

extern "C" TbBool get_ui_sprite_source_identity(const TbSprite* sprite, TbSpriteSourceIdentity* output)
{
    if (!output) return false;
    *output = {};
    if (!sprite || ui_sprite_publication_serial == UINT64_MAX) return false;
    const uintptr_t address = reinterpret_cast<uintptr_t>(sprite);
    for (auto* sheet = live_sprite_sheets; sheet; sheet = sheet->next) {
        if (sheet->sprites.empty()) continue;
        const uintptr_t begin = reinterpret_cast<uintptr_t>(sheet->sprites.data());
        if (address < begin || address - begin >= sheet->sprites.size() * sizeof(TbSprite)
            || (address - begin) % sizeof(TbSprite)) continue;
        const size_t index = (address - begin) / sizeof(TbSprite);
        if (index >= sheet->data.size() || index >= sheet->source_ids.size()) return false;
        const auto& bytes = sheet->data[index];
        const auto& actual = sheet->sprites[index];
        if (actual.Data != bytes.data() || (bytes.empty() && actual.SWidth && actual.SHeight)
            || !sheet->source_ids[index]) return false;
        *output = {bytes.data(), bytes.size(), sheet->source_ids[index], actual.SWidth, actual.SHeight};
        return true;
    }
    return false;
}
