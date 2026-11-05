// Vitox M8B.2: the host controller source for the REAL Vita3K ctrl path (compat/vitox_host_input.h).
// Windows.Gaming.Input.Gamepad (UWP app partition, the standard Xbox app input API) -> VitoxPadReading in SDL3 button
// order -> sdl-shim/vitox_sdl_gamepad.cpp -> vita3k/ctrl/src/ctrl.cpp -> modules/SceCtrl/SceCtrl.cpp -> guest.
// Mapping (Vita3K's default SDL binds): A->Cross B->Circle X->Square Y->Triangle LB->L RB->R View->Select Menu->Start
// D-pad->D-pad, sticks->analog (Y flipped to SDL's +down), triggers kept as axes (Vita3K maps them only for ext reads).
#include "common.h"

#include <vitox_host_input.h>

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Gaming.Input.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

using namespace winrt::Windows::Gaming::Input;

namespace {

std::mutex g_m;
std::array<Gamepad, VITOX_HOST_PADS> g_pads{ nullptr, nullptr, nullptr, nullptr };
std::array<VitoxPadReading, VITOX_HOST_PADS> g_cache{};
std::array<std::chrono::steady_clock::time_point, VITOX_HOST_PADS> g_cache_t{};
std::atomic<uint32_t> g_gen{ 0 };
winrt::event_token g_added, g_removed;
bool g_started = false;

// SDL3 SDL_GamepadButton order: SOUTH 0, EAST 1, WEST 2, NORTH 3, BACK 4, GUIDE 5, START 6, LEFT_STICK 7, RIGHT_STICK 8,
// LEFT_SHOULDER 9, RIGHT_SHOULDER 10, DPAD_UP 11, DPAD_DOWN 12, DPAD_LEFT 13, DPAD_RIGHT 14
uint32_t to_sdl_bits(GamepadButtons b) {
    const auto has = [b](GamepadButtons f) { return (b & f) == f; };
    uint32_t r = 0;
    r |= has(GamepadButtons::A) ? 1u << 0 : 0;
    r |= has(GamepadButtons::B) ? 1u << 1 : 0;
    r |= has(GamepadButtons::X) ? 1u << 2 : 0;
    r |= has(GamepadButtons::Y) ? 1u << 3 : 0;
    r |= has(GamepadButtons::View) ? 1u << 4 : 0;
    r |= has(GamepadButtons::Menu) ? 1u << 6 : 0;
    r |= has(GamepadButtons::LeftThumbstick) ? 1u << 7 : 0;
    r |= has(GamepadButtons::RightThumbstick) ? 1u << 8 : 0;
    r |= has(GamepadButtons::LeftShoulder) ? 1u << 9 : 0;
    r |= has(GamepadButtons::RightShoulder) ? 1u << 10 : 0;
    r |= has(GamepadButtons::DPadUp) ? 1u << 11 : 0;
    r |= has(GamepadButtons::DPadDown) ? 1u << 12 : 0;
    r |= has(GamepadButtons::DPadLeft) ? 1u << 13 : 0;
    r |= has(GamepadButtons::DPadRight) ? 1u << 14 : 0;
    return r;
}

// ---- 0.0.25.0 DIAGNOSTIC scripted buttons (vitox-launch.txt ui_script=<file>): "<ms> <A+B+...> [hold_ms]" lines, ms since
// the script was loaded; the bits are OR'ed into pad slot 0 (launcher UI, quick menu and game alike).
// 0.0.26.0: a token may also be an axis value, LX=0.7 / LY=-0.5 / RX / RY / LT / RT (added to the pad's own reading) ----
struct ScriptPress {
    int64_t at_ms, hold_ms;
    uint32_t bits;
    float axis[6]; // lx ly rx ry lt rt
    bool has_axis;
};
std::vector<ScriptPress> g_script; // guarded by g_m
std::chrono::steady_clock::time_point g_script_t0{};

uint32_t script_bits_locked(float *axis = nullptr) {
    if (g_script.empty())
        return 0;
    const int64_t t = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - g_script_t0).count();
    uint32_t bits = 0;
    for (const ScriptPress &p : g_script)
        if (t >= p.at_ms && t < p.at_ms + p.hold_ms) {
            bits |= p.bits;
            if (axis && p.has_axis)
                for (int i = 0; i < 6; ++i)
                    axis[i] += p.axis[i];
        }
    return bits;
}
void add_script_axes(VitoxPadReading &c) {
    float ax[6] = {};
    c.buttons |= script_bits_locked(ax);
    c.lx = std::clamp(c.lx + ax[0], -1.0f, 1.0f);
    c.ly = std::clamp(c.ly + ax[1], -1.0f, 1.0f);
    c.rx = std::clamp(c.rx + ax[2], -1.0f, 1.0f);
    c.ry = std::clamp(c.ry + ax[3], -1.0f, 1.0f);
    c.lt = std::clamp(c.lt + ax[4], 0.0f, 1.0f);
    c.rt = std::clamp(c.rt + ax[5], 0.0f, 1.0f);
}

