// Vitox SDL core shim - NOT SDL. Shadows <SDL3/SDL_mutex.h> for the selected Vita3K core
// sources only (kernel/src/kernel.cpp thread-start handshake).
// Shimmed symbols (sync): SDL_Semaphore, SDL_CreateSemaphore, SDL_SignalSemaphore,
// SDL_WaitSemaphore, SDL_DestroySemaphore.
#pragma once

#include <cstdint>

struct SDL_Semaphore;

extern "C" SDL_Semaphore *SDL_CreateSemaphore(std::uint32_t initial_value);
extern "C" void SDL_SignalSemaphore(SDL_Semaphore *sem);
extern "C" void SDL_WaitSemaphore(SDL_Semaphore *sem);
extern "C" void SDL_DestroySemaphore(SDL_Semaphore *sem);
