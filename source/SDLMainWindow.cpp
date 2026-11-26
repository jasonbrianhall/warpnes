#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#if defined(_WIN32) && !defined(__DJGPP__)
#include <direct.h>   // _mkdir, _chdir
#endif
#include <iostream>
#include <string>
#include <vector>

#include <SDL2/SDL.h>
#ifdef __DJGPP__
// MS-DOS (SDL3's DOS port, through msdos/compat/SDL2/SDL.h): SDL_main sets up
// the near pointers SDL needs; the frame goes straight to the screen (DOSVideo).
#include <SDL3/SDL_main.h>
#include <crt0.h>
#include "DOSVideo.hpp"
#endif

#include "Emulation/WarpNES.hpp"
#include "Emulation/ControllerSDL.hpp"
#include "Configuration.hpp"
#include "Constants.hpp"
#include "SDLCacheScaling.hpp"
#include "SDLGui.hpp"
#include "SDLInput.hpp"
#ifndef __DJGPP__
#include "XBRZScaler.hpp"
#endif

// Settings of this front end (display mode, controls, game folder). Kept
// apart from CONFIG_FILE_NAME, whose key codes belong to the Allegro build.
// MS-DOS: next to WARPNES.EXE (the game browser changes folders); elsewhere
// in the current folder, like CONFIG_FILE_NAME.
static std::string settingsFile = "warpnes.ini";
#define SDL_SETTINGS_FILE settingsFile

static SDLScalingCache* scalingCache = nullptr;
static SDL_Window* window;
static SDL_Renderer* renderer;
static SDL_Texture* texture;
static SDL_Texture* scanlineTexture;
static WarpNES* smbEngine = nullptr;     // the running game (null: none loaded)
static uint32_t renderBuffer[RENDER_WIDTH * RENDER_HEIGHT];
static uint16_t renderBuffer16[RENDER_WIDTH * RENDER_HEIGHT];
static uint16_t guiBuffer16[RENDER_WIDTH * RENDER_HEIGHT];   // picture + GUI

static SimpleINI settings;
static InputConfig input;
static bool running = true;
static bool paused = false;

// Zapper (light gun, port 2), aimed with the mouse
static int zapperSetting = 0;            // 0 auto, 1 on, 2 off (warpnes.ini [input] zapper)
static bool zapperLightGunGame = false;  // the loaded game is a known light-gun game
static int mousePicX = -1, mousePicY = -1; // mouse in picture coordinates (-1: none yet)
static bool mouseLeftDown = false;
static int triggerHold = 0;              // frames a short click still holds the trigger

// Light-gun games, recognised by file name (lower case)
static bool isLightGunGame(const std::string& path)
{
    std::string n = path;
    size_t slash = n.find_last_of("/\\");
    if (slash != std::string::npos) n = n.substr(slash + 1);
    for (auto& c : n) c = (char)tolower((unsigned char)c);
    static const char* names[] = {
        "duck hunt", "duckhunt", "duck_hunt", "hogan", "wild gunman", "wildgunman", "wild_gunman",
        "gumshoe", "barker bill", "shooting range", "freedom force", "mechanized attack",
        "to the earth", "chiller", "baby boomer", "laser invasion",
    };
    for (const char* k : names)
        if (n.find(k) != std::string::npos) return true;
    return false;
}

static bool zapperWanted()
{
    return zapperSetting == 1 || (zapperSetting == 0 && zapperLightGunGame);
}
#ifndef __DJGPP__
static int windowScale = 3;
static bool fullscreen = false;
#endif

static void saveSettings()
{
    if (!settings.saveToFile(SDL_SETTINGS_FILE))
        printf("Couldn't save %s\n", SDL_SETTINGS_FILE.c_str());
}

// Conversion from 16-bit RGB565 to 32-bit ARGB8888
static void convertRGB565ToARGB8888(const uint16_t* src, uint32_t* dst, int width, int height)
{
    for (int i = 0; i < width * height; i++) {
        uint16_t pixel16 = src[i];
        uint8_t r = (pixel16 >> 11) & 0x1F;
        uint8_t g = (pixel16 >> 5) & 0x3F;
        uint8_t b = pixel16 & 0x1F;
        r = (r << 3) | (r >> 2);
        g = (g << 2) | (g >> 4);
        b = (b << 3) | (b >> 2);
        dst[i] = (0xFF << 24) | (r << 16) | (g << 8) | b;
    }
}

/**
 * SDL Audio callback function.
 */
static void audioCallback(void* userdata, uint8_t* buffer, int len)
{
    if (smbEngine != nullptr)
    {
        smbEngine->audioCallback(buffer, len);
    }
    else
    {
        memset(buffer, 0, len);   // AUDIO_S8 silence
    }
}

