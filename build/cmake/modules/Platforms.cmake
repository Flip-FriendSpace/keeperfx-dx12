# KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
# Platforms.cmake - platform detection + directory-scope compile definitions.

if(WIN32)
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(KFX_MINGW_TRIPLET x86_64-w64-mingw32)
    else()
        set(KFX_MINGW_TRIPLET i686-w64-mingw32)
    endif()
    kfx_status("PLATFORM" "Windows / MinGW-w64 (${KFX_MINGW_TRIPLET})")
elseif(UNIX AND NOT APPLE)
    kfx_status("PLATFORM" "Linux (x86_64)")
else()
    message(FATAL_ERROR "Unsupported platform (only Windows/MinGW and Linux are supported)")
endif()

add_compile_definitions("DEBUG=$<IF:$<CONFIG:Debug>,1,0>")

option(KEEPERFX_TRACY "Enable Tracy profiler instrumentation" OFF)

# Static-linkage defines for the prebuilt Windows dependencies.
if(WIN32)
    add_compile_definitions(_CRT_NONSTDC_NO_WARNINGS _CRT_SECURE_NO_WARNINGS)
    # AL_LIBTYPE_STATIC tells <AL/al.h> not to mark its functions dllimport.
    # Needed when linking the kfx-deps static prebuilt (mirrors the Makefile);
    # not needed -- and not defined -- when vcpkg's openal-soft is in play,
    # which is linked dynamically. See Dependencies.cmake for why the two
    # differ.
    if(NOT VCPKG_TOOLCHAIN)
        add_compile_definitions(AL_LIBTYPE_STATIC)
    endif()
    add_compile_definitions(SPNG_STATIC=1)
    # MSVC's <math.h> only defines M_PI/M_E/etc. when this is set before the
    # first include; MinGW's <math.h> defines them unconditionally regardless,
    # so this is a no-op there. Directory-scope since math.h can be pulled in
    # transitively in an unpredictable order across source files.
    add_compile_definitions(_USE_MATH_DEFINES)
endif()
