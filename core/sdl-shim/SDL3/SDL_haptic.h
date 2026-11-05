// Vitox SDL shim - TYPE-ONLY (M4). NOT SDL. Needed by vita3k/ctrl/include/ctrl/state.h:
// SDL_Haptic (opaque, std::shared_ptr). No SDL_* functions.
#pragma once

#include "SDL_vitox_types.h"

typedef struct SDL_Haptic SDL_Haptic;