#ifndef __DJGPP__
static void applyWindowMode()
{
    if (fullscreen) {
        SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP);
    } else {
        SDL_SetWindowFullscreen(window, 0);
        SDL_RestoreWindow(window);   // un-maximize, or the size doesn't take
        SDL_SetWindowSize(window, RENDER_WIDTH * windowScale, RENDER_HEIGHT * windowScale);
    }
}

// ---------------------------------------------------------------- picture options
// Like the GTK4 build's Scaling and Filters: how the 256x240 picture fills
// the (resizable) window. The menus are drawn on top, unfiltered and sharp.
enum { SCALE_FIT, SCALE_INTEGER, SCALE_STRETCH, SCALE_COUNT };
enum { SMOOTH_NEAREST, SMOOTH_SHARP, SMOOTH_LINEAR, SMOOTH_COUNT };
enum { FILTER_NONE, FILTER_XBRZ2, FILTER_XBRZ3, FILTER_XBRZ4, FILTER_COUNT };
static const char* const scaleNames[] = { "Fit", "Integer", "Stretch" };
static const char* const smoothNames[] = { "Nearest", "Sharp bilinear", "Bilinear" };
static const char* const filterNames[] = { "None", "xBRZ 2x", "xBRZ 3x", "xBRZ 4x" };
static int scaleMode = SCALE_FIT;
static int smoothing = SMOOTH_SHARP;
static int filterMode = FILTER_NONE;

static SDL_Texture* overlayTex = nullptr;    // GUI and crosshair, with transparency
static SDL_Texture* prescaleTex = nullptr;   // sharp bilinear: whole-number nearest prescale
static int textureW = 0, textureH = 0, prescaleW = 0, prescaleH = 0;
static std::vector<uint32_t> filterBuffer;
// The GUI layer is drawn overlayScale times larger than the picture, to
// about the picture's size on the screen, so its text (DejaVu Sans Mono,
// a TrueType font) is rendered at that size instead of being blown up
static std::vector<uint16_t> overlay16;
static std::vector<uint32_t> overlayBuffer;
static int overlayScale = 1, overlayTexW = 0, overlayTexH = 0;
static const uint16_t OVERLAY_KEY = 0xF81F;  // magenta: not a GUI colour; transparent
static const int MAX_OVERLAY_SCALE = 8;

// Where the picture goes in the renderer's output (pixels)
static SDL_Rect pictureRect(int outW, int outH)
{
    SDL_Rect r;
    if (scaleMode == SCALE_STRETCH) { r.x = 0; r.y = 0; r.w = outW; r.h = outH; return r; }
    if (scaleMode == SCALE_INTEGER) {
        int k = SDL_max(1, SDL_min(outW / RENDER_WIDTH, outH / RENDER_HEIGHT));
        r.w = RENDER_WIDTH * k; r.h = RENDER_HEIGHT * k;
    } else {
        double k = SDL_min((double)outW / RENDER_WIDTH, (double)outH / RENDER_HEIGHT);
        r.w = (int)(RENDER_WIDTH * k + 0.5); r.h = (int)(RENDER_HEIGHT * k + 0.5);
    }
    r.x = (outW - r.w) / 2; r.y = (outH - r.h) / 2;
    return r;
}

// (Re)make a texture when the size it needs changes
static bool sizeTexture(SDL_Texture*& t, int& curW, int& curH, int w, int h, int access)
{
    if (t && curW == w && curH == h) return true;
    if (t) SDL_DestroyTexture(t);
    t = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, access, w, h);
    curW = t ? w : 0; curH = t ? h : 0;
    return t != nullptr;
}

static void loadVideoOptions()
{
    scaleMode = settings.getInt("video", "scale_mode", SCALE_FIT);
    smoothing = settings.getInt("video", "smoothing", SMOOTH_SHARP);
    filterMode = settings.getInt("video", "filter", FILTER_NONE);
    if (scaleMode < 0 || scaleMode >= SCALE_COUNT) scaleMode = SCALE_FIT;
    if (smoothing < 0 || smoothing >= SMOOTH_COUNT) smoothing = SMOOTH_SHARP;
    if (filterMode < 0 || filterMode >= FILTER_COUNT) filterMode = FILTER_NONE;
}

