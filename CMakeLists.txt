# Builds the SDL version of WarpNES. Used by GitHub Actions for the MSVC
# Windows build (SDL2 from vcpkg, see vcpkg.json); works on Linux too.
# The Makefile remains the way to build everything else (Allegro, GTK4,
# MinGW, NSF player); msdos/ has the DOS build.
cmake_minimum_required(VERSION 3.16)
project(warpnes CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Static MSVC runtime: WarpNES.exe needs no Visual C++ redistributable
# (pairs with the x64-windows-static vcpkg triplet, which links SDL2 in)
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

find_package(SDL2 CONFIG REQUIRED)

set(WARPNES_SOURCES
    source/Configuration.cpp
    source/Emulation/APU.cpp
    source/Emulation/PPU.cpp
    source/Zapper.cpp
    source/helper.cpp
    source/Emulation/WarpNES.cpp
    source/Emulation/GameGenie.cpp
    source/Emulation/AllegroMidi.cpp
    source/Emulation/Battery.cpp
    source/Emulation/Instructions.cpp
    source/Emulation/GxROM.cpp
    source/Emulation/UxROM.cpp
    source/Emulation/MMC1.cpp
    source/Emulation/MMC2.cpp
    source/Emulation/MMC3.cpp
    source/Emulation/MMC5.cpp
    source/Emulation/Mappers.cpp
    source/Emulation/Mapper40.cpp
    source/SDLMainWindow.cpp
    source/SDLGui.cpp
    source/SDLInput.cpp
    source/SDLCacheScaling.cpp
    source/Emulation/ControllerSDL.cpp
)
if(WIN32)
    list(APPEND WARPNES_SOURCES res/warpnes.rc)
endif()

add_executable(WarpNES WIN32 ${WARPNES_SOURCES})
target_include_directories(WarpNES PRIVATE source)
target_compile_definitions(WarpNES PRIVATE SDL_BUILD)

if(WIN32)
    target_compile_definitions(WarpNES PRIVATE WIN32 _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS NOMINMAX)
endif()
if(MSVC)
    # /utf-8: the sources are UTF-8; /MP: parallel compile
    target_compile_options(WarpNES PRIVATE /utf-8 /MP /W1)
else()
    target_compile_options(WarpNES PRIVATE -fpermissive -w)
endif()

if(TARGET SDL2::SDL2main)
    target_link_libraries(WarpNES PRIVATE SDL2::SDL2main)
endif()
if(TARGET SDL2::SDL2-static)
    target_link_libraries(WarpNES PRIVATE SDL2::SDL2-static)
else()
    target_link_libraries(WarpNES PRIVATE SDL2::SDL2)
endif()
