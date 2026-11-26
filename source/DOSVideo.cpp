/*
 * Screen output for the MS-DOS SDL build (DJGPP + SDL3's DOS port).
 * Adapted from SDLPoP-DOS (src/dos_video.c).
 *
 * The other SDL builds draw the frame with an SDL renderer. On DOS there is
 * only the software renderer, which is slow and cannot drive an 8-bit VGA
 * mode, so here the emulator's RGB565 frame (WarpNES::render16) goes straight
 * onto the window surface:
 *
 *   hicolor VESA mode (15/16/24/32 bit): the frame is scaled by a whole
 *     number and centered. 16-bit modes take the RGB565 pixels as they are.
 *   VGA mode 13h (320x200, 256 colors): the NES shows at most a few dozen
 *     colors at once, so a palette is built up as colors appear; the 256x240
 *     frame loses 20 lines at the top and bottom (mostly overscan).
 *
 * Which mode: the WARPNES_VIDEO environment variable (e.g.
 * SET WARPNES_VIDEO=320x240x16, or 320x200x8 for mode 13h), otherwise the
 * smallest hicolor mode the frame fits twice over (640x480: sharp, doubled
 * pixels), otherwise the smallest hicolor mode it fits in (320x240),
 * otherwise mode 13h.
 */
#ifdef __DJGPP__

#include "DOSVideo.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

static SDL_Rect dos_dest;   // where the picture lands on the screen
static int dos_src_y;       // first frame line shown (mode 13h crops)
static bool dos_indexed;

// ---------------------------------------------------------------- mode choice
static int bpp_rank(SDL_PixelFormat f)
{
    switch (SDL_BITSPERPIXEL(f)) {
        case 16: return 4;  // same as the frame: no conversion
        case 15: return 3;
        case 32: return 2;
        case 24: return 1;  // 3-byte pixels: slow to convert to
        default: return 0;
    }
}

static const SDL_DisplayMode* dos_pick_mode(void)
{
    int count = 0;
    SDL_DisplayMode** modes = SDL_GetFullscreenDisplayModes(SDL_GetPrimaryDisplay(), &count);
    if (modes == NULL || count == 0) return NULL;

    int want_w = 0, want_h = 0, want_bpp = 0;
    const char* env = getenv("WARPNES_VIDEO");
    if (env != NULL) sscanf(env, "%dx%dx%d", &want_w, &want_h, &want_bpp);

    const SDL_DisplayMode* best = NULL;
    long best_score = -1;
    for (int i = 0; i < count; ++i) {
        const SDL_DisplayMode* m = modes[i];
        if (m->w < 320 || m->h < 200) continue;
        int bpp = SDL_BITSPERPIXEL(m->format);
        bool indexed = SDL_ISPIXELFORMAT_INDEXED(m->format);
        if (indexed && (m->w != 320 || m->h != 200)) continue; // only mode 13h of the 8-bit modes
        long score;
        if (want_w > 0) {
            // As asked, as closely as possible.
            score = 100000 - labs((long)m->w * m->h - (long)want_w * want_h) / 16;
            if (m->w == want_w && m->h == want_h) score += 100000;
            if (want_bpp != 0 && bpp == want_bpp) score += 50000;
            if (want_bpp == 0 && !indexed) score += 1000 + bpp_rank(m->format);
        } else {
            // Hicolor that holds the whole frame first; then a mode it fits twice
            // over (640x480: doubled, sharp pixels); then the smallest such mode
            // (least to copy), then the closest pixel format.
            score = 0;
            if (!indexed && m->w >= 256 && m->h >= 240) score += 1000000;
            if (!indexed && m->w >= 512 && m->h >= 480) score += 200000;
            score += 100000 - ((long)m->w * m->h) / 16;
            score += bpp_rank(m->format);
        }
        if (score > best_score) { best_score = score; best = m; }
    }
    static SDL_DisplayMode chosen;
    if (best) chosen = *best;
    SDL_free(modes);
    return best ? &chosen : NULL;
}

