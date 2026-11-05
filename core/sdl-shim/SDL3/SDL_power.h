// Vitox SDL shim (M9A.6). NOT SDL. SDL_GetPowerInfo for vita3k/modules/ScePower/ScePower.cpp (values from SDL3
// SDL_power.h); defined in sdl-shim/vitox_sdl_power.cpp.
#pragma once

typedef enum SDL_PowerState {
    SDL_POWERSTATE_ERROR = -1,
    SDL_POWERSTATE_UNKNOWN,
    SDL_POWERSTATE_ON_BATTERY,
    SDL_POWERSTATE_NO_BATTERY,
    SDL_POWERSTATE_CHARGING,
    SDL_POWERSTATE_CHARGED
} SDL_PowerState;

extern "C" SDL_PowerState SDL_GetPowerInfo(int *seconds, int *percent);
