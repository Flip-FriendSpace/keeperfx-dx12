// SPDX-License-Identifier: GPL-2.0-or-later
#include "pre_inc.h"
#include "kfx/renderer/HdTerrainPack.h"
#include "bflib_basics.h"
#include "globals.h"
#include "config.h"
#include <cstdlib>
#include <fstream>
#include "post_inc.h"
namespace kfx::render {
namespace {
struct ProcessPack {
    HdTerrainPack pack;
    std::array<bool,TileSourceTileCount> reported{};
    ProcessPack() noexcept {
        const char* requested=std::getenv("KFX_HD_TERRAIN_PACK");
        if(requested && (std::strcmp(requested,"off")==0 || std::strcmp(requested,"0")==0))return;
        const bool explicit_path=requested && *requested;
        const char* path=explicit_path?requested:prepare_file_path(FGrp_FxData,"standard-hd.kfht");
        try {
            std::ifstream file(path,std::ios::binary|std::ios::ate);
            if(!file && !explicit_path)return; // An uninstalled bundle keeps native rendering.
            const auto length=file.tellg();
            if(!file || length<16 || length>std::streamoff(HdTerrainPack::MaxFileBytes)) {
                WARNLOG("HD terrain pack is unreadable or exceeds its size bound; using original textures");return;
            }
            std::vector<uint8_t> bytes(static_cast<size_t>(length));file.seekg(0);
            if(!file.read(reinterpret_cast<char*>(bytes.data()),length) || !pack.Parse(bytes)) {
                WARNLOG("HD terrain pack failed validation; using original textures");return;
            }
            SYNCLOG("HD terrain pack loaded: %lu authenticated tile replacements",static_cast<unsigned long>(pack.Count()));
        } catch(...) {WARNLOG("HD terrain pack could not be loaded; using original textures");}
    }
};
}
const HdTerrainPack::Record* ResolveHdTerrain(uint32_t tile,const TileSourceView& source) noexcept
{
    static ProcessPack current;
    const auto* record=current.pack.Match(tile,source);
    if(record && !current.reported[record->tile]) {
        current.reported[record->tile]=true;
        SYNCLOG("HD terrain matched tile %u: %ux%u at unchanged world size (logical %u)",record->tile,record->dimension,record->dimension,tile);
    }
    return record;
}
}