SDL_Window* dos_create_window(const char* title)
{
    const SDL_DisplayMode* mode = dos_pick_mode();
    int w = mode ? mode->w : 320;
    int h = mode ? mode->h : 200;
    SDL_Window* window = SDL_CreateWindow(title, w, h, SDL_WINDOW_FULLSCREEN);
    if (window == NULL) return NULL;
    if (mode) SDL_SetWindowFullscreenMode(window, mode);
    SDL_SyncWindow(window);
    SDL_Surface* screen = SDL_GetWindowSurface(window);
    if (screen == NULL) {
        printf("dos_create_window: SDL_GetWindowSurface: %s\n", SDL_GetError());
        return window;
    }
    dos_indexed = SDL_ISPIXELFORMAT_INDEXED(screen->format);
    if (dos_indexed) {
        // The window surface has no palette of its own: share the one of the
        // driver's surface behind it, which is the one that goes to the VGA DAC.
        SDL_Surface* internal = (SDL_Surface*)SDL_GetPointerProperty(SDL_GetWindowProperties(window),
                                                                     "SDL.internal.window.surface", NULL);
        SDL_Palette* pal = internal ? SDL_GetSurfacePalette(internal) : NULL;
        if (pal) SDL_SetSurfacePalette(screen, pal);
        else if (!SDL_GetSurfacePalette(screen)) SDL_CreateSurfacePalette(screen);
    }
    printf("Video: %dx%d, %d bits%s\n", screen->w, screen->h, SDL_BITSPERPIXEL(screen->format),
           dos_indexed ? " (VGA mode 13h)" : "");
    return window;
}

static void dos_layout(SDL_Surface* screen, int src_w, int src_h)
{
    int scale = SDL_min(screen->w / src_w, screen->h / src_h);
    if (scale < 1) scale = 1;
    int w = src_w * scale, h = src_h * scale;
    dos_src_y = 0;
    if (h > screen->h) {             // too tall (mode 13h): show the middle lines
        dos_src_y = (h - screen->h) / 2 / scale;
        h = screen->h;
    }
    if (w > screen->w) w = screen->w;
    dos_dest.w = w; dos_dest.h = h;
    dos_dest.x = (screen->w - w) / 2;
    dos_dest.y = (screen->h - h) / 2;
}

// ---------------------------------------------------------------- mode 13h
// RGB565 -> palette index, kept from frame to frame (the NES palette is small).
static uint16_t index_of[65536];     // 0xFFFF: not in the palette yet
static SDL_Color pal[256];
static int pal_count = -1;

// Index 0 stays black: it is also the border around the picture.
static void palette_reset(void)
{
    memset(index_of, 0xFF, sizeof(index_of));
    pal[0].r = pal[0].g = pal[0].b = 0; pal[0].a = 255;
    index_of[0x0000] = 0;
    pal_count = 1;
}

static void present_indexed(const uint16_t* frame, int w, int h, SDL_Surface* screen)
{
    if (pal_count < 0) palette_reset();
    int rows = dos_dest.h, cols = dos_dest.w;
    bool changed = false;
    for (int pass = 0; pass < 2; ++pass) {
        bool overflow = false;
        for (int y = 0; y < rows && !overflow; ++y) {
            const uint16_t* s = frame + (dos_src_y + y) * w;
            for (int x = 0; x < cols; ++x) {
                uint16_t c = s[x];
                if (index_of[c] != 0xFFFF) continue;
                if (pal_count == 256) { overflow = true; break; }
                int r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
                pal[pal_count].r = (Uint8)((r << 3) | (r >> 2));
                pal[pal_count].g = (Uint8)((g << 2) | (g >> 4));
                pal[pal_count].b = (Uint8)((b << 3) | (b >> 2));
                pal[pal_count].a = 255;
                index_of[c] = (uint16_t)pal_count++;
                changed = true;
            }
        }
        if (!overflow) break;
        // The palette filled up over time: start again from this frame.
        palette_reset();
    }
    // Only touch the palette (and so the VGA DAC) when it changed.
    SDL_Palette* screen_pal = SDL_GetSurfacePalette(screen);
    if (changed && screen_pal) SDL_SetPaletteColors(screen_pal, pal, 0, pal_count);

    Uint8* dst = (Uint8*)screen->pixels + dos_dest.y * screen->pitch + dos_dest.x;
    for (int y = 0; y < rows; ++y, dst += screen->pitch) {
        const uint16_t* s = frame + (dos_src_y + y) * w;
        for (int x = 0; x < cols; ++x) {
            uint16_t i = index_of[s[x]];
            dst[x] = (Uint8)(i == 0xFFFF ? 0 : i);
        }
    }
}

// ---------------------------------------------------------------- hicolor
static Uint32* tab32;                // RGB565 -> XRGB8888, for 24/32-bit modes

static void build_tab32(void)
{
    if (tab32) return;
    tab32 = (Uint32*)malloc(65536 * sizeof(Uint32));
    if (!tab32) return;
    for (int c = 0; c < 65536; ++c) {
        int r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
        r = (r << 3) | (r >> 2); g = (g << 2) | (g >> 4); b = (b << 3) | (b >> 2);
        tab32[c] = (Uint32)(r << 16 | g << 8 | b);
    }
}

