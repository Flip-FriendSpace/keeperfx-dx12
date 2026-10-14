cmake_minimum_required(VERSION 3.20)
project(keeperfx_dx12_foundation LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "The DX12 foundation targets Windows x64")
endif()
if(NOT DEFINED DX12_DEPS OR NOT DEFINED DX12_OUTPUT)
    message(FATAL_ERROR "Use local-dev/dx12-foundation.ps1 to prepare the pinned SDK")
endif()
get_filename_component(KFX_REPO "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${DX12_OUTPUT}")
set(KFX_DX12_EMBED_OWNED_ASSETS ON)
include("${CMAKE_CURRENT_SOURCE_DIR}/Runtime.cmake")
add_executable(keeperfx-dx12 main.cpp PaletteTests.cpp SpriteReplayTests.cpp TerrainReplayTests.cpp ShadowOperatorTests.cpp ShadowEdgeTests.cpp LegacySpriteOracle.cpp LegacyTerrainOracle.cpp LegacyShadowOracle.cpp
    OutlineReplayTests.cpp OutlineOperatorTests.cpp OutlineGLOracle.cpp EnhancedSpriteOperatorTests.cpp EnhancedSpriteReplayTests.cpp
    EnhancedWorldOperatorTests.cpp EnhancedWorldReplayTests.cpp EnhancedWorldGLOracle.cpp
    LensOperatorTests.cpp LensGLOracle.cpp
    WorldOverlayOperatorTests.cpp WorldOverlayGLOracle.cpp
    CompositionOperatorTests.cpp
    FrozenFrameTests.cpp FrozenWorld.cpp IndexedFallbackReplay.cpp RuntimeResourceTests.cpp RuntimeWorldTests.cpp RuntimeReclamationTests.cpp RuntimeStagingTests.cpp RuntimeTimingTests.cpp RuntimeLightmapTests.cpp RuntimeProcessBudgetTests.cpp LegacyWorldOracle.cpp "${KFX_REPO}/deps/glad/src/glad.c"
    "${KFX_REPO}/src/kfx/renderer/WorldScene.cpp"
    "${KFX_REPO}/src/kfx/renderer/SceneAssets.cpp"
    "${KFX_REPO}/src/kfx/renderer/TerrainMesh.cpp"
    "${KFX_REPO}/src/kfx/renderer/SceneCamera.cpp"
    "${KFX_REPO}/src/kfx/renderer/SceneSprites.cpp"
    "${KFX_REPO}/src/kfx/renderer/SceneSpriteReplay.cpp"
    "${KFX_REPO}/src/kfx/renderer/SceneCaptureCodec.cpp")
target_include_directories(keeperfx-dx12 PRIVATE "${KFX_REPO}/src" "${KFX_REPO}/deps/glad/include")
# Match the runtime's pinned MinGW COM declarations for direct adapter queries.
target_include_directories(keeperfx-dx12 SYSTEM PRIVATE
    "${DX12_HEADERS}/directx" "${DX12_HEADERS}/dxguids")
target_compile_definitions(keeperfx-dx12 PRIVATE __REQUIRED_RPCNDR_H_VERSION__=475)
target_link_libraries(keeperfx-dx12 PRIVATE kfx_dx12 user32 opengl32 gdi32 dwmapi)
target_compile_options(keeperfx-dx12 PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(keeperfx-dx12 PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_options(keeperfx-dx12 PRIVATE -static -municode)
# Native-stride original-C diagnostics validate row-copy reads before calling
# the unchanged kernel. Historical packed diagnostics delegate unchanged.
target_link_options(keeperfx-dx12 PRIVATE "-Wl,--wrap=LbPixelBlockCopyForward")

# Link the actual C sprite rasterizer into the diagnostic host, with isolated
# draw-target globals. Section GC excludes unrelated engine/UI dependencies.
file(GLOB GAME_SDL_HINTS "/opt/keeperfx-build/x64/RelWithDebInfo/deps/sdl3/*/x86_64-w64-mingw32/include")
find_path(GAME_SDL_INCLUDE SDL3/SDL.h HINTS ${GAME_SDL_HINTS} REQUIRED NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
set(LEGACY_SPRITE_SOURCE "${KFX_REPO}/src/kfx/renderer/software")
find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(CORRECTED_ORACLE "${CMAKE_CURRENT_BINARY_DIR}/CorrectedSpriteOracle.c")
add_custom_command(OUTPUT "${CORRECTED_ORACLE}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/generate-sprite-oracle.py"
        --repo "${KFX_REPO}" --output "${CORRECTED_ORACLE}"
    DEPENDS generate-sprite-oracle.py
        "${LEGACY_SPRITE_SOURCE}/bflib_vidraw_spr_norm.c"
        "${LEGACY_SPRITE_SOURCE}/bflib_vidraw_spr_remp.c" VERBATIM)
add_library(legacy_sprite_oracle OBJECT LegacySpriteOracleShim.c "${CORRECTED_ORACLE}"
    "${LEGACY_SPRITE_SOURCE}/bflib_vidraw.c"
    "${LEGACY_SPRITE_SOURCE}/bflib_vidraw_spr_norm.c"
    "${LEGACY_SPRITE_SOURCE}/bflib_vidraw_spr_remp.c")
target_include_directories(legacy_sprite_oracle PRIVATE "${KFX_REPO}/src" "${GAME_SDL_INCLUDE}")
target_compile_definitions(legacy_sprite_oracle PRIVATE BFDEBUG_LEVEL=0)
target_compile_options(legacy_sprite_oracle PRIVATE -ffunction-sections -fdata-sections -fwrapv -fno-strict-aliasing)
target_link_libraries(keeperfx-dx12 PRIVATE legacy_sprite_oracle)
add_library(legacy_terrain_oracle OBJECT LegacyTerrainOracleShim.c LegacyTerrainTrigOracleShim.c)
# The original Trig implementation requires compilation without optimization.
set_source_files_properties(LegacyTerrainTrigOracleShim.c PROPERTIES COMPILE_OPTIONS "-O0")
target_include_directories(legacy_terrain_oracle PRIVATE "${KFX_REPO}/src" "${GAME_SDL_INCLUDE}")
target_compile_definitions(legacy_terrain_oracle PRIVATE BFDEBUG_LEVEL=0)
target_compile_options(legacy_terrain_oracle PRIVATE -ffunction-sections -fdata-sections -fwrapv -fno-strict-aliasing)
target_link_libraries(keeperfx-dx12 PRIVATE legacy_terrain_oracle)
target_link_options(keeperfx-dx12 PRIVATE -Wl,--gc-sections)

add_executable(keeperfx-native-target-test LegacyNativeTargetTests.cpp LegacyWorldOracle.cpp
    LegacySpriteOracle.cpp LegacyTerrainOracle.cpp LegacyShadowOracle.cpp
    "${KFX_REPO}/src/kfx/renderer/WorldScene.cpp"
    "${KFX_REPO}/src/kfx/renderer/SceneAssets.cpp"
    "${KFX_REPO}/src/kfx/renderer/SceneCaptureCodec.cpp"
    "${KFX_REPO}/src/kfx/renderer/SceneSpriteReplay.cpp")
target_include_directories(keeperfx-native-target-test PRIVATE "${KFX_REPO}/src")
target_compile_options(keeperfx-native-target-test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(keeperfx-native-target-test PRIVATE legacy_sprite_oracle legacy_terrain_oracle)
target_link_options(keeperfx-native-target-test PRIVATE -static -Wl,--gc-sections "-Wl,--wrap=LbPixelBlockCopyForward")
