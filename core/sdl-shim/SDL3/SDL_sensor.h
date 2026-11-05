// Vitox SDL shim - TYPE-ONLY (M4). NOT SDL. Needed by vita3k/motion/include/motion/state.h:
// SDL_Sensor (opaque, std::unique_ptr).
//
// DESTRUCTOR REQUIREMENT (approved for 0.0.6.0): motion/state.h's SDL_SensorDeleter calls
// SDL_CloseSensor() inline, so MotionState's destructor references it. Defined (inert, counted)
// in sdl-shim/vitox_sdl_sensor.cpp; nothing else from SDL_sensor is provided.
#pragma once

#include "SDL_vitox_types.h"
#include "SDL_error.h" // M9A.4U: SDL_GetError (real SDL reaches it via SDL_gamepad.h)

typedef struct SDL_Sensor SDL_Sensor;

extern "C" void SDL_CloseSensor(SDL_Sensor *sensor);

// M9A.4U: what the REAL vita3k/motion/src/motion.cpp uses (values from SDL3 SDL_sensor.h). Defined in
// sdl-shim/vitox_sdl_sensor_enum.cpp as "no sensors on this host" (the Xbox has no accelerometer/gyroscope).
#define SDL_STANDARD_GRAVITY 9.80665f
// SDL_SensorType: SDL_vitox_types.h
extern "C" SDL_SensorID *SDL_GetSensors(int *count);
extern "C" SDL_SensorType SDL_GetSensorTypeForID(SDL_SensorID instance_id);
extern "C" SDL_Sensor *SDL_OpenSensor(SDL_SensorID instance_id);