static bool fast_format(SDL_PixelFormat f)
{
    return f == SDL_PIXELFORMAT_RGB565 || f == SDL_PIXELFORMAT_XRGB1555 ||
           f == SDL_PIXELFORMAT_XRGB8888 || f == SDL_PIXELFORMAT_RGB24 || f == SDL_PIXELFORMAT_BGR24;
}

static void present_fast(const uint16_t* frame, int w, int h, SDL_Surface* screen)
{
    int scale = dos_dest.w / w;
    if (scale < 1) scale = 1;
    int bpp = SDL_BYTESPERPIXEL(screen->format);
    int cols = dos_dest.w / scale, rows = dos_dest.h / scale;
    Uint8* drow = (Uint8*)screen->pixels + dos_dest.y * screen->pitch + dos_dest.x * bpp;
    int line_bytes = cols * scale * bpp;
    if (screen->format != SDL_PIXELFORMAT_RGB565 && screen->format != SDL_PIXELFORMAT_XRGB1555) build_tab32();
    for (int y = 0; y < rows; ++y) {
        const uint16_t* p = frame + (dos_src_y + y) * w;
        Uint8* d = drow;
        switch (screen->format) {
            case SDL_PIXELFORMAT_RGB565:
                if (scale == 1) memcpy(d, p, cols * 2);
                else if (scale == 2) {
                    Uint32* o = (Uint32*)d;
                    for (int x = 0; x < cols; ++x) o[x] = (Uint32)p[x] * 0x00010001u;
                } else {
                    Uint16* o = (Uint16*)d;
                    for (int x = 0; x < cols; ++x)
                        for (int k = 0; k < scale; ++k) *o++ = p[x];
                }
                break;
            case SDL_PIXELFORMAT_XRGB1555: {
                Uint16* o = (Uint16*)d;
                for (int x = 0; x < cols; ++x) {
                    Uint16 v = (Uint16)(((p[x] >> 1) & 0x7FE0) | (p[x] & 0x001F));
                    for (int k = 0; k < scale; ++k) *o++ = v;
                }
            } break;
            case SDL_PIXELFORMAT_XRGB8888: {
                Uint32* o = (Uint32*)d;
                for (int x = 0; x < cols; ++x) {
                    Uint32 v = tab32 ? tab32[p[x]] : 0;
                    for (int k = 0; k < scale; ++k) *o++ = v;
                }
            } break;
            case SDL_PIXELFORMAT_BGR24:  // B, G, R in memory
            default:                     // RGB24: R, G, B in memory
            {
                bool bgr = screen->format == SDL_PIXELFORMAT_BGR24;
                for (int x = 0; x < cols; ++x) {
                    Uint32 v = tab32 ? tab32[p[x]] : 0;
                    Uint8 r = (Uint8)(v >> 16), g = (Uint8)(v >> 8), b = (Uint8)v;
                    for (int k = 0; k < scale; ++k) {
                        d[0] = bgr ? b : r; d[1] = g; d[2] = bgr ? r : b; d += 3;
                    }
                }
            } break;
        }
        for (int k = 1; k < scale; ++k) memcpy(drow + k * screen->pitch, drow, line_bytes);
        drow += scale * screen->pitch;
    }
}

// Anything else: let SDL convert and scale.
static void present_generic(const uint16_t* frame, int w, int h, SDL_Surface* screen)
{
    SDL_Surface* src = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_RGB565, (void*)frame, w * 2);
    if (!src) return;
    SDL_SetSurfaceBlendMode(src, SDL_BLENDMODE_NONE);
    SDL_Rect sr = { 0, dos_src_y, w, h - 2 * dos_src_y };
    SDL_BlitSurfaceScaled(src, &sr, screen, &dos_dest, SDL_SCALEMODE_NEAREST);
    SDL_DestroySurface(src);
}

// ---------------------------------------------------------------- present
void dos_present(SDL_Window* window, const uint16_t* frame, int w, int h)
{
    SDL_Surface* screen = SDL_GetWindowSurface(window);
    if (screen == NULL) return;

    static int last_w, last_h, last_src_w, last_src_h;
    if (screen->w != last_w || screen->h != last_h || w != last_src_w || h != last_src_h) {
        last_w = screen->w; last_h = screen->h; last_src_w = w; last_src_h = h;
        SDL_FillSurfaceRect(screen, NULL, 0);
        dos_layout(screen, w, h);
    }

    if (dos_indexed) {
        present_indexed(frame, w, h, screen);
    } else if (fast_format(screen->format)) {
        present_fast(frame, w, h, screen);
    } else {
        present_generic(frame, w, h, screen);
    }
    SDL_UpdateWindowSurface(window);
}

#endif // __DJGPP__