// A clear GUI layer at the scale that matches the picture on the screen
// now (rounded up: a slightly larger layer scaled down looks better than
// a smaller one scaled up)
static void clearOverlay()
{
    int outW, outH;
    SDL_GetRendererOutputSize(renderer, &outW, &outH);
    SDL_Rect r = pictureRect(outW, outH);
    int k = (r.h + RENDER_HEIGHT - 1) / RENDER_HEIGHT;
    int kw = (r.w + RENDER_WIDTH - 1) / RENDER_WIDTH;
    k = SDL_max(1, SDL_min(MAX_OVERLAY_SCALE, SDL_max(k, kw)));
    overlayScale = k;
    overlay16.assign((size_t)RENDER_WIDTH * k * RENDER_HEIGHT * k, OVERLAY_KEY);
}
#endif

// ---------------------------------------------------------------- GUI host
class FrontendHost : public GuiHost
{
public:
    std::string lastVideoError;
    bool loadGame(const std::string& path) override
    {
        WarpNES* engine = new WarpNES();
        printf("Loading ROM: %s\n", path.c_str());
        if (!engine->loadROM(path)) {
            printf("Failed to load ROM file: %s\n", path.c_str());
            delete engine;
            return false;
        }
        engine->reset();
        // A fresh emulator per game: loading into the old one keeps some of
        // its state. The old one saves its battery RAM when deleted.
        SDL_LockAudio();
        WarpNES* old = smbEngine;
        smbEngine = engine;
        SDL_UnlockAudio();
        delete old;
        paused = false;
        zapperLightGunGame = isLightGunGame(path);
        engine->enableZapper(zapperWanted());
        memset(renderBuffer16, 0, sizeof(renderBuffer16));
#ifndef __DJGPP__
        size_t slash = path.find_last_of("/\\");
        std::string name = (slash == std::string::npos) ? path : path.substr(slash + 1);
        SDL_SetWindowTitle(window, (std::string(APP_TITLE) + " - " + name).c_str());
#endif
        return true;
    }
    bool gameLoaded() const override { return smbEngine != nullptr; }
    void resetGame() override { if (smbEngine) smbEngine->reset(); }
    void saveState(int slot) override
    {
        if (!smbEngine) return;
        smbEngine->saveState("save" + std::to_string(slot));
        printf("State %d saved\n", slot);
    }
    bool loadState(int slot) override
    {
        if (!smbEngine) return false;
        bool ok = smbEngine->loadState("save" + std::to_string(slot));
        printf(ok ? "State %d loaded\n" : "Failed to load state %d\n", slot);
        return ok;
    }
    void quit() override { running = false; }

#ifndef __DJGPP__
    const char* videoOption(int which) override
    {
        switch (which) {
            case 0: return scaleNames[scaleMode];
            case 1: return smoothNames[smoothing];
            case 2: return filterNames[filterMode];
        }
        return nullptr;
    }
    void cycleVideoOption(int which) override
    {
        switch (which) {
            case 0: scaleMode = (scaleMode + 1) % SCALE_COUNT; settings.setInt("video", "scale_mode", scaleMode); break;
            case 1: smoothing = (smoothing + 1) % SMOOTH_COUNT; settings.setInt("video", "smoothing", smoothing); break;
            case 2: filterMode = (filterMode + 1) % FILTER_COUNT; settings.setInt("video", "filter", filterMode); break;
        }
        saveSettings();
    }
#endif

    int zapperSetting() override { return ::zapperSetting; }
    void setZapperSetting(int v) override
    {
        ::zapperSetting = v;
        settings.setInt("input", "zapper", v);
        saveSettings();
        if (smbEngine) smbEngine->enableZapper(zapperWanted());
    }
    bool zapperActive() override { return smbEngine && zapperWanted(); }

#ifdef __DJGPP__
    int videoModeCount() override { return dos_mode_count(); }
    std::string videoModeName(int index) override
    {
        char buf[48];
        dos_mode_name(index, buf, sizeof(buf));
        return buf;
    }
    int currentVideoMode() override { return dos_current_mode(window); }
    std::string videoError() override { return lastVideoError; }
    bool setVideoMode(int index) override
    {
        if (!dos_set_mode(window, index)) {
            lastVideoError = SDL_GetError();
            return false;
        }
        char id[32];
        dos_mode_id(index, id, sizeof(id));
        settings.setString("video", "dos_mode", id);
        saveSettings();
        return true;
    }
#else
    // Window sizes 1x-5x, then full screen
    int videoModeCount() override { return 6; }
    std::string videoModeName(int index) override
    {
        if (index == 5) return "Full screen";
        int s = index + 1;
        return "Window " + std::to_string(s) + "x (" + std::to_string(RENDER_WIDTH * s) + "x" +
               std::to_string(RENDER_HEIGHT * s) + ")";
    }
    int currentVideoMode() override { return fullscreen ? 5 : windowScale - 1; }
    bool setVideoMode(int index) override
    {
        if (index < 0 || index > 5) return false;
        if (index == 5) fullscreen = true;
        else { fullscreen = false; windowScale = index + 1; }
        applyWindowMode();
        settings.setInt("video", "window_scale", windowScale);
        settings.setBool("video", "fullscreen", fullscreen);
        saveSettings();
        return true;
    }
#endif

