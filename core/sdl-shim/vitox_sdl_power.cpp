// Vitox SDL shim - power info (M9A.6). NOT SDL. The Xbox has no battery: SDL reports SDL_POWERSTATE_NO_BATTERY with
// unknown (-1) seconds and percent on such hosts, which is what ScePower.cpp's battery queries then translate.
#include <SDL3/SDL_power.h>

extern "C" SDL_PowerState SDL_GetPowerInfo(int *seconds, int *percent) {
    if (seconds)
        *seconds = -1;
    if (percent)
        *percent = -1;
    return SDL_POWERSTATE_NO_BATTERY;
}
