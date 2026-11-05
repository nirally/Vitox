// Vitox SDL shim - TYPE-ONLY declarations (M4 inventory). NOT SDL.
// Only the types that Vita3K subsystem STATE structs embed (ctrl/motion/touch state headers).
// Layouts and enum values are copied from external/sdl/include/SDL3 (SDL3 at Vita3K 7224a0fc):
//   SDL_GUID            - SDL_guid.h     { Uint8 data[16]; }
//   SDL_GamepadButton   - SDL_gamepad.h  values -1 .. SDL_GAMEPAD_BUTTON_COUNT
//   SDL_EventType       - SDL_events.h   4-byte enum (SDL_EVENT_ENUM_PADDING = 0x7FFFFFFF)
//   SDL_TouchFingerEvent, SDL_GamepadTouchpadEvent - SDL_events.h (embedded BY VALUE in TouchState)
// LAYOUT DEPENDENCY: TouchState embeds the two event structs by value; if a real SDL3 were later
// linked, these must match SDL's ABI exactly (static_asserts below pin the current sizes).
#pragma once

#include <cstdint>

typedef std::uint8_t Uint8;
typedef std::int32_t Sint32;
typedef std::uint32_t Uint32;
typedef std::uint64_t Uint64;

typedef Uint64 SDL_TouchID;
typedef Uint64 SDL_FingerID;
typedef Uint32 SDL_WindowID;
typedef Uint32 SDL_JoystickID;

typedef struct SDL_GUID {
    Uint8 data[16];
} SDL_GUID;

// Event-type CONSTANTS are deliberately absent: any code that dispatches on event types is not a
// state struct and must fail to compile against this shim.
typedef enum SDL_EventType {
    // M8B.1: the four finger-event values (SDL3 SDL_events.h) - only so the REAL touch.cpp compiles; no SDL event is
    // ever produced in Vitox, so its handle_touch_event() is never called
    SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN = 0x656, // (SDL_EVENT_GAMEPAD_AXIS_MOTION 0x650 + 6)
    SDL_EVENT_GAMEPAD_TOUCHPAD_MOTION,
    SDL_EVENT_GAMEPAD_TOUCHPAD_UP,
    // M9A.4U: real motion.cpp tells gamepad sensor events apart by this value (0x659); none is ever produced in Vitox
    SDL_EVENT_GAMEPAD_SENSOR_UPDATE,
    SDL_EVENT_FINGER_DOWN = 0x700,
    SDL_EVENT_FINGER_UP,
    SDL_EVENT_FINGER_MOTION,
    SDL_EVENT_FINGER_CANCELED,
    SDL_EVENT_ENUM_PADDING = 0x7FFFFFFF
} SDL_EventType;

typedef struct SDL_TouchFingerEvent {
    SDL_EventType type;
    Uint32 reserved;
    Uint64 timestamp;
    SDL_TouchID touchID;
    SDL_FingerID fingerID;
    float x;
    float y;
    float dx;
    float dy;
    float pressure;
    SDL_WindowID windowID;
} SDL_TouchFingerEvent;

typedef struct SDL_GamepadTouchpadEvent {
    SDL_EventType type;
    Uint32 reserved;
    Uint64 timestamp;
    SDL_JoystickID which;
    Sint32 touchpad;
    Sint32 finger;
    float x;
    float y;
    float pressure;
} SDL_GamepadTouchpadEvent;

// SDL3 SDL_sensor.h values (moved here from SDL_gamepad.h in M9A.4U; SDL_SENSOR_COUNT added)
typedef enum SDL_SensorType {
    SDL_SENSOR_INVALID = -1,
    SDL_SENSOR_UNKNOWN,
    SDL_SENSOR_ACCEL,
    SDL_SENSOR_GYRO,
    SDL_SENSOR_ACCEL_L,
    SDL_SENSOR_GYRO_L,
    SDL_SENSOR_ACCEL_R,
    SDL_SENSOR_GYRO_R,
    SDL_SENSOR_COUNT
} SDL_SensorType;

// M9A.4U: the two sensor event structs vita3k/motion/include/motion/event_handler.h takes by reference (SDL3 SDL_events.h)
typedef Uint32 SDL_SensorID;

typedef struct SDL_SensorEvent {
    SDL_EventType type;
    Uint32 reserved;
    Uint64 timestamp;
    SDL_SensorID which;
    float data[6];
    Uint64 sensor_timestamp;
} SDL_SensorEvent;

typedef struct SDL_GamepadSensorEvent {
    SDL_EventType type;
    Uint32 reserved;
    Uint64 timestamp;
    SDL_JoystickID which;
    Sint32 sensor;
    float data[3];
    Uint64 sensor_timestamp;
} SDL_GamepadSensorEvent;

static_assert(sizeof(SDL_GUID) == 16);
static_assert(sizeof(SDL_SensorEvent) == 56);
static_assert(sizeof(SDL_GamepadSensorEvent) == 48);
static_assert(sizeof(SDL_EventType) == 4);
static_assert(sizeof(SDL_TouchFingerEvent) == 56);
static_assert(sizeof(SDL_GamepadTouchpadEvent) == 40);
