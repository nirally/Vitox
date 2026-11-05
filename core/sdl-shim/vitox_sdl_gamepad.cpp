// Vitox SDL shim - the 18 SDL3 gamepad functions of the REAL vita3k/ctrl/src/ctrl.cpp + modules/SceCtrl/SceCtrl.cpp
// (M8B.2, approved architecture: Windows.Gaming.Input -> this shim -> real ctrl.cpp -> real SceCtrl HLE). NOT SDL.
// One SDL_Gamepad per host slot (compat/vitox_host_input.h); joystick instance id = slot + 1. No sensors, LED or
// rumble (reported unavailable). Stick Y keeps SDL's +down convention; values are SDL's Sint16 ranges.
#include <SDL3/SDL_gamepad.h>

#include <vitox_host_input.h>

#include <util/log.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>

struct SDL_Gamepad {
    unsigned slot;
    int player;
};

namespace {
std::atomic<const VitoxHostInput *> g_host{ nullptr };
SDL_Gamepad g_pads[VITOX_HOST_PADS] = { { 0, -1 }, { 1, -1 }, { 2, -1 }, { 3, -1 } };

bool read_slot(unsigned slot, VitoxPadReading &r) {
    const VitoxHostInput *h = g_host.load();
    return h && slot < VITOX_HOST_PADS && h->read(slot, &r);
}
Sint16 to_axis(float v) {
    const float c = std::clamp(v, -1.0f, 1.0f);
    return static_cast<Sint16>(c < 0 ? std::lround(c * 32768.0f) : std::lround(c * 32767.0f));
}
Sint16 to_trigger(float v) {
    return static_cast<Sint16>(std::lround(std::clamp(v, 0.0f, 1.0f) * 32767.0f));
}
// sampled diagnostic: a line when a slot's buttons change, at most every 100 ms per slot
std::atomic<uint32_t> g_last_buttons[VITOX_HOST_PADS];
std::atomic<int64_t> g_last_log_ms[VITOX_HOST_PADS];
void sample(unsigned slot, const VitoxPadReading &r) {
    if (g_last_buttons[slot].exchange(r.buttons) == r.buttons)
        return;
    const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (now - g_last_log_ms[slot].load() < 100)
        return;
    g_last_log_ms[slot] = now;
    LOG_INFO("Vitox [M8B.2] host pad {}: buttons 0x{:05X} | L ({:+.2f},{:+.2f}) R ({:+.2f},{:+.2f}) | LT {:.2f} RT {:.2f}", slot, r.buttons, r.lx, r.ly, r.rx, r.ry,
        r.lt, r.rt);
}
} // namespace

void vitox_set_host_input(const VitoxHostInput *host) { g_host = host; }
const VitoxHostInput *vitox_host_input() { return g_host.load(); }

SDL_JoystickID *SDL_GetGamepads(int *count) {
    auto *ids = static_cast<SDL_JoystickID *>(std::calloc(VITOX_HOST_PADS + 1, sizeof(SDL_JoystickID)));
    int n = 0;
    VitoxPadReading r;
    for (unsigned s = 0; ids && s < VITOX_HOST_PADS; ++s)
        if (read_slot(s, r))
            ids[n++] = s + 1;
    if (count)
        *count = n;
    return ids;
}
void SDL_free(void *mem) { std::free(mem); }

const char *SDL_GetGamepadNameForID(SDL_JoystickID id) {
    const VitoxHostInput *h = g_host.load();
    return h && id >= 1 && id <= VITOX_HOST_PADS ? h->name(id - 1) : nullptr;
}
SDL_GUID SDL_GetJoystickGUIDForID(SDL_JoystickID id) {
    SDL_GUID g{};
    const char tag[] = "VitoxWGIpad";
    std::memcpy(g.data, tag, sizeof(tag) - 1);
    g.data[15] = static_cast<Uint8>(id);
    return g;
}
SDL_Gamepad *SDL_OpenGamepad(SDL_JoystickID id) {
    if (id < 1 || id > VITOX_HOST_PADS)
        return nullptr;
    VitoxPadReading r;
    return read_slot(id - 1, r) ? &g_pads[id - 1] : nullptr;
}
void SDL_CloseGamepad(SDL_Gamepad *) {} // static slot objects
bool SDL_GamepadConnected(SDL_Gamepad *g) {
    VitoxPadReading r;
    return g && read_slot(g->slot, r);
}
const char *SDL_GetGamepadName(SDL_Gamepad *g) { return g ? SDL_GetGamepadNameForID(g->slot + 1) : nullptr; }
bool SDL_SetGamepadPlayerIndex(SDL_Gamepad *g, int player) {
    if (!g)
        return false;
    g->player = player;
    return true;
}
SDL_GamepadType SDL_GetGamepadType(SDL_Gamepad *) { return SDL_GAMEPAD_TYPE_XBOXONE; }
bool SDL_GamepadHasSensor(SDL_Gamepad *, SDL_SensorType) { return false; }           // no motion/sensor support
bool SDL_SetGamepadSensorEnabled(SDL_Gamepad *, SDL_SensorType, bool) { return false; }
SDL_PropertiesID SDL_GetGamepadProperties(SDL_Gamepad *) { return 0; }
bool SDL_GetBooleanProperty(SDL_PropertiesID, const char *, bool default_value) { return default_value; } // no LED capability
bool SDL_SetGamepadLED(SDL_Gamepad *, Uint8, Uint8, Uint8) { return false; }
bool SDL_RumbleGamepad(SDL_Gamepad *, Uint16, Uint16, Uint32) { return false; } // rumble: optional, not in this milestone

bool SDL_GetGamepadButton(SDL_Gamepad *g, SDL_GamepadButton button) {
    VitoxPadReading r;
    if (!g || button < 0 || button >= 32 || !read_slot(g->slot, r))
        return false;
    sample(g->slot, r);
    return (r.buttons >> button) & 1u;
}
Sint16 SDL_GetGamepadAxis(SDL_Gamepad *g, SDL_GamepadAxis axis) {
    VitoxPadReading r;
    if (!g || !read_slot(g->slot, r))
        return 0;
    switch (axis) {
    case SDL_GAMEPAD_AXIS_LEFTX: return to_axis(r.lx);
    case SDL_GAMEPAD_AXIS_LEFTY: return to_axis(r.ly);
    case SDL_GAMEPAD_AXIS_RIGHTX: return to_axis(r.rx);
    case SDL_GAMEPAD_AXIS_RIGHTY: return to_axis(r.ry);
    case SDL_GAMEPAD_AXIS_LEFT_TRIGGER: return to_trigger(r.lt);
    case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER: return to_trigger(r.rt);
    default: return 0;
    }
}
