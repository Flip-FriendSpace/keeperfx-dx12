# SPDX-License-Identifier: GPL-2.0-or-later
# KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
# Shared runtime library and shaders. Diagnostic hosts and legacy oracles are
# deliberately defined only by the standalone foundation CMakeLists.txt.
include_guard(GLOBAL)
if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8 OR
   NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
    message(FATAL_ERROR "The DX12 runtime targets Windows x64")
endif()
foreach(_required IN ITEMS DX12_DEPS DX12_OUTPUT KFX_REPO)
    if(NOT DEFINED ${_required})
        message(FATAL_ERROR "Missing ${_required}; prepare the pinned DX12 dependencies first")
    endif()
endforeach()
set(DX12_SOURCE "${KFX_REPO}/src/kfx/renderer/d3d12")
set(DX12_HEADERS "${DX12_DEPS}/headers/include")
if(NOT EXISTS "${DX12_DEPS}/allocator/src/D3D12MemAlloc.cpp" OR
   NOT EXISTS "${DX12_HEADERS}/directx/d3d12.h" OR
   NOT EXISTS "${DX12_DEPS}/dxc/bin/dxc")
    message(FATAL_ERROR "Missing pinned SDK files; run local-dev/prepare-dx12-dependencies.ps1")
endif()
add_library(dx12_allocator STATIC "${DX12_DEPS}/allocator/src/D3D12MemAlloc.cpp")
target_include_directories(dx12_allocator SYSTEM PRIVATE
    "${DX12_DEPS}/allocator/include" "${DX12_HEADERS}/directx" "${DX12_HEADERS}/dxguids")
# MinGW rpcndr.h reports 475. Official DirectX-Headers provide the MinGW COM
# struct-return ABI; never substitute MSVC ABI declarations.
target_compile_definitions(dx12_allocator PRIVATE __REQUIRED_RPCNDR_H_VERSION__=475 WIN32_LEAN_AND_MEAN NOMINMAX)
add_library(kfx_dx12 STATIC "${DX12_SOURCE}/Dx12Device.cpp"
    "${DX12_SOURCE}/SceneIndexedFrame.cpp" "${DX12_SOURCE}/OwnedWorldBridge.cpp")
if(KFX_DX12_EMBED_OWNED_ASSETS)
    target_sources(kfx_dx12 PRIVATE
        "${KFX_REPO}/src/kfx/renderer/OwnedRenderAssets.cpp"
        "${KFX_REPO}/src/kfx/renderer/OwnedRenderFrame.cpp"
        "${KFX_REPO}/src/kfx/renderer/PaletteIndexTable.cpp")
endif()
target_include_directories(kfx_dx12 PRIVATE "${KFX_REPO}/src")
target_include_directories(kfx_dx12 PUBLIC "${DX12_SOURCE}")
target_include_directories(kfx_dx12 SYSTEM PRIVATE
    "${DX12_DEPS}/allocator/include" "${DX12_HEADERS}/directx" "${DX12_HEADERS}/dxguids")
target_compile_definitions(kfx_dx12 PRIVATE __REQUIRED_RPCNDR_H_VERSION__=475 WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(kfx_dx12 PUBLIC dx12_allocator d3d12 dxgi dxguid)
if(NOT MSVC)
    target_compile_options(kfx_dx12 PRIVATE -Wall -Wextra -Werror)
endif()

set(DXC "${DX12_DEPS}/dxc/bin/dxc")
set(KFX_DX12_SHADER_OUTPUTS "")
function(kfx_dx12_shader SOURCE OUTPUT PROFILE ENTRY)
    set(_output "${DX12_OUTPUT}/shaders/${OUTPUT}.dxil")
    add_custom_command(OUTPUT "${_output}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${DX12_OUTPUT}/shaders"
        COMMAND "${DXC}" -T ${PROFILE} -E ${ENTRY} ${ARGN} -HV 2021 -Ges -WX -O3 -Zi -Qembed_debug
            -Fo "${_output}" "${DX12_SOURCE}/${SOURCE}.hlsl"
        DEPENDS "${DX12_SOURCE}/${SOURCE}.hlsl" VERBATIM)
    set(KFX_DX12_SHADER_OUTPUTS ${KFX_DX12_SHADER_OUTPUTS} "${_output}" PARENT_SCOPE)
endfunction()
foreach(_stage IN ITEMS vs ps)
    if(_stage STREQUAL "vs")
        set(_entry VS)
    else()
        set(_entry PS)
    endif()
    kfx_dx12_shader(Textured textured.${_stage} ${_stage}_6_0 ${_entry}Main)
    kfx_dx12_shader(Outline outline.${_stage} ${_stage}_6_0 ${_entry}Main)
    kfx_dx12_shader(RuntimeClear runtime-clear.${_stage} ${_stage}_6_0 ${_entry}Main)
    kfx_dx12_shader(EnhancedWorld enhanced-world-terrain.${_stage} ${_stage}_6_0 Terrain${_entry})
    kfx_dx12_shader(EnhancedWorld enhanced-world-shadow.${_stage} ${_stage}_6_0 Shadow${_entry})
    kfx_dx12_shader(Lens lens.${_stage} ${_stage}_6_0 Lens${_entry})
    kfx_dx12_shader(WorldOverlay world-overlay.${_stage} ${_stage}_6_0 WorldOverlay${_entry})
endforeach()
kfx_dx12_shader(Palette palette.vs vs_6_0 VSMain)
kfx_dx12_shader(Palette palette.serial.ps ps_6_0 PSMain)
kfx_dx12_shader(Palette palette.ordered.ps ps_6_0 PSMain -DORDERED_VIEWS=1)
kfx_dx12_shader(IndexedPresent indexed-present.cs cs_6_0 CSMain)
kfx_dx12_shader(RuntimeIndexedPresent runtime-indexed-present.cs cs_6_0 CSMain)
kfx_dx12_shader(RuntimePresent runtime-present.ps ps_6_0 PSMain)
kfx_dx12_shader(MapFade mapfade.ps ps_6_0 MapFadePS)
add_custom_target(kfx_dx12_shaders DEPENDS ${KFX_DX12_SHADER_OUTPUTS})
add_dependencies(kfx_dx12 kfx_dx12_shaders)
