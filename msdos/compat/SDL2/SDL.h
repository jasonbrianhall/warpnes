/*
 * SDL2/SDL.h for the MS-DOS build of WarpNES -- the slice of the SDL2 API
 * that WarpNES uses, on top of SDL3.
 *
 * SDL3 runs on MS-DOS (DJGPP, 32-bit DPMI: VESA/VGA video, Sound Blaster,
 * keyboard, mouse, gameport); SDL2 does not. msdos/Makefile puts this folder
 * first on the include path, so the unchanged "#include <SDL2/SDL.h>" in the
 * WarpNES sources lands here and the SDL2 names map onto SDL3.
 *
 * Adapted from SDLPoP-DOS (src/sdl2_compat.h).
 *
 * Every SDL2 function that returned 0 on success returns an int 0 here too,
 * so "< 0 means error" checks keep working.
 */
#ifndef WARPNES_SDL2_COMPAT_H
#define WARPNES_SDL2_COMPAT_H

/* Our own SDL2 names below, not SDL3's "renamed" error markers. */
#define SDL_oldnames_h_
#include <SDL3/SDL.h>
#include <string.h>

/* ---------------------------------------------------------------- basics */
#define SDL_TRUE  true
#define SDL_FALSE false
#define SDL_ENABLE  1
#define SDL_DISABLE 0
#define SDL_bool bool
#define SDL_INIT_GAMECONTROLLER SDL_INIT_GAMEPAD
#define SDL_INIT_TIMER 0
#define SDL_INIT_NOPARACHUTE 0
#define SDL_NUM_SCANCODES SDL_SCANCODE_COUNT
#define KMOD_SHIFT SDL_KMOD_SHIFT
#define KMOD_CTRL  SDL_KMOD_CTRL
#define KMOD_ALT   SDL_KMOD_ALT

static inline int SDL2_rc(bool ok) { return ok ? 0 : -1; }

#define SDL_Init(f)            SDL2_rc(SDL_Init(f))
#define SDL_InitSubSystem(f)   SDL2_rc(SDL_InitSubSystem(f))

/* ---------------------------------------------------------------- events */
#define SDL_KEYDOWN                SDL_EVENT_KEY_DOWN
#define SDL_KEYUP                  SDL_EVENT_KEY_UP
#define SDL_QUIT                   SDL_EVENT_QUIT
#define SDL_JOYAXISMOTION          SDL_EVENT_JOYSTICK_AXIS_MOTION
#define SDL_JOYHATMOTION           SDL_EVENT_JOYSTICK_HAT_MOTION
#define SDL_JOYBUTTONDOWN          SDL_EVENT_JOYSTICK_BUTTON_DOWN
#define SDL_JOYBUTTONUP            SDL_EVENT_JOYSTICK_BUTTON_UP
#define SDL_CONTROLLERAXISMOTION   SDL_EVENT_GAMEPAD_AXIS_MOTION
#define SDL_CONTROLLERBUTTONDOWN   SDL_EVENT_GAMEPAD_BUTTON_DOWN
#define SDL_CONTROLLERBUTTONUP     SDL_EVENT_GAMEPAD_BUTTON_UP
#define SDL_CONTROLLERDEVICEADDED  SDL_EVENT_GAMEPAD_ADDED
#define SDL_CONTROLLERDEVICEREMOVED SDL_EVENT_GAMEPAD_REMOVED
/* SDL3 has a separate event type per window event, so SDL2's single
 * SDL_WINDOWEVENT (and its .window.event sub-type) has no equivalent; the
 * DOS frontend doesn't use it. */
static inline int SDL2_PollEvent(SDL_Event* e) { return SDL_PollEvent(e) ? 1 : 0; }
#define SDL_PollEvent SDL2_PollEvent
#define SDL_GetKeyboardState(n)    ((const Uint8*)SDL_GetKeyboardState(n))
/* SDL2 event members that SDL3 renamed (gamepad events). */
#define cbutton gbutton
#define caxis   gaxis