// ---- 0.0.26.0 controller settings (launcher Settings > Input): button remap + radial stick dead zones, applied to what
// the GAME reads (read() below). The launcher UI and the quick menu keep the physical buttons, so a remap can never lock
// the user out of the menus. Saved as LocalState\vitox-input.txt ----
std::mutex g_cfg_m;
vitox::InputConfig g_cfg = vitox::input_config_default(); // guarded by g_cfg_m

void apply_deadzone(float &x, float &y, float dz) {
    if (dz <= 0)
        return;
    const float m = std::sqrt(x * x + y * y);
    if (m <= dz) {
        x = y = 0;
        return;
    }
    const float t = std::min(1.0f, (m - dz) / (1.0f - dz));
    x = x / m * t;
    y = y / m * t;
}

uint32_t button_bit(const std::string &n) {
    static const std::pair<const char *, uint32_t> k[] = { { "A", 1u << 0 }, { "B", 1u << 1 }, { "X", 1u << 2 }, { "Y", 1u << 3 }, { "VIEW", 1u << 4 },
        { "MENU", 1u << 6 }, { "START", 1u << 6 }, { "L3", 1u << 7 }, { "R3", 1u << 8 }, { "LB", 1u << 9 }, { "RB", 1u << 10 }, { "UP", 1u << 11 },
        { "DOWN", 1u << 12 }, { "LEFT", 1u << 13 }, { "RIGHT", 1u << 14 } };
    for (const auto &[name, bit] : k)
        if (n == name)
            return bit;
    return 0;
}

// caller holds g_m
void refresh(unsigned slot) {
    // ctrl.cpp asks ~25 times per guest poll (every button + axis): one WGI reading per pad per 2 ms is enough
    const auto now = std::chrono::steady_clock::now();
    if (now - g_cache_t[slot] > std::chrono::milliseconds(2)) {
        const GamepadReading r = g_pads[slot].GetCurrentReading();
        VitoxPadReading &c = g_cache[slot];
        c.buttons = to_sdl_bits(r.Buttons);
        c.lx = static_cast<float>(r.LeftThumbstickX);
        c.ly = static_cast<float>(-r.LeftThumbstickY);
        c.rx = static_cast<float>(r.RightThumbstickX);
        c.ry = static_cast<float>(-r.RightThumbstickY);
        c.lt = static_cast<float>(r.LeftTrigger);
        c.rt = static_cast<float>(r.RightTrigger);
        if (slot == 0)
            add_script_axes(c);
        g_cache_t[slot] = now;
    }
}

