#ifndef DOS_VIDEO_HPP
#define DOS_VIDEO_HPP

/*
 * Screen output for the MS-DOS SDL build (DJGPP + SDL3's DOS port).
 * See DOSVideo.cpp.
 */
#ifdef __DJGPP__

#include <SDL2/SDL.h>
#include <cstdint>

// Make the window (the whole screen) in the chosen video mode.
SDL_Window* dos_create_window(const char* title);

// Put a frame (RGB565, w x h pixels) on the screen.
void dos_present(SDL_Window* window, const uint16_t* frame, int w, int h);

#endif // __DJGPP__
#endif // DOS_VIDEO_HPP