/* ---------------------------------------------------------------- joysticks / game controllers */
/* SDL2 took device indices; SDL3 takes instance IDs. */
static inline SDL_JoystickID SDL2_joystick_id(int index) {
	int n = 0;
	SDL_JoystickID* ids = SDL_GetJoysticks(&n);
	SDL_JoystickID id = (ids && index >= 0 && index < n) ? ids[index] : 0;
	SDL_free(ids);
	return id;
}
static inline int SDL_NumJoysticks(void) {
	int n = 0;
	SDL_JoystickID* ids = SDL_GetJoysticks(&n);
	SDL_free(ids);
	return n;
}
#define SDL_GameController                    SDL_Gamepad
#define SDL_GameControllerButton              SDL_GamepadButton
#define SDL_GameControllerAxis                SDL_GamepadAxis
#define SDL_IsGameController(i)               SDL_IsGamepad(SDL2_joystick_id(i))
#define SDL_GameControllerOpen(i)             SDL_OpenGamepad(SDL2_joystick_id(i))
#define SDL_GameControllerClose               SDL_CloseGamepad
#define SDL_GameControllerName                SDL_GetGamepadName
#define SDL_GameControllerGetJoystick         SDL_GetGamepadJoystick
#define SDL_GameControllerAddMappingsFromFile SDL_AddGamepadMappingsFromFile
#define SDL_GameControllerEventState(s)       (SDL_SetGamepadEventsEnabled((s) != 0), (s))
#define SDL_JoystickOpen(i)                   SDL_OpenJoystick(SDL2_joystick_id(i))
#define SDL_JoystickClose                     SDL_CloseJoystick
#define SDL_JoystickName                      SDL_GetJoystickName
#define SDL_JoystickNameForIndex(i)           SDL_GetJoystickNameForID(SDL2_joystick_id(i))
#define SDL_JoystickInstanceID                SDL_GetJoystickID
#define SDL_JoystickNumButtons                SDL_GetNumJoystickButtons
#define SDL_JoystickNumAxes                   SDL_GetNumJoystickAxes
#define SDL_JoystickNumHats                   SDL_GetNumJoystickHats
#define SDL_JoystickGetButton(j, b)           ((Uint8)SDL_GetJoystickButton(j, b))
#define SDL_JoystickGetAxis                   SDL_GetJoystickAxis
#define SDL_JoystickGetHat                    SDL_GetJoystickHat
#define SDL_JoystickEventState(s)             (SDL_SetJoystickEventsEnabled((s) != 0), (s))
#define SDL_CONTROLLER_AXIS_LEFTX          SDL_GAMEPAD_AXIS_LEFTX
#define SDL_CONTROLLER_AXIS_LEFTY          SDL_GAMEPAD_AXIS_LEFTY
#define SDL_CONTROLLER_AXIS_RIGHTX         SDL_GAMEPAD_AXIS_RIGHTX
#define SDL_CONTROLLER_AXIS_RIGHTY         SDL_GAMEPAD_AXIS_RIGHTY
#define SDL_CONTROLLER_AXIS_TRIGGERLEFT    SDL_GAMEPAD_AXIS_LEFT_TRIGGER
#define SDL_CONTROLLER_AXIS_TRIGGERRIGHT   SDL_GAMEPAD_AXIS_RIGHT_TRIGGER
#define SDL_CONTROLLER_BUTTON_A            SDL_GAMEPAD_BUTTON_SOUTH
#define SDL_CONTROLLER_BUTTON_B            SDL_GAMEPAD_BUTTON_EAST
#define SDL_CONTROLLER_BUTTON_X            SDL_GAMEPAD_BUTTON_WEST
#define SDL_CONTROLLER_BUTTON_Y            SDL_GAMEPAD_BUTTON_NORTH
#define SDL_CONTROLLER_BUTTON_BACK         SDL_GAMEPAD_BUTTON_BACK
#define SDL_CONTROLLER_BUTTON_START        SDL_GAMEPAD_BUTTON_START
#define SDL_CONTROLLER_BUTTON_DPAD_UP      SDL_GAMEPAD_BUTTON_DPAD_UP
#define SDL_CONTROLLER_BUTTON_DPAD_DOWN    SDL_GAMEPAD_BUTTON_DPAD_DOWN
#define SDL_CONTROLLER_BUTTON_DPAD_LEFT    SDL_GAMEPAD_BUTTON_DPAD_LEFT
#define SDL_CONTROLLER_BUTTON_DPAD_RIGHT   SDL_GAMEPAD_BUTTON_DPAD_RIGHT

/* ---------------------------------------------------------------- window / renderer */
#define SDL_WINDOW_FULLSCREEN_DESKTOP SDL_WINDOW_FULLSCREEN
#define SDL_SetWindowFullscreen(w, f) SDL2_rc(SDL_SetWindowFullscreen(w, (f) != 0))

/* ---------------------------------------------------------------- audio (SDL2 callback device) */
#define AUDIO_U8     SDL_AUDIO_U8
#define AUDIO_S8     SDL_AUDIO_S8
#define AUDIO_S16SYS SDL_AUDIO_S16
typedef void (*SDL2_AudioCallback)(void* userdata, Uint8* stream, int len);
typedef struct SDL2_AudioSpec {
	int freq;
	SDL_AudioFormat format;
	Uint8 channels;
	Uint8 silence;
	Uint16 samples;
	Uint32 size;
	SDL2_AudioCallback callback;
	void* userdata;
} SDL2_AudioSpec;
#ifndef SDL2_COMPAT_IMPL
#define SDL_AudioSpec SDL2_AudioSpec
#endif
#ifdef __cplusplus
extern "C" {
#endif
int  SDL2_OpenAudio(SDL2_AudioSpec* desired, SDL2_AudioSpec* obtained);
void SDL2_PauseAudio(int pause_on);
void SDL2_LockAudio(void);
void SDL2_UnlockAudio(void);
void SDL2_CloseAudio(void);
#ifdef __cplusplus
}
#endif
#define SDL_OpenAudio   SDL2_OpenAudio
#define SDL_PauseAudio  SDL2_PauseAudio
#define SDL_LockAudio   SDL2_LockAudio
#define SDL_UnlockAudio SDL2_UnlockAudio
#define SDL_CloseAudio  SDL2_CloseAudio

#endif /* WARPNES_SDL2_COMPAT_H */