    InputConfig& input() override { return ::input; }
    void inputChanged() override
    {
        ::input.save(settings);
        saveSettings();
        ::input.openJoysticks();
    }

    std::string gameDirectory() override { return settings.getString("files", "game_dir", ""); }
    void setGameDirectory(const std::string& dir) override
    {
        settings.setString("files", "game_dir", dir);
        saveSettings();
    }
};

static FrontendHost host;
static Gui gui(host);

/**
 * Initialize libraries for use.
 */
static bool initialize()
{
    // Load the configuration
    Configuration::initialize(CONFIG_FILE_NAME);
    settings.loadFromFile(SDL_SETTINGS_FILE);
    input.load(settings);
    zapperSetting = settings.getInt("input", "zapper", 0);
    if (zapperSetting < 0 || zapperSetting > 2) zapperSetting = 0;

    // Initialize SDL with joystick support
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER) < 0)
    {
        std::cout << "SDL_Init() failed during initialize(): " << SDL_GetError() << std::endl;
        return false;
    }

#ifdef __DJGPP__
    // MS-DOS: the window is the whole screen, in a VESA or VGA mode; there is
    // no SDL renderer, the frame is put on the screen by DOSVideo.cpp.
    std::string mode = settings.getString("video", "dos_mode", "");
    window = dos_create_window(APP_TITLE, mode.empty() ? nullptr : mode.c_str());
    if (window == nullptr)
    {
        std::cout << "dos_create_window() failed during initialize(): " << SDL_GetError() << std::endl;
        return false;
    }
#else
    windowScale = settings.getInt("video", "window_scale", Configuration::getRenderScale());
    if (windowScale < 1 || windowScale > 5) windowScale = 3;
    fullscreen = settings.getBool("video", "fullscreen", false);

    // Create the window
    window = SDL_CreateWindow(APP_TITLE,
                              SDL_WINDOWPOS_UNDEFINED,
                              SDL_WINDOWPOS_UNDEFINED,
                              RENDER_WIDTH * windowScale,
                              RENDER_HEIGHT * windowScale,
                              SDL_WINDOW_RESIZABLE);   // resizable: maximize button
    if (window == nullptr)
    {
        std::cout << "SDL_CreateWindow() failed during initialize(): " << SDL_GetError() << std::endl;
        return false;
    }
    if (fullscreen) applyWindowMode();

    // Setup the renderer and texture buffer
    renderer = SDL_CreateRenderer(window, -1, (Configuration::getVsyncEnabled() ? SDL_RENDERER_PRESENTVSYNC : 0) | SDL_RENDERER_ACCELERATED);
    if (renderer == nullptr)
    {
        std::cout << "SDL_CreateRenderer() failed during initialize(): " << SDL_GetError() << std::endl;
        return false;
    }

    // The picture is placed by pictureRect() (Video > Scaling), not by a
    // renderer logical size
    SDL_SetWindowMinimumSize(window, RENDER_WIDTH, RENDER_HEIGHT);
    loadVideoOptions();
    if (!sizeTexture(texture, textureW, textureH, RENDER_WIDTH, RENDER_HEIGHT, SDL_TEXTUREACCESS_STREAMING))
    {
        std::cout << "SDL_CreateTexture() failed during initialize(): " << SDL_GetError() << std::endl;
        return false;
    }

    // The GUI draws its own pointer, and only while the mouse is in use
    SDL_ShowCursor(SDL_DISABLE);
#endif // !__DJGPP__

    if (Configuration::getAudioEnabled())
    {
        // Initialize audio
        SDL_AudioSpec desiredSpec;
        SDL_zero(desiredSpec);
        desiredSpec.freq = Configuration::getAudioFrequency();
        desiredSpec.format = AUDIO_S8;
        desiredSpec.channels = 1;
        desiredSpec.samples = 2048;
        desiredSpec.callback = audioCallback;
        desiredSpec.userdata = NULL;

        // No "obtained" spec: then SDL converts from exactly this format
        // (8-bit signed mono at the APU's rate) to whatever the device
        // uses. With one, SDL2 may hand the callback the device's own
        // format instead (on Windows, WASAPI's 32-bit float stereo), and
        // the 8-bit samples written into it come out as silence.
        if (SDL_OpenAudio(&desiredSpec, NULL) < 0)
        {
            std::cout << "SDL_OpenAudio() failed, no sound: " << SDL_GetError() << std::endl;
        }
        else
        {
            // Start playing audio
            SDL_PauseAudio(0);
        }
    }

