// Vitox M8B.2: the host's controller source (plain C ABI, no Vita3K/SDL/WinRT types). The UWP app implements it
// over Windows.Gaming.Input.Gamepad; the SDL3 gamepad shim (sdl-shim/vitox_sdl_gamepad.cpp) reads it, so the REAL
// vita3k/ctrl/src/ctrl.cpp + modules/SceCtrl/SceCtrl.cpp run unchanged. Without a registered host there are no pads.
#pragma once

#include <cstdint>

#define VITOX_HOST_PADS 4

struct VitoxPadReading {
    // bit (1u << SDL_GamepadButton): SOUTH=A EAST=B WEST=X NORTH=Y BACK=View START=Menu LEFT/RIGHT_STICK, LEFT/RIGHT_SHOULDER,
    // DPAD_UP/DOWN/LEFT/RIGHT (SDL3 enum order)
    uint32_t buttons;
    float lx, ly, rx, ry; // -1..1, SDL convention: +y = down
    float lt, rt;         // 0..1
};

// 0.0.24.54 (M9A.6): virtual touch cursor. L3 on the first pad toggles it; while active the left stick moves it, A
// touches the front panel at its position, and neither (nor L3) reaches the game's pad. The launcher feeds it to
// Vita3K's mouse-as-touch fields (touch_vsync_update, unchanged); the presenter draws it.
struct VitoxTouchCursor {
    bool active;
    bool pressed;  // active and A held
    float x, y;    // 0..1 over the Vita screen (the presented viewport)
    float shown_s; // seconds since it was turned on
    float press_s; // seconds since the last A press, < 0 = none yet
};

// 0.0.25.0: the Vitox quick menu (in-game overlay). R3 on the first pad opens it while a game session is active; while
// it is visible no pad input reaches the game (every slot reads neutral) and the buttons still held when it closes stay
// masked until released. The host advances it; the launcher pauses the guest while `open` and stops the session on `quit`.
struct VitoxOverlayTick {
    bool visible;  // drawn (open, or still animating closed)
    bool open;     // the menu is open: the launcher keeps the guest paused
    bool quit;     // "Return to Launcher" was confirmed (latched until the next session starts)
    float anim;    // 0 = closed .. 1 = fully open (eased)
    float time_s;  // seconds since the menu last opened (animation clock)
    int selected;  // focused item index
    bool confirm;  // the focused item is waiting for a second A press
    float confirm_anim; // 0..1 eased confirm state
    float press_s; // seconds since the last A press on an item, < 0 = none
};

struct VitoxHostInput {
    bool (*read)(unsigned slot, VitoxPadReading *out); // false: no pad in this slot (slot < VITOX_HOST_PADS)
    const char *(*name)(unsigned slot);
    uint32_t (*generation)(); // changes on every add/remove: the launcher then calls Vita3K's refresh_controllers
    void (*touch_cursor)(VitoxTouchCursor *out); // advances the cursor to now, then reports it (0.0.24.54)
    void (*overlay)(VitoxOverlayTick *out);      // 0.0.25.0: advances the quick menu to now, then reports it (may be null)
};

void vitox_set_host_input(const VitoxHostInput *host); // nullptr = no pads
const VitoxHostInput *vitox_host_input();
