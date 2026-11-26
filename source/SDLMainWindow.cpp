#include <cstdio>
#include <cstdlib>
#include <cstring>
#if defined(_WIN32) && !defined(__DJGPP__)
#include <direct.h>   // _mkdir, _chdir
#endif
#include <iostream>
#include <string>

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
        SDL_SetWindowSize(window, RENDER_WIDTH * windowScale, RENDER_HEIGHT * windowScale);
    }
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
                              0);
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

    if (SDL_RenderSetLogicalSize(renderer, RENDER_WIDTH, RENDER_HEIGHT) < 0)
    {
        std::cout << "SDL_RenderSetLogicalSize() failed during initialize(): " << SDL_GetError() << std::endl;
        return false;
    }

    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, RENDER_WIDTH, RENDER_HEIGHT);
    if (texture == nullptr)
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
        desiredSpec.freq = Configuration::getAudioFrequency();
        desiredSpec.format = AUDIO_S8;
        desiredSpec.channels = 1;
        desiredSpec.samples = 2048;
        desiredSpec.callback = audioCallback;
        desiredSpec.userdata = NULL;

        SDL_AudioSpec obtainedSpec;
        SDL_OpenAudio(&desiredSpec, &obtainedSpec);

        // Start playing audio
        SDL_PauseAudio(0);
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
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
#endif
    SDL_DestroyWindow(window);

    SDL_Quit();
}

// Put a 256x240 RGB565 picture on the screen
static void present(const uint16_t* frame)
{
#ifdef __DJGPP__
    // MS-DOS: straight onto the VESA/VGA screen
    dos_present(window, frame, RENDER_WIDTH, RENDER_HEIGHT);
#else
    convertRGB565ToARGB8888(frame, renderBuffer, RENDER_WIDTH, RENDER_HEIGHT);
    SDL_RenderClear(renderer);
    SDL_UpdateTexture(texture, NULL, renderBuffer, sizeof(uint32_t) * RENDER_WIDTH);
    SDL_RenderSetLogicalSize(renderer, RENDER_WIDTH, RENDER_HEIGHT);
    SDL_RenderCopy(renderer, texture, NULL, NULL);

    // Render scanlines if enabled
    if (Configuration::getScanlinesEnabled() && scanlineTexture)
    {
        SDL_RenderSetLogicalSize(renderer, RENDER_WIDTH * 3, RENDER_HEIGHT * 3);
        SDL_RenderCopy(renderer, scanlineTexture, NULL, NULL);
        SDL_RenderSetLogicalSize(renderer, RENDER_WIDTH, RENDER_HEIGHT);
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
    // SDL2 already gives renderer logical coordinates (RenderSetLogicalSize)
    *px = (int)x;
    *py = (int)y;
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
        gui.handle(ge, now);
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        ge.type = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ? GuiEvent::MouseDown : GuiEvent::MouseUp;
        ge.button = event.button.button;
        mouseToPicture(event.button.x, event.button.y, &ge.x, &ge.y);
        gui.handle(ge, now);
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
        gui.handle(ge, now);
        break;
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
        ge.type = event.type == SDL_MOUSEBUTTONDOWN ? GuiEvent::MouseDown : GuiEvent::MouseUp;
        ge.button = event.button.button;
        mouseToPicture((float)event.button.x, (float)event.button.y, &ge.x, &ge.y);
        gui.handle(ge, now);
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

            smbEngine->update();
            smbEngine->render16(renderBuffer16);
        }

        gui.setPausedLabel(paused && smbEngine && !gui.active());
        if (gui.visible(now))
        {
            int y0 = 0, y1 = RENDER_HEIGHT;
#ifdef __DJGPP__
            dos_visible_lines(RENDER_HEIGHT, &y0, &y1);
#endif
            memcpy(guiBuffer16, renderBuffer16, sizeof(guiBuffer16));
            gui.draw(guiBuffer16, RENDER_WIDTH, RENDER_HEIGHT, y0, y1, now);
            present(guiBuffer16);
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
