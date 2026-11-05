// Call counters of the Vitox SDL core shim (for the core probe's log).
#pragma once

#define VITOX_SDL_SHIM_SYMBOLS                                                     \
    "SDLCALL SDL_Thread SDL_ThreadFunction SDL_CreateThread SDL_DetachThread "     \
    "SDL_Semaphore SDL_CreateSemaphore SDL_SignalSemaphore SDL_WaitSemaphore "     \
    "SDL_DestroySemaphore"

struct VitoxSdlShimReport {
    unsigned create_thread, detach_thread, threads_named;
    unsigned create_sem, signal_sem, wait_sem, destroy_sem;
};

extern "C" VitoxSdlShimReport vitox_sdl_shim_report();

// Type-only shim (M4) + the one approved function, SDL_CloseSensor (vitox_sdl_sensor.cpp).
#define VITOX_SDL_TYPE_SHIM_SYMBOLS                                                              \
    "types: SDL_Gamepad SDL_Haptic SDL_Sensor SDL_GamepadButton SDL_GUID SDL_EventType "         \
    "SDL_TouchFingerEvent SDL_GamepadTouchpadEvent SDL_TouchID SDL_FingerID SDL_WindowID "       \
    "SDL_JoystickID; functions: SDL_CloseSensor"

struct VitoxSdlSensorReport {
    unsigned close_total, close_non_null;
};

extern "C" VitoxSdlSensorReport vitox_sdl_sensor_report();

// M6C: the one approved SDL_timer function, SDL_GetTicks (vitox_sdl_timer.cpp), for overlay/common_dialog.cpp.
#define VITOX_SDL_TIMER_SHIM_SYMBOLS "functions: SDL_GetTicks"

struct VitoxSdlTimerReport {
    unsigned get_ticks;
};

extern "C" VitoxSdlTimerReport vitox_sdl_timer_report();