// ---- 0.0.25.0 Vitox quick menu (in-game overlay, compat/vitox_host_input.h VitoxOverlayTick) ----
constexpr uint32_t kBitB = 1u << 1, kBitMenu = 1u << 6, kBitR3 = 1u << 8;
constexpr int kOverlayItems = 2; // 0.0.28.0: 0 = Statistics Overlay (toggle), 1 = Return to Launcher
struct Overlay {
    bool enabled = false; // a game session is running
    bool open = false, quit = false, confirm = false;
    float phase = 0, confirm_phase = 0; // linear 0..1
    int selected = 0;
    uint32_t prev = 0, suppress = 0;
    std::chrono::steady_clock::time_point last{}, opened{}, toggled{}, pressed{};
    bool ever_pressed = false;
};
Overlay g_overlay; // guarded by g_m

float ease_out(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return 1 - (1 - t) * (1 - t) * (1 - t);
}

// caller holds g_m
void overlay_advance() {
    Overlay &o = g_overlay;
    const auto now = std::chrono::steady_clock::now();
    const float dt = o.last == std::chrono::steady_clock::time_point{} ? 0.0f : std::min(0.1f, std::chrono::duration<float>(now - o.last).count());
    o.last = now;
    uint32_t cur = 0;
    if (g_pads[0]) {
        refresh(0);
        cur = g_cache[0].buttons;
    } else {
        cur = script_bits_locked();
    }
    const uint32_t edge = cur & ~o.prev;
    o.prev = cur;
    o.suppress &= cur; // a masked button is released: it reaches the game again
    const auto close = [&](const char *why) {
        o.open = false;
        o.confirm = false;
        o.toggled = now;
        o.suppress |= cur; // what is still held when the menu closes must not reach the game
        vitox::log_line(vitox::Log::Core, "vitox overlay: CLOSED (%s)", why);
    };
    if (!o.enabled) {
        o.open = o.confirm = false;
    } else if (!o.quit) {
        if ((edge & kBitR3) && now - o.toggled > std::chrono::milliseconds(250)) {
            if (o.open) {
                close("R3");
            } else {
                o.open = true;
                o.confirm = false;
                o.selected = 0;
                o.opened = o.toggled = now;
                vitox::log_line(vitox::Log::Core, "vitox overlay: OPENED (R3)");
            }
        } else if (o.open) {
            if (edge & kBitB) {
                if (o.confirm) {
                    o.confirm = false;
                    vitox::log_line(vitox::Log::Core, "vitox overlay: confirm cancelled (B)");
                } else {
                    close("B");
                }
            } else if (edge & kBitMenu) {
                close("Menu");
            } else if (edge & (1u << 11)) {
                o.selected = (o.selected + kOverlayItems - 1) % kOverlayItems;
                o.confirm = false; // moving away cancels a pending Return to Launcher
            } else if (edge & (1u << 12)) {
                o.selected = (o.selected + 1) % kOverlayItems;
                o.confirm = false;
            } else if (edge & (1u << 0)) {
                o.pressed = now;
                o.ever_pressed = true;
                if (o.selected == 0) {
                    // 0.0.28.0: the statistics overlay - the same setting as Settings > Performance Statistics Overlay;
                    // the menu stays open, the game stays paused behind it, the display mode is kept
                    const bool on = !vitox::stats_enabled();
                    vitox::stats_set_enabled(on);
                    vitox::log_line(vitox::Log::Core, "vitox overlay: Statistics Overlay -> %s (A)", on ? "ON" : "OFF");
                } else if (!o.confirm) {
                    o.confirm = true;
                    vitox::log_line(vitox::Log::Core, "vitox overlay: Return to Launcher selected, waiting for confirmation (A)");
                } else {
                    o.quit = true;
                    o.open = false;
                    o.suppress |= cur;
                    vitox::log_line(vitox::Log::Core, "vitox overlay: Return to Launcher CONFIRMED (A)");
                }
            }
        }
    }
    const float target = o.open || o.quit ? 1.0f : 0.0f;
    const float rate = dt / (target > o.phase ? 0.28f : 0.20f);
    o.phase = target > o.phase ? std::min(target, o.phase + rate) : std::max(target, o.phase - rate);
    const float ct = o.confirm ? 1.0f : 0.0f;
    o.confirm_phase = ct > o.confirm_phase ? std::min(ct, o.confirm_phase + dt / 0.18f) : std::max(ct, o.confirm_phase - dt / 0.18f);
}

