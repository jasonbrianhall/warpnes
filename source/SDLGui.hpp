#ifndef SDL_GUI_HPP
#define SDL_GUI_HPP

/*
 * NESticle-style GUI for the SDL front end (SDLMainWindow.cpp): a menu bar
 * with drop-down menus and windows for loading games (long file names where
 * the system has them), choosing the display mode and configuring input.
 *
 * Everything is drawn in software over the 256x240 RGB565 game picture, so it
 * looks and works the same on MS-DOS (SDL3 DOS port) and Linux/Windows
 * (SDL2). It keeps no SDL state of its own: the front end turns SDL events
 * into GuiEvents (in picture coordinates) and implements GuiHost.
 */

#include <cstdint>
#include <memory>
#include <string>

class InputConfig;

struct GuiEvent
{
    enum Type { MouseMove, MouseDown, MouseUp, MouseWheel, KeyDown, JoyButton };
    Type type;
    int x = 0, y = 0;        // mouse position in picture coordinates
    int button = 0;          // mouse: 1 left, 2 middle, 3 right; joystick: button number
    int wheel = 0;           // MouseWheel: +1 up, -1 down
    int scancode = 0;        // KeyDown: SDL scancode
    bool ctrl = false, shift = false;
    int joystick = -1;       // JoyButton: joystick number (InputConfig numbering)
};

// What the GUI asks of the front end
class GuiHost
{
public:
    virtual ~GuiHost() {}
    virtual bool loadGame(const std::string& path) = 0;
    virtual bool gameLoaded() const = 0;
    virtual void resetGame() = 0;
    virtual void saveState(int slot) = 0;   // slots 1-4
    virtual bool loadState(int slot) = 0;
    virtual void quit() = 0;

    virtual int videoModeCount() = 0;
    virtual std::string videoModeName(int index) = 0;
    virtual int currentVideoMode() = 0;
    virtual bool setVideoMode(int index) = 0;
    virtual std::string videoError() { return ""; }  // why setVideoMode failed

    virtual InputConfig& input() = 0;
    virtual void inputChanged() = 0;        // save, reopen joysticks

    virtual std::string gameDirectory() = 0; // where the game browser starts
    virtual void setGameDirectory(const std::string& dir) = 0;
};

class GuiDialog;

class Gui
{
public:
    explicit Gui(GuiHost& host);
    ~Gui();

    // Menu bar open (the game is paused while it is)
    bool active() const { return barOpen; }
    void open();
    void close();
    void openGameBrowser();
    void showMessage(const std::string& title, const std::string& text);

    // Something to draw over the picture this frame (menus, or just the
    // mouse cursor for a few seconds after the mouse moves)
    bool visible(uint32_t nowMs) const;

    // Returns true when the GUI used the event
    bool handle(const GuiEvent& e, uint32_t nowMs);

    // Draw over a copy of the picture: w x h RGB565; only lines y0..y1-1 are
    // on the screen (VGA mode 13h shows 200 of the 240)
    void draw(uint16_t* pixels, int w, int h, int y0, int y1, uint32_t nowMs);

    // "Paused" label while the game is paused with the menus closed
    void setPausedLabel(bool on) { pausedLabel = on; }

    GuiHost& host;

private:
    friend class GuiDialog;
    void runMenuItem(int menu, int item);
    int menuAt(int x, int y) const;
    int itemAt(int menu, int x, int y) const;
    void menuBox(int menu, int& x, int& y, int& w, int& h) const;

    bool barOpen = false;
    int openMenu = -1;       // drop-down showing
    int hotItem = -1;
    std::unique_ptr<GuiDialog> dialog;
    std::unique_ptr<GuiDialog> pendingMessage;  // message box over the dialog
    int mouseX = 128, mouseY = 120;
    uint32_t lastMouseMove = 0;
    bool mouseSeen = false;
    bool pausedLabel = false;
    int top = 0, bottom = 240;  // visible lines
    int width = 256;
};

#endif // SDL_GUI_HPP
