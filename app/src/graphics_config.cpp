// Vitox 0.0.27.0: graphics settings (launcher Settings > Graphics), saved as LocalState\vitox-graphics.txt.
//
// Internal resolution upscaling: the multiplier the next game session renders at (core launcher
// vitox_launcher_set_resolution_multiplier -> Vita3K res_multiplier). Every value is a multiple of 0.5 from 1.0 to the
// maximum (4.0 by default; `max_resolution_multiplier=` in the file raises or lowers it, up to 8.0).
//
// Crash guard: a session started above 1.0x writes vitox-graphics-pending.txt; it is removed once the session has run
// for kGuardSeconds or ends. If the file is still there when Vitox starts, the last session at that multiplier died
// early (GPU memory, driver): the setting reverts to the last multiplier that ran, and the launcher says so.
#include "common.h"

#include <vitox_homebrew_launcher.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

namespace vitox {

namespace {
std::mutex g_m;
float g_mult = 1.0f;      // the selected multiplier
float g_last_good = 1.0f; // the last multiplier a session ran at past the guard window
float g_max = kResolutionMaxDefault;
std::wstring g_dir;       // LocalState
std::wstring g_notice;    // set when the crash guard reverted the setting
bool g_async = true;      // 0.0.28.3 asynchronous pipeline compilation
bool g_prewarm = true;    // 0.0.28.3 pipeline pre-warm at launch
// 0.0.30.0: per-game values for the next session only (game menu); never saved here
float g_ov_mult = 0;
int g_ov_async = -1, g_ov_prewarm = -1;

std::wstring file() { return g_dir + L"\\vitox-graphics.txt"; }
std::wstring pending_file() { return g_dir + L"\\vitox-graphics-pending.txt"; }

float parse(const std::string &v, float fallback) {
    char *end = nullptr;
    const float f = std::strtof(v.c_str(), &end);
    return end && end != v.c_str() && std::isfinite(f) ? f : fallback;
}

// caller holds g_m
void save_locked() {
    if (g_dir.empty())
        return;
    std::ofstream f(file(), std::ios::trunc);
    char buf[256];
    snprintf(buf, sizeof(buf),
        "# Vitox graphics settings (launcher Settings > Graphics, Shader Compilation)\nresolution_multiplier=%.1f\nlast_good_resolution_multiplier=%.1f\n"
        "async_pipeline_compilation=%d\npipeline_prewarm=%d\n",
        g_mult, g_last_good, g_async ? 1 : 0, g_prewarm ? 1 : 0);
    f << buf;
    if (g_max != kResolutionMaxDefault) {
        snprintf(buf, sizeof(buf), "max_resolution_multiplier=%.1f\n", g_max);
        f << buf;
    }
}

// the guard thread of the running session
std::thread g_guard;
std::mutex g_guard_m;
std::condition_variable g_guard_cv;
bool g_guard_stop = false;
} // namespace

float resolution_snap(float m, float max) {
    float s = std::round(m / kResolutionStep) * kResolutionStep;
    if (!(s >= kResolutionMin))
        s = kResolutionMin;
    return std::min(s, std::max(kResolutionMin, max));
}

int resolution_width(float m) { return static_cast<int>(std::lround(960.0 * m)); }
int resolution_height(float m) { return static_cast<int>(std::lround(544.0 * m)); }

float graphics_max_multiplier() {
    std::lock_guard<std::mutex> lock(g_m);
    return g_max;
}

float graphics_resolution_multiplier() {
    std::lock_guard<std::mutex> lock(g_m);
    return g_mult;
}

void graphics_set_resolution_multiplier(float m) {
    std::lock_guard<std::mutex> lock(g_m);
    g_mult = resolution_snap(m, g_max);
    g_notice.clear();
    save_locked();
    vitox_launcher_set_resolution_multiplier(g_mult);
}

bool graphics_async_pipelines() {
    std::lock_guard<std::mutex> lock(g_m);
    return g_async;
}

bool graphics_pipeline_prewarm() {
    std::lock_guard<std::mutex> lock(g_m);
    return g_prewarm;
}

void graphics_set_async_pipelines(bool on) {
    std::lock_guard<std::mutex> lock(g_m);
    g_async = on;
    save_locked();
    vitox_launcher_set_pipeline_options(g_async, g_prewarm);
}

void graphics_set_pipeline_prewarm(bool on) {
    std::lock_guard<std::mutex> lock(g_m);
    g_prewarm = on;
    save_locked();
    vitox_launcher_set_pipeline_options(g_async, g_prewarm);
}

std::wstring graphics_take_notice() {
    std::lock_guard<std::mutex> lock(g_m);
    std::wstring n;
    n.swap(g_notice);
    return n;
}

std::wstring graphics_notice() {
    std::lock_guard<std::mutex> lock(g_m);
    return g_notice;
}

void graphics_load(const std::wstring &local_state) {
    std::lock_guard<std::mutex> lock(g_m);
    g_dir = local_state;
    float mult = 1.0f, good = 1.0f, max = kResolutionMaxDefault;
    bool found = false;
    {
        std::ifstream f(file());
        for (std::string line; f && std::getline(f, line);) {
            if (line.empty() || line[0] == '#')
                continue;
            const size_t eq = line.find('=');
            if (eq == std::string::npos)
                continue;
            const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
            if (k == "resolution_multiplier")
                mult = parse(v, 1.0f), found = true;
            else if (k == "last_good_resolution_multiplier")
                good = parse(v, 1.0f);
            else if (k == "max_resolution_multiplier")
                max = parse(v, kResolutionMaxDefault);
            else if (k == "async_pipeline_compilation" && (v == "0" || v == "1"))
                g_async = v == "1";
            else if (k == "pipeline_prewarm" && (v == "0" || v == "1"))
                g_prewarm = v == "1";
        }
    }
    // validate: a configurable maximum within the backend's hard limit, every value on the 0.5 grid
    g_max = resolution_snap(max, kResolutionMaxHard);
    g_mult = resolution_snap(mult, g_max);
    g_last_good = resolution_snap(good, g_max);
    // the crash guard
    std::ifstream pf(pending_file());
    std::string pending;
    if (pf && std::getline(pf, pending)) {
        pf.close();
        const float failed = resolution_snap(parse(pending, 1.0f), kResolutionMaxHard);
        if (failed > kResolutionMin && failed == g_mult && g_last_good < failed) {
            wchar_t buf[200];
            swprintf_s(buf, L"The last game at %.1fx (%d × %d) stopped early, so Internal Resolution Upscaling went back to %.1fx", failed,
                resolution_width(failed), resolution_height(failed), g_last_good);
            g_notice = buf;
            log_line(Log::Lifecycle, "vitox graphics: crash guard - the session at %.1fx did not pass %d s; reverting to the last working %.1fx", failed,
                kResolutionGuardSeconds, g_last_good);
            g_mult = g_last_good;
        }
        DeleteFileW(pending_file().c_str());
    }
    if (found && (g_mult != resolution_snap(mult, g_max) || mult != g_mult))
        log_line(Log::Lifecycle, "vitox graphics: stored multiplier %.3f -> %.1fx (snapped / clamped / reverted)", mult, g_mult);
    save_locked();
    vitox_launcher_set_resolution_multiplier(g_mult);
    vitox_launcher_set_pipeline_options(g_async, g_prewarm);
    log_line(Log::Lifecycle, "vitox graphics: internal resolution %.1fx = %dx%d (max %.1fx, last working %.1fx)%s | async pipelines %s, pre-warm %s", g_mult,
        resolution_width(g_mult), resolution_height(g_mult), g_max, g_last_good, found ? "" : " [default]", g_async ? "on" : "off", g_prewarm ? "on" : "off");
}

void graphics_session_override(float resolution, int async, int prewarm) {
    std::lock_guard<std::mutex> lock(g_m);
    g_ov_mult = resolution > 0 ? resolution_snap(resolution, kResolutionMaxHard) : 0;
    g_ov_async = async;
    g_ov_prewarm = prewarm;
}

void graphics_session_begin() {
    float m;
    {
        std::lock_guard<std::mutex> lock(g_m);
        // 0.0.30.0: a game's own values (game menu) win for this session
        m = g_ov_mult > 0 ? g_ov_mult : g_mult;
        const bool async = g_ov_async >= 0 ? g_ov_async == 1 : g_async, prewarm = g_ov_prewarm >= 0 ? g_ov_prewarm == 1 : g_prewarm;
        vitox_launcher_set_resolution_multiplier(m);
        vitox_launcher_set_pipeline_options(async, prewarm);
        if (g_ov_mult > 0 || g_ov_async >= 0 || g_ov_prewarm >= 0)
            log_line(Log::Lifecycle, "vitox graphics: this game's own settings: resolution %.1fx%s, async %s%s, pre-warm %s%s", m, g_ov_mult > 0 ? "" : " (global)",
                async ? "on" : "off", g_ov_async >= 0 ? "" : " (global)", prewarm ? "on" : "off", g_ov_prewarm >= 0 ? "" : " (global)");
        // the crash guard protects the GLOBAL multiplier: a per-game value is the user's choice for that title only
        if (g_ov_mult > 0 || g_dir.empty() || m <= kResolutionMin || m <= g_last_good)
            return; // nothing new to prove
        std::ofstream f(pending_file(), std::ios::trunc);
        f << m << "\n";
    }
    {
        std::lock_guard<std::mutex> lock(g_guard_m);
        g_guard_stop = false;
    }
    g_guard = std::thread([m] {
        std::unique_lock<std::mutex> lock(g_guard_m);
        const bool stopped = g_guard_cv.wait_for(lock, std::chrono::seconds(kResolutionGuardSeconds), [] { return g_guard_stop; });
        if (!stopped) {
            std::lock_guard<std::mutex> l2(g_m);
            g_last_good = std::max(g_last_good, m);
            DeleteFileW(pending_file().c_str());
            save_locked();
            log_line(Log::Lifecycle, "vitox graphics: %.1fx ran %d s - recorded as working", m, kResolutionGuardSeconds);
        }
    });
}

void graphics_session_end() {
    {
        std::lock_guard<std::mutex> lock(g_guard_m);
        g_guard_stop = true;
    }
    g_guard_cv.notify_all();
    if (g_guard.joinable())
        g_guard.join();
    // the session ended on its own terms (quit, exit or a trap the launcher reported): not a GPU failure
    std::lock_guard<std::mutex> lock(g_m);
    g_ov_mult = 0, g_ov_async = g_ov_prewarm = -1; // 0.0.30.0: per-game values last one session
    if (!g_dir.empty())
        DeleteFileW(pending_file().c_str());
}

} // namespace vitox