#ifndef __DJGPP__
    scalingCache = new SDLScalingCache(renderer);
    scalingCache->initialize();
#endif

    input.openJoysticks();
    return true;
}

/**
 * Shutdown libraries for exit.
 */
static void shutdown()
{
    SDL_CloseAudio();
    delete smbEngine;   // saves battery RAM
    smbEngine = nullptr;

    input.closeJoysticks();

#ifndef __DJGPP__
    if (scalingCache) {
        delete scalingCache;
        scalingCache = nullptr;
    }
    SDL_DestroyTexture(scanlineTexture);
    if (overlayTex) SDL_DestroyTexture(overlayTex);
    if (prescaleTex) SDL_DestroyTexture(prescaleTex);
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
#endif
    SDL_DestroyWindow(window);

    SDL_Quit();
}

// Put a 256x240 RGB565 picture on the screen
static void present(const uint16_t* frame, const uint16_t* overlay = nullptr)
{
#ifdef __DJGPP__
    // MS-DOS: straight onto the VESA/VGA screen
    dos_present(window, frame, RENDER_WIDTH, RENDER_HEIGHT);
#else
    int outW, outH;
    SDL_GetRendererOutputSize(renderer, &outW, &outH);
    SDL_Rect dst = pictureRect(outW, outH);

    // The game picture, through the filter (xBRZ 2x-4x) if one is chosen
    convertRGB565ToARGB8888(frame, renderBuffer, RENDER_WIDTH, RENDER_HEIGHT);
    int f = filterMode == FILTER_NONE ? 1 : filterMode + 1;
    int tw = RENDER_WIDTH * f, th = RENDER_HEIGHT * f;
    const uint32_t* pixels = renderBuffer;
    if (f > 1) {
        filterBuffer.resize((size_t)tw * th);
        xbrzStyleScale(f, renderBuffer, filterBuffer.data(), RENDER_WIDTH, RENDER_HEIGHT);
        pixels = filterBuffer.data();
    }
    if (!sizeTexture(texture, textureW, textureH, tw, th, SDL_TEXTUREACCESS_STREAMING)) return;
    SDL_UpdateTexture(texture, NULL, pixels, tw * (int)sizeof(uint32_t));

    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);

    SDL_Texture* src = texture;
    SDL_ScaleMode filterHow = smoothing == SMOOTH_NEAREST ? SDL_ScaleModeNearest : SDL_ScaleModeLinear;
    if (smoothing == SMOOTH_SHARP) {
        // Sharp bilinear: the largest whole-number scale with nearest
        // neighbour, then bilinear for the rest. Crisp pixels, all the same
        // size, at any window size.
        int k = SDL_min(dst.w / tw, dst.h / th);
        if (dst.w % tw == 0 && dst.h % th == 0 && dst.w / tw == dst.h / th) {
            filterHow = SDL_ScaleModeNearest;   // exact multiple: nearest is perfect
        } else if (k >= 2 && SDL_RenderTargetSupported(renderer) &&
                   sizeTexture(prescaleTex, prescaleW, prescaleH, tw * k, th * k, SDL_TEXTUREACCESS_TARGET)) {
            SDL_SetTextureScaleMode(texture, SDL_ScaleModeNearest);
            SDL_SetRenderTarget(renderer, prescaleTex);
            SDL_RenderCopy(renderer, texture, NULL, NULL);
            SDL_SetRenderTarget(renderer, NULL);
            src = prescaleTex;
        }
    }
    SDL_SetTextureScaleMode(src, filterHow);
    SDL_RenderCopy(renderer, src, NULL, &dst);

    // Menus, pointer and crosshair over it, sharp: the layer is drawn at
    // overlayScale, near the picture's size on the screen
    if (overlay) {
        int ow = RENDER_WIDTH * overlayScale, oh = RENDER_HEIGHT * overlayScale;
        overlayBuffer.resize((size_t)ow * oh);
        for (size_t i = 0; i < overlayBuffer.size(); i++) {
            uint16_t c = overlay[i];
            if (c == OVERLAY_KEY) { overlayBuffer[i] = 0; continue; }
            uint32_t r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
            overlayBuffer[i] = 0xFF000000u | (((r << 3) | (r >> 2)) << 16) | (((g << 2) | (g >> 4)) << 8) | ((b << 3) | (b >> 2));
        }
        if (sizeTexture(overlayTex, overlayTexW, overlayTexH, ow, oh, SDL_TEXTUREACCESS_STREAMING)) {
            SDL_SetTextureBlendMode(overlayTex, SDL_BLENDMODE_BLEND);
            SDL_SetTextureScaleMode(overlayTex, (dst.w == ow && dst.h == oh) ? SDL_ScaleModeNearest : SDL_ScaleModeLinear);
            SDL_UpdateTexture(overlayTex, NULL, overlayBuffer.data(), ow * (int)sizeof(uint32_t));
            SDL_RenderCopy(renderer, overlayTex, NULL, &dst);
        }
    }
    SDL_RenderPresent(renderer);
