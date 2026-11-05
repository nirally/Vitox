// Vitox SDL shim - SDL_CloseSensor (approved, M4/0.0.6.0). NOT SDL.
// Needed only because motion/state.h's SDL_SensorDeleter references it from ~MotionState.
// An inert MotionState never opens sensors: null -> no-op. Non-null (unexpected) is counted and
// returned without touching the pointer; it never aborts. 0.0.6.0 requires non-null calls == 0.
#include <SDL3/SDL_sensor.h>

#include "vitox_sdl_shim_report.h"

#include <atomic>

namespace {
std::atomic<unsigned> g_close_total{ 0 }, g_close_non_null{ 0 };
}

extern "C" void SDL_CloseSensor(SDL_Sensor *sensor) {
    ++g_close_total;
    if (sensor)
        ++g_close_non_null;
}

extern "C" VitoxSdlSensorReport vitox_sdl_sensor_report() { return { g_close_total.load(), g_close_non_null.load() }; }
