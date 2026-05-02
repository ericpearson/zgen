# Copyright (C) 2026 pagefault
# SPDX-License-Identifier: GPL-3.0-only

cmake_minimum_required(VERSION 3.16)
project(genesis)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Rendering backend: OpenGL (default) or Vulkan
option(USE_VULKAN "Build with Vulkan renderer instead of OpenGL" OFF)

find_package(SDL3 QUIET CONFIG)
if(NOT SDL3_FOUND)
    find_package(PkgConfig)
    if(PkgConfig_FOUND)
        pkg_check_modules(SDL3 REQUIRED IMPORTED_TARGET sdl3)
        set(SDL3_LIBRARIES PkgConfig::SDL3)
    else()
        message(FATAL_ERROR "SDL3 not found via CMake config or pkg-config.")
    endif()
else()
    set(SDL3_LIBRARIES SDL3::SDL3)
endif()

# --- Genesis app-only sources (everything shared lives in genesis_core) ---
set(GENESIS_OWN_SOURCES
    src/main.cpp
    src/ui/keybindings.cpp
    src/ui/app_ui.cpp
)

# --- Dear ImGui core (always compiled) ---
set(IMGUI_CORE_SOURCES
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_widgets.cpp
    third_party/imgui/backends/imgui_impl_sdl3.cpp
)

# --- Backend-specific sources ---
if(USE_VULKAN)
    find_package(Vulkan REQUIRED)

    # shaderc — runtime GLSL→SPIR-V compilation. Try CMake config (less common
    # on Linux distros) then fall back to pkg-config / direct -lshaderc, which
    # is what apt's libshaderc-dev and MSYS2's mingw-w64-x86_64-shaderc ship.
    find_package(shaderc QUIET CONFIG)
    if(TARGET shaderc::shaderc)
        set(SHADERC_TARGET shaderc::shaderc)
    else()
        find_library(SHADERC_LIB NAMES shaderc_shared shaderc REQUIRED)
        find_path(SHADERC_INC shaderc/shaderc.hpp REQUIRED)
        add_library(shaderc_runtime INTERFACE)
        target_link_libraries(shaderc_runtime INTERFACE ${SHADERC_LIB})
        target_include_directories(shaderc_runtime INTERFACE ${SHADERC_INC})
        set(SHADERC_TARGET shaderc_runtime)
    endif()

    set(RENDERER_SOURCES
        src/ui/game_renderer_vk.cpp
        src/ui/vulkan_context.cpp
    )
    set(IMGUI_BACKEND_SOURCES
        third_party/imgui/backends/imgui_impl_vulkan.cpp
    )
    set(RENDERER_LIBS Vulkan::Vulkan ${SHADERC_TARGET})
    set(RENDERER_DEFS USE_VULKAN)
else()
    find_package(OpenGL REQUIRED)

    set(RENDERER_SOURCES
        src/ui/game_renderer_gl.cpp
    )
    set(IMGUI_BACKEND_SOURCES
        third_party/imgui/backends/imgui_impl_opengl3.cpp
    )
    if(NOT APPLE)
        find_package(GLEW REQUIRED)
        set(RENDERER_LIBS OpenGL::GL GLEW::GLEW)
    else()
        set(RENDERER_LIBS OpenGL::GL)
    endif()
    set(RENDERER_DEFS "")
endif()

# --- Shared core static library ---
# Compiled once and linked by `genesis` plus every heavy test target. This
# replaces the per-target source-list duplication that previously caused
# each shared file to be recompiled six times on every clean build.
add_library(genesis_core STATIC
    src/genesis.cpp
    src/config_dir.cpp
    src/debug_flags.cpp
    src/cpu/m68k.cpp
    src/cpu/m68k_opcodes.cpp
    src/cpu/m68k_ops_data.cpp
    src/cpu/m68k_ops_arith.cpp
    src/cpu/m68k_ops_logic.cpp
    src/cpu/m68k_ops_branch.cpp
    src/cpu/m68k_ops_shift.cpp
    src/cpu/z80.cpp
    src/video/vdp.cpp
    src/video/vdp_render.cpp
    src/video/vdp_dma.cpp
    src/video/vdp_scanline.cpp
    src/memory/bus.cpp
    src/memory/cartridge.cpp
    src/audio/ym2612.cpp
    src/audio/psg.cpp
    src/cheats/cheat_types.cpp
    src/cheats/cheat_format.cpp
    src/cheats/cheat_file.cpp
    src/cheats/genesis_cheats.cpp
    src/cheats/ram_search.cpp
    src/cheats/cheat_engine.cpp
    third_party/blip_buf/blip_buf.c
)

target_include_directories(genesis_core PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/blip_buf
)

if(MSVC)
    target_compile_options(genesis_core PRIVATE
        $<$<CONFIG:Debug>:/Od /Zi>
        $<$<NOT:$<CONFIG:Debug>>:/O2>)
