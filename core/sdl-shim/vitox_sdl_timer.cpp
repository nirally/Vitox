// Vitox SDL shim - SDL_GetTicks (approved, M6C). NOT SDL.
// SDL3 semantics: milliseconds since SDL initialization as Uint64 (never wraps). The shim's epoch is the
// first call; GetTickCount64() is monotonic and also 64-bit, so there is no wrap to preserve.
#include <SDL3/SDL_timer.h>

#include "vitox_sdl_shim_report.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <atomic>

namespace {
std::atomic<unsigned> g_get_ticks{ 0 };
}

extern "C" Uint64 SDL_GetTicks(void) {
    static const ULONGLONG epoch = GetTickCount64();
    ++g_get_ticks;
    return static_cast<Uint64>(GetTickCount64() - epoch);
}

extern "C" VitoxSdlTimerReport vitox_sdl_timer_report() { return { g_get_ticks.load() }; }