bool overlay_blocks_locked() { return g_overlay.enabled && (g_overlay.open || g_overlay.quit || g_overlay.phase > 0); }

void overlay_tick(VitoxOverlayTick *out) {
    std::lock_guard<std::mutex> lock(g_m);
    overlay_advance();
    const Overlay &o = g_overlay;
    const auto now = std::chrono::steady_clock::now();
    *out = VitoxOverlayTick{};
    out->visible = o.enabled && o.phase > 0;
    out->open = o.enabled && o.open;
    out->quit = o.enabled && o.quit;
    out->anim = ease_out(o.phase);
    out->time_s = std::chrono::duration<float>(now - o.opened).count();
    out->selected = o.selected;
    out->confirm = o.confirm;
    out->confirm_anim = ease_out(o.confirm_phase);
    out->press_s = o.ever_pressed ? std::chrono::duration<float>(now - o.pressed).count() : -1.0f;
}

// ---- 0.0.24.54 (M9A.6) virtual touch cursor (compat/vitox_host_input.h): pad slot 0, L3 toggles ----
constexpr uint32_t kBitA = 1u << 0, kBitL3 = 1u << 7;
struct Cursor {
    bool active = false, a_down = false, l3_down = false;
    float x = 0.5f, y = 0.5f;
    std::chrono::steady_clock::time_point last{}, shown{}, pressed{};
    bool ever_pressed = false;
};
Cursor g_cursor; // guarded by g_m

// stick -> velocity in screen widths per second: radial dead zone, then a quadratic curve (fine aim near the centre,
// about 0.9 s across the screen at full tilt)
void stick_velocity(float sx, float sy, float &vx, float &vy) {
    constexpr float dead = 0.18f, full_speed = 1.1f;
    const float m = std::sqrt(sx * sx + sy * sy);
    vx = vy = 0;
    if (m <= dead)
        return;
    const float t = std::min(1.0f, (m - dead) / (1.0f - dead));
    const float speed = full_speed * t * t;
    vx = sx / m * speed;
    vy = sy / m * speed;
}

void touch_cursor(VitoxTouchCursor *out) {
    std::lock_guard<std::mutex> lock(g_m);
    const auto now = std::chrono::steady_clock::now();
    Cursor &c = g_cursor;
    const float dt = c.last == std::chrono::steady_clock::time_point{} ? 0.0f
                                                                        : std::min(0.1f, std::chrono::duration<float>(now - c.last).count());
    c.last = now;
    if (g_pads[0] && !overlay_blocks_locked()) {
        refresh(0);
        const VitoxPadReading &r = g_cache[0];
        const bool l3 = (r.buttons & kBitL3) != 0;
        if (l3 && !c.l3_down) {
            c.active = !c.active;
            c.a_down = false;
            if (c.active)
                c.shown = now;
            vitox::log_line(vitox::Log::Core, "m9a6 touch cursor: %s at (%.3f, %.3f)", c.active ? "ON" : "OFF", c.x, c.y);
        }
        c.l3_down = l3;
        if (c.active) {
            float vx, vy;
            stick_velocity(r.lx, r.ly, vx, vy);
            // the Vita screen is 960x544: the same stick speed in pixels on both axes
            c.x = std::clamp(c.x + vx * dt, 0.0f, 1.0f);
            c.y = std::clamp(c.y + vy * dt * (960.0f / 544.0f), 0.0f, 1.0f);
            const bool a = (r.buttons & kBitA) != 0;
            if (a && !c.a_down) {
                c.pressed = now;
                c.ever_pressed = true;
            }
            c.a_down = a;
        }
    } else if (!g_pads[0]) {
        c.active = c.a_down = c.l3_down = false;
    } else {
        c.a_down = false; // the quick menu is up: the cursor keeps its place, no touch
    }
    out->active = c.active;
    out->pressed = c.active && c.a_down;
    out->x = c.x;
    out->y = c.y;
    out->shown_s = c.active ? std::chrono::duration<float>(now - c.shown).count() : 0.0f;
    out->press_s = c.ever_pressed ? std::chrono::duration<float>(now - c.pressed).count() : -1.0f;
}

