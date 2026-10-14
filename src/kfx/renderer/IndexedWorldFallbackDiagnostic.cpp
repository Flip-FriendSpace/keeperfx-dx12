// SPDX-License-Identifier: GPL-2.0-or-later
// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
#include "pre_inc.h"
#include "kfx/renderer/IndexedWorldFallbackDiagnostic.h"
#include "kfx/renderer/IndexedWorldFallbackCodec.h"
#include "game_legacy.h"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include "post_inc.h"

extern "C" int WorldSceneWriteIndexedFallback(void)
{
    const char* directory = std::getenv("KFX_SCENE_TEST_DIR");
    if (!directory || !*directory || (local_system_flags & GSF_NetworkActive)
        || !(game.operation_flags & GOF_Paused)) return false;
    const auto frame = RendererGetIndexedWorldFallbackDiagnostic();
    if (!frame) return false;
    try {
        const auto bytes = kfx::render::EncodeIndexedWorldFallback(*frame);
        const auto filename = "indexed-fallback-" + std::to_string(frame->sequence) + ".kfxidx";
        const auto target = std::filesystem::path(directory) / filename;
        const auto temporary = std::filesystem::path(target.string() + ".partial");
        if (std::filesystem::exists(target) || std::filesystem::exists(temporary))
            throw std::runtime_error("Indexed fallback diagnostic output already exists");
        std::ofstream output(temporary, std::ios::binary);
        output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        output.close();
        if (!output) throw std::runtime_error("Indexed fallback diagnostic write failed");
        std::filesystem::rename(temporary, target);
        SYNCLOG("Owned indexed fallback written: %s", filename.c_str());
        return true;
    } catch (const std::exception& error) {
        ERRORLOG("Indexed fallback diagnostic rejected: %s", error.what());
        return false;
    }
}
