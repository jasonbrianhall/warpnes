#ifndef DOS_VIDEO_HPP
#define DOS_VIDEO_HPP

/*
 * Screen output for the MS-DOS SDL build (DJGPP + SDL3's DOS port).
 * See DOSVideo.cpp.
 */
#ifdef __DJGPP__

#include <SDL2/SDL.h>
#include <cstdint>

// Make the window (the whole screen) in the chosen video mode: the
// WARPNES_VIDEO environment variable, else preferredMode ("640x480x16", saved
// by the GUI; may be null), else the best mode available.
SDL_Window* dos_create_window(const char* title, const char* preferredMode);

// Video modes offered in the GUI (Video > Display Mode)
int dos_mode_count(void);
void dos_mode_name(int index, char* buf, int size);   // "640x480 16-bit"
void dos_mode_id(int index, char* buf, int size);     // "640x480x16"
int dos_current_mode(SDL_Window* window);
bool dos_set_mode(SDL_Window* window, int index);

// Mouse position on the screen -> position in the picture last presented
void dos_screen_to_picture(float sx, float sy, int* px, int* py);
// Lines of a pic_h-line picture that are on the screen (mode 13h shows 200)
void dos_visible_lines(int pic_h, int* y0, int* y1);

// Put a frame (RGB565, w x h pixels) on the screen.
void dos_present(SDL_Window* window, const uint16_t* frame, int w, int h);

#endif // __DJGPP__
#endif // DOS_VIDEO_HPP
