// Vitox (M7A, user-approved 2026-10-19): Vita3K's vblank thread (display/src/display.cpp vblank_sync_thread) calls
// touch_vsync_update() and refresh_motion() every vblank. Their TUs (touch.cpp, motion.cpp) are built on the SDL
// event/sensor API, i.e. the input runtime, which is not part of the first GXM visual milestone. Under VITOX_UWP
// only those two calls are compiled out (core-build/CMakeLists.txt, "M7A display"); each skipped call is counted
// here. Everything else in the vblank thread (vblank_count, callbacks, waiters, pacing) is the real code.
#pragma once

enum VitoxVblankInputSite : unsigned {
    VITOX_VBLANK_TOUCH = 0,  // touch_vsync_update(emuenv) skipped
    VITOX_VBLANK_MOTION = 1, // refresh_motion(emuenv.motion, emuenv.ctrl) skipped
    VITOX_VBLANK_SITE_COUNT = 2,
};

extern "C" void vitox_vblank_input_skipped(unsigned site);
extern "C" unsigned long long vitox_vblank_input_skipped_count(unsigned site);
