// Vitox SDL shim (M4 types; M8B.2 functions). NOT SDL. Needed by vita3k/ctrl/include/ctrl/state.h:
// SDL_Gamepad (opaque, std::shared_ptr), SDL_GamepadButton (by value), SDL_GUID (via SDL_vitox_types.h).
// M8B.2: exactly the 18 gamepad functions the REAL vita3k/ctrl/src/ctrl.cpp + modules/SceCtrl/SceCtrl.cpp call, with
// SDL3's signatures and enum values, implemented over the host's Windows.Gaming.Input adapter in vitox_sdl_gamepad.cpp
// (compat/vitox_host_input.h). Any other SDL gamepad call still fails to compile.
#pragma once

#include "SDL_vitox_types.h"

typedef struct SDL_Gamepad SDL_Gamepad;
typedef std::int16_t Sint16;
typedef std::uint16_t Uint16;
typedef Uint32 SDL_PropertiesID;

typedef enum SDL_GamepadAxis {
    SDL_GAMEPAD_AXIS_INVALID = -1,
    SDL_GAMEPAD_AXIS_LEFTX,
    SDL_GAMEPAD_AXIS_LEFTY,
    SDL_GAMEPAD_AXIS_RIGHTX,
    SDL_GAMEPAD_AXIS_RIGHTY,
    SDL_GAMEPAD_AXIS_LEFT_TRIGGER,
    SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,
    SDL_GAMEPAD_AXIS_COUNT
} SDL_GamepadAxis;

typedef enum SDL_GamepadType {
    SDL_GAMEPAD_TYPE_UNKNOWN = 0,
    SDL_GAMEPAD_TYPE_STANDARD,
    SDL_GAMEPAD_TYPE_XBOX360,
    SDL_GAMEPAD_TYPE_XBOXONE,
    SDL_GAMEPAD_TYPE_PS3,
    SDL_GAMEPAD_TYPE_PS4,
    SDL_GAMEPAD_TYPE_PS5,
    SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO,
    SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT,
    SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT,
    SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR,
    SDL_GAMEPAD_TYPE_COUNT
} SDL_GamepadType;

// SDL_SensorType: moved to SDL_vitox_types.h (M9A.4U, shared with SDL_sensor.h)

#define SDL_PROP_GAMEPAD_CAP_RGB_LED_BOOLEAN "SDL.joystick.cap.rgb_led"
#define SDL_HAPTIC_INFINITY 4294967295U


typedef enum SDL_GamepadButton {
    SDL_GAMEPAD_BUTTON_INVALID = -1,
    SDL_GAMEPAD_BUTTON_SOUTH,
    SDL_GAMEPAD_BUTTON_EAST,
    SDL_GAMEPAD_BUTTON_WEST,
    SDL_GAMEPAD_BUTTON_NORTH,
    SDL_GAMEPAD_BUTTON_BACK,
    SDL_GAMEPAD_BUTTON_GUIDE,
    SDL_GAMEPAD_BUTTON_START,
    SDL_GAMEPAD_BUTTON_LEFT_STICK,
    SDL_GAMEPAD_BUTTON_RIGHT_STICK,
    SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,
    SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,
    SDL_GAMEPAD_BUTTON_DPAD_UP,
    SDL_GAMEPAD_BUTTON_DPAD_DOWN,
    SDL_GAMEPAD_BUTTON_DPAD_LEFT,
    SDL_GAMEPAD_BUTTON_DPAD_RIGHT,
    SDL_GAMEPAD_BUTTON_MISC1,
    SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1,
    SDL_GAMEPAD_BUTTON_LEFT_PADDLE1,
    SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2,
    SDL_GAMEPAD_BUTTON_LEFT_PADDLE2,
    SDL_GAMEPAD_BUTTON_TOUCHPAD,
    SDL_GAMEPAD_BUTTON_MISC2,
    SDL_GAMEPAD_BUTTON_MISC3,
    SDL_GAMEPAD_BUTTON_MISC4,
    SDL_GAMEPAD_BUTTON_MISC5,
    SDL_GAMEPAD_BUTTON_MISC6,
    SDL_GAMEPAD_BUTTON_COUNT
} SDL_GamepadButton;

static_assert(SDL_GAMEPAD_BUTTON_COUNT == 26);

// M8B.2: the REAL ctrl.cpp / SceCtrl.cpp gamepad calls (sdl-shim/vitox_sdl_gamepad.cpp)
SDL_JoystickID *SDL_GetGamepads(int *count);
const char *SDL_GetGamepadNameForID(SDL_JoystickID instance_id);
SDL_GUID SDL_GetJoystickGUIDForID(SDL_JoystickID instance_id);
SDL_Gamepad *SDL_OpenGamepad(SDL_JoystickID instance_id);
void SDL_CloseGamepad(SDL_Gamepad *gamepad);
bool SDL_GamepadConnected(SDL_Gamepad *gamepad);
const char *SDL_GetGamepadName(SDL_Gamepad *gamepad);
bool SDL_SetGamepadPlayerIndex(SDL_Gamepad *gamepad, int player_index);
SDL_GamepadType SDL_GetGamepadType(SDL_Gamepad *gamepad);
bool SDL_GamepadHasSensor(SDL_Gamepad *gamepad, SDL_SensorType type);
bool SDL_SetGamepadSensorEnabled(SDL_Gamepad *gamepad, SDL_SensorType type, bool enabled);
SDL_PropertiesID SDL_GetGamepadProperties(SDL_Gamepad *gamepad);
bool SDL_GetBooleanProperty(SDL_PropertiesID props, const char *name, bool default_value);
bool SDL_SetGamepadLED(SDL_Gamepad *gamepad, Uint8 red, Uint8 green, Uint8 blue);
bool SDL_GetGamepadButton(SDL_Gamepad *gamepad, SDL_GamepadButton button);
Sint16 SDL_GetGamepadAxis(SDL_Gamepad *gamepad, SDL_GamepadAxis axis);
bool SDL_RumbleGamepad(SDL_Gamepad *gamepad, Uint16 low_frequency_rumble, Uint16 high_frequency_rumble, Uint32 duration_ms);
void SDL_free(void *mem);
