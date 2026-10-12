# VBoy Color as a libretro core (RetroArch and the other libretro frontends):
# Beetle VB with the app's coloring - see libretro_core.cpp. Its own project,
# apart from the app's build:
#
#   cmake -S platform/libretro -B build/libretro -DCMAKE_BUILD_TYPE=Release
#   cmake --build build/libretro -j
#
# gives vboycolor_libretro.so (.dll on Windows, .dylib on macOS; Android:
# configure with the NDK's toolchain file) - copy it into the frontend's
# cores folder, and vboycolor_libretro.info into its info folder.
cmake_minimum_required(VERSION 3.22)
project(vboycolor_libretro LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()
if(MSVC)
    add_compile_options(/utf-8)
    # (the C runtime built in: nothing to install beside the core)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# The C++ runtime linked into the core (Linux: no libstdc++ version to
# match). Off for a manylinux build (CI's Linux cores), whose compiler
# already links what's newer than the old system's libstdc++ in.
option(VBC_STATIC_RUNTIME "Link the C++ runtime into the core" ON)

get_filename_component(REPO ${CMAKE_CURRENT_SOURCE_DIR}/../.. ABSOLUTE)
set(VB_CORE_DIR ${REPO}/third_party/beetle-vb-libretro)

# The version, as the app's (cmake/GenerateVersion.cmake: v1.0.0-dev.N, or
# the release's name).
set(VBGO_VERSION "1.0.0" CACHE STRING "The release's version (as the app's CMakeLists.txt)")
option(VBGO_RELEASE_BUILD "This is an official numbered release, not a dev build" OFF)
set(VBGO_PRERELEASE "" CACHE STRING "Pre-release label, e.g. beta.7 (empty: none)")
set(GENERATED ${CMAKE_BINARY_DIR}/generated)
add_custom_target(vboycolor_version ALL
    COMMAND ${CMAKE_COMMAND} -DSRC_DIR=${REPO} -DOUT_FILE=${GENERATED}/Version.h -DVBGO_VERSION=${VBGO_VERSION}
            -DVBGO_RELEASE_BUILD=${VBGO_RELEASE_BUILD} -DVBGO_PRERELEASE=${VBGO_PRERELEASE}
            -P ${REPO}/cmake/GenerateVersion.cmake
    BYPRODUCTS ${GENERATED}/Version.h)

# ---- Beetle VB (its retro_* entry points renamed beetle_retro_*) ----
include(${REPO}/cmake/PatchBeetleVip.cmake)
vbgo_generate_patched_vip(${VB_CORE_DIR} ${GENERATED}/beetle-vb)

add_library(beetle_vb STATIC
    ${VB_CORE_DIR}/mednafen/hw_cpu/v810/v810_cpu.cpp
    ${VB_CORE_DIR}/mednafen/mempatcher.cpp
    ${VB_CORE_DIR}/libretro.cpp
    ${VB_CORE_DIR}/mednafen/vb/vsu.c
    ${VB_CORE_DIR}/mednafen/vb/input.c
    ${VB_CORE_DIR}/mednafen/vb/timer.c
    ${VBGO_PATCHED_VIP_SOURCE}
    ${VB_CORE_DIR}/mednafen/hw_cpu/v810/fpu-new/softfloat.c
    ${VB_CORE_DIR}/mednafen/sound/Blip_Buffer.c
    ${VB_CORE_DIR}/mednafen/state.c
    ${VB_CORE_DIR}/mednafen/settings.c
    ${VB_CORE_DIR}/libretro-common/compat/compat_strl.c
    ${VB_CORE_DIR}/libretro-common/compat/compat_snprintf.c
    ${REPO}/core/emu/vbgo_tiletrack.c)
set_target_properties(beetle_vb PROPERTIES POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden
    CXX_VISIBILITY_PRESET hidden)
target_include_directories(beetle_vb PUBLIC ${VB_CORE_DIR} ${VB_CORE_DIR}/mednafen
    ${VB_CORE_DIR}/mednafen/include ${VB_CORE_DIR}/libretro-common/include ${REPO}/core/emu)
target_include_directories(beetle_vb PRIVATE ${VB_CORE_DIR}/mednafen/vb)
# (RETRO_API empty: Beetle's entry points stay inside the core - only ours are exported)
target_compile_definitions(beetle_vb PRIVATE WANT_32BPP __LIBRETRO__ STDC_HEADERS __STDC_LIMIT_MACROS
    MEDNAFEN_VERSION="0.9.31" MEDNAFEN_VERSION_NUMERIC=931 RETRO_API=)
if(MSVC)
    target_compile_definitions(beetle_vb PRIVATE INLINE=_inline NOMINMAX _CRT_SECURE_NO_DEPRECATE)
    target_compile_options(beetle_vb PRIVATE /W0 /FI${CMAKE_CURRENT_SOURCE_DIR}/beetle_rename.h)
else()
    target_compile_definitions(beetle_vb PRIVATE INLINE=inline)
    target_compile_options(beetle_vb PRIVATE -w -include ${CMAKE_CURRENT_SOURCE_DIR}/beetle_rename.h)
endif()

# ---- The color packs, built in ----
file(GLOB PACK_FILES CONFIGURE_DEPENDS ${REPO}/colorpacks/*.vbcp)
set(PACKS_C ${GENERATED}/vbc_packs.c)
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/packs)
add_custom_command(OUTPUT ${PACKS_C}
    COMMAND ${CMAKE_COMMAND} -DPACK_DIR=${REPO}/colorpacks -DOUT=${PACKS_C} -P ${REPO}/cmake/EmbedColorPacks.cmake
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/packs
    DEPENDS ${PACK_FILES} ${REPO}/colorpacks/aliases.txt ${REPO}/cmake/EmbedColorPacks.cmake
    COMMENT "Zipping the color packs into the core")

# ---- The core ----
add_library(vboycolor_libretro SHARED
    libretro_core.cpp
    ${PACKS_C}
    ${REPO}/core/emu/ShadeColorizer.cpp
    ${REPO}/core/emu/TileColorPack.cpp
    ${REPO}/core/emu/ColorPackRenderer.cpp
    ${REPO}/core/emu/DepthColors.cpp)
add_dependencies(vboycolor_libretro vboycolor_version)
set_target_properties(vboycolor_libretro PROPERTIES PREFIX "" C_VISIBILITY_PRESET hidden
    CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
if(ANDROID)
    # (RetroArch for Android's name for a core)
    set_target_properties(vboycolor_libretro PROPERTIES OUTPUT_NAME vboycolor_libretro_android)
endif()
target_include_directories(vboycolor_libretro PRIVATE ${REPO}/core ${REPO}/core/emu ${REPO}/third_party ${GENERATED})
if(NOT MSVC)
    target_compile_options(vboycolor_libretro PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-include cstdint>)
endif()
target_link_libraries(vboycolor_libretro PRIVATE beetle_vb)
if(UNIX AND NOT APPLE)
    # Only retro_* leave the core; no C++ runtime of its own to clash with the frontend's.
    target_link_options(vboycolor_libretro PRIVATE -Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/link.T
        -Wl,--no-undefined)
    if(NOT ANDROID AND VBC_STATIC_RUNTIME)
        target_link_options(vboycolor_libretro PRIVATE -static-libgcc -static-libstdc++)
    endif()
    target_link_libraries(vboycolor_libretro PRIVATE m)
elseif(WIN32 AND NOT MSVC)
    target_link_options(vboycolor_libretro PRIVATE -static-libgcc -static-libstdc++ -static)
endif()
if(ANDROID)
    target_link_libraries(vboycolor_libretro PRIVATE log)
endif()
