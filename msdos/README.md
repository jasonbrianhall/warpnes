WarpNES (SDL version) for MS-DOS
================================

This is the SDL build of WarpNES (source/SDLMainWindow.cpp), built for MS-DOS
as a 32-bit protected-mode (DPMI) program on SDL3, whose DOS port provides the
video (VESA and VGA), the Sound Blaster, the keyboard and a gameport joystick.

It is separate from the Allegro 4 DOS build (../build_dos.sh, dos_main.cpp).

The build setup (SDL3 with its DOS port, the generated SDL config headers,
CWSDPMI and the Makefile layout) is copied from SDLPoP-DOS's msdos/ folder.


Running it
----------

    C:\WARPNES> WARPNES.EXE GAME.NES
    C:\WARPNES> WARPNES.EXE GAME.NES > LOG.TXT   keep the messages (video mode, ROM info)

The DPMI host (CWSDPMI) is built into the EXE; under Windows 9x or with
HDPMI32 loaded, that one is used instead.

Requirements:

  * 486DX or better; realistically a fast Pentium (the emulator is not light).
    In DOSBox, `cycles=max`.
  * 16 MB of RAM or more.
  * VGA. A VESA BIOS (or UniVBE) gives hicolor modes; without one the
    emulator runs in VGA mode 13h, 256 colours.
  * Optional: Sound Blaster (1.x/2.0, Pro, 16) or compatible, with the BLASTER
    environment variable set (e.g. `SET BLASTER=A220 I7 D1 H5 T6`).
  * Optional: gameport joystick.


Video modes
-----------

By default the emulator picks the smallest hicolor (VESA) mode the 256x240
NES picture fits in twice over -- normally 640x480 -- and draws it doubled,
with sharp pixels. If there is none, the smallest hicolor mode it fits in
(e.g. 320x240), and with no VESA hicolor modes at all, VGA mode 13h
(320x200: the top and bottom 20 lines, mostly overscan, are cut off).

16-bit modes are the fastest: the emulator's frames are already RGB565.

To choose, set WARPNES_VIDEO to WIDTHxHEIGHTxBITS before starting:

    SET WARPNES_VIDEO=320x240x16     least to copy (fastest on slow machines)
    SET WARPNES_VIDEO=640x480x16     picture doubled
    SET WARPNES_VIDEO=320x200x8      VGA mode 13h


Keys
----

The same as the SDL build elsewhere: arrows, X = A, Z = B, [ = Select,
] = Start; player 2 on WASD / K / J / U / I. F5-F8 save state (Shift+F5-F8
load), R resets, Esc quits. There is no fullscreen toggle (F11/F): it is
always full screen.

Save states use 8.3 names (SAVE1.SAV, ...); the settings file smbc.conf is
SMBC.CON on plain DOS.


Building
--------

Everything is in this folder; no CMake, nothing is downloaded. The compiler
is a DJGPP cross compiler on the PATH (`i586-pc-msdosdjgpp-gcc`/`g++`;
prebuilt ones: https://github.com/andrewwutw/build-djgpp/releases) when there
is one, otherwise the `djfdyuruiry/djgpp` Docker image (pulled
automatically). Force either with `make DOCKER=1` / `make DOCKER=0`. Plus
GNU make.

    cd msdos
    make                  # SDL3 (into build/libSDL3.a), the emulator, warpnes.exe with CWSDPMI inside
    make run ROM=SMB.NES  # play it in DOSBox (ROM in this folder), this folder as C:
    make dist             # build/dist/WARPNES: the files to copy to a DOS machine
    make clean

`make msdos_original` makes a warpnes.exe that needs CWSDPMI.EXE (copied
next to it) or another DPMI host.

What is here:

  * SDL3/ -- the complete SDL3 source (https://libsdl.org, commit 4ca1b1c),
    which includes the DOS port, with SDLPoP-DOS's small fixes to it (marked
    "SDLPoP-DOS": keyboard and Sound Blaster interrupt handlers not chained to
    the BIOS, the mouse driver polled at most 100 times a second, a
    performance counter that never goes backwards). The Makefile compiles
    the files SDL's own build uses for DJGPP.
  * sdl3-config/ -- SDL_build_config.h and SDL_revision.h, which SDL's CMake
    would generate; made once for DJGPP.
  * cwsdpmi/ -- CWSDPMI r7 by Charles W. Sandmann (binaries, source and
    documentation), the DPMI host. CWSDSTUB.EXE is put in front of the program.
  * compat/SDL2/SDL.h and sdl2_compat.c -- WarpNES is written for SDL2; SDL3
    is the one that runs on DOS. compat/ is first on the include path, so the
    sources' `#include <SDL2/SDL.h>` gets the SDL2 names WarpNES uses mapped
    onto SDL3 (events, joysticks/game controllers, and the SDL2-style audio
    callback device).

How it fits together, in ../source:

  * SDLMainWindow.cpp -- on DOS (`#ifdef __DJGPP__`) there is no SDL
    renderer: the window is the whole screen, and each frame from
    WarpNES::render16() goes to DOSVideo.cpp. It also includes
    SDL3/SDL_main.h, which on DOS sets up the near pointers SDL needs.
  * DOSVideo.cpp -- picks the video mode and puts the RGB565 frame straight
    onto the screen surface (whole-number scaling; 15/16/24/32-bit modes;
    a palette built as colours appear for mode 13h).
  * Emulation/ControllerSDL.cpp -- one line for SDL3's key event layout.

Everything else is the same emulator source the Linux and Windows SDL builds
use; the existing `__DJGPP__` code paths (8.3 save names, no std::mutex) apply.