else()
    target_compile_options(genesis_core PRIVATE
        $<$<CONFIG:Debug>:-g -O0>
        $<$<NOT:$<CONFIG:Debug>>:-O3>)
endif()

add_executable(genesis
    ${GENESIS_OWN_SOURCES}
    ${RENDERER_SOURCES}
    ${IMGUI_CORE_SOURCES}
    ${IMGUI_BACKEND_SOURCES}
)

target_include_directories(genesis PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/imgui
    ${CMAKE_SOURCE_DIR}/third_party/imgui/backends
)

target_link_libraries(genesis PRIVATE genesis_core ${SDL3_LIBRARIES} ${RENDERER_LIBS})

# On Windows, build as a GUI application (no console window).
# SDL3's SDL_main.h provides the WinMain → main entry point shim.
if(WIN32)
    set_target_properties(genesis PROPERTIES WIN32_EXECUTABLE TRUE)
endif()

target_compile_definitions(genesis PRIVATE
    GL_SILENCE_DEPRECATION
    ${RENDERER_DEFS}
)

if(MSVC)
    target_compile_options(genesis PRIVATE $<$<CONFIG:Debug>:/Od /Zi> $<$<CONFIG:Release>:/O2>)
else()
    target_compile_options(genesis PRIVATE $<$<CONFIG:Debug>:-g -O0> $<$<NOT:$<CONFIG:Debug>>:-O3>)
endif()

# Z80 conformance test (ZEXDOC)
add_executable(zexdoc_test
    tests/zexdoc_test.cpp
    src/cpu/z80.cpp
)

target_include_directories(zexdoc_test PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)

if(MSVC)
    target_compile_options(zexdoc_test PRIVATE /O2)
else()
    target_compile_options(zexdoc_test PRIVATE -O3)
endif()

# M68K CPU instruction test suite
add_executable(m68k_test
    tests/m68k_test.cpp
    src/cpu/m68k.cpp
    src/cpu/m68k_opcodes.cpp
    src/cpu/m68k_ops_data.cpp
    src/cpu/m68k_ops_arith.cpp
    src/cpu/m68k_ops_logic.cpp
    src/cpu/m68k_ops_shift.cpp
    src/cpu/m68k_ops_branch.cpp
)

target_include_directories(m68k_test PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)

if(MSVC)
    target_compile_options(m68k_test PRIVATE /O2)
else()
    target_compile_options(m68k_test PRIVATE -O2)
endif()

# PSG (SN76489) unit test suite
add_executable(psg_test
    tests/psg_test.cpp
    src/audio/psg.cpp
    third_party/blip_buf/blip_buf.c
)

target_include_directories(psg_test PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/blip_buf
)

if(MSVC)
    target_compile_options(psg_test PRIVATE /O2)
else()
    target_compile_options(psg_test PRIVATE -O2)
endif()

add_executable(ym2612_test
    tests/ym2612_test.cpp
    src/audio/ym2612.cpp
)

target_include_directories(ym2612_test PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)

if(MSVC)
    target_compile_options(ym2612_test PRIVATE /O2)
else()
    target_compile_options(ym2612_test PRIVATE -O2)
endif()

# Audio mixing test suite
add_executable(audio_mix_test
    tests/audio_mix_test.cpp
    src/audio/psg.cpp
    src/audio/ym2612.cpp
    third_party/blip_buf/blip_buf.c
)

target_include_directories(audio_mix_test PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/blip_buf
)

if(MSVC)
    target_compile_options(audio_mix_test PRIVATE /O2)
else()
    target_compile_options(audio_mix_test PRIVATE -O2)
endif()

add_executable(audio_queue_test
    tests/audio_queue_test.cpp
)

target_include_directories(audio_queue_test PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)

if(MSVC)
    target_compile_options(audio_queue_test PRIVATE /O2)
else()
    target_compile_options(audio_queue_test PRIVATE -O2)
endif()

add_executable(cheat_engine_test
    tests/cheat_engine_test.cpp
    src/cheats/cheat_types.cpp
    src/cheats/cheat_format.cpp
    src/cheats/cheat_file.cpp
    src/cheats/ram_search.cpp
    src/cheats/cheat_engine.cpp
)

target_include_directories(cheat_engine_test PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)

if(MSVC)
    target_compile_options(cheat_engine_test PRIVATE /O2)
else()
    target_compile_options(cheat_engine_test PRIVATE -O2)
endif()

add_executable(cartridge_test
    tests/cartridge_test.cpp
    src/memory/cartridge.cpp
)

target_include_directories(cartridge_test PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)

if(MSVC)
    target_compile_options(cartridge_test PRIVATE /O2)
else()
    target_compile_options(cartridge_test PRIVATE -O2)
endif()

