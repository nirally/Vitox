// Vitox SDL shim - SDL_timer, ONE function (approved, M6C): SDL_GetTicks. NOT SDL.
// Needed only by vita3k/overlay/src/common_dialog.cpp (one call, elapsed dialog/UI time).
// Same signature as SDL3 (Uint64 milliseconds, no wrap). Nothing else from SDL_timer is declared, so any
// other SDL timer API (SDL_GetTicksNS, SDL_Delay, SDL_AddTimer, ...) fails to compile.
#pragma once

#include "SDL_vitox_types.h"

extern "C" Uint64 SDL_GetTicks(void);
