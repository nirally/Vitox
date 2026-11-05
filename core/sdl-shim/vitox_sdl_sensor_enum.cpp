// Vitox SDL shim - sensor enumeration (M9A.4U). NOT SDL. Built in vitox_hle against the real SDL3 headers, for
// vita3k/motion/src/motion.cpp (linked for SceMotion). The Xbox has no built-in accelerometer/gyroscope: SDL itself
// reports zero sensors there, which upstream handles as has_device_motion_support = false (start_sensor_sampling then
// never opens a sensor). SDL_CloseSensor stays in vitox_sdl_sensor.cpp.
#include <SDL3/SDL_error.h>
#include <SDL3/SDL_sensor.h>

extern "C" SDL_SensorID *SDL_GetSensors(int *count) {
    if (count)
        *count = 0;
    return nullptr;
}
extern "C" SDL_SensorType SDL_GetSensorTypeForID(SDL_SensorID) { return SDL_SENSOR_INVALID; }
extern "C" SDL_Sensor *SDL_OpenSensor(SDL_SensorID) { return nullptr; }
// only reached from motion.cpp's SDL_OpenSensor failure log, which cannot happen with zero sensors
extern "C" const char *SDL_GetError(void) { return "Vitox SDL shim: no sensors on this host"; }