#endif
}

// Mouse position in picture coordinates
static void mouseToPicture(float x, float y, int* px, int* py)
{
#ifdef __DJGPP__
    dos_screen_to_picture(x, y, px, py);
#else
    // Window coordinates -> renderer output pixels -> picture
    int winW, winH, outW, outH;
    SDL_GetWindowSize(window, &winW, &winH);
    SDL_GetRendererOutputSize(renderer, &outW, &outH);
    float ox = winW > 0 ? x * outW / winW : x, oy = winH > 0 ? y * outH / winH : y;
    SDL_Rect r = pictureRect(outW, outH);
    *px = r.w > 0 ? (int)SDL_floor((ox - r.x) * RENDER_WIDTH / r.w) : -1;
    *py = r.h > 0 ? (int)SDL_floor((oy - r.y) * RENDER_HEIGHT / r.h) : -1;
#endif
}

// Keys that work while playing (the menus have their own)
static void hotkey(int sc, bool ctrl, bool shift)
{
    if (sc == SDL_SCANCODE_ESCAPE) { gui.open(); return; }
    if (ctrl && sc == SDL_SCANCODE_O) { gui.openGameBrowser(); return; }
    if (ctrl && sc == SDL_SCANCODE_R) { host.resetGame(); return; }
    if (ctrl && sc == SDL_SCANCODE_P) { paused = !paused; return; }
    if (sc >= SDL_SCANCODE_F5 && sc <= SDL_SCANCODE_F8) {
        int slot = sc - SDL_SCANCODE_F5 + 1;
        if (shift) host.loadState(slot); else host.saveState(slot);
        return;
    }
#ifndef __DJGPP__
    if (sc == SDL_SCANCODE_F11) {
        host.setVideoMode(fullscreen ? windowScale - 1 : 5);
        return;
    }
#endif
}

// Left button in the game (the GUI didn't take it): the Zapper's trigger
static void zapperButton(const GuiEvent& ge)
{
    if (ge.button != 1) return;
    if (ge.type == GuiEvent::MouseDown) {
        mouseLeftDown = true;
        triggerHold = 3;   // a quick click still holds the trigger for a few frames
    } else {
        mouseLeftDown = false;
    }
}

// Aim and trigger for this frame
static void updateZapper(bool playing)
{
    if (!smbEngine || !smbEngine->isZapperEnabled()) return;
    bool onPicture = mousePicX >= 0 && mousePicX < RENDER_WIDTH && mousePicY >= 0 && mousePicY < RENDER_HEIGHT;
    bool trigger = playing && (mouseLeftDown || triggerHold > 0);
    if (triggerHold > 0) triggerHold--;
    smbEngine->setZapperState(onPicture ? mousePicX : -1, onPicture ? mousePicY : -1, trigger);
}

// Crosshair over the picture (only on what is shown, not what the Zapper sees)
static void drawCrosshair(uint16_t* buf, int scale = 1)
{
    if (!smbEngine || !smbEngine->isZapperEnabled()) return;
    int cx = mousePicX, cy = mousePicY;
    if (cx < 0 || cx >= RENDER_WIDTH || cy < 0 || cy >= RENDER_HEIGHT) return;
    const uint16_t red = 0xF800, black = 0x0000;
    auto put = [&](int x, int y, uint16_t c) {
        if (x < 0 || x >= RENDER_WIDTH || y < 0 || y >= RENDER_HEIGHT) return;
        for (int dy = 0; dy < scale; dy++)
            for (int dx = 0; dx < scale; dx++)
                buf[(y * scale + dy) * RENDER_WIDTH * scale + x * scale + dx] = c;
    };
    for (int d = 2; d <= 6; d++) {
        // black edges first, so the red shows on light backgrounds too
        put(cx - d, cy - 1, black); put(cx - d, cy + 1, black);
        put(cx + d, cy - 1, black); put(cx + d, cy + 1, black);
        put(cx - 1, cy - d, black); put(cx + 1, cy - d, black);
        put(cx - 1, cy + d, black); put(cx + 1, cy + d, black);
    }
    for (int d = 2; d <= 6; d++) {
        put(cx - d, cy, red); put(cx + d, cy, red);
        put(cx, cy - d, red); put(cx, cy + d, red);
    }
    put(cx, cy, red);
}

