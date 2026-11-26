#ifndef SDL_INPUT_HPP
#define SDL_INPUT_HPP

/*
 * Controller input for the SDL front end (SDLMainWindow.cpp): which keys and
 * joystick buttons drive each NES controller, configurable from the GUI
 * (Input menu) and kept in warpnes.ini.
 *
 * Each NES button has one keyboard key, and each player can have a joystick:
 * its stick and first hat give the directions, and A/B/Select/Start (and
 * optionally the directions too) can be bound to its buttons.
 */

#include <SDL2/SDL.h>
#include <string>

class SimpleINI;

enum { NES_BUTTONS = 8 };   // A, B, Select, Start, Up, Down, Left, Right (ControllerButton order)

struct PlayerInput
{
    int key[NES_BUTTONS];        // SDL scancode, 0 = none
    int joystick;                // joystick number (0, 1, ...), -1 = none
    int joyButton[NES_BUTTONS];  // joystick button, -1 = none (directions: stick/hat)
};

class InputConfig
{
public:
    PlayerInput player[2];

    InputConfig();
    void setDefaults(int p);
    void load(SimpleINI& ini);
    void save(SimpleINI& ini) const;

    // Joysticks: opened at startup and when the configuration changes.
    void openJoysticks();
    void closeJoysticks();
    int joystickCount() const;
    std::string joystickName(int index) const;
    // Joystick number (0, 1, ...) for an SDL joystick instance ID, or -1
    int joystickIndexFromID(SDL_JoystickID id) const;

    // Current state of player p's NES buttons, from the keyboard state and
    // the joysticks.
    void read(int p, const Uint8* keys, bool out[NES_BUTTONS]) const;

    static const char* buttonName(int b);
    static std::string keyName(int scancode);

private:
    enum { MAX_JOYSTICKS = 4 };
    SDL_Joystick* joy[MAX_JOYSTICKS];
    int joyCount;
};

#endif // SDL_INPUT_HPP
