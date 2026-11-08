// Vitox M8A - USER-SELECTABLE HOMEBREW LOADER (see vitox_homebrew_launcher.h). Generic: no TITLE_ID, import set or
// sentinel of any particular homebrew is known here. All emulation is real Vita3K code (the same path as M7A/M7B):
//   discovery -> selection -> fs_utils::copy_directory_contents -> io/kernel/param.sfo -> load_module("app0:eboot.bin")
//   -> import inventory (selected REAL dispatcher only) -> guest main thread + vblank + render threads -> Vita3K's
//   shutdown order. A guest that does not exit by timeout_s is stopped by the host (display queue aborted first).
#include "vitox_homebrew_launcher.h"

#include "vitox_import_dispatch.h"

#include <config/state.h>
#include <cpu/disasm/functions.h> // 0.0.24.63: standalone disassembler (diagnostic)
#include <cpu/disasm/state.h>
#include <cpu/functions.h> // M8D: read_pc/read_lr (shutdown diagnostics)
#include <ctrl/functions.h> // M8B.2: refresh_controllers (the frontend's job in Vita3K)
#include <ctrl/state.h>
#include <dialog/state.h>             // M8F.2A
#include <ime/functions.h>            // M8F.2A: ime_commit_text, submit_current_ime, dismiss_current_ime
#include <ime/state.h>
#include <overlay/display_manager.h>  // M8F.2A: Vita3K's overlay (common_dialog_overlay draws the IME dialog)
#include <overlay/font.h>
#include <overlay/input.h>
#include <overlay/trophy_notification.h> // 0.0.32.0: trophy-unlocked banner
#include <util/string_utils.h>
#include <vitox_host_input.h>
#include <vitox_host_text.h> // M8F.2B
#include <vitox_firmware_root.h> // M8C.3
#include <vitox_fself.h>         // M8C.3
#include <vitox_self_check.h>    // M9A.2C
#include <vitox_license_check.h> // M9A.3A
#include <display/functions.h>
#include <display/state.h>
#include <display/state.h> // 0.0.24.54: viewport for the touch cursor
#include <emuenv/state.h>
#include <touch/state.h>   // 0.0.24.54: mouse-as-touch fields
#include <audio/state.h> // M9A.4R: AudioState::init
#include <ngs/state.h>   // M9A.7: ngs::init / ngs::deinit (upstream app_init order)
#include <vitox_xaudio2_audio.h> // M9A.5: soak counters
#include <vitox_vk_entry.h>      // 0.0.28.3: vitox_prewarm_set_enabled
#include <http/state.h>  // M9A.5: upstream shutdown order (http/net/netctl/np deinit)
#include <net/state.h>
#include <np/functions.h>
#include <np/state.h>
#include <gxm/state.h>
#include <io/functions.h>
#include <io/state.h>
#include <io/vfs.h>
#include <kernel/state.h>
#include <kernel/sync_primitives.h> // 0.0.24.58: waited-object dump
#include <kernel/thread/thread_state.h>
#include <mem/functions.h>
#include <mem/state.h>
#include <module/load_module.h> // M9A.2A: is_lle_module
#include <modules/module_parent.h>
#include <nids/functions.h>
#include <packages/license.h> // M9A.3A: License (the rif map is only counted, never filled here)
#include <packages/sfo.h>
#include <renderer/frame_host.h>
#include <renderer/functions.h>
#include <renderer/shaders.h> // 0.0.24.173: get_shaders_cache_hashs (pipeline pre-warm)
#include <renderer/state.h>
#include <util/bytes.h> // 0.0.29.1: byte_swap (NoNpDrm license sku_flag, as upstream get_license)
#include <util/fs.h>
#include <util/log.h>

#include <spdlog/sinks/callback_sink.h>
#include <spdlog/sinks/null_sink.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <psapi.h> // M9A.5: K32GetProcessMemoryInfo (soak line)
#include <dxgi1_4.h> // 0.0.25.2: GPU memory per cleanup step (launcher sessions)

#include <algorithm>
#include <atomic>
#include <cmath>
#include <unordered_map>
#include <chrono>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <thread>
#include <vector>

std::string vitox_sha256_hex(const void *data, size_t size); // compat/vitox_sha256.cpp

namespace {

// 0.0.24.57 (M9A.6 DIAGNOSTIC, read-only): guest import calls per NID (lock-free open addressing), so the 30 s thread
// dump can show which imports a stalled game keeps calling (Vita3K warns only once per stub)
struct ImportCount {
    std::atomic<uint32_t> nid{ 0 };
    std::atomic<uint64_t> calls{ 0 };
};
constexpr size_t k_import_slots = 4096;
ImportCount g_import_counts[k_import_slots];
uint64_t g_import_seen[k_import_slots]; // UI thread only: counts at the previous dump
// 0.0.24.59: true while this NID has logged fewer than 40 result lines
std::mutex g_import_note_m;
std::unordered_map<uint32_t, unsigned> g_import_notes;
bool note_import_result(uint32_t nid) {
    std::lock_guard<std::mutex> lock(g_import_note_m);
    return g_import_notes[nid]++ < 40;
}
// 0.0.24.74 (M9A.6 DIAGNOSTIC): scripted left stick (auto_stick), layered over the host controller source
struct StickSegment {
    float from_s, to_s, lx, ly;
    uint32_t buttons; // 0.0.24.98: optional 5th field, VitoxPadReading button bits held during the segment
};
std::vector<StickSegment> g_stick_segments;
const VitoxHostInput *g_stick_real = nullptr;
std::atomic<int64_t> g_stick_t0_us{ 0 }; // guest start (steady clock, microseconds); 0 = not started
bool stick_read(unsigned slot, VitoxPadReading *out) {
    bool ok = g_stick_real && g_stick_real->read(slot, out);
    if (slot != 0)
        return ok;
    if (!ok)
        *out = VitoxPadReading{};
    const int64_t t0 = g_stick_t0_us.load();
    if (t0) {
        const double t = (std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count() - t0) / 1e6;
        for (const StickSegment &g : g_stick_segments)
            if (t >= g.from_s && t < g.to_s) {
                out->lx = g.lx;
                out->ly = g.ly;
                out->buttons |= g.buttons;
            }
    }
    return true; // slot 0 always present while scripted input is configured
}
const char *stick_name(unsigned slot) {
    const char *n = g_stick_real ? g_stick_real->name(slot) : nullptr;
    return n ? n : (slot == 0 ? "Vitox scripted pad (auto_stick)" : nullptr);
}
uint32_t stick_generation() { return g_stick_real ? g_stick_real->generation() : 1; }
void stick_touch_cursor(VitoxTouchCursor *out) {
    if (g_stick_real && g_stick_real->touch_cursor)
        g_stick_real->touch_cursor(out);
    else
        *out = VitoxTouchCursor{};
}
void stick_overlay(VitoxOverlayTick *out) {
    if (g_stick_real && g_stick_real->overlay)
        g_stick_real->overlay(out);
    else
        *out = VitoxOverlayTick{};
}
const VitoxHostInput k_stick_host{ &stick_read, &stick_name, &stick_generation, &stick_touch_cursor, &stick_overlay };

void count_import(uint32_t nid) {
    if (!nid)
        return;
    for (size_t i = (nid * 2654435761u) % k_import_slots, n = 0; n < k_import_slots; i = (i + 1) % k_import_slots, ++n) {
        uint32_t cur = g_import_counts[i].nid.load(std::memory_order_relaxed);
        if (cur == 0 && g_import_counts[i].nid.compare_exchange_strong(cur, nid))
            cur = nid;
        if (cur == nid) {
            g_import_counts[i].calls.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
}

// Every EXPORT in vita3k/modules (generated at configure time by core-build/CMakeLists.txt, M8A): used only to CLASSIFY
// an import outside the selected dispatcher (implemented in Vita3K / Vita3K stub / absent). Nothing here is callable.
struct Vita3kExport {
    const char *name, *module;
    bool stub;
};
const Vita3kExport kVita3kExports[] = {
#define VITOX_EXPORT(n, m, s) { n, m, s },
#include <vitox_vita3k_exports.inc>
#undef VITOX_EXPORT
};

const VitoxCoreHost *g_host = nullptr;
std::atomic<uint64_t> g_vita3k_errors{ 0 };
namespace sfs = std::filesystem;

template <typename... Args>
void out(fmt::format_string<Args...> f, Args &&...args) {
    g_host->log(fmt::format(f, std::forward<Args>(args)...).c_str());
}
void snap(const char *tag) { g_host->mem_snapshot(tag); }
// 0.0.24.3 (M9A.4, observability only): durable checkpoint = the same core_log line, then the host's write-through
// milestone line + a flush of every log to disk (a stall / forced stop no longer loses what was logged before it)
template <typename... Args>
void cp(fmt::format_string<Args...> f, Args &&...args) {
    const std::string s = fmt::format(f, std::forward<Args>(args)...);
    g_host->log(s.c_str());
    if (g_host->checkpoint)
        g_host->checkpoint(s.c_str());
}
const char *pf(bool b) { return b ? "PASS" : "FAIL"; }

void install_log_bridge() {
    auto sink = std::make_shared<spdlog::sinks::callback_sink_mt>([](const spdlog::details::log_msg &msg) {
        const auto level = spdlog::level::to_string_view(msg.level);
        if (msg.level >= spdlog::level::err)
            ++g_vita3k_errors;
        const std::string line = fmt::format("[vita3k {}] {}", std::string_view(level.data(), level.size()),
            std::string_view(msg.payload.data(), msg.payload.size()));
        g_host->log(line.c_str());
    });
    auto logger = std::make_shared<spdlog::logger>("vita3k", sink);
    logger->set_level(spdlog::level::info);
    spdlog::set_default_logger(logger);
}
// 0.0.25.2: this process's D3D12 local-segment usage (MiB) and private commit (MiB), for the cleanup step log
std::string gpu_mem() {
    double gpu = -1;
    IDXGIFactory4 *f = nullptr;
    if (SUCCEEDED(CreateDXGIFactory2(0, __uuidof(IDXGIFactory4), reinterpret_cast<void **>(&f)))) {
        IDXGIAdapter1 *a = nullptr;
        if (SUCCEEDED(f->EnumAdapters1(0, &a))) {
            IDXGIAdapter3 *a3 = nullptr;
            if (SUCCEEDED(a->QueryInterface(__uuidof(IDXGIAdapter3), reinterpret_cast<void **>(&a3)))) {
                DXGI_QUERY_VIDEO_MEMORY_INFO mi{};
                if (SUCCEEDED(a3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &mi)))
                    gpu = mi.CurrentUsage / 1048576.0;
                a3->Release();
            }
            a->Release();
        }
        f->Release();
    }
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&pmc), sizeof(pmc));
    return fmt::format("gpu_local_mib={:.1f} private_mib={:.1f}", gpu, pmc.PrivateUsage / 1048576.0);
}

void silence_log() {
    spdlog::set_default_logger(std::make_shared<spdlog::logger>("vita3k-m8a-off", std::make_shared<spdlog::sinks::null_sink_mt>())); }

std::string u8(const sfs::path &p) {
    const auto s = p.u8string();
    return std::string(s.begin(), s.end());
}
std::string read_all(const sfs::path &p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
std::set<std::string> names(const sfs::path &dir) {
    std::set<std::string> s;
    std::error_code ec;
    for (auto it = sfs::directory_iterator(dir, ec); !ec && it != sfs::directory_iterator(); it.increment(ec))
        s.insert(u8(it->path().filename()));
    return s;
}
// relative path -> size of every regular file under dir (no symlinks followed)
std::map<std::string, uintmax_t> tree(const sfs::path &dir) {
    std::map<std::string, uintmax_t> t;
    std::error_code ec;
    for (auto it = sfs::recursive_directory_iterator(dir, sfs::directory_options::none, ec); !ec && it != sfs::recursive_directory_iterator(); it.increment(ec)) {
        std::error_code e2;
        if (it->is_regular_file(e2))
            t[u8(it->path().lexically_relative(dir))] = it->file_size(e2);
    }
    return t;
}
std::string trim(std::string s) {
    const auto ws = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!s.empty() && ws(s.back()))
        s.pop_back();
    size_t i = 0;
    while (i < s.size() && ws(s[i]))
        ++i;
    return s.substr(i);
}
// Vita title IDs are 9 upper-case letters/digits; this one is also used as the ux0:app directory name
bool valid_title_id(const std::string &id) {
    return id.size() == 9 && std::all_of(id.begin(), id.end(), [](char c) { return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); });
}

class VitoxFrameHost : public renderer::FrameHost {
public:
    explicit VitoxFrameHost(const vitox::ExternalPresenter &p)
        : p(p) {}
    renderer::DisplayHandle handle() const override { return {}; }
    int drawable_width() const override { return static_cast<int>(p.width()); }
    int drawable_height() const override { return static_cast<int>(p.height()); }
    std::vector<std::string> font_dirs() const override { return {}; }

private:
    const vitox::ExternalPresenter &p;
};

volatile long *dzn_pso_failures() {
    HMODULE dzn = GetModuleHandleW(L"vulkan_dzn.dll");
    return dzn ? reinterpret_cast<volatile long *>(GetProcAddress(dzn, "vitox_dzn_pso_failures")) : nullptr;
}

struct Candidate {
    sfs::path dir;
    std::string folder, title_id, title, app_ver, category, eboot_sha, content_id;
    uintmax_t eboot_size = 0;
    std::string reject; // empty = valid
};

Candidate inspect(const sfs::path &dir, bool hash = true) {
    Candidate c;
    c.dir = dir;
    c.folder = u8(dir.filename());
    std::error_code ec;
    if (sfs::is_symlink(dir, ec)) {
        c.reject = "symlink/junction (not followed)";
        return c;
    }
    const sfs::path sfo_path = dir / "sce_sys" / "param.sfo", eboot_path = dir / "eboot.bin";
    if (!sfs::is_regular_file(sfo_path, ec)) {
        c.reject = "no sce_sys/param.sfo";
        return c;
    }
    if (!sfs::is_regular_file(eboot_path, ec)) {
        c.reject = "no eboot.bin";
        return c;
    }
    c.eboot_size = sfs::file_size(eboot_path, ec);
    if (ec || c.eboot_size == 0) {
        c.reject = "eboot.bin empty or unreadable";
        return c;
    }
    const std::string sfo_bytes = read_all(sfo_path);
    SfoFile sfo_file;
    if (sfo_bytes.empty() || !sfo::load(sfo_file, std::vector<uint8_t>(sfo_bytes.begin(), sfo_bytes.end()))) {
        c.reject = "param.sfo does not parse (real sfo::load)";
        return c;
    }
    sfo::get_data_by_key(c.title_id, sfo_file, "TITLE_ID");
    sfo::get_data_by_key(c.title, sfo_file, "TITLE");
    sfo::get_data_by_key(c.app_ver, sfo_file, "APP_VER");
    sfo::get_data_by_key(c.category, sfo_file, "CATEGORY");
    sfo::get_data_by_key(c.content_id, sfo_file, "CONTENT_ID"); // M9A.3A (absent in most homebrew: empty)
    c.content_id = trim(c.content_id.c_str());
    c.title_id = trim(c.title_id.c_str()); // SFO strings are NUL-padded
    if (!valid_title_id(c.title_id)) {
        c.reject = fmt::format("TITLE_ID '{}' is not 9 characters A-Z/0-9", c.title_id);
        return c;
    }
    if (hash && c.eboot_size <= 64u * 1024 * 1024) {
        const std::string eb = read_all(eboot_path);
        c.eboot_sha = vitox_sha256_hex(eb.data(), eb.size());
    }
    return c;
}

// M8F.2A: upstream app/src/app_init.cpp poll_overlay_input (a static function there), unchanged: the overlay's pad source
// is the keyboard state + every Vita3K controller (here through the M8B.2 WGI-backed SDL gamepad shim)
float sdl_axis_to_float(int16_t axis, float mult) {
    const float unsigned_axis = static_cast<float>(axis - INT16_MIN);
    const float f = unsigned_axis / static_cast<float>(UINT16_MAX);
    return std::clamp(((f * 2.0f) - 1.0f) * mult, -1.0f, 1.0f);
}

