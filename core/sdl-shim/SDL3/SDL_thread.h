// Vitox SDL core shim - NOT SDL. Shadows <SDL3/SDL_thread.h> for the selected Vita3K core
// sources only (kernel/src/kernel.cpp guest-thread creation). Real SDL3 refuses UWP
// (SDL_platform_defines.h: "Windows RT/UWP is no longer supported in SDL") and is not used.
// Only the symbols below exist; any other SDL API or header is a compile error by design.
// Shimmed symbols (thread): SDLCALL, SDL_Thread, SDL_ThreadFunction, SDL_CreateThread, SDL_DetachThread.
#pragma once

#include "SDL_mutex.h"

#ifndef SDLCALL
#define SDLCALL __cdecl
#endif

struct SDL_Thread;
typedef int(SDLCALL *SDL_ThreadFunction)(void *data);

extern "C" SDL_Thread *SDL_CreateThread(SDL_ThreadFunction fn, const char *name, void *data);
extern "C" void SDL_DetachThread(SDL_Thread *thread);
