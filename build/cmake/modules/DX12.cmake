# SPDX-License-Identifier: GPL-2.0-or-later
# KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
# Opt-in developer runtime; no diagnostic/oracle host is linked into the game.
set(KFX_REPO "${CMAKE_SOURCE_DIR}")
set(DX12_DEPS "" CACHE PATH "Prepared pinned DX12 dependency directory")
set(DX12_OUTPUT "${CMAKE_BINARY_DIR}/dx12-runtime")
set(KFX_DX12_EMBED_OWNED_ASSETS OFF)
include("${CMAKE_SOURCE_DIR}/local-dev/dx12/Runtime.cmake")
foreach(_target IN LISTS KFX_TARGETS)
    if(_target STREQUAL "keeperfx")
        set_target_properties(${_target} PROPERTIES OUTPUT_NAME keeperfx-dx12)
    else()
        set_target_properties(${_target} PROPERTIES OUTPUT_NAME keeperfx-dx12-hvlog)
    endif()
    target_compile_definitions(${_target} PRIVATE KFX_ENABLE_DX12=1)
    target_sources(${_target} PRIVATE "${CMAKE_SOURCE_DIR}/local-dev/dx12/AgilityExports.cpp")
    target_link_libraries(${_target} PRIVATE kfx_dx12)
    add_custom_command(TARGET ${_target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${_target}>/D3D12"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${DX12_DEPS}/agility/build/native/bin/x64/D3D12Core.dll"
            "${DX12_DEPS}/agility/build/native/bin/x64/d3d12SDKLayers.dll"
            "$<TARGET_FILE_DIR:${_target}>/D3D12"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${DX12_OUTPUT}/shaders" "$<TARGET_FILE_DIR:${_target}>/shaders"
        VERBATIM)
endforeach()