static void handleEvent(const SDL_Event& event, uint32_t now)
{
    GuiEvent ge;
    switch (event.type)
    {
    case SDL_QUIT:
        running = false;
        break;
#ifndef __DJGPP__
    case SDL_WINDOWEVENT:
        if (event.window.event == SDL_WINDOWEVENT_CLOSE) running = false;
        break;
#endif

    case SDL_KEYDOWN: {
#if SDL_MAJOR_VERSION >= 3
        int sc = event.key.scancode;
        int mod = event.key.mod;
#else
        int sc = event.key.keysym.scancode;
        int mod = event.key.keysym.mod;
#endif
        ge.type = GuiEvent::KeyDown;
        ge.scancode = sc;
        ge.ctrl = (mod & KMOD_CTRL) != 0;
        ge.shift = (mod & KMOD_SHIFT) != 0;
        if (gui.active()) gui.handle(ge, now);
        else if (!event.key.repeat) hotkey(sc, ge.ctrl, ge.shift);
        break;
    }

#if SDL_MAJOR_VERSION >= 3
    case SDL_EVENT_MOUSE_MOTION:
        ge.type = GuiEvent::MouseMove;
        mouseToPicture(event.motion.x, event.motion.y, &ge.x, &ge.y);
        mousePicX = ge.x; mousePicY = ge.y;
        gui.handle(ge, now);
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        ge.type = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ? GuiEvent::MouseDown : GuiEvent::MouseUp;
        ge.button = event.button.button;
        mouseToPicture(event.button.x, event.button.y, &ge.x, &ge.y);
        mousePicX = ge.x; mousePicY = ge.y;
        if (!gui.handle(ge, now)) zapperButton(ge);
        else if (ge.type == GuiEvent::MouseUp && ge.button == 1) mouseLeftDown = false;
        break;
    case SDL_EVENT_MOUSE_WHEEL: {
        float mx, my;
        SDL_GetMouseState(&mx, &my);
        ge.type = GuiEvent::MouseWheel;
        ge.wheel = event.wheel.y > 0 ? 1 : (event.wheel.y < 0 ? -1 : 0);
        mouseToPicture(mx, my, &ge.x, &ge.y);
        gui.handle(ge, now);
        break;
    }
    case SDL_EVENT_JOYSTICK_ADDED:
    case SDL_EVENT_JOYSTICK_REMOVED:
        input.openJoysticks();
        break;
#else
    case SDL_MOUSEMOTION:
        ge.type = GuiEvent::MouseMove;
        mouseToPicture((float)event.motion.x, (float)event.motion.y, &ge.x, &ge.y);
        mousePicX = ge.x; mousePicY = ge.y;
        gui.handle(ge, now);
        break;
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
        ge.type = event.type == SDL_MOUSEBUTTONDOWN ? GuiEvent::MouseDown : GuiEvent::MouseUp;
        ge.button = event.button.button;
        mouseToPicture((float)event.button.x, (float)event.button.y, &ge.x, &ge.y);
        mousePicX = ge.x; mousePicY = ge.y;
        if (!gui.handle(ge, now)) zapperButton(ge);
        else if (ge.type == GuiEvent::MouseUp && ge.button == 1) mouseLeftDown = false;
        break;
    case SDL_MOUSEWHEEL:
        // position: the GUI keeps the last one from the motion events
        ge.type = GuiEvent::MouseWheel;
        ge.wheel = event.wheel.y > 0 ? 1 : (event.wheel.y < 0 ? -1 : 0);
        ge.x = -1;
        gui.handle(ge, now);
        break;
    case SDL_JOYDEVICEADDED:
    case SDL_JOYDEVICEREMOVED:
        input.openJoysticks();
        break;
#endif

    case SDL_JOYBUTTONDOWN:
        // for "press a button" in the Input dialog
        if (gui.active()) {
            ge.type = GuiEvent::JoyButton;
            ge.joystick = input.joystickIndexFromID(event.jbutton.which);
            ge.button = event.jbutton.button;
            if (ge.joystick >= 0) gui.handle(ge, now);
        }
        break;

    default:
        break;
    }
}

