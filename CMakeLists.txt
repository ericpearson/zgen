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

# --- Core emulator sources (always compiled) ---
set(CORE_SOURCES
    src/main.cpp
    src/config_dir.cpp
    src/genesis.cpp
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

    set(RENDERER_SOURCES
        src/ui/game_renderer_vk.cpp
        src/ui/vulkan_context.cpp
    )
    set(IMGUI_BACKEND_SOURCES
        third_party/imgui/backends/imgui_impl_vulkan.cpp
    )
    set(RENDERER_LIBS Vulkan::Vulkan)
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

add_executable(genesis
    ${CORE_SOURCES}
    ${RENDERER_SOURCES}
    ${IMGUI_CORE_SOURCES}
    ${IMGUI_BACKEND_SOURCES}
)

target_include_directories(genesis PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/imgui
    ${CMAKE_SOURCE_DIR}/third_party/imgui/backends
)

target_link_libraries(genesis PRIVATE ${SDL3_LIBRARIES} ${RENDERER_LIBS})

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
)

target_include_directories(psg_test PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
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
)

target_include_directories(audio_mix_test PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)

if(MSVC)
    target_compile_options(audio_mix_test PRIVATE /O2)
else()
    target_compile_options(audio_mix_test PRIVATE -O2)
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

# --- CTest integration ---
enable_testing()
add_test(NAME m68k_test COMMAND m68k_test)
add_test(NAME psg_test COMMAND psg_test)
add_test(NAME ym2612_test COMMAND ym2612_test)
add_test(NAME audio_mix_test COMMAND audio_mix_test)
add_test(NAME cheat_engine_test COMMAND cheat_engine_test)
add_test(NAME zexdoc_test COMMAND zexdoc_test ${CMAKE_SOURCE_DIR}/tests/zexdoc.com)
set_tests_properties(zexdoc_test PROPERTIES TIMEOUT 600)