overlay::button_states poll_overlay_input(EmuEnvState &emuenv) {
    overlay::button_states states{};
    uint32_t buttons = 0;
    float axes[4] = {};

    std::lock_guard<std::mutex> lock(emuenv.ctrl.mutex);

    auto &kb = emuenv.ctrl.keyboard_state;
    buttons |= kb.buttons_ext;
    axes[0] += kb.axes[0];
    axes[1] += kb.axes[1];
    axes[2] += kb.axes[2];
    axes[3] += kb.axes[3];

    for (const auto &[_, controller] : emuenv.ctrl.controllers) {
        SDL_Gamepad *gp = controller.controller.get();
        for (const auto &binding : get_controller_bindings_ext(emuenv)) {
            if (SDL_GetGamepadButton(gp, binding.controller))
                buttons |= binding.button;
        }

        const auto &axis_binds = emuenv.cfg.controller_axis_binds;
        if (axis_binds.size() > 5) {
            if (SDL_GetGamepadAxis(gp, static_cast<SDL_GamepadAxis>(axis_binds[4])) > 0x3FFF)
                buttons |= SCE_CTRL_L2;
            if (SDL_GetGamepadAxis(gp, static_cast<SDL_GamepadAxis>(axis_binds[5])) > 0x3FFF)
                buttons |= SCE_CTRL_R2;
        }

        if (axis_binds.size() > 3) {
            const float mult = emuenv.cfg.controller_analog_multiplier;
            axes[0] += sdl_axis_to_float(SDL_GetGamepadAxis(gp, static_cast<SDL_GamepadAxis>(axis_binds[0])), mult);
            axes[1] += sdl_axis_to_float(SDL_GetGamepadAxis(gp, static_cast<SDL_GamepadAxis>(axis_binds[1])), mult);
            axes[2] += sdl_axis_to_float(SDL_GetGamepadAxis(gp, static_cast<SDL_GamepadAxis>(axis_binds[2])), mult);
            axes[3] += sdl_axis_to_float(SDL_GetGamepadAxis(gp, static_cast<SDL_GamepadAxis>(axis_binds[3])), mult);
        }
    }

    // 0.1.1.4: no Vita3K controller registered (no physical pad yet, e.g. a scripted DIAGNOSTIC run): the overlay also
    // reads Vitox pad slot 0 (which carries ui-script presses), so common dialogs can be answered
    if (emuenv.ctrl.controllers.empty()) {
        VitoxPadReading r{};
        const VitoxHostInput *hi = g_stick_real ? g_stick_real : vitox_host_input(); // g_stick_real: only with auto_stick
        if (hi && hi->read(0, &r)) {
            static const std::pair<uint32_t, uint32_t> k_map[] = { { 1u << 0, SCE_CTRL_CROSS }, { 1u << 1, SCE_CTRL_CIRCLE },
                { 1u << 2, SCE_CTRL_SQUARE }, { 1u << 3, SCE_CTRL_TRIANGLE }, { 1u << 4, SCE_CTRL_SELECT }, { 1u << 6, SCE_CTRL_START },
                { 1u << 9, SCE_CTRL_L1 }, { 1u << 10, SCE_CTRL_R1 }, { 1u << 11, SCE_CTRL_UP }, { 1u << 12, SCE_CTRL_DOWN },
                { 1u << 13, SCE_CTRL_LEFT }, { 1u << 14, SCE_CTRL_RIGHT } };
            for (const auto &[bit, sce] : k_map)
                if (r.buttons & bit)
                    buttons |= sce;
        }
    }
    auto set = [&](uint32_t mask, overlay::pad_button btn) {
        if (buttons & mask)
            states[static_cast<size_t>(btn)] = true;
    };
    set(SCE_CTRL_UP, overlay::pad_button::dpad_up);
    set(SCE_CTRL_DOWN, overlay::pad_button::dpad_down);
    set(SCE_CTRL_LEFT, overlay::pad_button::dpad_left);
    set(SCE_CTRL_RIGHT, overlay::pad_button::dpad_right);
    set(SCE_CTRL_TRIANGLE, overlay::pad_button::triangle);
    set(SCE_CTRL_CIRCLE, overlay::pad_button::circle);
    set(SCE_CTRL_CROSS, overlay::pad_button::cross);
    set(SCE_CTRL_SQUARE, overlay::pad_button::square);
    set(SCE_CTRL_START, overlay::pad_button::start);
    set(SCE_CTRL_SELECT, overlay::pad_button::select);
    set(SCE_CTRL_L1, overlay::pad_button::L1);
    set(SCE_CTRL_R1, overlay::pad_button::R1);
    set(SCE_CTRL_L2, overlay::pad_button::L2);
    set(SCE_CTRL_R2, overlay::pad_button::R2);
    set(SCE_CTRL_L3, overlay::pad_button::L3);
    set(SCE_CTRL_R3, overlay::pad_button::R3);

    constexpr float threshold = 0.5f;
    if (axes[0] < -threshold)
        states[static_cast<size_t>(overlay::pad_button::ls_left)] = true;
    if (axes[0] > threshold)
        states[static_cast<size_t>(overlay::pad_button::ls_right)] = true;
    if (axes[1] < -threshold)
        states[static_cast<size_t>(overlay::pad_button::ls_up)] = true;
    if (axes[1] > threshold)
        states[static_cast<size_t>(overlay::pad_button::ls_down)] = true;
    if (axes[2] < -threshold)
        states[static_cast<size_t>(overlay::pad_button::rs_left)] = true;
    if (axes[2] > threshold)
        states[static_cast<size_t>(overlay::pad_button::rs_right)] = true;
    if (axes[3] < -threshold)
        states[static_cast<size_t>(overlay::pad_button::rs_up)] = true;
    if (axes[3] > threshold)
        states[static_cast<size_t>(overlay::pad_button::rs_down)] = true;

    return states;
}

std::string ime_text_utf8(Ime &ime) {
    std::lock_guard<std::mutex> lock(ime.mutex);
    return string_utils::utf16_to_utf8(ime.str);
}

// M8F.2B: makes the shared Ime text equal to the host text with Vita3K's REAL editing helpers - drop any preedit, caret
// to the end, backspace the differing tail, commit the new tail (the calls the SDL / Qt frontends make per key/text event;
// ime_commit_text keeps maxTextLength)
void ime_sync_text(Ime &ime, const std::u16string &target) {
    std::lock_guard<std::mutex> lock(ime.mutex);
    ime_set_preedit(ime, u"");
    while (ime.edit_text.caretIndex < ime.str.length()) {
        const uint32_t c = ime.edit_text.caretIndex;
        ime_cursor_right(ime);
        if (ime.edit_text.caretIndex == c)
            break;
    }
    size_t p = 0;
    while (p < ime.str.size() && p < target.size() && ime.str[p] == target[p])
        ++p;
    while (ime.str.size() > p) {
        const size_t n = ime.str.size();
        ime_backspace(ime);
        if (ime.str.size() == n)
            break;
    }
    if (target.size() > p)
        ime_commit_text(ime, target.substr(p));
}

unsigned host_pad_count() {
    const VitoxHostInput *hi = vitox_host_input();
    unsigned n = 0;
    for (unsigned s = 0; hi && s < VITOX_HOST_PADS; ++s)
        n += hi->name(s) != nullptr;
    return n;
}

std::atomic<const VitoxHostText *> g_host_text{ nullptr };

} // namespace

void vitox_set_host_text(const VitoxHostText *host) { g_host_text = host; }
const VitoxHostText *vitox_host_text() { return g_host_text.load(); }

// 0.0.24.76: "<x>,<y>,<from>,<to>[,<period_ms>][;...]", up to 6 segments (Vita pixels, seconds after the guest starts)
struct TouchSegment {
    float x, y, from_s, to_s, period_s;
};
static bool vitox_parse_auto_touch(const std::string &v, std::vector<TouchSegment> *out) {
    size_t n = 0;
    for (size_t b = 0, e; b <= v.size(); b = e + 1) {
        e = v.find(';', b);
        const std::string seg = v.substr(b, (e == std::string::npos ? v.size() : e) - b);
        float x, y, f0, f1, period_ms = 1000;
        const int k = sscanf_s(seg.c_str(), "%f,%f,%f,%f,%f", &x, &y, &f0, &f1, &period_ms);
        if ((k != 4 && k != 5) || x < 0 || x >= 960 || y < 0 || y >= 544 || f0 < 0 || f1 <= f0 || f1 > 3600 || period_ms < 50 || period_ms > 10000 || ++n > 6)
            return false;
        if (out)
            out->push_back(TouchSegment{ x, y, f0, f1, period_ms / 1000.0f });
        if (e == std::string::npos)
            break;
    }
    return n > 0;
}

// 0.0.24.74: "<from>,<to>,<lx>,<ly>[,<buttons>][;...]", up to 6 segments, times 0..3600 s, stick -1..1
// 0.0.24.98: optional decimal button mask (bit = SDL_GamepadButton: 1 = A/south, 4096 = D-pad down), held for the segment
static bool vitox_parse_auto_stick(const std::string &v, std::vector<StickSegment> *out) {
    size_t n = 0;
    for (size_t b = 0, e; b <= v.size(); b = e + 1) {
        e = v.find(';', b);
        const std::string seg = v.substr(b, (e == std::string::npos ? v.size() : e) - b);
        StickSegment g{};
        const int got = sscanf_s(seg.c_str(), "%f,%f,%f,%f,%u", &g.from_s, &g.to_s, &g.lx, &g.ly, &g.buttons);
        if ((got != 4 && got != 5) || g.from_s < 0 || g.to_s <= g.from_s || g.to_s > 3600 || g.lx < -1
            || g.lx > 1 || g.ly < -1 || g.ly > 1 || ++n > 6)
            return false;
        if (out)
            out->push_back(g);
        if (e == std::string::npos)
            break;
    }
    return n > 0;
}

