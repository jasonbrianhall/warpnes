/*
 * Controller input for the SDL front end. See SDLInput.hpp.
 */
#include "SDLInput.hpp"
#include "Configuration.hpp"

#include <cstdio>
#include <cstring>

static const char* const buttonNames[NES_BUTTONS] = {
    "A", "B", "Select", "Start", "Up", "Down", "Left", "Right"
};
static const char* const buttonKeys[NES_BUTTONS] = {
    "a", "b", "select", "start", "up", "down", "left", "right"
};

static const int JOY_DEADZONE = 8000;

InputConfig::InputConfig() : joyCount(0)
{
    for (int i = 0; i < MAX_JOYSTICKS; i++) joy[i] = nullptr;
    setDefaults(0);
    setDefaults(1);
}

void InputConfig::setDefaults(int p)
{
    PlayerInput& in = player[p];
    if (p == 0) {
        // The keys the SDL build has always used
        const int keys[NES_BUTTONS] = {
            SDL_SCANCODE_X, SDL_SCANCODE_Z, SDL_SCANCODE_LEFTBRACKET, SDL_SCANCODE_RIGHTBRACKET,
            SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT
        };
        memcpy(in.key, keys, sizeof(keys));
    } else {
        const int keys[NES_BUTTONS] = {
            SDL_SCANCODE_K, SDL_SCANCODE_J, SDL_SCANCODE_U, SDL_SCANCODE_I,
            SDL_SCANCODE_W, SDL_SCANCODE_S, SDL_SCANCODE_A, SDL_SCANCODE_D
        };
        memcpy(in.key, keys, sizeof(keys));
    }
    in.joystick = p;  // player 1: first joystick, player 2: second
    // A on button 1, B on button 0 (the old defaults), Select 8, Start 9;
    // directions on the stick/hat
    const int buttons[NES_BUTTONS] = { 1, 0, 8, 9, -1, -1, -1, -1 };
    memcpy(in.joyButton, buttons, sizeof(buttons));
}

void InputConfig::load(SimpleINI& ini)
{
    for (int p = 0; p < 2; p++) {
        char name[48];
        PlayerInput& in = player[p];
        snprintf(name, sizeof(name), "p%d_joystick", p + 1);
        in.joystick = ini.getInt("input", name, in.joystick);
        for (int b = 0; b < NES_BUTTONS; b++) {
            snprintf(name, sizeof(name), "p%d_key_%s", p + 1, buttonKeys[b]);
            in.key[b] = ini.getInt("input", name, in.key[b]);
            snprintf(name, sizeof(name), "p%d_joy_%s", p + 1, buttonKeys[b]);
            in.joyButton[b] = ini.getInt("input", name, in.joyButton[b]);
        }
    }
}

void InputConfig::save(SimpleINI& ini) const
{
    for (int p = 0; p < 2; p++) {
        char name[48];
        const PlayerInput& in = player[p];
        snprintf(name, sizeof(name), "p%d_joystick", p + 1);
        ini.setInt("input", name, in.joystick);
        for (int b = 0; b < NES_BUTTONS; b++) {
            snprintf(name, sizeof(name), "p%d_key_%s", p + 1, buttonKeys[b]);
            ini.setInt("input", name, in.key[b]);
            snprintf(name, sizeof(name), "p%d_joy_%s", p + 1, buttonKeys[b]);
            ini.setInt("input", name, in.joyButton[b]);
        }
    }
}

void InputConfig::openJoysticks()
{
    closeJoysticks();
    int n = SDL_NumJoysticks();
    for (int i = 0; i < n && joyCount < MAX_JOYSTICKS; i++) {
        SDL_Joystick* j = SDL_JoystickOpen(i);
        if (j) {
            joy[joyCount++] = j;
            printf("Joystick %d: %s\n", joyCount, SDL_JoystickName(j) ? SDL_JoystickName(j) : "?");
        }
    }
}

void InputConfig::closeJoysticks()
{
    for (int i = 0; i < joyCount; i++) {
        if (joy[i]) SDL_JoystickClose(joy[i]);
        joy[i] = nullptr;
    }
    joyCount = 0;
}

int InputConfig::joystickCount() const { return joyCount; }

std::string InputConfig::joystickName(int index) const
{
    if (index < 0 || index >= joyCount || !joy[index]) return "";
    const char* n = SDL_JoystickName(joy[index]);
    return n ? n : "Joystick";
}

int InputConfig::joystickIndexFromID(SDL_JoystickID id) const
{
    for (int i = 0; i < joyCount; i++)
        if (joy[i] && SDL_JoystickInstanceID(joy[i]) == id) return i;
    return -1;
}

void InputConfig::read(int p, const Uint8* keys, bool out[NES_BUTTONS]) const
{
    const PlayerInput& in = player[p];
    for (int b = 0; b < NES_BUTTONS; b++)
        out[b] = in.key[b] > 0 && in.key[b] < SDL_NUM_SCANCODES && keys[in.key[b]];

    if (in.joystick < 0 || in.joystick >= joyCount || !joy[in.joystick]) return;
    SDL_Joystick* j = joy[in.joystick];

    int buttons = SDL_JoystickNumButtons(j);
    for (int b = 0; b < NES_BUTTONS; b++) {
        int jb = in.joyButton[b];
        if (jb >= 0 && jb < buttons && SDL_JoystickGetButton(j, jb)) out[b] = true;
    }

    // Directions: first stick and first hat
    if (SDL_JoystickNumAxes(j) >= 2) {
        int x = SDL_JoystickGetAxis(j, 0), y = SDL_JoystickGetAxis(j, 1);
        if (x < -JOY_DEADZONE) out[6] = true;
        if (x > JOY_DEADZONE) out[7] = true;
        if (y < -JOY_DEADZONE) out[4] = true;
        if (y > JOY_DEADZONE) out[5] = true;
    }
    if (SDL_JoystickNumHats(j) >= 1) {
        Uint8 h = SDL_JoystickGetHat(j, 0);
        if (h & SDL_HAT_UP) out[4] = true;
        if (h & SDL_HAT_DOWN) out[5] = true;
        if (h & SDL_HAT_LEFT) out[6] = true;
        if (h & SDL_HAT_RIGHT) out[7] = true;
    }
}

const char* InputConfig::buttonName(int b)
{
    return (b >= 0 && b < NES_BUTTONS) ? buttonNames[b] : "?";
}

std::string InputConfig::keyName(int scancode)
{
    if (scancode <= 0) return "(none)";
    const char* n = SDL_GetScancodeName((SDL_Scancode)scancode);
    if (!n || !*n) {
        char buf[24];
        snprintf(buf, sizeof(buf), "Key %d", scancode);
        return buf;
    }
    return n;
}
