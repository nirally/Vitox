// Vitox SDL shim (M9A.4U). NOT SDL. SDL_GetError only, for vita3k/motion/src/motion.cpp's sensor-open failure log;
// defined in sdl-shim/vitox_sdl_sensor_enum.cpp.
#pragma once

extern "C" const char *SDL_GetError(void);