// 0.0.25.0: one key=value line of vitox-launch.txt or of a vitox-profiles.txt section (false = not a supported line)
static bool vitox_apply_config_line(VitoxLaunchConfig &cfg, const std::string &k, const std::string &v) {
    if (k == "ui" && (v == "launcher" || v == "direct"))
        cfg.ui_direct = v == "direct";
    else if (k == "ui_script" && !v.empty() && v.size() < sizeof(cfg.ui_script)
        && std::all_of(v.begin(), v.end(), [](char c) { return isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-' || c == '_'; }))
        strcpy_s(cfg.ui_script, v.c_str());
    else if (k == "app" && v.size() < sizeof(cfg.app))
            strcpy_s(cfg.app, v.c_str());
        else if (k == "presenter" && (v == "visual" || v == "headless"))
            cfg.headless = v == "headless";
        else if (k == "unsupported_imports" && (v == "stop" || v == "trap" || v == "fail-closed"))
            cfg.trap = v == "trap", cfg.fail_closed = v == "fail-closed";
        else if (k == "async_pipelines" && (v == "on" || v == "off"))
            cfg.sync_pipelines = v == "off"; // M9A.6
        else if (k == "auto_stick" && v.size() < sizeof(cfg.auto_stick) && vitox_parse_auto_stick(v, nullptr)) // 0.0.24.74 diagnostic
            strcpy_s(cfg.auto_stick, v.c_str());
        else if (k == "auto_touch" && v.size() < sizeof(cfg.auto_touch) && vitox_parse_auto_touch(v, nullptr)) // 0.0.24.60/.76 diagnostic
            strcpy_s(cfg.auto_touch, v.c_str());
        else if (k == "firmware" && (v == "none" || v == "staging-pgf"))
            cfg.staged_pgf = v == "staging-pgf";
        else if (k == "app_root" && (v == "homebrew" || v == "app-staging"))
            strcpy_s(cfg.app_root, v.c_str());
        else if (k == "licensed_load" && (v == "off" || v == "staged-license"))
            cfg.licensed_load = v == "staged-license";
        else if (k == "lle_modules" && !v.empty() && v.size() < sizeof(cfg.lle_modules) && [&] {
                     // allowlist: the app-bundled SDK runtime modules upstream preloads from app0:sce_module
                     for (size_t b = 0, e; b <= v.size(); b = e + 1) {
                         e = v.find(',', b);
                         const std::string m = trim(v.substr(b, (e == std::string::npos ? v.size() : e) - b));
                         // M9A.6: libult (upstream SceUlt HLE is all stubs; load_sys_module(ULT) loads app0:sce_module/libult.suprx)
                         // M9A.6 (user-approved 2026-10-26): libpvf + libSceFt2 - staged firmware modules (vs0:sys/external),
                         // preloaded as upstream does; their decrypt uses the M8C.2 staged-firmware path (needs StagedPgf)
                         // M9A.6 (user-approved 2026-10-27): libscemp4 - the staged firmware MP4 library (vs0:sys/external), which upstream
                         // auto-LLEs for SCE_SYSMODULE_MP4 (load_module.cpp auto_lle_modules); upstream's load_sys_module loads it when the
                         // game asks for SCE_SYSMODULE_MP4 (its HLE SceMp4 is all UNIMPLEMENTED: the opening movie never opens)
                         // M9A.7 (next title user-approved 2026-10-28): libface + libsmart - app-bundled plain SELFs that upstream
                         // auto-LLEs (auto_lle_modules: SCE_SYSMODULE_FACE / SCE_SYSMODULE_SMART); load_sys_module loads them
                         // from app0:sce_module
                         // 0.1.1.8: libsystemgesture - the staged firmware gesture library (vs0:sys/external), which upstream
                         // auto-LLEs (SCE_SYSMODULE_SYSTEM_GESTURE); its HLE SceSystemGesture is all UNIMPLEMENTED, so touch
                         // recognizers never report a TAP (menus that need a tap see the finger down but never select)
                         if (m != "libc" && m != "libfios2" && m != "libult" && m != "libpvf" && m != "libSceFt2" && m != "libscemp4" &&
                             m != "libface" && m != "libsmart" && m != "libsystemgesture")
                             return false;
                         if (e == std::string::npos)
                             break;
                     }
                     return true;
                 }())
            strcpy_s(cfg.lle_modules, v.c_str()), cfg.lle_set = true;
        else if (k == "ime_input" && !v.empty() && v.size() < sizeof(cfg.ime_input)
            && std::all_of(v.begin(), v.end(), [](char c) { return isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-' || c == '_'; }))
            strcpy_s(cfg.ime_input, v.c_str());
        else if (k == "timeout_s" && !v.empty() && v.size() <= 4 && std::all_of(v.begin(), v.end(), ::isdigit) && std::stoul(v) >= 5 && std::stoul(v) <= 3600) // M9A.5: up to 60 min
            cfg.timeout_s = static_cast<unsigned>(std::stoul(v));
        else
            return false;
    return true;
}

// 0.0.25.0: key=value lines of one file; `section` empty = the lines before any [SECTION] header (vitox-launch.txt has
// none), else only the lines of [section]
static void vitox_read_config_lines(VitoxLaunchConfig &cfg, const sfs::path &p, const std::string &section) {
    std::ifstream f(p);
    if (!f)
        return;
    std::string line, current;
    while (std::getline(f, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#')
            continue;
        if (line.front() == '[' && line.back() == ']') {
            current = trim(line.substr(1, line.size() - 2));
            continue;
        }
        if (current != section)
            continue;
        const size_t eq = line.find('=');
        const std::string k = eq == std::string::npos ? line : trim(line.substr(0, eq)), v = eq == std::string::npos ? "" : trim(line.substr(eq + 1));
        if (!vitox_apply_config_line(cfg, k, v) && !cfg.error[0])
            snprintf(cfg.error, sizeof(cfg.error), "unsupported line '%.60s'%s", line.c_str(), section.empty() ? "" : " (vitox-profiles.txt)");
    }
}

extern "C" VitoxLaunchConfig vitox_launcher_read_config(const wchar_t *dev_root) {
    VitoxLaunchConfig cfg{};
    cfg.timeout_s = 60;
    const sfs::path p = sfs::path(dev_root) / "homebrew" / "vitox-launch.txt";
    std::error_code fec;
    if (!sfs::is_regular_file(p, fec))
        return cfg;
    cfg.found = true;
    vitox_read_config_lines(cfg, p, "");
    // M9A.3A: the licensed-load mode is valid only with its companions (checked after every line is read)
    if (const char *e = vitox_licensed_load_config_error(cfg.licensed_load, cfg.app_root, cfg.fail_closed, cfg.headless); e && !cfg.error[0])
        strcpy_s(cfg.error, e);
    return cfg;
}

extern "C" unsigned vitox_launcher_discover(const wchar_t *dev_root, VitoxAppInfo *out, unsigned max) {
    std::vector<VitoxAppInfo> apps;
    for (const char *root_name : { "homebrew", "app-staging" }) {
        const bool staging = strcmp(root_name, "app-staging") == 0;
        const sfs::path root = sfs::path(dev_root) / root_name;
        std::error_code ec;
        for (auto it = sfs::directory_iterator(root, ec); !ec && it != sfs::directory_iterator(); it.increment(ec)) {
            std::error_code e2;
            if (!it->is_directory(e2) || (staging && it->path().filename() == "licenses"))
                continue;
            const Candidate c = inspect(it->path(), false);
            if (!c.reject.empty() || (staging && c.folder != c.title_id))
                continue;
            VitoxAppInfo a{};
            strncpy_s(a.title_id, c.title_id.c_str(), _TRUNCATE);
            strncpy_s(a.title, trim(c.title.c_str()).c_str(), _TRUNCATE);
            strncpy_s(a.folder, c.folder.c_str(), _TRUNCATE);
            strncpy_s(a.app_root, root_name, _TRUNCATE);
            strncpy_s(a.app_ver, trim(c.app_ver.c_str()).c_str(), _TRUNCATE);
            strncpy_s(a.category, trim(c.category.c_str()).c_str(), _TRUNCATE);
            wcsncpy_s(a.dir, c.dir.wstring().c_str(), _TRUNCATE);
            apps.push_back(a);
        }
    }
    std::sort(apps.begin(), apps.end(), [](const VitoxAppInfo &x, const VitoxAppInfo &y) {
        const int t = _stricmp(x.title[0] ? x.title : x.title_id, y.title[0] ? y.title : y.title_id);
        return t != 0 ? t < 0 : strcmp(x.title_id, y.title_id) < 0;
    });
    for (unsigned i = 0; i < apps.size() && i < max; ++i)
        out[i] = apps[i];
    return static_cast<unsigned>(apps.size());
}

extern "C" VitoxLaunchConfig vitox_launcher_config_for(const wchar_t *dev_root, const VitoxAppInfo *app) {
    VitoxLaunchConfig cfg = vitox_launcher_read_config(dev_root);
    // the per-run keys of the base file belong to the harness run that wrote it, not to a library launch
    cfg.app[0] = 0;
    cfg.auto_stick[0] = cfg.auto_touch[0] = cfg.ime_input[0] = 0;
    cfg.licensed_load = false;
    cfg.trap = false;
    vitox_read_config_lines(cfg, sfs::path(dev_root) / "homebrew" / "vitox-profiles.txt", app->title_id);
    strncpy_s(cfg.app, app->folder, _TRUNCATE);
    strncpy_s(cfg.app_root, app->app_root, _TRUNCATE);
    cfg.headless = false;
    cfg.no_timeout = true;
    // 0.0.27.1: a library launch no longer depends on hand-written profile keys to start at all
    std::error_code fec;
    const bool staging_ready = sfs::exists(sfs::path(dev_root) / "firmware-staging" / "stage-complete.txt", fec);
    if (!cfg.lle_set) {
        // derived: the allowlisted app-bundled runtime modules the app ships in sce_module, plus (with the staged firmware)
        // the firmware modules upstream loads on demand through load_sys_module
        std::string lle;
        std::error_code ec;
        for (const char *m : { "libc", "libfios2", "libult", "libface", "libsmart" })
            if (sfs::is_regular_file(sfs::path(app->dir) / "sce_module" / (std::string(m) + ".suprx"), ec))
                lle += (lle.empty() ? "" : ",") + std::string(m);
        if ((cfg.staged_pgf || staging_ready) && !lle.empty())
            lle += ",libSceFt2,libpvf,libscemp4,libsystemgesture";
        strncpy_s(cfg.lle_modules, lle.c_str(), _TRUNCATE);
    }
    // (1) the firmware modules (libSceFt2 / libpvf / libscemp4) exist only in the staged firmware's vs0: a title that
    // selects one gets the staged firmware devices automatically (before: "Failed to read module file vs0:..." and the
    // game never started unless its profile also said firmware=staging-pgf)
    const std::string lle_list = std::string(",") + cfg.lle_modules + ",";
    const bool needs_vs0 = lle_list.find(",libSceFt2,") != std::string::npos || lle_list.find(",libpvf,") != std::string::npos
        || lle_list.find(",libscemp4,") != std::string::npos || lle_list.find(",libsystemgesture,") != std::string::npos;
    if (needs_vs0 && !cfg.staged_pgf) {
        if (staging_ready)
            cfg.staged_pgf = true;
        else if (!cfg.error[0])
            strcpy_s(cfg.error, "it needs the Vita firmware modules, but the firmware is not staged on this console");
    }
    // (2) a user session runs under the fail-closed gate (the mode every title was validated in): the game runs, and only
    // a CALL to an unsupported system function stops it. Without a gate the launcher refused any title with unsupported
    // imports before its first instruction, silently. An explicit unsupported_imports=trap (diagnostic) is kept.
    if (!cfg.trap)
        cfg.fail_closed = true;
    if (const char *e = vitox_licensed_load_config_error(cfg.licensed_load, cfg.app_root, cfg.fail_closed, cfg.headless); e && !cfg.error[0])
        strcpy_s(cfg.error, e);
    return cfg;
}

extern "C" VitoxLaunchResult vitox_homebrew_launch_run(const VitoxCoreHost *host, vitox::ExternalPresenter *presenter, void (*pump)(),
    const VitoxLaunchConfig *config, const wchar_t *local_state, const wchar_t *dev_root, const wchar_t *static_assets) {
    g_host = host;
    VitoxLaunchResult r{};
    g_vita3k_errors = 0;
    install_log_bridge();
    // M9A.2A: app_root=app-staging discovers apps in <dev_root>\app-staging (commercial-app staging); homebrew default
    const sfs::path cfg_root = sfs::path(dev_root) / "homebrew";
    const sfs::path root = sfs::path(dev_root) / (config->app_root[0] ? config->app_root : "homebrew");
    const fs::path ls(local_state), vita_fs = fs::path(dev_root) / "vita-fs";
    const sfs::path s_vita_fs(vita_fs.native());
    out("M8A launcher begin: homebrew root '{}' | config {} (app='{}' presenter={} timeout_s={}{}{}) | vita-fs '{}'", u8(root),
        config->found ? "vitox-launch.txt" : "absent (defaults)", config->app, config->headless ? "headless" : "visual", config->timeout_s,
        config->error[0] ? " | CONFIG ERROR: " : "", config->error, u8(s_vita_fs));
    snap("m8a_base");
    vitox_import_checkpoint_hook(host->checkpoint);
    cp("m9a4 cp 00 launcher begin: app='{}' app_root={} presenter={} fail_closed={} trap={} lle_modules={} timeout_s={} licensed_load={}", config->app,
        config->app_root[0] ? config->app_root : "homebrew", config->headless ? "headless" : "visual", config->fail_closed, config->trap,
        config->lle_modules[0] ? config->lle_modules : "-", config->timeout_s, config->licensed_load ? "staged-license" : "off");

    // ---- 1. discovery (generic: every direct sub-directory of the root) ----
    // M9A.2C: in app-staging, 'licenses' is the separate license staging folder (never an app, never read here) and an
    // app folder must be named after its param.sfo TITLE_ID (the harness app-push layout)
    const bool staging = strcmp(config->app_root, "app-staging") == 0;
    std::vector<Candidate> cands;
    std::error_code ec;
    for (auto it = sfs::directory_iterator(root, ec); !ec && it != sfs::directory_iterator(); it.increment(ec)) {
        std::error_code e2;
        if (!it->is_directory(e2))
            continue;
        if (staging && it->path().filename() == "licenses") {
            out("m9a.2c discovery: 'licenses' is the license staging folder, not an app (skipped; nothing in it is read or installed)");
            continue;
        }
        cands.push_back(inspect(it->path()));
        Candidate &c = cands.back();
        if (staging && c.reject.empty() && c.folder != c.title_id)
            c.reject = fmt::format("app-staging folder name is not its param.sfo TITLE_ID '{}'", c.title_id);
    }
    std::map<std::string, unsigned> id_count;
    for (const Candidate &c : cands)
        if (c.reject.empty())
            ++id_count[c.title_id];
    for (Candidate &c : cands) {
        if (c.reject.empty()) {
            ++r.candidates;
            out("m8a discovered: '{}' TITLE_ID={} TITLE='{}' APP_VER={} CATEGORY={} | eboot.bin {} B SHA-256 {}{}", u8(c.dir), c.title_id, c.title, c.app_ver,
                c.category, c.eboot_size, c.eboot_sha.empty() ? "(not hashed, > 64 MiB)" : c.eboot_sha,
                id_count[c.title_id] > 1 ? " | DUPLICATE TITLE_ID (select by folder name)" : "");
        } else {
            ++r.rejected;
            out("m8a rejected: '{}': {}", u8(c.dir), c.reject);
        }
    }
    r.discovered = r.candidates > 0;
    out("m8a discovery: root {} | candidates valid={} rejected={} -> {}", sfs::is_directory(root, ec) ? "present" : "MISSING", r.candidates, r.rejected,
        pf(r.discovered));

    // ---- 2. selection (fail closed: config error, no match, ambiguous match) ----
    const Candidate *sel = nullptr;
    std::string why;
    if (config->error[0])
        why = fmt::format("config error: {}", config->error);
    else if (!r.discovered)
        why = "no valid candidate";
    else if (!config->app[0]) {
        if (r.candidates == 1) {
            for (const Candidate &c : cands)
                if (c.reject.empty())
                    sel = &c;
            why = "no app= in the config: the single valid candidate";
        } else
            why = fmt::format("no app= in the config and {} candidates: ambiguous", r.candidates);
    } else {
        std::vector<const Candidate *> by_folder, by_id;
        for (const Candidate &c : cands) {
            if (!c.reject.empty())
                continue;
            if (c.folder == config->app)
                by_folder.push_back(&c);
            if (c.title_id == config->app)
                by_id.push_back(&c);
        }
        if (by_folder.size() == 1)
            sel = by_folder[0], why = "app= matches the folder name";
        else if (by_id.size() == 1)
            sel = by_id[0], why = "app= matches the param.sfo TITLE_ID";
        else if (by_id.size() > 1)
            why = fmt::format("app={} matches {} folders by TITLE_ID: ambiguous", config->app, by_id.size());
        else
            why = fmt::format("app={} matches no valid candidate", config->app);
    }
    r.selected = sel != nullptr;
    out("m8a selection: {} -> {}{}", why, sel ? fmt::format("'{}' ({})", sel->folder, sel->title_id) : std::string("NONE"), r.selected ? "" : " | launch refused");
    cp("m9a4 cp 01 discovery+selection: candidates valid={} rejected={} -> selected={} {}", r.candidates, r.rejected, r.selected,
        sel ? fmt::format("'{}' ({})", sel->folder, sel->title_id) : std::string("NONE"));
    if (!r.selected) {
        silence_log();
        return r;
    }
    strncpy_s(r.title_id, sel->title_id.c_str(), _TRUNCATE);
    strncpy_s(r.title, sel->title.c_str(), _TRUNCATE);
    strncpy_s(r.folder, sel->folder.c_str(), _TRUNCATE);
    const std::string title_id = sel->title_id;

    // ---- 2a. M9A.3A: app-staging source safety, then (licensed_load only) app + staged-license VALIDATION. Read-only:
    // no key is used, nothing is decrypted, installed or registered; the run always stops before guest code (below). ----
    std::map<std::string, uintmax_t> staging_before;
    std::string licensed_why;
    const auto stop = [&](const std::string &reason) {
        strncpy_s(r.stop_reason, reason.c_str(), _TRUNCATE);
        out("m9a.3 STOP: {} | nothing installed, no license registered, nothing decrypted, no guest code run", reason);
        silence_log();
        return r;
    };
    if (staging) {
        staging_before = tree(root);
        // no symlink/junction anywhere in the selected app (the iterator does not follow them; MSVC reports junctions apart)
        unsigned links = 0;
        std::string first_link;
        for (auto it = sfs::recursive_directory_iterator(sel->dir, sfs::directory_options::none, ec); !ec && it != sfs::recursive_directory_iterator(); it.increment(ec)) {
            std::error_code e2;
            const auto t = it->symlink_status(e2).type();
            if ((t == sfs::file_type::symlink || t == sfs::file_type::junction) && links++ == 0)
                first_link = u8(it->path().lexically_relative(sel->dir));
        }
        ec.clear();
        if (links)
            return stop(fmt::format("scope violation: {} symlink/junction item(s) inside the staged app (first '{}')", links, first_link));
    }
    if (config->licensed_load) { // (the config gate guarantees app-staging + fail-closed + headless)
        r.licensed_requested = true;
        licensed_why = vitox_app_identity_error(sel->folder, sel->title_id, sel->content_id);
        const sfs::path lic_dir = root / "licenses" / (licensed_why.empty() ? sel->content_id : std::string("-"));
        if (licensed_why.empty()) {
            // discovery: licenses\<CONTENT_ID> is a plain folder holding exactly one regular file, work.bin
            std::error_code e2;
            const auto st = sfs::symlink_status(lic_dir, e2);
            if (!sfs::exists(st))
                licensed_why = fmt::format("missing license: no staged licenses\\{}", sel->content_id);
            else if (st.type() != sfs::file_type::directory)
                licensed_why = fmt::format("scope violation: licenses\\{} is not a plain folder (symlink/junction/file)", sel->content_id);
            else {
                unsigned entries = 0;
                bool work = false;
                for (auto it = sfs::directory_iterator(lic_dir, ec); !ec && it != sfs::directory_iterator(); it.increment(ec)) {
                    ++entries;
                    std::error_code e3;
                    work |= it->path().filename() == "work.bin" && it->symlink_status(e3).type() == sfs::file_type::regular;
                }
                ec.clear();
                if (!work)
                    licensed_why = fmt::format("missing license: no regular work.bin in licenses\\{}", sel->content_id);
                else if (entries != 1)
                    licensed_why = fmt::format("licenses\\{} holds {} items (exactly one work.bin expected)", sel->content_id, entries);
                else {
                    std::string lb = read_all(lic_dir / "work.bin");
                    const VitoxLicenseCheck lc = vitox_license_check(reinterpret_cast<const uint8_t *>(lb.data()), lb.size(), sel->content_id, sel->title_id);
                    std::fill(lb.begin(), lb.end(), '\0'); // validation only: the bytes are not kept
                    if (!lc.ok)
                        licensed_why = "malformed or mismatched license: " + lc.reason;
                }
            }
        }
        r.licensed_valid = licensed_why.empty();
        out("m9a.3 licensed-load validation (staged-license): app '{}' TITLE_ID={} CONTENT_ID={} | license licenses\\{}\\work.bin -> {}{}", sel->folder,
            sel->title_id, sel->content_id.empty() ? "-" : sel->content_id, sel->content_id.empty() ? "-" : sel->content_id, pf(r.licensed_valid),
            r.licensed_valid ? "" : " | " + licensed_why);
    }

    // ---- 2b. M9A.2C: app-staging content must be directly loader-readable, checked before anything is installed ----
    // Required: eboot.bin and every SELF in sce_module (the app may load any of them). A protected (NPDRM/encrypted) one
    // fails the launch closed: no supported licensed-load mode exists in this build, so nothing is decrypted, installed
    // or run. The homebrew root keeps its unchanged behaviour (no check).
    // 0.0.29.1 (user-approved 2026-11-04): a game installed from a NoNpDrm dump by the launcher's USB installer has its
    // PFS layer decrypted and its license at ux0:license/<TITLE_ID>/<CONTENT_ID>.rif (upstream copy_license). Its SELFs
    // stay NPDRM-encrypted; Vita3K's loader decrypts them with that license (decrypt_fself), so they are accepted here.
    const sfs::path nonpdrm_rif = s_vita_fs / "ux0" / "license" / title_id / (sel->content_id + ".rif");
    const bool nonpdrm_license = staging && !sel->content_id.empty() && sfs::is_regular_file(nonpdrm_rif, ec);
    ec.clear();
    if (staging) {
        r.content_checked = true;
        std::vector<sfs::path> req{ sel->dir / "eboot.bin" };
        for (auto it = sfs::directory_iterator(sel->dir / "sce_module", ec); !ec && it != sfs::directory_iterator(); it.increment(ec)) {
            std::error_code e2;
            const std::string ext = u8(it->path().extension());
            if (it->is_regular_file(e2) && (ext == ".suprx" || ext == ".self" || ext == ".sprx"))
                req.push_back(it->path());
        }
        ec.clear();
        std::string first;
        for (const sfs::path &p : req) {
            const std::string b = read_all(p);
            const VitoxSelfCheck c = vitox_self_check(reinterpret_cast<const uint8_t *>(b.data()), b.size());
            const bool licensed = !c.readable && nonpdrm_license && c.kind == "SELF" && c.segments > 0; // NPDRM SELF + license
            r.content_protected += !c.readable && !licensed;
            if (!c.readable && !licensed && first.empty())
                first = u8(p.lexically_relative(sel->dir));
            out("m9a.2c content: '{}' {} B {} self_type={} -> {} ({})", u8(p.lexically_relative(sel->dir)), b.size(), c.kind, c.self_type,
                c.readable ? "loader-readable" : licensed ? "NPDRM, licensed (decrypt_fself at load)" : "PROTECTED", c.detail);
        }
        r.content_readable = r.content_protected == 0;
        out("m9a.2c content check: {} required executable(s), protected={} -> {}{}", req.size(), r.content_protected, pf(r.content_readable),
            r.content_readable || config->licensed_load
                ? ""
                : fmt::format(" | FAIL CLOSED: '{}' is not directly loader-readable and this build has no supported licensed-load mode "
                              "(app_root=app-staging accepts loader-readable content only) | nothing installed, no guest code run",
                      first));
        cp("m9a4 cp 02 content check: {} required executable(s), protected={} -> {}", req.size(), r.content_protected, pf(r.content_readable));
        if (!r.content_readable && !config->licensed_load) {
            silence_log();
            return r;
        }
    }

    // ---- 2c. M9A.3A: licensed_load always stops here in this build (prep only: protected SELF execution unsupported) ----
    if (config->licensed_load) {
        r.staging_unchanged = tree(root) == staging_before;
        out("m9a.3 staging: app-staging tree {} file(s), unchanged after validation -> {}", staging_before.size(), pf(r.staging_unchanged));
        if (!r.licensed_valid)
            return stop("licensed-load validation failed: " + licensed_why);
        return stop(r.content_protected
                ? fmt::format("licensed-load validation PASSED, but {} protected executable(s) need protected SELF execution, which this build "
                              "does not support (M9A.3A prep only)",
                      r.content_protected)
                : std::string("licensed-load validation PASSED, but licensed-load execution is not supported in this build (M9A.3A prep only)"));
    }

    // ---- 3. install into ux0:app/<TITLE_ID> (real fs_utils copy; nothing is deleted) ----
    const sfs::path s_app = s_vita_fs / "ux0" / "app" / title_id;
    const auto apps_before = names(s_vita_fs / "ux0" / "app"), data_before = names(s_vita_fs / "ux0" / "data");
    const auto root_before = tree(root);
    const auto src_tree = tree(sel->dir);
    const bool app_preexisting = sfs::exists(s_app, ec);
    const bool copied = fs_utils::copy_directory_contents(fs::path(sel->dir.native()), fs::path(s_app.native()));
    const auto dst_tree = tree(s_app);
    unsigned missing = 0;
    for (const auto &[rel, size] : src_tree) {
        const auto it = dst_tree.find(rel);
        missing += it == dst_tree.end() || it->second != size;
    }
    const bool eq_eboot = read_all(sel->dir / "eboot.bin") == read_all(s_app / "eboot.bin");
    const bool eq_sfo = read_all(sel->dir / "sce_sys" / "param.sfo") == read_all(s_app / "sce_sys" / "param.sfo");
    r.installed = copied && missing == 0 && eq_eboot && eq_sfo;
    out("m8a install: '{}' -> '{}' (guest ux0:app/{}, {}) | fs_utils::copy_directory_contents={} | source files {} (missing/size-mismatch at destination {}) "
        "| eboot.bin equal {} | param.sfo equal {} -> {}",
        u8(sel->dir), u8(s_app), title_id, app_preexisting ? "updated in place" : "new", copied, src_tree.size(), missing, eq_eboot, eq_sfo, pf(r.installed));
    cp("m9a4 cp 03 install validation: files {} missing/size-mismatch {} eboot equal {} param.sfo equal {} -> {}", src_tree.size(), missing, eq_eboot, eq_sfo,
        pf(r.installed));
    if (!r.installed) {
        silence_log();
        return r;
    }

    // ---- 4. EmuEnvState + io + renderer (Vita3K app order: renderer, late_init, mem) ----
    auto *emuenv = new EmuEnvState();
    emuenv->config_path = ls;
    emuenv->log_path = ls / "log";
    emuenv->cache_path = ls / "cache";
    vitox_xaudio2_dump_begin(ls.wstring()); // 0.1.1.3 DIAGNOSTIC: boot buzz
    {
        // 0.1.1.4: the 40-lines-per-NID import log budget is per game session (a previous game used it up before)
        std::lock_guard<std::mutex> lock(g_import_note_m);
        g_import_notes.clear();
    }
    emuenv->vita_fs_path = vita_fs;
    emuenv->static_assets_path = fs::path(static_assets);
    std::atomic<bool> exited{ false };
    std::atomic<int> exit_code{ -1 };
    std::mutex exec_m; // 0.0.29.3: a LoadExec relaunch request (upstream sdl-frontend session.cpp relaunches with it)
    std::optional<AppLaunchRequest> exec_request;
    emuenv->kernel.process_exit_callback = [&](int res, std::optional<AppLaunchRequest> relaunch) {
        if (relaunch && relaunch->reason == AppLaunchReason::LoadExec) {
            std::lock_guard<std::mutex> lock(exec_m);
            exec_request = std::move(relaunch);
        }
        exit_code = res;
        exited = true;
    };
    vitox_import_stats_reset();
    bool ok = init(emuenv->io, emuenv->cache_path, emuenv->log_path, emuenv->vita_fs_path, false);
    vitox_set_external_presenter(presenter);
    vitox_xp_set_variant(VitoxXpVariant::P_Production);
    VitoxFrameHost frame_host(*presenter);
    const bool rinit = ok && renderer::init(frame_host, emuenv->renderer, renderer::Backend::Vulkan, emuenv->cfg, emuenv->get_root_paths());
    bool late = false;
    if (rinit) {
        emuenv->renderer->late_init(emuenv->cfg, title_id, emuenv->mem);
        const bool need_page_table = emuenv->renderer->mapping_method == MappingMethod::PageTable || emuenv->renderer->mapping_method == MappingMethod::NativeBuffer;
        late = init(emuenv->mem, need_page_table);
        // 0.0.27.0: the user's internal resolution (Settings > Graphics) for this session
        emuenv->cfg.current_config.resolution_multiplier = vitox_launcher_resolution_multiplier();
        emuenv->cfg.resolution_multiplier = emuenv->cfg.current_config.resolution_multiplier;
        const auto &cc = emuenv->cfg.current_config;
        auto &rs = *emuenv->renderer;
        rs.res_multiplier = cc.resolution_multiplier;
        out("vitox graphics: internal resolution {:.1f}x = {}x{} (output scaled by the screen renderer)", rs.res_multiplier,
            static_cast<int>(std::lround(960 * rs.res_multiplier)), static_cast<int>(std::lround(544 * rs.res_multiplier)));
        rs.set_vsync_state(cc.v_sync);
        // M9A.10 EXPERIMENT (0.0.24.131): surface sync ON. Upstream defaults disable-surface-sync=true, which is harmless with
        // memory mapping (the GPU renders into guest memory); with mapping off (Xbox) it is the only way a rendered surface
        // reaches guest memory. Gravity Rush appears to read its luminance back on the CPU for tone-map exposure.
        rs.set_surface_sync_state(cc.disable_surface_sync); // 0.0.24.131 forced false: no effect (Vulkan surface sync needs memory mapping); reverted 0.0.24.132
        rs.set_screen_filter(cc.screen_filter);
        rs.set_anisotropic_filtering(cc.anisotropic_filtering);
        rs.set_stretch_display(cc.stretch_the_display_area);
        rs.stretch_hd_pixel_perfect(cc.fullscreen_hd_res_pixel_perfect);
        // M9A.6: async_pipelines=off selects upstream's own async-pipeline-compilation=false (synchronous compilation). Gravity
        // Rush crashed twice in the async compiler thread (PipelineCache::compile_pipeline, access violation on a host pointer).
        // 0.0.28.3: Settings > Shader Compilation decides for a library session (no_timeout); a direct run also honours async_pipelines=off
        const bool vitox_setting_async = vitox_launcher_async_pipelines();
        const bool vitox_sync = config->no_timeout ? !vitox_setting_async : (config->sync_pipelines || !vitox_setting_async);
        rs.set_async_compilation(vitox_sync ? false : cc.async_pipeline_compilation);
        out("m9a6 pipelines: asynchronous compilation {} | pre-warm {} (Settings: async {}, pre-warm {}; {} session)",
            vitox_sync ? "OFF" : (cc.async_pipeline_compilation ? "on" : "off (config)"), vitox_launcher_pipeline_prewarm() ? "on" : "OFF",
            vitox_setting_async ? "on" : "off", vitox_launcher_pipeline_prewarm() ? "on" : "off", config->no_timeout ? "library" : (config->sync_pipelines ? "direct, async_pipelines=off" : "direct"));
        // M8F.2A (approved): Vita3K's overlay + common-dialog wiring, as upstream app_init.cpp does it (the renderer then
        // creates common_dialog_overlay itself while a dialog is RUNNING; its input thread reads the pads polled here)
        if (!emuenv->overlay_manager)
            emuenv->overlay_manager = std::make_unique<overlay::display_manager>();
        rs.overlay_manager = emuenv->overlay_manager.get();
        emuenv->overlay_manager->set_flip_request_callback([&rs]() {
            rs.async_flip_requested.store(true, std::memory_order_relaxed);
            rs.command_buffer_queue.wake();
        });
        rs.common_dialog = &emuenv->common_dialog;
        rs.sys_date_format = cc.sys_date_format;
        rs.sys_lang = cc.sys_lang;
        rs.sys_button = cc.sys_button;
        emuenv->overlay_manager->set_input_callbacks(
            [emuenv]() -> overlay::button_states {
                // M8F.2B: while the system keyboard is up the pad types on it - the overlay does not also take that
                // press as Cross/Circle (it sees the pads again once the keyboard hides)
                const VitoxHostText *ht = vitox_host_text();
                if (ht && ht->keyboard_visible())
                    return overlay::button_states{};
                return poll_overlay_input(*emuenv);
            },
            [emuenv](bool intercepted) { emuenv->ctrl.overlay_input_intercepted.store(intercepted, std::memory_order_relaxed); },
            [emuenv]() -> overlay::overlay_touch_state {
                auto &mouse = emuenv->ctrl.overlay_mouse;
                overlay::overlay_touch_state state;
                state.x = mouse.x.load(std::memory_order_relaxed);
                state.y = mouse.y.load(std::memory_order_relaxed);
                state.pressed = mouse.pressed.load(std::memory_order_relaxed);
                return state;
            });
        // 0.0.32.0 (user request): the trophy-unlocked banner, as upstream app_init.cpp registers it (grade icons from
        // static_assets\icons; the banner is drawn top-left in Vitox, see core-build trophy_notification substitution)
        emuenv->np.trophy_state.add_trophy_unlock_callback([emuenv](NpTrophyUnlockCallbackData &data) {
            if (!emuenv->overlay_manager)
                return;
            auto notif = emuenv->overlay_manager->get<overlay::trophy_notification>();
            if (!notif)
                notif = emuenv->overlay_manager->create<overlay::trophy_notification>();
            std::vector<uint8_t> grade_icon_buf;
            const char *grade_name = nullptr;
            switch (data.trophy_kind) {
            case np::trophy::SceNpTrophyGrade::SCE_NP_TROPHY_GRADE_PLATINUM: grade_name = "platinum"; break;
            case np::trophy::SceNpTrophyGrade::SCE_NP_TROPHY_GRADE_GOLD: grade_name = "gold"; break;
            case np::trophy::SceNpTrophyGrade::SCE_NP_TROPHY_GRADE_SILVER: grade_name = "silver"; break;
            case np::trophy::SceNpTrophyGrade::SCE_NP_TROPHY_GRADE_BRONZE: grade_name = "bronze"; break;
            default: break;
            }
            if (grade_name) {
                const auto icon_path = emuenv->static_assets_path / "icons" / (std::string(grade_name) + ".png");
                std::ifstream file(icon_path.c_str(), std::ios::binary | std::ios::ate);
                if (file) {
                    const auto size = file.tellg();
                    file.seekg(0);
                    grade_icon_buf.resize(static_cast<size_t>(size));
                    file.read(reinterpret_cast<char *>(grade_icon_buf.data()), size);
                }
            }
            LOG_INFO("Vitox trophy banner: '{}' grade {} | trophy icon {} B, grade icon {} B", data.trophy_name, grade_name ? grade_name : "?",
                data.icon_buf.size(), grade_icon_buf.size());
            notif->enqueue(data.trophy_name, data.icon_buf, grade_icon_buf);
        });
    }
    const VitoxExternalPresentStats s0 = vitox_external_present_stats();
    r.renderer = rinit && late && s0.images >= 2 && s0.image_handles_ok == s0.images && s0.gpu_sync;
    snap("m8a_after_renderer");
    out("m8a renderer::init(Vulkan) -> {} | GPU '{}' | late_init + init(mem) -> {} | external images={} shared={} gpu_sync={} -> {}", rinit,
        rinit ? std::string(emuenv->renderer->get_gpu_name()) : std::string("-"), late, s0.images, s0.image_handles_ok, s0.gpu_sync, pf(r.renderer));

    // ---- 5. kernel + app setup (load_app_impl() equivalent, TITLE_ID from param.sfo) ----
    const auto call_import = [emuenv](CPUState &cpu, uint32_t nid, SceUID thread_id) {
        count_import(nid); // 0.0.24.57 diagnostic
        const uint32_t a0 = read_reg(cpu, 0), a1 = read_reg(cpu, 1), a2 = read_reg(cpu, 2), a3 = read_reg(cpu, 3);
        // 0.0.24.64 (M9A.6 DIAGNOSTIC, read-only): the game's own TTY output (printf -> sceIoWrite on tty0/1), which upstream
        // logs only at trace level - at most 4000 lines
        if (const char *wn = import_name(nid); wn && std::string_view(wn) == "sceIoWrite" && emuenv->io.tty_files.contains(static_cast<SceUID>(a0))
            && a2 && a2 < 2048 && Ptr<const char>(a1).valid(emuenv->mem)) {
            static std::atomic<unsigned> tty_lines{ 0 };
            if (tty_lines++ < 4000) {
                std::string text(Ptr<const char>(a1).get(emuenv->mem), a2);
                while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
                    text.pop_back();
                LOG_INFO("Vitox [M9A.6] TTY fd {}: {}", static_cast<int>(a0), text);
            }
        }
        ::call_import(*emuenv, cpu, nid, thread_id);
        // 0.0.24.59 (M9A.6 DIAGNOSTIC, read-only): imports that return a SCE error code, and every file/dir open with its
        // path and result - a game that silently stops loading usually got one of these (at most 40 lines per NID)
        const uint32_t ret = read_reg(cpu, 0);
        const char *nm = import_name(nid);
        const std::string_view name = nm ? nm : "";
        const bool is_open = name.find("Open") != std::string_view::npos && (name.starts_with("sceIo") || name.starts_with("_sceIo") || name.find("Fios") != std::string_view::npos);
        const bool is_error = (ret & 0xF0000000u) == 0x80000000u && ret != a0; // 0.0.24.64: r0 unchanged = a void import
        if ((is_open || is_error) && note_import_result(nid)) {
            std::string path;
            if (is_open && a0 >= 0x80000000u && a0 < 0xF0000000u)
                path = Ptr<const char>(a0).get(emuenv->mem) ? std::string(Ptr<const char>(a0).get(emuenv->mem), 0, 160) : std::string();
            LOG_INFO("Vitox [M9A.6] import {} thread {} ({}, 0x{:X}, 0x{:X}, 0x{:X}) -> 0x{:08X}{}{}", name.empty() ? "?" : nm, thread_id, path.empty() ? fmt::format("0x{:X}", a0) : "'" + path + "'",
                a1, a2, a3, ret, is_error ? " ERROR" : "", "");
        }
    };
    ok = r.renderer && emuenv->kernel.init(emuenv->mem, call_import, emuenv->cfg.current_config.cpu_opt);
    emuenv->io.title_id = title_id;
    emuenv->io.app_path = title_id;
    emuenv->io.savedata = title_id;
    emuenv->io.user_id = "00";
    init_device_paths(emuenv->io);
    init_savedata_app_path(emuenv->io, emuenv->vita_fs_path);
    vfs::FileBuffer param_sfo;
    std::string sfo_title, sfo_id;
    if (vfs::read_app_file(param_sfo, emuenv->vita_fs_path, emuenv->io.app_path, "sce_sys/param.sfo") && sfo::load(emuenv->sfo_handle, param_sfo)) {
        sfo::get_data_by_key(sfo_title, emuenv->sfo_handle, "TITLE");
        sfo::get_data_by_key(sfo_id, emuenv->sfo_handle, "TITLE_ID");
    }
    r.sfo = trim(sfo_id.c_str()) == title_id;
    // 0.0.31.5: the DLC folder (addcont0: = ux0:addcont/<addcont>), as upstream sfo::get_param_info + apps_list
    // select_app: INSTALL_DIR_ADDCONT, else TITLE_ID. Left empty, addcont0: was ux0:addcont/ and no DLC was found.
    {
        std::string addcont;
        if (r.sfo && sfo::get_data_by_key(addcont, emuenv->sfo_handle, "INSTALL_DIR_ADDCONT"))
            addcont = trim(addcont.c_str());
        emuenv->io.addcont = addcont.empty() ? std::string(title_id) : addcont;
        init_device_paths(emuenv->io);
        std::error_code dlc_ec;
        unsigned dlc_count = 0;
        for (auto it = sfs::directory_iterator(s_vita_fs / "ux0" / "addcont" / emuenv->io.addcont, dlc_ec); !dlc_ec && it != sfs::directory_iterator(); it.increment(dlc_ec))
            ++dlc_count;
        out("m8a setup: addcont0: -> ux0:{} ({} DLC folder(s))", emuenv->io.device_paths.addcont0, dlc_count);
    }
    out("m8a setup: KernelState::init -> {} | installed param.sfo via vfs: TITLE='{}' TITLE_ID='{}' -> {}", ok, sfo_title, sfo_id.c_str(), pf(r.sfo));
    cp("m9a4 cp 04 renderer+setup: renderer {} | KernelState::init {} | param.sfo {}", pf(r.renderer), ok, pf(r.sfo));
    // M9A.4R: what app_init does for audio (state.audio.init(audio_backend)), with the UWP XAudio2 backend
    // M9A.5 failure-path check: with no backend, AudioState::open_port must return nullptr (the guest then gets
    // SCE_AUDIO_OUT_ERROR_NOT_OPENED from sceAudioOutOpenPort) instead of dereferencing a null adapter / port
    {
        AudioState no_backend;
        const bool null_ok = no_backend.open_port(2, 48000, 256) == nullptr;
        cp("m9a5 audio failure path: open_port with no backend -> {}", null_ok ? "nullptr PASS" : "FAIL");
    }
    emuenv->cfg.current_config.audio_backend = "XAudio2";
    const bool audio_ok = emuenv->audio.init(emuenv->cfg.current_config.audio_backend);
    cp("m9a4 cp 04a audio: AudioState::init('{}') -> {}", emuenv->cfg.current_config.audio_backend, pf(audio_ok));
    // M9A.7: upstream app_init starts the NGS engine right after audio (voice definitions in guest memory)
    const bool ngs_ok = ngs::init(emuenv->ngs, emuenv->mem);
    cp("m9a7 cp 04b ngs: ngs::init -> {}", pf(ngs_ok));
    init_exported_vars(*emuenv);
    // M8C.3 (approved): staged firmware for the PGF module only. The firmware devices (os0/vs0/sa0/pd0) resolve to the
    // STAGED tree, read-only; ux0 (the app, its data) stays in the active vita-fs; Vita3K's own MANUAL modules mode with
    // lle-modules=[libpgf] (HTTP/NET stay on Vita3K's HLE "marked loaded" path); the REAL decrypt_fself for LLE modules.
    const sfs::path staged_fw = sfs::path(dev_root) / "firmware-staging" / "vita-fs";
    std::map<std::string, uintmax_t> staged_before;
    if (config->staged_pgf) {
        const bool complete = sfs::exists(sfs::path(dev_root) / "firmware-staging" / "stage-complete.txt", ec);
        staged_before = tree(staged_fw);
        vitox_set_firmware_root(complete ? &staged_fw : nullptr);
        vitox_set_fself_real_decrypt(complete);
        emuenv->cfg.current_config.modules_mode = ModulesMode::MANUAL;
        emuenv->cfg.current_config.lle_modules = { "libpgf" };
        out("m8c.3 firmware=staging-pgf: staging complete={} | firmware devices os0/vs0/sa0/pd0 -> '{}' ({} files, read-only use) | modules MANUAL [libpgf] | "
            "REAL decrypt_fself {}",
            complete, u8(staged_fw), staged_before.size(), complete ? "ON" : "off (no staging: refused)");
        ok = ok && complete;
        // M8F.2A: the overlay's firmware fonts (ltn.pvf ...) come from the same staged sa0 (renderer init pointed it at the
        // active vita-fs, which has no sa0)
        if (complete)
            overlay::fontmgr::set_firmware_font_dir(u8(staged_fw / "sa0" / "data" / "font" / "pvf") + "/");
    }
    // M9A.2A: MANUAL lle-modules for the app-bundled SDK runtime (added to libpgf when the staged PGF is on)
    if (config->lle_modules[0]) {
        emuenv->cfg.current_config.modules_mode = ModulesMode::MANUAL;
        const std::string v = config->lle_modules;
        for (size_t b = 0, e; b <= v.size(); b = e + 1) {
            e = v.find(',', b);
            const std::string m = trim(v.substr(b, (e == std::string::npos ? v.size() : e) - b));
            if (std::find(emuenv->cfg.current_config.lle_modules.begin(), emuenv->cfg.current_config.lle_modules.end(), m) == emuenv->cfg.current_config.lle_modules.end())
                emuenv->cfg.current_config.lle_modules.push_back(m);
            if (e == std::string::npos)
                break;
        }
        std::string all;
        for (const auto &m : emuenv->cfg.current_config.lle_modules)
            all += (all.empty() ? "" : ",") + m;
        out("m9a modules: MANUAL lle-modules=[{}] | is_lle_module libc={} libfios2={} libult={} | SCE_SYSMODULE_MP4={}", all, is_lle_module(std::string("libc"), *emuenv),
            is_lle_module(std::string("libfios2"), *emuenv), is_lle_module(std::string("libult"), *emuenv), is_lle_module(SCE_SYSMODULE_MP4, *emuenv));
    }
    out("m8f overlay: overlay_manager={} renderer.common_dialog={} sys_button={} | firmware font dir '{}'", emuenv->overlay_manager != nullptr,
        emuenv->renderer && emuenv->renderer->common_dialog != nullptr, emuenv->renderer ? emuenv->renderer->sys_button : -1,
        overlay::fontmgr::get_firmware_font_dir());
    vitox_init_selected_libraries(*emuenv);
    // M8B.2: what Vita3K's frontend does for input - default binds (app.cpp reset_controller_binding values; the config
    // file is not rewritten) and refresh_controllers on start and on every host add/remove
    if (emuenv->cfg.controller_binds.size() != 15 || emuenv->cfg.controller_axis_binds.size() != 6) {
        emuenv->cfg.controller_binds = { SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_WEST, SDL_GAMEPAD_BUTTON_NORTH,
            SDL_GAMEPAD_BUTTON_BACK, SDL_GAMEPAD_BUTTON_GUIDE, SDL_GAMEPAD_BUTTON_START, SDL_GAMEPAD_BUTTON_LEFT_STICK, SDL_GAMEPAD_BUTTON_RIGHT_STICK,
            SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, SDL_GAMEPAD_BUTTON_DPAD_UP, SDL_GAMEPAD_BUTTON_DPAD_DOWN,
            SDL_GAMEPAD_BUTTON_DPAD_LEFT, SDL_GAMEPAD_BUTTON_DPAD_RIGHT };
        emuenv->cfg.controller_axis_binds = { SDL_GAMEPAD_AXIS_LEFTX, SDL_GAMEPAD_AXIS_LEFTY, SDL_GAMEPAD_AXIS_RIGHTX, SDL_GAMEPAD_AXIS_RIGHTY,
            SDL_GAMEPAD_AXIS_LEFT_TRIGGER, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER };
    }
    if (config->auto_stick[0] && vitox_parse_auto_stick(config->auto_stick, &g_stick_segments)) { // 0.0.24.74 diagnostic
        g_stick_real = vitox_host_input();
        vitox_set_host_input(&k_stick_host);
        out("m9a6 auto_stick: {} segment(s) '{}' over {}", g_stick_segments.size(), config->auto_stick, g_stick_real ? "the host controller" : "no host controller");
    }
    const VitoxHostInput *host_input = vitox_host_input();
    uint32_t input_gen = host_input ? host_input->generation() : 0;
    refresh_controllers(emuenv->ctrl, *emuenv);
    out("m8b.2 input: host adapter {} | Vita3K controllers after refresh_controllers={} | binds {} / axis binds {} (Vita3K defaults)",
        host_input ? "Windows.Gaming.Input" : "NONE", emuenv->ctrl.controllers_num, emuenv->cfg.controller_binds.size(), emuenv->cfg.controller_axis_binds.size());

    // 0.0.29.1: the NoNpDrm license -> emuenv.license.rif[TITLE_ID], the same lines as upstream get_license()
    // (packages/src/license.cpp, called by app.cpp before load; not linked here), and the REAL decrypt_fself so
    // load_module decrypts the NPDRM eboot / sce_module SELFs with emuenv.license.rif[title_id].key
    if (nonpdrm_license) {
        auto &lic = emuenv->license.rif[title_id];
        lic = {};
        const std::string b = read_all(nonpdrm_rif);
        memcpy(&lic, b.data(), std::min(b.size(), sizeof(lic)));
        lic.sku_flag = byte_swap(lic.sku_flag); // (upstream: convert to little endian)
        vitox_set_fself_real_decrypt(true);
        out("vitox nonpdrm: license '{}' ({} B) -> emuenv.license.rif[{}] | REAL decrypt_fself ON for this session", u8(nonpdrm_rif), b.size(), title_id);
    }
    // M9A.3A inertness evidence (every run that reaches load): the licensed path is off, so real decrypt is only what
    // firmware=staging-pgf set, no license is registered and ux0:license is not touched by this launcher
    out("m9a.3 inert: licensed_load={} | real decrypt_fself={} (firmware=staging-pgf {}) | license.rif entries={} | ux0:license entries={}",
        config->licensed_load ? "staged-license" : "off", vitox_fself_real_decrypt(), config->staged_pgf ? "on" : "off", emuenv->license.rif.size(),
        names(s_vita_fs / "ux0" / "license").size());

    // ---- 6. load + import inventory ----
    // 0.0.29.3: a LoadExec relaunch loads the requested executable (upstream interface.cpp: "app0:" + emuenv.self_path)
    emuenv->self_path = config->exec_self[0] ? config->exec_self : "eboot.bin";
    emuenv->self_name = u8(sfs::path(emuenv->self_path).filename());
    if (config->exec_self[0])
        cp("vitox load_exec: relaunch main module app0:{} args '{}'", emuenv->self_path, config->exec_args);
    const SceUID main_module_id = ok && r.sfo ? load_module(*emuenv, "app0:" + emuenv->self_path) : -1;
    snap("m8a_after_load");
    ThreadStatePtr main_thread;
    std::chrono::steady_clock::time_point t_run;
    if (main_module_id >= 0) {
        const auto mod = emuenv->kernel.loaded_modules[main_module_id];
        const auto &info = mod->info;
        r.loaded = info.start_entry.address() != 0;
        out("m8a load_module('app0:eboot.bin') uid={} module='{}' start_entry=0x{:08X} -> {}", main_module_id, info.module_name, info.start_entry.address(),
            pf(r.loaded));
        cp("m9a4 cp 05 eboot load: uid={} module='{}' start_entry=0x{:08X} -> {}", main_module_id, info.module_name, info.start_entry.address(), pf(r.loaded));
        // M9A.2A: upstream interface.cpp load_app_impl() preload of the app-bundled SDK runtime, in upstream's order (libc,
        // then libfios2), only what is_lle_module() selects (MANUAL lle_modules) and only from app0:sce_module. It runs
        // after the main module: loading it registered the process param, incl. __sce_libcparam (kernel.cpp), which libc's
        // variable import resolves against. The kernel-module preloads (os0:kd/bootimage, sysmodule) are not done here.
        if (config->lle_modules[0]) {
            SceUInt32 preload_disabled = 0;
            if (const auto pp = emuenv->kernel.process_param.get(emuenv->mem))
                if (const auto pd = Ptr<SceUInt32>(pp->process_preload_disabled))
                    preload_disabled = *pd.get(emuenv->mem);
            struct Preload {
                uint32_t code;
                SceSysmoduleModuleId id;
                const char *name;
                bool load_from_app; // upstream add_preload_module: app0:sce_module first, else vs0:sys/external
            };
            // M9A.6: upstream's libSceFt2 (0x00800000) and libpvf (0x01000000) preloads, from vs0 (the staged firmware)
            for (const Preload &p : { Preload{ 0x00010000, SCE_SYSMODULE_INVALID, "libc", true }, Preload{ 0x00200000, SCE_SYSMODULE_FIOS2, "libfios2", true },
                     Preload{ 0x00800000, SCE_SYSMODULE_INVALID, "libSceFt2", false }, Preload{ 0x01000000, SCE_SYSMODULE_INVALID, "libpvf", false } }) {
                if (preload_disabled & p.code) {
                    out("m9a preload {}: disabled by the process param (0x{:08X})", p.name, preload_disabled);
                    continue;
                }
                const std::string file = fmt::format("{}.suprx", p.name);
                const bool lle = is_lle_module(std::string(p.name), *emuenv);
                if (!p.load_from_app && !lle)
                    continue; // M9A.6: vs0 preloads only when selected (no log noise for every other title)
                const bool present = p.load_from_app ? sfs::exists(s_app / "sce_module" / file, ec) : true; // vs0: load_module reports absence
                SceUID uid = -1;
                if (lle && present)
                    uid = load_module(*emuenv, (p.load_from_app ? "app0:sce_module/" : "vs0:sys/external/") + file);
                if (p.id != SCE_SYSMODULE_INVALID)
                    emuenv->kernel.loaded_sysmodules[p.id] = {};
                const auto m = uid >= 0 ? emuenv->kernel.loaded_modules[uid] : nullptr;
                out("m9a preload {}: lle={} {}{} present={} -> load_module uid={}{}{}", p.name, lle, p.load_from_app ? "app0:sce_module/" : "vs0:sys/external/", file, present, uid,
                    m ? fmt::format(" module='{}'", m->info.module_name) : std::string(), lle && present && uid < 0 ? " | FAILED (upstream logs and ignores)" : "");
                cp("m9a4 cp {} {} preload: lle={} present={} -> uid={}{}", !p.load_from_app ? "07b" : p.id == SCE_SYSMODULE_INVALID ? "06" : "07", p.name, lle, present, uid,
                    m ? fmt::format(" module='{}'", m->info.module_name) : std::string());
            }
        }
        // 0.0.24.3 (M9A.4, read-only): variable-import binding status after the preloads. load_var_imports binds a NID with
        // no export to a 4-byte stub holding 0xDEADBEEF (and reuses that stub for later imports of the same NID).
        {
            const std::lock_guard<std::mutex> guard(emuenv->kernel.export_nids_mutex);
            unsigned vars = 0, stubbed = 0;
            for (const auto &[nid, vb] : emuenv->kernel.var_binding_infos) {
                ++vars;
                const auto it = emuenv->kernel.export_nids.find(nid);
                const Address a = it != emuenv->kernel.export_nids.end() ? it->second : 0;
                const uint32_t v = a ? *Ptr<uint32_t>(a).get(emuenv->mem) : 0;
                const bool stub = !a || v == 0xDEADBEEF;
                stubbed += stub;
                const char *nm = import_name(nid);
                out("m9a4 var import: nid=0x{:08X} {} module_nid=0x{:08X} -> export 0x{:08X} first word 0x{:08X} -> {}", nid, nm ? nm : "?", vb.module_nid, a, v,
                    stub ? "STUB (unresolved: 0xDEADBEEF stub value)" : "BOUND");
            }
            cp("m9a4 cp 08 var bindings: {} var import(s), bound={} stub={}", vars, vars - stubbed, stubbed);
        }
        std::map<uint32_t, uint32_t> bound;
        for (const auto &[nid, b] : emuenv->kernel.func_binding_infos)
            bound[nid] = b.library_nid;
        r.bound_imports = static_cast<unsigned>(bound.size());
        unsigned in_vita3k = 0, stubs = 0, absent = 0;
        for (const auto &[nid, lib] : bound) {
            const VitoxSelectedImport *imp = vitox_resolve_selected_import(nid);
            const char *name = import_name(nid);
            if (imp) {
                out("m8a import: library_nid=0x{:08X} nid=0x{:08X} {} -> selected, REAL ImportFn={}", lib, nid, name, reinterpret_cast<const void *>(*imp->fn));
                continue;
            }
            ++r.unsupported_imports;
            std::string where;
            bool stub = false;
            for (const Vita3kExport &e : kVita3kExports)
                if (name && strcmp(e.name, name) == 0) {
                    where += fmt::format("{}{}{}", where.empty() ? "" : ", ", e.module, e.stub ? " (likely UNIMPLEMENTED stub)" : "");
                    stub |= e.stub;
                }
            if (where.empty())
                ++absent;
            else if (stub)
                ++stubs;
            else
                ++in_vita3k;
            out("m8a import: library_nid=0x{:08X} nid=0x{:08X} {} -> NOT SELECTED | Vita3K: {}", lib, nid, name ? name : "(no name in nids.inc)",
                where.empty() ? std::string("no export of this name") : "exported in " + where);
        }
        r.imports_supported = r.loaded && r.unsupported_imports == 0;
        out("m8a imports: bound={} selected={} not selected={} (of those: implemented in Vita3K={} likely Vita3K stub={} absent={}) -> {}{}", r.bound_imports,
            r.bound_imports - r.unsupported_imports, r.unsupported_imports, in_vita3k, stubs, absent, pf(r.imports_supported),
            r.imports_supported ? ""
            : config->trap        ? " | M8B.1 TRAP MODE (diagnostic): the guest runs; the first CALL to an unselected NID stops it, nothing is returned"
            : config->fail_closed ? " | M8E RUNTIME FAIL-CLOSED: the guest runs; a CALL to an unselected NID stops it, nothing is returned, the run FAILS"
                                  : " | STOP: the guest is not started (no fake success for unselected imports)");
        const bool runtime_gate = config->trap || config->fail_closed;
        vitox_import_trap_enable(runtime_gate);
        cp("m9a4 cp 09 import inventory + gate: bound={} selected={} not selected={} (Vita3K-implemented={} stub={} absent={}) | runtime gate {} ({})",
            r.bound_imports, r.bound_imports - r.unsupported_imports, r.unsupported_imports, in_vita3k, stubs, absent, runtime_gate ? "ON" : "OFF",
            config->trap ? "trap" : config->fail_closed ? "fail-closed" : "none");
        if (r.loaded && (r.imports_supported || runtime_gate)) {
            // M9A.2A: upstream run_app(): module_start of every loaded non-main module before the main thread, under the
            // fail-closed gate (a call to an unselected NID from a module_start traps like any other)
            bool module_trap = false;
            for (const auto &[uid, m] : emuenv->kernel.loaded_modules) {
                if (uid == main_module_id || module_trap)
                    continue;
                cp("m9a4 cp 10 module_start begin '{}' (uid {}) start_entry=0x{:08X}", m->info.module_name, uid, m->info.start_entry.address());
                const uint32_t sr = start_module(*emuenv, m->info);
                VitoxTrapRecord t;
                module_trap = vitox_import_traps(&t, 1) != 0;
                out("m9a module_start '{}' (uid {}) -> 0x{:08X}{}", m->info.module_name, uid, sr, module_trap ? " | TRAPPED: no further module_start, no main thread" : "");
                cp("m9a4 cp 11 module_start end '{}' (uid {}) -> 0x{:08X} trapped={}", m->info.module_name, uid, sr, module_trap);
            }
            if (!module_trap) {
                emuenv->renderer->set_app(emuenv->io.title_id.c_str(), "eboot.bin");
                // 0.0.24.173 (stutter, option 1; 0.0.24.174: moved here, shaders_path is only set by set_app): as upstream app.cpp does at game start, read the shader cache list and the saved pipeline
                // cache (never done here before, so the saved pipeline cache data was never used). In this build read_pipeline_cache()
                // also pre-warms every recorded pipeline + D3D12 variant (vitox_pipeline_prewarm.h) before the game's first draw.
                {
                    // 0.0.24.198: pre-warm loading screen title = the SFO TITLE's printable ASCII (TM etc. dropped), else the title ID
                    std::string ascii;
                    for (const char ch : std::string(r.title))
                        if (static_cast<unsigned char>(ch) >= 32 && static_cast<unsigned char>(ch) < 127)
                            ascii += ch;
                    while (!ascii.empty() && ascii.back() == ' ')
                        ascii.pop_back();
                    vitox_prewarm_set_enabled(vitox_launcher_pipeline_prewarm()); // 0.0.28.3: Settings > Shader Compilation
                    if (vitox_launcher_pipeline_prewarm()) // off: no loading screen; the saved Vulkan pipeline cache is still read
                        vitox_loading_configure(ascii.size() >= 3 ? ascii.c_str() : emuenv->io.title_id.c_str(), pump);
                    const auto t0 = std::chrono::steady_clock::now();
                    const bool have = emuenv->cfg.shader_cache && renderer::get_shaders_cache_hashs(*emuenv->renderer);
                    out("m9a11 pipeline pre-warm: shader cache list {} ({} entries), {} ms (details: 'Vitox pre-warm' lines)", have ? "read" : (emuenv->cfg.shader_cache ? "absent or empty" : "disabled by config"),
                        emuenv->renderer->shaders_cache_hashs.size(), std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
                }
            }
            main_thread = module_trap ? ThreadStatePtr{} : emuenv->kernel.create_thread(emuenv->mem, emuenv->io.title_id.c_str(), info.start_entry, SCE_KERNEL_DEFAULT_PRIORITY_USER,
                SCE_KERNEL_CPU_MASK_USER_ALL, SCE_KERNEL_STACK_SIZE_USER_MAIN, nullptr);
            cp("m9a4 cp 12 before main thread start: module_trap={} main thread created={}{}", module_trap, main_thread != nullptr,
                main_thread ? fmt::format(" id={} entry=0x{:08X}", main_thread->id, info.start_entry.address()) : std::string());
            if (main_thread) {
                emuenv->main_thread_id = main_thread->id;
                t_run = std::chrono::steady_clock::now();
                g_stick_t0_us = std::chrono::duration_cast<std::chrono::microseconds>(t_run.time_since_epoch()).count(); // 0.0.24.74
                // 0.0.29.3: the LoadExec argv as the main thread's argument block (upstream load_app_impl: NUL-separated)
                SceSize arg_size = 0;
                Address arg_addr = 0;
                if (config->exec_args[0]) {
                    std::vector<uint8_t> buf;
                    const std::string a = config->exec_args;
                    for (size_t b = 0, e; b <= a.size(); b = e + 1) {
                        e = a.find('\n', b);
                        const std::string one = a.substr(b, (e == std::string::npos ? a.size() : e) - b);
                        buf.insert(buf.end(), one.begin(), one.end());
                        buf.push_back(0);
                        if (e == std::string::npos)
                            break;
                    }
                    arg_addr = alloc(emuenv->mem, static_cast<uint32_t>(buf.size()), "arg");
                    memcpy(Ptr<uint8_t>(arg_addr).get(emuenv->mem), buf.data(), buf.size());
                    arg_size = static_cast<SceSize>(buf.size());
                }
                const int sr = main_thread->start(arg_size, Ptr<void>(arg_addr), true);
                cp("m9a4 cp 13 main thread start returned: start={}", sr);
                start_sync_thread(*emuenv);
                renderer::start_render_thread(*emuenv->renderer, emuenv->display, emuenv->gxm, emuenv->mem, emuenv->cfg);
                r.thread = sr >= 0;
                out("m8a run: {} main thread id={} start={} | vblank + render threads started -> {}", title_id, main_thread->id, sr, pf(r.thread));
            }
        }
    } else {
        out("m8a load_module('app0:eboot.bin') FAILED: {}", main_module_id);
    }

    // ---- 7. guest runs until it exits or timeout_s (the UI thread pumps CoreWindow events) ----
    uint64_t first_present_n = 0, last_present_n = 0;
    std::chrono::steady_clock::time_point t_first_present, t_last_present;
    volatile long *pso = dzn_pso_failures();
    // M8F.2A (DIAGNOSTIC only): scripted IME dialog input through Vita3K's frontend-neutral helpers. Nothing writes the
    // dialog result, status or guest buffer here: ime_commit_text / submit_current_ime / dismiss_current_ime do.
    std::vector<std::string> ime_script;
    if (config->ime_input[0]) {
        std::ifstream f(cfg_root / config->ime_input);
        for (std::string line; f && std::getline(f, line);) {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (!line.empty() && line[0] != '#')
                ime_script.push_back(line);
        }
        out("m8f ime_input '{}' (DIAGNOSTIC): {} scripted dialog session(s)", config->ime_input, ime_script.size());
    }
    size_t ime_line = 0;
    unsigned ime_sessions = 0, ime_actions_applied = 0;
    bool ime_running = false;
    // M8F.2B: host text (system keyboard) for an unscripted session
    const VitoxHostText *host_text = vitox_host_text();
    bool host_text_active = false;
    uint32_t host_text_gen = 0;
    std::u16string host_text_last;
    const auto host_text_end = [&](const char *why) {
        if (!host_text_active)
            return;
        host_text->hide();
        host_text_active = false;
        out("m8f.2b host text: hide ({}) | keyboard visible now {} | host pads {}", why, host_text->keyboard_visible(), host_pad_count());
    };
    std::deque<std::string> ime_actions;
    std::chrono::steady_clock::time_point ime_next_action;
    const auto ime_tick = [&](std::chrono::steady_clock::time_point now) {
        const DialogState &dlg = emuenv->common_dialog;
        const bool running = is_ime_dialog_active(*emuenv);
        if (running && !ime_running) {
            // upstream sceImeDialogInit sets status RUNNING before it copies the initial text into the shared Ime state,
            // all under common_dialog.mutex: wait for it to return before reading the seed (0.0.23.2 seeded a stale '')
            { std::lock_guard<std::recursive_mutex> init_done(emuenv->common_dialog.mutex); }
            ++ime_sessions;
            const std::string initial = ime_text_utf8(emuenv->ime);
            out("m8f ime dialog #{} RUNNING: title '{}' | shared ime text '{}' | max_length {} cancelable {} multiline {}", ime_sessions, dlg.ime.title,
                initial, dlg.ime.max_length, dlg.ime.cancelable, dlg.ime.multiline);
            ime_actions.clear();
            if (ime_line < ime_script.size()) {
                const std::string &l = ime_script[ime_line++];
                for (size_t b = 0, e; b <= l.size(); b = e + 1) {
                    e = l.find('|', b);
                    if (e == std::string::npos)
                        e = l.size();
                    ime_actions.push_back(l.substr(b, e - b));
                }
                ime_next_action = now + std::chrono::milliseconds(300);
            } else if (host_text) {
                std::u16string seed;
                {
                    std::lock_guard<std::mutex> lock(emuenv->ime.mutex);
                    seed = emuenv->ime.str;
                }
                const unsigned pads = host_pad_count();
                const bool shown = host_text->show(seed.data(), static_cast<uint32_t>(seed.size()), dlg.ime.max_length);
                host_text_active = true;
                host_text_gen = host_text->generation();
                host_text_last = seed;
                out("m8f.2b host text: show (seed '{}', max {}) -> keyboard show request {} | keyboard visible {} | host pads {}", initial,
                    dlg.ime.max_length, shown, host_text->keyboard_visible(), pads);
            } else
                out("m8f ime dialog #{}: no scripted input and no host text source (waits for the overlay)", ime_sessions);
        } else if (!running && ime_running) {
            out("m8f ime dialog #{} left RUNNING: status {} result {} button {} | dialog text '{}'", ime_sessions, static_cast<int>(dlg.status), static_cast<int>(dlg.result), dlg.ime.status,
                dlg.ime.text);
            ime_actions.clear();
            host_text_end("dialog finished");
        }
        ime_running = running;
        if (running && host_text_active && host_text->generation() != host_text_gen) {
            host_text_gen = host_text->generation();
            char16_t buf[SCE_IME_DIALOG_MAX_TEXT_LENGTH];
            const uint32_t n = host_text->text(buf, SCE_IME_DIALOG_MAX_TEXT_LENGTH);
            const std::u16string target(buf, std::min<uint32_t>(n, SCE_IME_DIALOG_MAX_TEXT_LENGTH));
            if (target != host_text_last) {
                host_text_last = target;
                ime_sync_text(emuenv->ime, target);
                out("m8f.2b host text update: '{}' -> shared ime text '{}'", string_utils::utf16_to_utf8(target), ime_text_utf8(emuenv->ime));
            }
        }
        // M8F.2C: Enter on the system keyboard = the SDL frontend's Return: submit_current_ime (the text is synced above)
        if (running && host_text_active && host_text->take_enter()) {
            const bool submitted = submit_current_ime(*emuenv);
            out("m8f.2c host text: Enter on the keyboard -> submit_current_ime -> {} (status {} result {} button {}) | dialog text '{}'", submitted,
                static_cast<int>(dlg.status), static_cast<int>(dlg.result), dlg.ime.status, dlg.ime.text);
        }
        if (!running || ime_actions.empty() || now < ime_next_action)
            return;
        const std::string a = ime_actions.front();
        ime_actions.pop_front();
        ime_next_action = now + std::chrono::milliseconds(300);
        ++ime_actions_applied;
        if (a.rfind("enter:", 0) == 0) {
            const std::u16string text = string_utils::utf8_to_utf16(a.substr(6));
            {
                std::lock_guard<std::mutex> lock(emuenv->ime.mutex);
                ime_commit_text(emuenv->ime, text);
            }
            const std::string typed = ime_text_utf8(emuenv->ime);
            const bool submitted = submit_current_ime(*emuenv);
            out("m8f ime action '{}': ime_commit_text -> shared ime text '{}' | submit_current_ime -> {} (status {} result {} button {})", a, typed,
                submitted, static_cast<int>(dlg.status), static_cast<int>(dlg.result), dlg.ime.status);
        } else if (a == "cancel") {
            const bool dismissed = dismiss_current_ime(*emuenv);
            out("m8f ime action 'cancel': dismiss_current_ime -> {} (status {} result {} button {})", dismissed, static_cast<int>(dlg.status), static_cast<int>(dlg.result), dlg.ime.status);
        } else
            out("m8f ime action '{}': unknown action, ignored", a);
        // the action finished the dialog: this session ends here. The guest may Term + Init the next one before the next
        // tick, so the next RUNNING seen is a new session even without a non-RUNNING tick in between.
        if (dlg.status != SCE_COMMON_DIALOG_STATUS_RUNNING) {
            out("m8f ime dialog #{} finished by the scripted action: status {} result {} button {} | dialog text '{}'", ime_sessions,
                static_cast<int>(dlg.status), static_cast<int>(dlg.result), dlg.ime.status, dlg.ime.text);
            ime_actions.clear();
            ime_running = false;
        }
    };
    if (host_text)
        out("m8f.2b host text source: registered (CoreTextEditContext + system keyboard) | host pads {}", host_pad_count());
    if (r.thread) {
        auto next_status = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        bool cursor_was_active = false, cursor_was_pressed = false; // 0.0.24.54 touch cursor
        unsigned thread_dump_tick = 0; // 0.0.24.56 thread dump
        bool auto_touch_down = false; // 0.0.24.60 auto_touch
        std::vector<TouchSegment> touch_segments;
        if (config->auto_touch[0])
            vitox_parse_auto_touch(config->auto_touch, &touch_segments);
        unsigned auto_touch_taps = 0;
        VitoxTrapRecord trap_probe;
        // 0.0.25.0: the Vitox quick menu (R3): the guest is paused while it is open (upstream's pause: kernel threads +
        // audio), the presenter repaints the last frame under it, and Return to Launcher stops the session as a host stop
        bool overlay_paused = false;
        const auto overlay_pause = [&](bool on) {
            if (on == overlay_paused)
                return;
            overlay_paused = on;
            if (on)
                emuenv->kernel.pause_threads();
            else
                emuenv->kernel.resume_threads();
            if (emuenv->audio.adapter)
                emuenv->audio.switch_state(on);
            out("vitox overlay: guest {} after {:.1f} s", on ? "PAUSED (quick menu open)" : "RESUMED",
                std::chrono::duration<double>(std::chrono::steady_clock::now() - t_run).count());
        };
        while (!exited && vitox_import_traps(&trap_probe, 1) == 0
            && (config->no_timeout || std::chrono::steady_clock::now() - t_run < std::chrono::seconds(config->timeout_s))) {
            pump();
            if (host_input && host_input->overlay) {
                VitoxOverlayTick ov{};
                host_input->overlay(&ov);
                if (ov.quit) {
                    r.user_quit = true;
                    cp("vitox overlay: Return to Launcher after {:.1f} s: the host stops the guest",
                        std::chrono::duration<double>(std::chrono::steady_clock::now() - t_run).count());
                    break;
                }
                overlay_pause(ov.open);
                if (ov.visible)
                    presenter->present_overlay();
            }
            ime_tick(std::chrono::steady_clock::now());
            // 0.0.24.54 (M9A.6): the virtual touch cursor drives Vita3K's own mouse-as-touch input (touch_vsync_update,
            // unchanged): front panel at the cursor while A is held. Only written while the cursor is on.
            if (host_input && host_input->touch_cursor) {
                VitoxTouchCursor tc{};
                host_input->touch_cursor(&tc);
                auto &ts = emuenv->touch;
                if (tc.active) {
                    const auto &d = emuenv->display;
                    ts.renderer_focused = true;
                    ts.mouse_x = d.viewport_x + tc.x * d.viewport_w;
                    ts.mouse_y = d.viewport_y + tc.y * d.viewport_h;
                    ts.mouse_button_left = tc.pressed;
                } else if (cursor_was_active) {
                    ts.mouse_button_left = false;
                }
                if (tc.active != cursor_was_active || tc.pressed != cursor_was_pressed)
                    out("m9a6 touch cursor: {} {} at vita ({:.0f}, {:.0f}) | viewport {}x{} at ({},{}) | touch_mode front {}", tc.active ? "on" : "off",
                        tc.pressed ? "DOWN" : "up", tc.x * 960, tc.y * 544, emuenv->display.viewport_w, emuenv->display.viewport_h, emuenv->display.viewport_x,
                        emuenv->display.viewport_y, static_cast<int>(ts.touch_mode[SCE_TOUCH_PORT_FRONT]));
                cursor_was_active = tc.active;
                cursor_was_pressed = tc.pressed;
            }
            // 0.0.24.60/.76 (M9A.6 DIAGNOSTIC): scripted taps (auto_touch segments), only while the L3 cursor is off
            if (!touch_segments.empty() && !cursor_was_active) {
                const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - t_run).count();
                auto &ts = emuenv->touch;
                const auto &d = emuenv->display;
                bool down = false;
                for (const TouchSegment &g : touch_segments) {
                    if (t < g.from_s || t >= g.to_s + 1)
                        continue;
                    down = t < g.to_s && std::fmod(t - g.from_s, static_cast<double>(g.period_s)) < std::min(0.15, 0.45 * g.period_s);
                    ts.renderer_focused = true;
                    ts.mouse_x = d.viewport_x + g.x / 960.0f * d.viewport_w;
                    ts.mouse_y = d.viewport_y + g.y / 544.0f * d.viewport_h;
                    ts.mouse_button_left = down;
                    if (down != auto_touch_down && down && auto_touch_taps++ < 12)
                        out("m9a6 auto_touch: tap #{} at vita ({:.0f}, {:.0f}) t={:.1f}s | touch_mode front {}", auto_touch_taps, g.x, g.y, t,
                            static_cast<int>(ts.touch_mode[SCE_TOUCH_PORT_FRONT]));
                    break;
                }
                auto_touch_down = down;
            }
            if (host_input && host_input->generation() != input_gen) {
                input_gen = host_input->generation();
                refresh_controllers(emuenv->ctrl, *emuenv);
                out("m8b.2 input: host pad added/removed -> refresh_controllers: Vita3K controllers={}", emuenv->ctrl.controllers_num);
            }
            const auto now = std::chrono::steady_clock::now();
            const uint64_t p = vitox_external_present_stats().presents_ok;
            if (p && !first_present_n)
                first_present_n = p, t_first_present = now;
            if (p != last_present_n)
                last_present_n = p, t_last_present = now;
            if (now >= next_status) {
                next_status = now + std::chrono::seconds(5);
                const auto x = vitox_external_present_stats();
                out("m8a status t={:.0f}s: presents={} draws={} pipelines={} | dzn PSO failures={} | vblank_count={}", std::chrono::duration<double>(now - t_run).count(),
                    x.presents_ok, x.cmd_draw_indexed, x.pipelines_created, pso ? *pso : -1, emuenv->display.vblank_count.load());
                cp("m9a4 cp status t={:.0f}s presents={} draws={}", std::chrono::duration<double>(now - t_run).count(), x.presents_ok, x.cmd_draw_indexed);
                // M9A.5 soak counters: guest threads, host handle count, audio buffers / underruns / host errors
                size_t guest_threads = 0;
                {
                    std::lock_guard<std::mutex> lock(emuenv->kernel.mutex);
                    guest_threads = emuenv->kernel.threads.size();
                }
                DWORD handles = 0;
                GetProcessHandleCount(GetCurrentProcess(), &handles);
                const VitoxXAudio2Stats a = vitox_xaudio2_stats();
                PROCESS_MEMORY_COUNTERS_EX pmc{};
                pmc.cb = sizeof(pmc);
                const bool pm = K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&pmc), sizeof(pmc));
                out("m9a5 soak t={:.0f}s: guest_threads={} handles={} audio_ports={} buffers={} underruns={} audio_errors={} | private_mib={:.1f} ws_mib={:.1f}",
                    std::chrono::duration<double>(now - t_run).count(), guest_threads, handles, a.ports_opened, a.buffers_submitted, a.underruns, a.errors,
                    pm ? pmc.PrivateUsage / 1048576.0 : -1.0, pm ? pmc.WorkingSetSize / 1048576.0 : -1.0);
                if (thread_dump_tick % 6 == 5)
                    out("vitox gpu mem (in session): {}", gpu_mem());
                // 0.0.24.56 (M9A.6 DIAGNOSTIC, read-only): every 30 s, each guest thread's state, wait target and PC/LR
                // (module + offset) - finds what a stalled game waits on (Gravity Rush loading screen)
                if (++thread_dump_tick % 6 == 0) {
                    std::vector<ThreadStatePtr> ts;
                    {
                        std::lock_guard<std::mutex> lock(emuenv->kernel.mutex);
                        for (const auto &[_, t] : emuenv->kernel.threads)
                            ts.push_back(t);
                    }
                    const auto where = [&](Address a) {
                        const SceKernelModuleInfo *m = emuenv->kernel.find_module_by_addr(a);
                        return m ? fmt::format("{}+0x{:X}", m->module_name, a - m->segments[0].vaddr.address()) : fmt::format("0x{:08X}", a);
                    };
                    for (const auto &t : ts) {
                        const auto st = static_cast<SceUInt32>(t->status);
                        const Address pc = t->cpu ? read_pc(*t->cpu) : 0, lr = t->cpu ? read_lr(*t->cpu) : 0;
                        // 0.0.24.58: the waited object's live state - a semaphore with val >= the wanted count, or an event flag
                        // whose pattern already matches, while a thread still sleeps on it, is a lost wake-up
                        std::string obj;
                        if (t->wait_target.type == SCE_KERNEL_WAITTYPE_SEMAPHORE) {
                            if (const auto o = emuenv->kernel.objects.find<Semaphore>(t->wait_target.id))
                                obj = fmt::format(" | sema '{}' val {} max {} waiters {}", o->name, o->val, o->max, o->waiters.size());
                        } else if (t->wait_target.type == SCE_KERNEL_WAITTYPE_EVENTFLAG) {
                            if (const auto o = emuenv->kernel.objects.find<EventFlag>(t->wait_target.id))
                                obj = fmt::format(" | eventflag '{}' flags 0x{:X} waiters {}", o->name, o->flags, o->waiters.size());
                        } else if (t->wait_target.id) {
                            if (const auto o = emuenv->kernel.objects.find(t->wait_target.id))
                                if (const auto *sp = dynamic_cast<const SyncPrimitive *>(o.get()))
                                    obj = fmt::format(" | object '{}'", sp->name);
                        }
                        out("m9a6 thread {} '{}' status {} wait type 0x{:X} id {} | pc {} lr {} | r0 0x{:X} r1 0x{:X} r2 0x{:X}{}", t->id, t->name, st,
                            t->wait_target.type, t->wait_target.id, where(pc), where(lr), t->cpu ? read_reg(*t->cpu, 0) : 0, t->cpu ? read_reg(*t->cpu, 1) : 0,
                            t->cpu ? read_reg(*t->cpu, 2) : 0, obj);
                        // 0.0.24.61: likely return addresses on the guest stack (odd = Thumb, inside a loaded module), nearest first
                        if (t->cpu && static_cast<SceUInt32>(t->status) == SCE_KERNEL_THREAD_STATUS_WAITING) {
                            const Address sp = read_sp(*t->cpu);
                            std::string chain;
                            int found = 0;
                            const Address top = t->stack_top();
                            std::vector<Address> stack_ras;
                            for (uint32_t i = 0; i < 1024 && found < 14 && sp + i * 4 + 4 <= top; ++i) {
                                if (!Ptr<uint32_t>(sp + i * 4).valid(emuenv->mem))
                                    break;
                                const uint32_t v = *Ptr<uint32_t>(sp + i * 4).get(emuenv->mem);
                                if (!(v & 1) || v < 0x80000000u || v >= 0xE0000000u)
                                    continue;
                                const SceKernelModuleInfo *m = emuenv->kernel.find_module_by_addr(v);
                                if (!m || !m->segments[0].size || v >= m->segments[0].vaddr.address() + m->segments[0].memsz)
                                    continue; // code segment only
                                chain += fmt::format(" {}+0x{:X}@sp+0x{:X}", m->module_name, v - m->segments[0].vaddr.address(), i * 4);
                                stack_ras.push_back(v);
                                ++found;
                            }
                            out("m9a6 stack {} '{}':{}", t->id, t->name, chain);
                            // 0.0.24.61: once (the 4th dump, ~120 s), the main thread's callers disassembled (upstream's capstone)
                            // 0.0.24.63: a local DisasmState (the CPU's own one is not set up in this build: 0.0.24.61 crashed)
                            static DisasmState disasm{};
                            static const bool disasm_ok = init(disasm);
                            if (thread_dump_tick == 24 && t->name == emuenv->io.title_id && disasm_ok) {
                                for (const Address ra : stack_ras) {
                                    const Address at0 = (ra & ~1u) - 40;
                                    std::string lines;
                                    for (Address at = at0; at < (ra & ~1u) + 8;) {
                                        uint16_t sz = 0;
                                        const std::string ins = Ptr<uint32_t>(at).valid(emuenv->mem)
                                            ? disassemble(disasm, Ptr<const uint8_t>(at).get(emuenv->mem), 4, at, true, &sz)
                                            : std::string("(unmapped)");
                                        lines += fmt::format("\n    {}{:08X} {}", at == (ra & ~1u) ? "=>" : "  ", at, ins);
                                        at += sz ? sz : 2;
                                    }
                                    out("m9a6 disasm before return address {} ({}):{}", where(ra), log_hex(ra), lines);
                                }
                            }
                        }
                    }
                    // 0.0.24.57: the imports called in the last 30 s, most called first
                    std::vector<std::pair<uint64_t, uint32_t>> calls;
                    for (size_t i = 0; i < k_import_slots; ++i) {
                        const uint32_t nid = g_import_counts[i].nid.load(std::memory_order_relaxed);
                        const uint64_t c = g_import_counts[i].calls.load(std::memory_order_relaxed);
                        if (nid && c > g_import_seen[i])
                            calls.emplace_back(c - g_import_seen[i], nid);
                        g_import_seen[i] = c;
                    }
                    std::sort(calls.begin(), calls.end(), std::greater<>());
                    std::string top;
                    for (size_t i = 0; i < calls.size() && i < 80; ++i) // 0.1.1.2: 80 (was 40)
                        top += fmt::format("{}{} {}", i ? ", " : "", import_name(calls[i].second), calls[i].first);
                    out("m9a6 imports (30 s): {} distinct | {}", calls.size(), top);
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        overlay_pause(false); // the shutdown below expects running (unpaused) guest threads
    }
    r.run_seconds = r.thread ? std::chrono::duration<double>(std::chrono::steady_clock::now() - t_run).count() : 0;
    std::vector<VitoxTrapRecord> traps(64);
    traps.resize(vitox_import_traps(traps.data(), static_cast<unsigned>(traps.size())));
    r.traps = static_cast<unsigned>(traps.size());
    r.trapped = !traps.empty();
    r.exited = exited;
    r.exit_ok = exited && exit_code == 0;
    r.timed_out = r.thread && !exited && !r.trapped;
    {
        std::lock_guard<std::mutex> lock(exec_m);
        if (exec_request && !r.trapped) {
            r.load_exec = true;
            strncpy_s(r.exec_self, exec_request->self_path.c_str(), _TRUNCATE);
            std::string a;
            for (const std::string &s : exec_request->argv)
                a += (a.empty() ? "" : "\n") + s;
            strncpy_s(r.exec_args, a.c_str(), _TRUNCATE);
            cp("vitox load_exec: the guest requested app0:{} ({} arg(s)) -> the host relaunches the app with it", exec_request->self_path, exec_request->argv.size());
        }
    }
    out("m8a run end: guest exited={} exit_code=0x{:X} after {:.1f} s{}{}", exited.load(), static_cast<uint32_t>(exit_code.load()), r.run_seconds,
        r.user_quit ? std::string(" | RETURN TO LAUNCHER (quick menu): the host stops the guest")
        : r.timed_out ? fmt::format(" | TIMEOUT ({} s): the host stops the guest", config->timeout_s) : std::string(),
        r.trapped ? fmt::format(" | M8B.1 TRAPPED ({} trap call(s)): FAIL / NEEDS_CLOSURE", r.traps) : std::string());
    cp("m9a4 cp 14 run end: thread={} exited={} exit_code=0x{:X} timed_out={} traps={} after {:.1f} s", r.thread, exited.load(),
        static_cast<uint32_t>(exit_code.load()), r.timed_out, r.traps, r.run_seconds);
    for (const VitoxTrapRecord &t : traps) {
        const char *nm = import_name(t.nid);
        std::string where;
        for (const Vita3kExport &e : kVita3kExports)
            if (nm && strcmp(e.name, nm) == 0)
                where += fmt::format("{}{}{}", where.empty() ? "" : ", ", e.module, e.stub ? " (likely UNIMPLEMENTED stub)" : "");
        out("m8b.1 trap: nid=0x{:08X} {} library_nid=0x{:08X} | thread {} '{}' pc=0x{:08X} lr=0x{:08X} | Vita3K: {}", t.nid, nm ? nm : "?", t.library_nid,
            t.thread_id, t.thread_name, t.pc, t.lr, where.empty() ? std::string("no export of this name") : "exported in " + where);
    }
    vitox_import_trap_enable(false);

    // ---- 8. Vita3K shutdown order: renderer preclose, render thread, gxm, display ----
    if (r.thread) {
        if (r.timed_out || r.trapped) {
            // the guest is still running: stop its threads FIRST, while the render, display-host and vblank threads still run -
            // a thread parked in sceDisplayWaitVblankStart only wakes on the next vblank (0.0.19.9), and one inside
            // sceGxmEndScene blocks on the full command queue once stop_render_thread has reset it (0.0.20.2)
            // 0.0.27.2: the GXM display host thread runs the game's display callback ON a guest thread (SceGxmDisplayQueue;
            // run_guest_function waits for that thread to go dormant). Deleting the guest threads while a callback was in
            // flight left the host thread waiting forever, and gxm.deinit's join then hung Return to Launcher (seen with
            // Danganronpa). So, as upstream gxm shutdown does first: no new display entries, let the entry in flight finish
            // while the guest still runs, and only then stop the guest threads.
            {
                emuenv->gxm.display_queue.abort();
                HANDLE dh = nullptr;
                if (emuenv->gxm.display_host_thread.joinable())
                    DuplicateHandle(GetCurrentProcess(), static_cast<HANDLE>(emuenv->gxm.display_host_thread.native_handle()), GetCurrentProcess(), &dh,
                        SYNCHRONIZE, FALSE, 0);
                const bool done = !dh || WaitForSingleObject(dh, 3000) == WAIT_OBJECT_0;
                if (dh)
                    CloseHandle(dh);
                out("m8a timeout/trap shutdown: display queue aborted first; display host thread {} before the guest threads stop",
                    !dh ? "not running" : done ? "exited" : "still busy (3 s)");
            }
            // M9A.4R: a thread inside sceAudioOutOutput waits on its port - mark every port stopping and wake it first
            emuenv->audio.stop_all_ports();
            {
                std::lock_guard<std::mutex> lock(emuenv->kernel.mutex);
                for (auto &[_, t] : emuenv->kernel.threads)
                    t->exit_delete(false);
            }
            bool gone = false;
            {
                std::unique_lock<std::mutex> lock(emuenv->kernel.mutex);
                gone = emuenv->kernel.thread_deleted_cond.wait_for(lock, std::chrono::seconds(5), [&] { return emuenv->kernel.threads.empty(); });
            }
            out("m8a timeout/trap shutdown: guest threads stopped before the render-thread stop -> {} ({} left)", gone ? "all deleted" : "NOT all deleted",
                emuenv->kernel.threads.size());
            if (!gone) {
                std::lock_guard<std::mutex> lock(emuenv->kernel.mutex);
                for (const auto &[tid, t] : emuenv->kernel.threads)
                    out("m8a timeout/trap shutdown: remaining thread id {} '{}' status {} | pc 0x{:08X} lr 0x{:08X} | last import NID 0x{:08X}", tid, t->name,
                        static_cast<int>(t->status), t->cpu ? read_pc(*t->cpu) : 0u, t->cpu ? read_lr(*t->cpu) : 0u, vitox_last_import_nid(tid));
            }
        }
        emuenv->renderer->preclose_action();
        renderer::stop_render_thread(*emuenv->renderer);
        if (r.timed_out || r.trapped) {
            // the guest never reached sceGxmTerminate: do what it does before the join (as the M7B.2 early stop)
            emuenv->gxm.display_queue.abort();
            HANDLE h = nullptr;
            if (emuenv->gxm.display_host_thread.joinable())
                DuplicateHandle(GetCurrentProcess(), static_cast<HANDLE>(emuenv->gxm.display_host_thread.native_handle()), GetCurrentProcess(), &h, SYNCHRONIZE,
                    FALSE, 0);
            if (h && WaitForSingleObject(h, 2000) == WAIT_TIMEOUT) {
                std::lock_guard<std::mutex> guard(emuenv->gxm.sync_objects_mutex);
                for (SceGxmSyncObject *so : emuenv->gxm.sync_objects) {
                    {
                        std::lock_guard<std::mutex> so_guard(so->lock);
                        so->being_deleted = true;
                    }
                    so->cond.notify_all();
                }
            }
            bool still_blocked = h && WaitForSingleObject(h, 3000) != WAIT_OBJECT_0;
            if (still_blocked) {
                // 0.0.27.2: last resort - the upstream abort flag (its loop checks it before every callback)
                emuenv->display.abort = true;
                emuenv->renderer->notification_ready.notify_all();
                still_blocked = WaitForSingleObject(h, 2000) != WAIT_OBJECT_0;
            }
            out("m8a timeout/trap shutdown: display queue aborted; display host thread {}", !h ? "not running" : still_blocked ? "STILL BLOCKED" : "exited");
            if (h)
                CloseHandle(h);
            if (still_blocked && emuenv->gxm.display_host_thread.joinable()) {
                // a thread that can never finish must not hang Return to Launcher in gxm.deinit's join: leave it parked
                emuenv->gxm.display_host_thread.detach();
                out("m8a timeout/trap shutdown: display host thread DETACHED (it waits on a guest thread that no longer exists)");
            }
        }
        emuenv->gxm.deinit();
    }
    host_text_end("run end");
    // M8F.2A: upstream order (app_init.cpp): the overlay manager goes after gxm.deinit, before display.deinit; its
    // destructor joins the overlay input thread
    if (emuenv->renderer) {
        emuenv->renderer->overlay_manager = nullptr;
        emuenv->renderer->common_dialog = nullptr;
    }
    const bool overlay_had = emuenv->overlay_manager != nullptr;
    emuenv->overlay_manager.reset();
    out("m8f overlay teardown: overlay_manager {} | ime sessions {} scripted actions {} | ctrl overlay_input_intercepted={}",
        overlay_had ? "destroyed (input thread joined)" : "absent", ime_sessions, ime_actions_applied, emuenv->ctrl.overlay_input_intercepted.load());
    if (r.thread)
        emuenv->display.deinit();
    const VitoxExternalPresentStats s1 = vitox_external_present_stats();
    r.presents = s1.presents_ok;
    r.present_seconds = first_present_n ? std::chrono::duration<double>(t_last_present - t_first_present).count() : 0;
    r.gpu_draws = s1.cmd_draw_indexed;
    r.pipelines = s1.pipelines_created;
    r.pipeline_failures = s1.pipeline_failures;
    r.pso_failures = pso ? *pso : -1;
    r.vita3k_errors = g_vita3k_errors;
    out("m8a GPU: presents ok={} failed={} over {:.1f} s | vkCmdDrawIndexed={} | pipelines ok={} failed={} | shader modules ok={} failed={} | dzn PSO failures={} "
        "| [vita3k error] lines={}",
        s1.presents_ok, s1.presents_failed, r.present_seconds, s1.cmd_draw_indexed, s1.pipelines_created, s1.pipeline_failures, s1.shader_modules,
        s1.shader_module_failures, r.pso_failures, g_vita3k_errors.load());

    // ---- 9. imports actually invoked ----
    std::vector<VitoxImportStat> stats(512);
    const unsigned n = vitox_import_stats(stats.data(), static_cast<unsigned>(stats.size()));
    unsigned unknown = 0;
    for (unsigned i = 0; i < n; ++i) {
        r.guest_calls += stats[i].calls;
        unknown += stats[i].resolved ? 0 : 1;
        out("m8a invoked: nid=0x{:08X} {} calls={} -> {}", stats[i].nid, stats[i].name ? stats[i].name : import_name(stats[i].nid), stats[i].calls,
            stats[i].resolved ? "REAL ImportFn" : "UNKNOWN (not selected): FAIL");
    }
    r.unknown_zero = unknown == 0 && emuenv->missing_nids.empty();
    out("m8a invoked: distinct NIDs={} total calls={} unknown={} emuenv.missing_nids={} -> {}", n, r.guest_calls, unknown, emuenv->missing_nids.size(),
        pf(r.unknown_zero));

    // ---- 10. cleanup through Vita3K ----
    cp("m9a4 cp 16 cleanup begin");
    out("vitox gpu mem cleanup: begin | {}", gpu_mem());
    main_thread.reset();
    {
        std::lock_guard<std::mutex> lock(emuenv->kernel.mutex);
        for (auto &[_, t] : emuenv->kernel.threads)
            t->exit_delete(false);
    }
    bool threads_gone = false;
    {
        std::unique_lock<std::mutex> lock(emuenv->kernel.mutex);
        threads_gone = emuenv->kernel.thread_deleted_cond.wait_for(lock, std::chrono::seconds(5), [&] { return emuenv->kernel.threads.empty(); });
    }
    // M9A.5: upstream shutdown_app_runtime's HLE subsystem order once no guest thread runs - audio ports (XAudio2 voices),
    // the SceNetCtl adhoc thread (joined), NP/trophy, HTTP (TLS stand-in objects), net (sockets + WSACleanup balancing
    // sceNetInit's WSAStartup)
    if (threads_gone) {
        const size_t ports = emuenv->audio.out_ports.size();
        const bool adhoc_running = emuenv->netctl.adhocThread.joinable();
        const bool net_inited = emuenv->net.inited;
        emuenv->audio.deinit();
        emuenv->netctl.deinit();
        ngs::deinit(emuenv->ngs, emuenv->mem); // M9A.7: upstream order (after netctl, before np)
        deinit(emuenv->np);
        emuenv->http.deinit();
        emuenv->net.deinit();
        out("m9a5 hle deinit: audio ports closed {} | netctl adhoc thread {} | np/http deinit | net deinit (WSACleanup {}) -> {}", ports,
            adhoc_running ? "joined" : "not running", net_inited ? "called" : "not needed",
            pf(emuenv->audio.out_ports.empty() && !emuenv->netctl.adhocThread.joinable() && !emuenv->net.inited));
    }
    const int unload_res = main_module_id >= 0 ? unload_module(*emuenv, main_module_id) : -1;
    if (emuenv->renderer) {
        out("vitox gpu mem cleanup: before VKState::cleanup | {}", gpu_mem());
        emuenv->renderer->cleanup();
        out("vitox gpu mem cleanup: after VKState::cleanup (device + instance destroyed) | {}", gpu_mem());
        emuenv->renderer.reset();
        out("vitox gpu mem cleanup: after ~VKState | {}", gpu_mem());
    }
    if (threads_gone) {
        emuenv->kernel.deinit(emuenv->mem);
        deinit_mem(emuenv->mem);
    }
    vitox_set_external_presenter(nullptr);
    if (threads_gone) {
        io_deinit(emuenv->io);
        delete emuenv;
    }
    out("vitox gpu mem cleanup: after kernel/mem deinit + ~EmuEnvState | {}", gpu_mem());
    snap("m8a_after_cleanup");
    // Vita3K's process-wide VEH captured the freed MemState: re-anchor it (as after every earlier core probe)
    auto *veh_anchor = new MemState;
    if (init(*veh_anchor, false))
        deinit_mem(*veh_anchor);
    r.cleanup = threads_gone && (main_module_id < 0 || unload_res == 0);
    out("m8a cleanup: guest threads deleted={} unload_module={} VKState cleanup + ~EmuEnvState={} -> {}", threads_gone, unload_res, threads_gone, pf(r.cleanup));
    cp("m9a4 cp 17 cleanup end: threads deleted={} unload_module={} -> {}", threads_gone, unload_res, pf(r.cleanup));

    vitox_set_fself_real_decrypt(false); // 0.0.29.1: also when a NoNpDrm license turned it on
    if (config->staged_pgf) {
        vitox_set_fself_real_decrypt(false);
        vitox_set_firmware_root(nullptr);
        const bool same = tree(staged_fw) == staged_before;
        r.root_safety_staging = same;
        out("m8c.3 staged firmware tree unchanged={} ({} files) | firmware root + real decrypt cleared", same, staged_before.size());
    }
    // ---- 11. root safety: other apps untouched, no data folder removed, the homebrew root itself unchanged ----
    auto apps_after = names(s_vita_fs / "ux0" / "app"), data_after = names(s_vita_fs / "ux0" / "data");
    apps_after.erase(title_id);
    auto apps_cmp = apps_before;
    apps_cmp.erase(title_id);
    const bool data_kept = std::includes(data_after.begin(), data_after.end(), data_before.begin(), data_before.end());
    const bool root_same = tree(root) == root_before;
    r.root_safety = apps_after == apps_cmp && data_kept && root_same && sfs::exists(s_app / "eboot.bin", ec) && (!config->staged_pgf || r.root_safety_staging);
    out("m8a root safety: other ux0:app entries unchanged={} ({}) | ux0:data entries kept={} (before {} after {}) | homebrew root unchanged={} -> {}",
        apps_after == apps_cmp, apps_cmp.size(), data_kept, data_before.size(), data_after.size(), root_same, pf(r.root_safety));
    cp("m9a4 cp 18 root safety -> {}", pf(r.root_safety));
    out("M8A end: {} '{}' ({}) | discovered={} selected={} installed={} sfo={} renderer={} loaded={} imports_supported={} thread={} exited={} exit_ok={} "
        "timed_out={} unknown_zero={} cleanup={} root_safety={}",
        title_id, r.title, r.folder, r.discovered, r.selected, r.installed, r.sfo, r.renderer, r.loaded, r.imports_supported, r.thread, r.exited, r.exit_ok,
        r.timed_out, r.unknown_zero, r.cleanup, r.root_safety);
    silence_log();
    return r;
}

// ---- 0.0.27.0 internal resolution upscaling ----
static std::atomic<float> g_vitox_res_multiplier{ 1.0f };

extern "C" void vitox_launcher_set_resolution_multiplier(float multiplier) {
    float m = std::round(multiplier * 2.0f) / 2.0f; // 0.5 steps
    if (!(m >= 1.0f))
        m = 1.0f;
    if (m > 8.0f)
        m = 8.0f;
    g_vitox_res_multiplier.store(m);
}

extern "C" float vitox_launcher_resolution_multiplier() { return g_vitox_res_multiplier.load(); }

// ---- 0.0.28.3 shader compilation settings ----
static std::atomic<bool> g_vitox_async_pipelines{ true }, g_vitox_pipeline_prewarm{ true };

extern "C" void vitox_launcher_set_pipeline_options(bool async, bool prewarm) {
    g_vitox_async_pipelines.store(async);
    g_vitox_pipeline_prewarm.store(prewarm);
}

extern "C" bool vitox_launcher_async_pipelines() { return g_vitox_async_pipelines.load(); }
extern "C" bool vitox_launcher_pipeline_prewarm() { return g_vitox_pipeline_prewarm.load(); }
