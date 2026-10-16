cmake_minimum_required(VERSION 3.22)
project(vbcgx_core LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(REPO ${CMAKE_CURRENT_SOURCE_DIR}/vendor/vboycolor)
set(VB_CORE_DIR ${REPO}/third_party/beetle-vb-libretro)
set(GENERATED ${CMAKE_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${GENERATED})
file(WRITE ${GENERATED}/Version.h "static constexpr const char *kGeneratedVersionString = \"0.2.73-dev\";\n")
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
    VB_STATIC_MEMORY MEDNAFEN_VERSION="0.9.31" MEDNAFEN_VERSION_NUMERIC=931 RETRO_API=)
if(MSVC)
    target_compile_definitions(beetle_vb PRIVATE INLINE=_inline NOMINMAX _CRT_SECURE_NO_DEPRECATE)
    target_compile_options(beetle_vb PRIVATE /W0 /FI${CMAKE_CURRENT_SOURCE_DIR}/beetle_rename.h)
else()
    target_compile_definitions(beetle_vb PRIVATE INLINE=inline)
    target_compile_options(beetle_vb PRIVATE -w -include ${REPO}/platform/libretro/beetle_rename.h)
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


add_library(vboycolor STATIC
    ${REPO}/platform/libretro/libretro_core.cpp
    ${PACKS_C}
    ${REPO}/core/emu/ShadeColorizer.cpp
    ${REPO}/core/emu/TileColorPack.cpp
    ${REPO}/core/emu/ColorPackRenderer.cpp
    ${REPO}/core/emu/DepthColors.cpp)
target_include_directories(vboycolor PUBLIC ${REPO}/core ${REPO}/core/emu ${REPO}/third_party ${VB_CORE_DIR}/libretro-common/include ${GENERATED})
target_compile_options(vboycolor PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-include cstdint>)
option(VBC_HOST_TESTS "Build native core tests" OFF)
if(VBC_HOST_TESTS)
    add_executable(core-smoke tests/core-smoke.cpp)
    target_link_libraries(core-smoke PRIVATE vboycolor beetle_vb m)
    add_executable(input-check tests/input.cpp)
    target_include_directories(input-check PRIVATE source)
    add_executable(video-check tests/video.cpp)
    target_include_directories(video-check PRIVATE source)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Generic" AND CMAKE_SYSTEM_PROCESSOR STREQUAL "powerpc")
    set_target_properties(beetle_vb vboycolor PROPERTIES POSITION_INDEPENDENT_CODE OFF INTERPROCEDURAL_OPTIMIZATION TRUE)
    set_source_files_properties(${VB_CORE_DIR}/mednafen/hw_cpu/v810/v810_cpu.cpp PROPERTIES
        COMPILE_OPTIONS "-fno-gcse;-fno-crossjumping;-fno-tree-tail-merge;--param=max-goto-duplication-insns=64")
endif()
