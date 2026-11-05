// Vitox M3 Probe - shared declarations (M1 shell + Dynarmic/VEH/unwind/fastmem probes + Vita3K core link).
// Standalone UWP probe. Links no Vita3K / emulator code.
#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

struct VitoxTouchCursor; // compat/vitox_host_input.h
struct VitoxOverlayTick;
struct VitoxPadReading;
struct VitoxLaunchConfig; // core-probe/vitox_homebrew_launcher.h

#define VITOX_M1_VERSION "0.0.13.0"

namespace vitox {

// ---- logging (log.cpp) ----
enum class Log : int { Lifecycle, Mem, Probe, Fs, Dynarmic, Veh, Unwind, Fastmem, Core, Count };

bool log_open(const std::wstring &dir);
void log_line(Log which, const char *fmt, ...);
void log_checkpoint(const char *text); // 0.0.24.3: durable milestone line + flush every log to disk
void crash_install(const std::wstring &dir);
std::string narrow(const std::wstring &w);
std::wstring widen(const std::string &s);
std::string win32_err(DWORD e);

// ---- on-screen status (app.cpp) ----
struct Status {
    std::mutex m;
    std::string device = "?";
    std::string api = "pending";
    std::string jit = "pending";       // overall probe 4 result
    std::string jit_rx = "pending";    // RW -> RX -> execute
    std::string jit_patch = "pending"; // RX -> RW -> patch -> RX -> execute
    std::string jit_rwx = "pending";   // direct RWX allocation (diagnostic)
    std::string fs_local = "pending";
    std::string fs_dev = "pending";
    std::string mem = "pending";
    std::string dyn = "pending";    // M2 tests 1-5
    std::string veh = "pending";    // M2 test 6
    std::string unwind = "pending"; // M2 test 7
    std::string fastmem = "pending"; // M2.5
    std::string core = "pending";    // M3 Vita3K core link
    std::string io = "pending";      // M4.2 real Vita3K IO + config
    std::string hb = "pending";      // M5B first headless homebrew (VITOX0001)
    std::string m6a = "pending";     // M6A Vulkan(dzn)->D3D12 present probe (visual)
    std::string m6b = "pending";     // M6B dzn shader pipeline probe (visual)
    std::string m6b1 = "pending";    // M6B.1 dzn draw-failure diagnostic (non-visual)
    std::string m6c = "pending";     // M6C real Vita3K Vulkan renderer via the external presenter (visual)
};
Status &status();
void set_status(std::string Status::*field, const std::string &value);

// ---- probes ----
void probe_api();                                // probe_api.cpp
void probe_jit();                                // probe_jit.cpp
void probe_fs(const std::wstring &local_state);  // probe_fs.cpp
void mem_snapshot(const char *tag);              // probe_mem.cpp
void mem_thread_start();
void mem_thread_stop();
void probe_dynarmic();                           // probe_dynarmic.cpp (supplied)
void probe_veh();                                // probe_veh.cpp (supplied)
void probe_unwind();                             // probe_unwind.cpp (supplied)
void probe_fastmem();                            // probe_fastmem.cpp (M2.5)
void probe_core();                               // probe_core.cpp (M3, vitox_core.lib)
void probe_io(const std::wstring &local_state);  // probe_io.cpp (M4.2, vitox_hostio.lib)
void probe_homebrew(const std::wstring &local_state, const std::wstring &install_dir); // probe_homebrew.cpp (M5B, vitox_hle.lib)
// probe_dzn.cpp (M6A): returns a status line; `pump` processes window events while it renders
std::string probe_dzn(void *core_window /* IUnknown* of the CoreWindow */, unsigned width, unsigned height, void (*pump)());
std::string probe_m6b(void *core_window /* IUnknown* of the CoreWindow */, unsigned width, unsigned height, void (*pump)()); // probe_m6b.cpp
std::string probe_m6b1(const std::wstring &local_state); // probe_m6b1.cpp (M6B.1, non-visual)
// probe_m6c.cpp (M6C): host D3D12 presenter + the real Vita3K renderer probe (vitox_m6c_probe.lib)
std::string probe_m6c(void *core_window /* IUnknown* of the CoreWindow */, unsigned width, unsigned height, void (*pump)(), const std::wstring &local_state);
// probe_m6c.cpp (M7A): VITOX0002 GXM homebrew through the real Vita3K core (vitox_m7a_probe.lib); headless = non-visual precheck
std::string probe_m7a(void *core_window, unsigned width, unsigned height, void (*pump)(), const std::wstring &local_state, bool headless,
    unsigned homebrew /* 2 = VITOX0002, 3 = VITOX0003 */);
// probe_m6c.cpp (M7B Phase A): open-shacccg shader generation + Vita3K validation, NON-VISUAL (vitox_m7b_probe.lib)
std::string probe_m7b(void *core_window, const std::wstring &local_state);
// probe_m6c.cpp (M8A): the user-selectable homebrew launcher (vitox_m8a_launcher.lib). App + presenter mode come from
// D:\DevelopmentFiles\Vitox\homebrew\vitox-launch.txt at run time.
bool m8a_launch_headless();
// host_input.cpp (M8B.2): Windows.Gaming.Input -> compat/vitox_host_input.h (the SDL3 gamepad shim's source)
void host_input_start();
// probe_m6c.cpp (M8C.1): staged firmware installer (vitox_m8c_firmware.lib), non-visual
std::string probe_m8c();
std::string probe_m8c2(const std::wstring &local_state); // M8C.2: staged real LLE PGF precheck
void host_input_stop();
// host_input.cpp (0.0.24.54): the virtual touch cursor, advanced to now (the presenter draws it)
void touch_cursor_state(::VitoxTouchCursor *out);
// host_text.cpp (M8F.2B): CoreTextEditContext + system keyboard -> compat/vitox_host_text.h (Vita3K IME dialog text)
void host_text_start();
void host_text_stop();
std::string probe_m8a(void *core_window, unsigned width, unsigned height, void (*pump)(), const std::wstring &local_state);
// 0.0.25.0: one launch with a given config (the library UI); `cfg` null = read vitox-launch.txt (probe_m8a)
std::string probe_m8a_with(void *core_window, unsigned width, unsigned height, void (*pump)(), const std::wstring &local_state,
    const VitoxLaunchConfig *cfg, bool *user_quit);
extern const wchar_t *const kDevRoot; // D:\DevelopmentFiles\Vitox

// ---- 0.0.25.0 Vitox launcher UI + quick menu ----
// host_input.cpp
void overlay_state(VitoxOverlayTick *out);   // advances the quick menu to now (same as the VitoxHostInput hook)
void overlay_session(bool active);           // a game session starts (true) / ends (false): resets the menu
void ui_script_load(const std::wstring &path); // DIAGNOSTIC scripted buttons (vitox-launch.txt ui_script=)
bool host_pad_poll(VitoxPadReading *out);    // every connected gamepad OR'ed (launcher UI; independent of host_input_start)
// 0.0.26.0 controller settings (host_input.cpp; launcher Settings > Input): applied to the game's pad reads only
constexpr uint8_t kInputNone = 0xFF;
struct InputConfig {
    uint8_t map[16];      // [physical SDL bit] = the SDL bit the game sees (kInputNone = disabled)
    float dead_l, dead_r; // radial stick dead zones 0..0.9 (rescaled outside)
};
InputConfig input_config_default();
InputConfig input_config();
void input_config_set(const InputConfig &c);
bool input_config_load(const std::wstring &path);
bool input_config_save(const std::wstring &path);
uint32_t input_remap(uint32_t bits, const InputConfig &c);
int input_button_count();               // the remappable buttons (Settings > Input row order)
uint32_t input_button_bit(int i);       // its SDL bit
const wchar_t *input_button_xbox(int i); // "A", "LB", "D-pad Up", ...
const wchar_t *input_button_vita(int i); // "Cross", "L", "Up", ...
int input_button_index(uint32_t bit);   // -1 = not remappable

// 0.0.27.0 graphics settings (graphics_config.cpp; launcher Settings > Graphics)
constexpr float kResolutionMin = 1.0f, kResolutionStep = 0.5f;
constexpr float kResolutionMaxDefault = 4.0f; // the selectable maximum (vitox-graphics.txt max_resolution_multiplier= changes it)
constexpr float kResolutionMaxHard = 8.0f;    // never above (7680 x 4352: within D3D12's 16384 texture limit)
constexpr int kResolutionGuardSeconds = 45;   // a session above the last working multiplier must survive this long
float resolution_snap(float m, float max);    // nearest multiple of 0.5 in [1, max]
int resolution_width(float m);                // round(960 * m)
int resolution_height(float m);               // round(544 * m)
void graphics_load(const std::wstring &local_state);
float graphics_resolution_multiplier();
float graphics_max_multiplier();
void graphics_set_resolution_multiplier(float m); // snaps, saves, applies to the next session
std::wstring graphics_notice();                   // the crash guard's message (empty = none)
std::wstring graphics_take_notice();
void graphics_session_override(float resolution, int async, int prewarm); // 0.0.30.0: per-game values for the next session (0 / -1 = global)
void graphics_session_begin(); // a game session starts (crash guard armed above the last working multiplier)
void graphics_session_end();
// 0.0.28.3 Settings > Shader Compilation (saved in vitox-graphics.txt; both default ON; applied at the next game start)
bool graphics_async_pipelines();
bool graphics_pipeline_prewarm();
void graphics_set_async_pipelines(bool on);
void graphics_set_pipeline_prewarm(bool on);

// 0.0.28.0 performance statistics overlay (perf_stats.cpp). ONE setting: the launcher's Settings page and the R3 quick
// menu both use these functions; saved in LocalState\vitox-overlay.txt
enum class StatsMode : int { FpsOnly = 0, Full = 1 };
void stats_load(const std::wstring &local_state);
bool stats_enabled();
void stats_set_enabled(bool on); // saved at once
StatsMode stats_mode();
void stats_set_mode(StatsMode m); // saved at once; kept while the overlay is off
// 0.0.30.0: a game's own overlay setting for one session (-1 global, 0 off, 1 FPS only, 2 full): the global setting is
// restored by stats_session_override(-1); while overridden, the R3 toggle changes this session only (nothing saved)
void stats_session_override(int v);
void stats_session_begin(uint32_t screen_h); // a visual game session starts: the sampler thread runs
void stats_session_end();
void stats_frame_presented(); // the host presenter put one game frame on screen
// the latest panel pixels (premultiplied BGRA, w*h*4) if newer than `have_version`
bool stats_take_panel(uint32_t have_version, std::vector<uint8_t> &bgra, uint32_t &w, uint32_t &h, uint32_t &version);

// overlay_panel.cpp
struct OverlayInfo {
    std::wstring title, title_id, icon_path;
};
struct OverlayItemRect {
    float x0, y0, x1, y1;
};
void overlay_set_info(const OverlayInfo &info);
OverlayInfo overlay_info();
// renders the panel (4w x h, premultiplied BGRA rows of 4w*4 bytes); k = pixel scale (1 = 1080p)
// 0.0.28.0: 4 variants (w each; OFF normal | OFF confirm | ON normal | ON confirm), items[0] = Statistics Overlay,
// items[1] = Return to Launcher
bool overlay_render_panel(uint32_t w, uint32_t h, float k, std::vector<uint8_t> &bgra, OverlayItemRect items[2]);

// Resolve an export at runtime without a static import. Logs nothing.
FARPROC resolve(const wchar_t *module, const char *fn);

} // namespace vitox