static void mainLoop()
{
    int progStartTime = SDL_GetTicks();
    int frame = 0;

    while (running)
    {
        uint32_t now = SDL_GetTicks();
        SDL_Event event;
        while (SDL_PollEvent(&event)) handleEvent(event, now);
        if (!running) break;

        // No game: the game browser stays up
        if (!smbEngine && !gui.active()) gui.openGameBrowser();

        if (smbEngine && !gui.active() && !paused)
        {
            const Uint8* keys = SDL_GetKeyboardState(NULL);
            bool p1[NES_BUTTONS], p2[NES_BUTTONS];
            input.read(0, keys, p1);
            input.read(1, keys, p2);
            // $4016 reads player 1 of controller 1, $4017 player 2 of controller 2
            Controller& c1 = smbEngine->getController1();
            Controller& c2 = smbEngine->getController2();
            for (int b = 0; b < NES_BUTTONS; b++) {
                c1.setButtonState(PLAYER_1, (ControllerButton)b, p1[b]);
                c2.setButtonState(PLAYER_2, (ControllerButton)b, p2[b]);
            }

            updateZapper(true);
            smbEngine->update();
            smbEngine->render16(renderBuffer16);
        }
        else if (smbEngine)
        {
            updateZapper(false);   // menus up or paused: trigger released
        }
        gui.setZapperMode(smbEngine && smbEngine->isZapperEnabled());

        gui.setPausedLabel(paused && smbEngine && !gui.active());
        if (gui.visible(now))
        {
            int y0 = 0, y1 = RENDER_HEIGHT;
#ifdef __DJGPP__
            dos_visible_lines(RENDER_HEIGHT, &y0, &y1);
#endif
#ifdef __DJGPP__
            memcpy(guiBuffer16, renderBuffer16, sizeof(guiBuffer16));
            if (!gui.active()) drawCrosshair(guiBuffer16);
            gui.draw(guiBuffer16, RENDER_WIDTH, RENDER_HEIGHT, y0, y1, now);
            present(guiBuffer16);
#else
            // On their own layer, so smoothing and filters leave them sharp
            clearOverlay();
            if (!gui.active()) drawCrosshair(overlay16.data(), overlayScale);
            gui.draw(overlay16.data(), RENDER_WIDTH, RENDER_HEIGHT, y0, y1, now, overlayScale);
            present(renderBuffer16, overlay16.data());
#endif
        }
        else if (smbEngine && smbEngine->isZapperEnabled())
        {
#ifdef __DJGPP__
            memcpy(guiBuffer16, renderBuffer16, sizeof(guiBuffer16));
            drawCrosshair(guiBuffer16);
            present(guiBuffer16);
#else
            clearOverlay();
            drawCrosshair(overlay16.data(), overlayScale);
            present(renderBuffer16, overlay16.data());
#endif
        }
        else
        {
            present(renderBuffer16);
        }

        // Frame timing
        int nowTicks = SDL_GetTicks();
        int delay = progStartTime + int(double(frame) * double(MS_PER_SEC) / double(Configuration::getFrameRate())) - nowTicks;
        if (delay > 0)
        {
            SDL_Delay(delay);
        }
        else
        {
            frame = 0;
            progStartTime = nowTicks;
        }
        frame++;
    }
}

#if defined(_WIN32) && !defined(__DJGPP__)
// Settings and save states go in the current folder. Started from the
// installer's shortcut that is Program Files, which can't be written to:
// then use %APPDATA%\WarpNES instead. (The portable zip, unpacked
// somewhere writable, keeps everything next to the program.)
static void useWritableFolder()
{
    FILE* f = fopen("warpnes.tmp", "wb");
    if (f) { fclose(f); remove("warpnes.tmp"); return; }
    const char* appdata = getenv("APPDATA");
    if (!appdata || !*appdata) return;
    std::string dir = std::string(appdata) + "\\WarpNES";
    _mkdir(dir.c_str());
    if (_chdir(dir.c_str()) == 0) printf("Settings and saves: %s\n", dir.c_str());
}
#endif

int main(int argc, char** argv)
{
#if defined(_WIN32) && !defined(__DJGPP__)
    useWritableFolder();
#endif
#ifdef __DJGPP__
    setvbuf(stdout, NULL, _IONBF, 0); // WARPNES ROM.NES > LOG.TXT keeps everything, even after a crash
    // Long file names when DOS has them (Windows 9x, DOSLFN; SET LFN=N turns
    // them off); show short names as they are rather than lowercased
    _crt0_startup_flags |= _CRT0_FLAG_PRESERVE_FILENAME_CASE;
    if (argv[0]) {
        std::string exe = argv[0];
        size_t slash = exe.find_last_of("/\\");
        if (slash != std::string::npos) settingsFile = exe.substr(0, slash + 1) + "warpnes.ini";
    }
#endif

    if (!initialize())
    {
        std::cout << "Failed to initialize. Please check previous error messages for more information. The program will now exit.\n";
        return -1;
    }

    // A game on the command line; without one, the game browser opens
    if (argc >= 2)
    {
        if (!host.loadGame(argv[1]))
        {
            gui.openGameBrowser();
            gui.showMessage("Error", std::string("Couldn't load ") + argv[1]);
        }
    }

    mainLoop();

    shutdown();

    return 0;
}