bool read(unsigned slot, VitoxPadReading *out) {
    std::lock_guard<std::mutex> lock(g_m);
    if (slot >= VITOX_HOST_PADS || !g_pads[slot])
        return false;
    refresh(slot);
    *out = g_cache[slot];
    // 0.0.25.0: the quick menu owns every pad while it is visible; R3 is its button (the Vita has no R3)
    if (overlay_blocks_locked()) {
        *out = VitoxPadReading{};
        return true;
    }
    if (slot == 0) {
        out->buttons &= ~(kBitR3 | g_overlay.suppress);
        // L3 is the cursor toggle; while the cursor is on, the left stick and A drive it instead of the game
        out->buttons &= ~kBitL3;
        if (g_cursor.active) {
            out->buttons &= ~kBitA;
            out->lx = out->ly = 0;
        }
    }
    // 0.0.26.0: the user's button map + stick dead zones (Settings > Input)
    vitox::InputConfig cfg;
    {
        std::lock_guard<std::mutex> lock(g_cfg_m);
        cfg = g_cfg;
    }
    out->buttons = vitox::input_remap(out->buttons, cfg);
    apply_deadzone(out->lx, out->ly, cfg.dead_l);
    apply_deadzone(out->rx, out->ry, cfg.dead_r);
    return true;
}
const char *name(unsigned slot) {
    std::lock_guard<std::mutex> lock(g_m);
    return slot < VITOX_HOST_PADS && g_pads[slot] ? "Xbox Controller (Windows.Gaming.Input)" : nullptr;
}
uint32_t generation() { return g_gen.load(); }

const VitoxHostInput kHost{ &read, &name, &generation, &touch_cursor, &overlay_tick };

void add(Gamepad const &g) {
    std::lock_guard<std::mutex> lock(g_m);
    for (const auto &p : g_pads)
        if (p == g)
            return;
    for (unsigned s = 0; s < VITOX_HOST_PADS; ++s)
        if (!g_pads[s]) {
            g_pads[s] = g;
            g_cache_t[s] = {};
            ++g_gen;
            vitox::log_line(vitox::Log::Core, "m8b.2 host input: gamepad ADDED -> slot %u (Windows.Gaming.Input)", s);
            return;
        }
}
void remove(Gamepad const &g) {
    std::lock_guard<std::mutex> lock(g_m);
    for (unsigned s = 0; s < VITOX_HOST_PADS; ++s)
        if (g_pads[s] == g) {
            g_pads[s] = nullptr;
            ++g_gen;
            vitox::log_line(vitox::Log::Core, "m8b.2 host input: gamepad REMOVED <- slot %u", s);
        }
}

} // namespace