add_executable(cpz_trace tests/cpz_trace.cpp)
target_link_libraries(cpz_trace PRIVATE genesis_core)
target_compile_definitions(cpz_trace PRIVATE GL_SILENCE_DEPRECATION)
if(MSVC)
    target_compile_options(cpz_trace PRIVATE
        $<$<CONFIG:Debug>:/Od /Zi>
        $<$<NOT:$<CONFIG:Debug>>:/O2>)
else()
    target_compile_options(cpz_trace PRIVATE
        $<$<CONFIG:Debug>:-g -O0>
        $<$<NOT:$<CONFIG:Debug>>:-O3>)
endif()

add_executable(vdp_state_dump tests/vdp_state_dump.cpp)
target_link_libraries(vdp_state_dump PRIVATE genesis_core)
target_compile_definitions(vdp_state_dump PRIVATE GL_SILENCE_DEPRECATION)
if(MSVC)
    target_compile_options(vdp_state_dump PRIVATE
        $<$<CONFIG:Debug>:/Od /Zi>
        $<$<NOT:$<CONFIG:Debug>>:/O2>)
else()
    target_compile_options(vdp_state_dump PRIVATE
        $<$<CONFIG:Debug>:-g -O0>
        $<$<NOT:$<CONFIG:Debug>>:-O3>)
endif()

# --- CTest integration ---
enable_testing()
add_test(NAME m68k_test COMMAND m68k_test)
add_test(NAME psg_test COMMAND psg_test)
add_test(NAME ym2612_test COMMAND ym2612_test)
add_test(NAME audio_mix_test COMMAND audio_mix_test)
add_test(NAME audio_queue_test COMMAND audio_queue_test)
add_test(NAME cheat_engine_test COMMAND cheat_engine_test)
add_test(NAME cartridge_test COMMAND cartridge_test)
add_test(NAME zexdoc_test COMMAND zexdoc_test ${CMAKE_SOURCE_DIR}/tests/zexdoc.com)
set_tests_properties(zexdoc_test PROPERTIES TIMEOUT 600)

# Bus timing test suite (Z80 bank access, 68K Z80-area wait states)
add_executable(bus_timing_test tests/bus_timing_test.cpp)
target_link_libraries(bus_timing_test PRIVATE genesis_core)
if(MSVC)
    target_compile_options(bus_timing_test PRIVATE
        $<$<CONFIG:Debug>:/Od /Zi>
        $<$<NOT:$<CONFIG:Debug>>:/O2>)
else()
    target_compile_options(bus_timing_test PRIVATE
        $<$<CONFIG:Debug>:-g -O0>
        $<$<NOT:$<CONFIG:Debug>>:-O3>)
endif()

add_test(NAME bus_timing_test COMMAND bus_timing_test)

# M68K refresh-accounting test — locks the per-frame cycle count to the
# NTSC H40 wall-clock baseline (128006 M68K cycles = 896040 MCLK / 7) and
# verifies refresh is only charged at VDP/IO access sites.
add_executable(m68k_refresh_test tests/m68k_refresh_test.cpp)
target_link_libraries(m68k_refresh_test PRIVATE genesis_core)
if(MSVC)
    target_compile_options(m68k_refresh_test PRIVATE
        $<$<CONFIG:Debug>:/Od /Zi>
        $<$<NOT:$<CONFIG:Debug>>:/O2>)
else()
    target_compile_options(m68k_refresh_test PRIVATE
        $<$<CONFIG:Debug>:-g -O0>
        $<$<NOT:$<CONFIG:Debug>>:-O3>)
endif()

add_test(NAME m68k_refresh_test COMMAND m68k_refresh_test WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set_tests_properties(m68k_refresh_test PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)

# VDP per-column vscroll runtime test — exercises the slot-driven
# columnVscroll[] capture pipeline with a mid-line vsram change, proving
# late columns see post-change values while early columns retain the
# pre-change capture. Required by the per-column renderer switch.
add_executable(vdp_vscroll_test tests/vdp_vscroll_test.cpp)
target_link_libraries(vdp_vscroll_test PRIVATE genesis_core)
if(MSVC)
    target_compile_options(vdp_vscroll_test PRIVATE
        $<$<CONFIG:Debug>:/Od /Zi>
        $<$<NOT:$<CONFIG:Debug>>:/O2>)
else()
    target_compile_options(vdp_vscroll_test PRIVATE
        $<$<CONFIG:Debug>:-g -O0>
        $<$<NOT:$<CONFIG:Debug>>:-O3>)
endif()

add_test(NAME vdp_vscroll_test COMMAND vdp_vscroll_test)

add_executable(vdp_slot_table_test
    tests/vdp_slot_table_test.cpp
)

target_include_directories(vdp_slot_table_test PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)

if(MSVC)
    target_compile_options(vdp_slot_table_test PRIVATE /O2)
else()
    target_compile_options(vdp_slot_table_test PRIVATE -O2)
endif()

add_test(NAME vdp_slot_table_test COMMAND vdp_slot_table_test)
