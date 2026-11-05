// Vitox SDL shim - TYPE-ONLY (M4). NOT SDL. Needed by vita3k/touch/include/touch/state.h:
// SDL_TouchFingerEvent and SDL_GamepadTouchpadEvent embedded BY VALUE (see SDL_vitox_types.h).
// No SDL_Event union, no event constants, no SDL_* functions.
#pragma once

#include "SDL_vitox_types.h"