namespace vitox {

void touch_cursor_state(VitoxTouchCursor *out) { touch_cursor(out); }

void overlay_state(VitoxOverlayTick *out) { overlay_tick(out); }

void overlay_session(bool active) {
    std::lock_guard<std::mutex> lock(g_m);
    g_overlay = Overlay{};
    g_overlay.enabled = active;
    if (active && g_pads[0]) {
        refresh(0);
        g_overlay.prev = g_cache[0].buttons; // a button held at launch is not a press
        g_overlay.suppress = g_cache[0].buttons & ~kBitR3;
    }
}

void ui_script_load(const std::wstring &path) {
    std::ifstream f(path);
    std::vector<ScriptPress> s;
    for (std::string line; f && std::getline(f, line);) {
        if (line.empty() || line[0] == '#')
            continue;
        std::istringstream is(line);
        ScriptPress p{ 0, 150, 0, {}, false };
        std::string names;
        if (!(is >> p.at_ms >> names))
            continue;
        is >> p.hold_ms;
        static const char *const kAxes[6] = { "LX", "LY", "RX", "RY", "LT", "RT" };
        for (size_t b = 0, e; b <= names.size(); b = e + 1) {
            e = names.find('+', b);
            if (e == std::string::npos)
                e = names.size();
            const std::string tok = names.substr(b, e - b);
            const size_t eq = tok.find('=');
            if (eq != std::string::npos) {
                for (int i = 0; i < 6; ++i)
                    if (tok.compare(0, eq, kAxes[i]) == 0) {
                        p.axis[i] = static_cast<float>(std::atof(tok.c_str() + eq + 1));
                        p.has_axis = true;
                    }
            } else {
                p.bits |= button_bit(tok);
            }
        }
        if (p.bits || p.has_axis)
            s.push_back(p);
    }
    std::lock_guard<std::mutex> lock(g_m);
    g_script = std::move(s);
    g_script_t0 = std::chrono::steady_clock::now();
    log_line(Log::Core, "vitox ui_script (DIAGNOSTIC): %u scripted press(es) from %s", static_cast<unsigned>(g_script.size()), narrow(path).c_str());
}

bool host_pad_poll(VitoxPadReading *out) {
    *out = VitoxPadReading{};
    bool any = false;
    try {
        const auto pads = Gamepad::Gamepads();
        for (uint32_t i = 0; i < pads.Size(); ++i) {
            const GamepadReading r = pads.GetAt(i).GetCurrentReading();
            out->buttons |= to_sdl_bits(r.Buttons);
            if (!any) {
                out->lx = static_cast<float>(r.LeftThumbstickX);
                out->ly = static_cast<float>(-r.LeftThumbstickY);
                out->rx = static_cast<float>(r.RightThumbstickX);
                out->ry = static_cast<float>(-r.RightThumbstickY);
                out->lt = static_cast<float>(r.LeftTrigger);
                out->rt = static_cast<float>(r.RightTrigger);
            }
            any = true;
        }
    } catch (...) {
    }
    std::lock_guard<std::mutex> lock(g_m);
    add_script_axes(*out);
    return any || !g_script.empty();
}

// ---- 0.0.26.0 controller settings ----
namespace {
struct ButtonName {
    const char *key;  // file key
    const wchar_t *xbox, *vita;
    uint32_t bit;
};
// the remappable buttons, in the Settings > Input row order (SDL bit order = Vita3K's default binds: A Cross, B Circle,
// X Square, Y Triangle, View Select, Menu Start, LB L, RB R, D-pad)
const ButtonName kButtons[] = {
    { "A", L"A", L"Cross", 0 }, { "B", L"B", L"Circle", 1 }, { "X", L"X", L"Square", 2 }, { "Y", L"Y", L"Triangle", 3 },
    { "LB", L"LB", L"L", 9 }, { "RB", L"RB", L"R", 10 }, { "VIEW", L"View", L"Select", 4 }, { "MENU", L"Menu", L"Start", 6 },
    { "UP", L"D-pad Up", L"Up", 11 }, { "DOWN", L"D-pad Down", L"Down", 12 }, { "LEFT", L"D-pad Left", L"Left", 13 },
    { "RIGHT", L"D-pad Right", L"Right", 14 },
};
} // namespace

InputConfig input_config_default() {
    InputConfig c{};
    for (uint8_t &m : c.map)
        m = kInputNone;
    for (const ButtonName &b : kButtons)
        c.map[b.bit] = static_cast<uint8_t>(b.bit);
    c.dead_l = c.dead_r = 0.0f; // the pre-0.0.26 behaviour: raw Windows.Gaming.Input values
    return c;
}

int input_button_count() { return static_cast<int>(sizeof(kButtons) / sizeof(kButtons[0])); }
uint32_t input_button_bit(int i) { return kButtons[i].bit; }
const wchar_t *input_button_xbox(int i) { return kButtons[i].xbox; }
const wchar_t *input_button_vita(int i) { return kButtons[i].vita; }
int input_button_index(uint32_t bit) {
    for (int i = 0; i < input_button_count(); ++i)
        if (kButtons[i].bit == bit)
            return i;
    return -1;
}

uint32_t input_remap(uint32_t bits, const InputConfig &c) {
    uint32_t keep = bits, out = 0;
    for (const ButtonName &b : kButtons) {
        keep &= ~(1u << b.bit);
        if ((bits & (1u << b.bit)) && c.map[b.bit] != kInputNone)
            out |= 1u << c.map[b.bit];
    }
    return keep | out; // L3 / R3 / Guide pass unchanged
}

InputConfig input_config() {
    std::lock_guard<std::mutex> lock(g_cfg_m);
    return g_cfg;
}
void input_config_set(const InputConfig &c) {
    std::lock_guard<std::mutex> lock(g_cfg_m);
    g_cfg = c;
}

bool input_config_load(const std::wstring &path) {
    std::ifstream f(path);
    if (!f)
        return false;
    InputConfig c = input_config_default();
    unsigned n = 0;
    for (std::string line; std::getline(f, line);) {
        if (line.empty() || line[0] == '#')
            continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        while (!v.empty() && (v.back() == '\r' || v.back() == ' '))
            v.pop_back();
        if (k == "deadzone.left")
            c.dead_l = std::clamp(static_cast<float>(std::atof(v.c_str())), 0.0f, 0.9f);
        else if (k == "deadzone.right")
            c.dead_r = std::clamp(static_cast<float>(std::atof(v.c_str())), 0.0f, 0.9f);
        else if (k.rfind("map.", 0) == 0)
            for (const ButtonName &from : kButtons)
                if (k.compare(4, std::string::npos, from.key) == 0) {
                    if (v == "NONE")
                        c.map[from.bit] = kInputNone;
                    for (const ButtonName &to : kButtons)
                        if (v == to.key)
                            c.map[from.bit] = static_cast<uint8_t>(to.bit);
                    ++n;
                }
    }
    input_config_set(c);
    log_line(Log::Core, "vitox input config: loaded %s (%u mapping line(s), dead zones L %.2f R %.2f)", narrow(path).c_str(), n, c.dead_l, c.dead_r);
    return true;
}

bool input_config_save(const std::wstring &path) {
    const InputConfig c = input_config();
    std::ofstream f(path, std::ios::trunc);
    if (!f)
        return false;
    f << "# Vitox controller settings (launcher Settings > Input). map.<Xbox button>=<the button the game sees, by its Xbox name> | NONE\n";
    for (const ButtonName &b : kButtons) {
        f << "map." << b.key << "=";
        const int t = input_button_index(c.map[b.bit]);
        f << (c.map[b.bit] == kInputNone || t < 0 ? "NONE" : kButtons[t].key) << "\n";
    }
    char buf[64];
    snprintf(buf, sizeof(buf), "deadzone.left=%.2f\ndeadzone.right=%.2f\n", c.dead_l, c.dead_r);
    f << buf;
    return static_cast<bool>(f);
}

void host_input_start() {
    if (g_started)
        return;
    g_started = true;
    g_added = Gamepad::GamepadAdded([](auto &&, Gamepad const &g) { add(g); });
    g_removed = Gamepad::GamepadRemoved([](auto &&, Gamepad const &g) { remove(g); });
    for (const Gamepad &g : Gamepad::Gamepads())
        add(g);
    vitox_set_host_input(&kHost);
    log_line(Log::Core, "m8b.2 host input: Windows.Gaming.Input started, %u gamepad(s) at start", static_cast<unsigned>(Gamepad::Gamepads().Size()));
}

void host_input_stop() {
    if (!g_started)
        return;
    vitox_set_host_input(nullptr);
    Gamepad::GamepadAdded(g_added);
    Gamepad::GamepadRemoved(g_removed);
    std::lock_guard<std::mutex> lock(g_m);
    g_pads.fill(nullptr);
    g_started = false;
}

} // namespace vitox
