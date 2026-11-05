// Vitox 0.0.28.0: the Performance Statistics Overlay.
//
// One setting, two editors: the launcher's Settings > Performance Statistics Overlay page and the in-game quick menu
// (R3) both call stats_set_enabled / stats_set_mode here; nothing else stores the state. Saved as LocalState\vitox-overlay.txt.
//
// While a visual game session runs, a sampler thread measures (only while the overlay is enabled):
//   FPS        - guest frames that reached the screen (stats_frame_presented, called by the host presenter for every game
//                frame it presents), counted over 500 ms windows and smoothed (exponential moving average)
//   Memory     - Windows.System.MemoryManager.AppMemoryUsage: the memory the Vitox app process is charged for (the
//                quantity its 5 GB app limit applies to) - NOT the console's total memory
//   CPU        - QueryProcessCycleTime of the Vitox process over 1 s, divided by the TSC cycles that passed times the
//                logical processors the app sees: the Vitox process's share of the CPU it can use
// 0.0.28.2: GPU rows removed at the user's request (GPU load is not available to Xbox apps anyway: the Windows GPU
// performance counters are desktop-only and D3D12/DXGI expose memory budgets, not load).
// After each update the panel is drawn with Direct2D into a CPU bitmap; the presenter only uploads the new pixels and
// composites them (shaders/stats.hlsl) over the game frame in the top-right corner.
#include "common.h"

#include <d2d1.h>
#include <dwrite_1.h>
#include <dxgi1_4.h>
#include <wincodec.h>
#include <intrin.h>

#include <winrt/Windows.System.h>
#include <winrt/base.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

using winrt::com_ptr;

namespace vitox {

namespace {

// ---- the setting ----
std::mutex g_cfg_m;
std::wstring g_dir;
std::atomic<bool> g_enabled{ false };
std::atomic<int> g_mode{ static_cast<int>(StatsMode::FpsOnly) };

void save_locked() {
    if (g_dir.empty())
        return;
    std::ofstream f(g_dir + L"\\vitox-overlay.txt", std::ios::trunc);
    f << "# Vitox performance statistics overlay (Settings > Performance Statistics Overlay, and the R3 quick menu)\n";
    f << "stats_enabled=" << (g_enabled.load() ? 1 : 0) << "\n";
    f << "stats_mode=" << (g_mode.load() == static_cast<int>(StatsMode::Full) ? "full" : "fps") << "\n";
}

// ---- the sampler (one game session) ----
std::atomic<uint64_t> g_frames{ 0 };
std::mutex g_m;
std::condition_variable g_cv;
std::thread g_thread;
bool g_stop = false;
uint32_t g_screen_h = 1080;
// the latest panel (premultiplied BGRA, tightly packed)
std::mutex g_px_m;
std::vector<uint8_t> g_px;
uint32_t g_px_w = 0, g_px_h = 0, g_px_version = 0;

D2D1_COLOR_F rgba(uint32_t rgb, float a = 1.0f) {
    return D2D1::ColorF(((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, a);
}

struct Sample {
    double fps = -1;               // < 0 = not measured yet
    double mem_gb = -1, mem_limit_gb = -1;
    double cpu = -1;               // percent
};

class Painter {
public:
    bool init() {
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IWICImagingFactory), wic_.put_void()))
            || FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory), d2d_.put_void()))
            || FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<::IUnknown **>(dw_.put()))))
            return false;
        return true;
    }
    // draws the panel for `mode`; false on failure
    bool paint(StatsMode mode, const Sample &s, float k, std::vector<uint8_t> &out, uint32_t &w, uint32_t &h) {
        const bool full = mode == StatsMode::Full;
        w = static_cast<uint32_t>(std::lround((full ? 316.0f : 156.0f) * k));
        h = static_cast<uint32_t>(std::lround((full ? 174.0f : 54.0f) * k));
        com_ptr<IWICBitmap> bmp;
        if (FAILED(wic_->CreateBitmap(w, h, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnDemand, bmp.put())))
            return false;
        const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
        com_ptr<ID2D1RenderTarget> rt;
        if (FAILED(d2d_->CreateWicBitmapRenderTarget(bmp.get(), props, rt.put())))
            return false;
        rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        com_ptr<ID2D1SolidColorBrush> b;
        rt->CreateSolidColorBrush(rgba(0xFFFFFF), b.put());
        const float W = static_cast<float>(w), H = static_cast<float>(h), r = (full ? 18.0f : 16.0f) * k;
        rt->BeginDraw();
        rt->Clear(D2D1::ColorF(0, 0, 0, 0));
        // dark translucent glass, a violet rim, a faint violet top light (readable on bright and dark scenes)
        const D2D1_ROUNDED_RECT rr{ D2D1::RectF(1.0f * k, 1.0f * k, W - 1.0f * k, H - 1.0f * k), r, r };
        b->SetColor(rgba(0x0B0517, 0.80f));
        rt->FillRoundedRectangle(rr, b.get());
        {
            com_ptr<ID2D1GradientStopCollection> stops;
            const D2D1_GRADIENT_STOP gs[] = { { 0.0f, rgba(0x8A4DFF, 0.30f) }, { 1.0f, rgba(0x8A4DFF, 0.0f) } };
            rt->CreateGradientStopCollection(gs, 2, stops.put());
            com_ptr<ID2D1LinearGradientBrush> g;
            rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({ 0, 0 }, { 0, std::min(H, 70.0f * k) }), stops.get(), g.put());
            rt->FillRoundedRectangle(rr, g.get());
        }
        b->SetColor(rgba(0xB07CFF, 0.60f));
        rt->DrawRoundedRectangle(rr, b.get(), 1.5f * k);
        wchar_t v[64];
        if (!full) {
            // FPS: <value> - small and quiet
            b->SetColor(rgba(0xA66BFF));
            rt->FillEllipse(D2D1::Ellipse({ 22 * k, H / 2 }, 5 * k, 5 * k), b.get());
            text(rt.get(), L"FPS:", 18 * k, DWRITE_FONT_WEIGHT_BOLD, rgba(0xD9C9FF), D2D1::RectF(36 * k, 0, W, H), DWRITE_TEXT_ALIGNMENT_LEADING);
            fmt_fps(s, v);
            text(rt.get(), v, 24 * k, DWRITE_FONT_WEIGHT_BOLD, rgba(0xFFFFFF), D2D1::RectF(36 * k, 0, W - 16 * k, H), DWRITE_TEXT_ALIGNMENT_TRAILING);
        } else {
            text(rt.get(), L"VITOX PERFORMANCE", 13 * k, DWRITE_FONT_WEIGHT_BOLD, rgba(0xB98BFF), D2D1::RectF(20 * k, 12 * k, W - 20 * k, 34 * k),
                DWRITE_TEXT_ALIGNMENT_LEADING, 3 * k);
            b->SetColor(rgba(0xB48CFF, 0.25f));
            rt->FillRectangle(D2D1::RectF(20 * k, 40 * k, W - 20 * k, 41 * k), b.get());
            float y = 48 * k;
            const float row = 39 * k;
            const auto line = [&](const wchar_t *label, const wchar_t *note, const wchar_t *value, uint32_t vc) {
                text(rt.get(), label, 18 * k, DWRITE_FONT_WEIGHT_SEMI_BOLD, rgba(0xD9C9FF), D2D1::RectF(20 * k, y, W, y + row), DWRITE_TEXT_ALIGNMENT_LEADING);
                if (note)
                    text(rt.get(), note, 12 * k, DWRITE_FONT_WEIGHT_NORMAL, rgba(0x8F7DB8), D2D1::RectF(20 * k + measure(label, 18 * k) + 7 * k, y + 2 * k, W, y + row),
                        DWRITE_TEXT_ALIGNMENT_LEADING);
                text(rt.get(), value, 21 * k, DWRITE_FONT_WEIGHT_BOLD, rgba(vc), D2D1::RectF(20 * k, y, W - 20 * k, y + row), DWRITE_TEXT_ALIGNMENT_TRAILING);
                y += row;
            };
            fmt_fps(s, v);
            line(L"FPS", nullptr, v, 0xFFFFFF);
            if (s.mem_gb >= 0)
                swprintf_s(v, L"%.2f GB", s.mem_gb);
            else
                wcscpy_s(v, L"–");
            line(L"Memory", L"Vitox", v, 0xFFFFFF);
            if (s.cpu >= 0)
                swprintf_s(v, L"%.0f%%", s.cpu);
            else
                wcscpy_s(v, L"–");
            line(L"CPU", L"Vitox", v, 0xFFFFFF);
        }
        if (FAILED(rt->EndDraw()))
            return false;
        out.resize(static_cast<size_t>(w) * h * 4);
        const WICRect all{ 0, 0, static_cast<INT>(w), static_cast<INT>(h) };
        return SUCCEEDED(bmp->CopyPixels(&all, w * 4, static_cast<UINT>(out.size()), out.data()));
    }

private:
    static void fmt_fps(const Sample &s, wchar_t (&v)[64]) {
        if (s.fps >= 0)
            swprintf_s(v, L"%.0f", s.fps);
        else
            wcscpy_s(v, L"–");
    }
    IDWriteTextFormat *format(float size, DWRITE_FONT_WEIGHT weight) {
        const int key = static_cast<int>(size * 10) * 10 + static_cast<int>(weight) / 100;
        for (auto &f : formats_)
            if (f.first == key)
                return f.second.get();
        com_ptr<IDWriteTextFormat> f;
        dw_->CreateTextFormat(L"Segoe UI", nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", f.put());
        if (f) {
            f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
        formats_.emplace_back(key, f);
        return f.get();
    }
    float measure(const wchar_t *s, float size) {
        com_ptr<IDWriteTextLayout> l;
        DWRITE_TEXT_METRICS m{};
        if (IDWriteTextFormat *f = format(size, DWRITE_FONT_WEIGHT_SEMI_BOLD);
            f && SUCCEEDED(dw_->CreateTextLayout(s, static_cast<UINT32>(wcslen(s)), f, 1000, 100, l.put())))
            l->GetMetrics(&m);
        return m.widthIncludingTrailingWhitespace;
    }
    void text(ID2D1RenderTarget *rt, const wchar_t *s, float size, DWRITE_FONT_WEIGHT weight, D2D1_COLOR_F c, D2D1_RECT_F r, DWRITE_TEXT_ALIGNMENT align,
        float spacing = 0) {
        IDWriteTextFormat *f = format(size, weight);
        com_ptr<IDWriteTextLayout> l;
        if (!f || FAILED(dw_->CreateTextLayout(s, static_cast<UINT32>(wcslen(s)), f, r.right - r.left, r.bottom - r.top, l.put())))
            return;
        l->SetTextAlignment(align);
        if (spacing != 0)
            if (auto l1 = l.try_as<IDWriteTextLayout1>())
                l1->SetCharacterSpacing(0, spacing, 0, { 0, static_cast<UINT32>(wcslen(s)) });
        com_ptr<ID2D1SolidColorBrush> b;
        rt->CreateSolidColorBrush(c, b.put());
        rt->DrawTextLayout({ r.left, r.top }, l.get(), b.get());
    }
    com_ptr<IWICImagingFactory> wic_;
    com_ptr<ID2D1Factory> d2d_;
    com_ptr<IDWriteFactory> dw_;
    std::vector<std::pair<int, com_ptr<IDWriteTextFormat>>> formats_;
};

uint64_t filetime_100ns(const FILETIME &f) { return (static_cast<uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime; }

// QueryProcessCycleTime is exported by kernel32/kernelbase but not declared for the app partition: resolved at run time
using QueryProcessCycleTimeFn = BOOL(WINAPI *)(HANDLE, PULONG64);
QueryProcessCycleTimeFn cycle_time_fn() {
    static const QueryProcessCycleTimeFn fn = [] {
        FARPROC f = resolve(L"kernelbase.dll", "QueryProcessCycleTime");
        if (!f)
            f = resolve(L"kernel32.dll", "QueryProcessCycleTime");
        return reinterpret_cast<QueryProcessCycleTimeFn>(f);
    }();
    return fn;
}
// CPU time of this process in "ticks", and the ticks one CPU provides per wall second: cycles/TSC when available,
// else GetProcessTimes (100 ns units)
bool process_cpu(uint64_t &ticks, uint64_t &clock) {
    if (const auto fn = cycle_time_fn()) {
        ULONG64 c = 0;
        if (fn(GetCurrentProcess(), &c)) {
            ticks = c;
            clock = __rdtsc();
            return true;
        }
    }
    FILETIME c, e, k, u;
    if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u))
        return false;
    ticks = filetime_100ns(k) + filetime_100ns(u);
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    clock = filetime_100ns(now);
    return true;
}

void sampler() {
    const HRESULT ci = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    Painter painter;
    const bool painter_ok = painter.init();
    SYSTEM_INFO si{};
    GetNativeSystemInfo(&si);
    const double cpus = std::max<DWORD>(1, si.dwNumberOfProcessors);
    log_line(Log::Lifecycle, "vitox stats: sampler start (painter %s, %u logical CPUs, CPU source %s)", painter_ok ? "ok" : "FAILED",
        static_cast<unsigned>(cpus), cycle_time_fn() ? "QueryProcessCycleTime/TSC" : "GetProcessTimes (coarse)");
    using Clock = std::chrono::steady_clock;
    Sample s;
    bool active = false;
    Clock::time_point fps_t{}, slow_t{};
    uint64_t fps_frames = 0, cpu_proc = 0, cpu_tsc = 0;
    Clock::time_point cpu_t{};
    int last_mode = -1;
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(g_m);
            if (g_cv.wait_for(lock, std::chrono::milliseconds(100), [] { return g_stop; }))
                break;
        }
        if (!g_enabled.load()) {
            active = false; // no collection while the overlay is off; the next enable starts fresh
            continue;
        }
        const auto now = Clock::now();
        const int mode = g_mode.load();
        bool changed = false;
        if (!active) {
            active = true;
            s = Sample{};
            fps_t = slow_t = cpu_t = now;
            fps_frames = g_frames.load();
            if (!process_cpu(cpu_proc, cpu_tsc))
                cpu_proc = 0;
            slow_t = now - std::chrono::seconds(1); // memory at once
            changed = true;
        }
        // FPS: frames that reached the screen in each ~500 ms window, smoothed
        if (now - fps_t >= std::chrono::milliseconds(500)) {
            const uint64_t f = g_frames.load();
            const double dt = std::chrono::duration<double>(now - fps_t).count();
            const double w = static_cast<double>(f - fps_frames) / dt;
            s.fps = s.fps < 0 ? w : s.fps * 0.5 + w * 0.5;
            fps_frames = f;
            fps_t = now;
            changed = true;
        }
        // memory and CPU: once a second, only when the full panel shows them
        if (mode == static_cast<int>(StatsMode::Full) && now - slow_t >= std::chrono::seconds(1)) {
            try {
                s.mem_gb = static_cast<double>(winrt::Windows::System::MemoryManager::AppMemoryUsage()) / (1024.0 * 1024.0 * 1024.0);
                s.mem_limit_gb = static_cast<double>(winrt::Windows::System::MemoryManager::AppMemoryUsageLimit()) / (1024.0 * 1024.0 * 1024.0);
            } catch (...) {
            }
            // 0.0.28.1: CPU from QueryProcessCycleTime (cycles every thread of this process ran) over the TSC cycles that
            // passed, per logical CPU. GetProcessTimes advanced in coarse steps on the Xbox (whole seconds read 0%).
            uint64_t cyc = 0, tsc = 0;
            if (process_cpu(cyc, tsc)) {
                const double elapsed = static_cast<double>(tsc - cpu_tsc);
                if (elapsed > 0 && cpu_proc && cyc >= cpu_proc) {
                    const double now_pct = std::clamp(static_cast<double>(cyc - cpu_proc) / (elapsed * cpus) * 100.0, 0.0, 100.0);
                    s.cpu = s.cpu < 0 ? now_pct : s.cpu * 0.4 + now_pct * 0.6;
                }
                cpu_proc = cyc;
                cpu_tsc = tsc;
                cpu_t = now;
            }
            slow_t = now;
            changed = true;
        }
        if (mode != last_mode)
            changed = true, last_mode = mode;
        if (changed && painter_ok) {
            std::vector<uint8_t> px;
            uint32_t w = 0, h = 0;
            if (painter.paint(static_cast<StatsMode>(mode), s, static_cast<float>(g_screen_h) / 1080.0f, px, w, h)) {
                std::lock_guard<std::mutex> lock(g_px_m);
                g_px.swap(px);
                g_px_w = w;
                g_px_h = h;
                ++g_px_version;
            }
        }
    }
    if (SUCCEEDED(ci))
        CoUninitialize();
    log_line(Log::Lifecycle, "vitox stats: sampler stop");
}

} // namespace

void stats_load(const std::wstring &local_state) {
    std::lock_guard<std::mutex> lock(g_cfg_m);
    g_dir = local_state;
    bool on = false;
    StatsMode mode = StatsMode::FpsOnly;
    std::ifstream f(g_dir + L"\\vitox-overlay.txt");
    for (std::string line; f && std::getline(f, line);) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.pop_back();
        if (line == "stats_enabled=1")
            on = true;
        else if (line == "stats_mode=full")
            mode = StatsMode::Full; // anything else (missing, invalid) stays FPS Only
    }
    g_enabled = on;
    g_mode = static_cast<int>(mode);
    log_line(Log::Lifecycle, "vitox stats: overlay %s, mode %s", on ? "ON" : "OFF", mode == StatsMode::Full ? "Full Performance" : "FPS Only");
}

bool stats_enabled() { return g_enabled.load(); }
StatsMode stats_mode() { return static_cast<StatsMode>(g_mode.load()); }

// 0.0.30.0: per-game overlay for one session; the global values are kept aside and restored at the end
namespace {
bool g_overridden = false, g_saved_enabled = false;
int g_saved_mode = 0;
} // namespace

void stats_session_override(int v) {
    std::lock_guard<std::mutex> lock(g_cfg_m);
    if (v < 0) {
        if (g_overridden) {
            g_enabled = g_saved_enabled;
            g_mode = g_saved_mode;
            g_overridden = false;
        }
        return;
    }
    if (!g_overridden) {
        g_saved_enabled = g_enabled.load();
        g_saved_mode = g_mode.load();
        g_overridden = true;
    }
    g_enabled = v != 0;
    if (v != 0)
        g_mode = v == 2 ? 1 : 0;
    log_line(Log::Lifecycle, "vitox stats: this game's own overlay setting: %s", v == 0 ? "OFF" : v == 1 ? "FPS Only" : "Full Performance");
}

void stats_set_enabled(bool on) {
    std::lock_guard<std::mutex> lock(g_cfg_m);
    if (g_enabled.exchange(on) != on)
        log_line(Log::Lifecycle, "vitox stats: overlay %s%s", on ? "ON" : "OFF", g_overridden ? " (this session only: the game has its own setting)" : "");
    if (!g_overridden)
        save_locked();
}

void stats_set_mode(StatsMode m) {
    std::lock_guard<std::mutex> lock(g_cfg_m);
    const int v = m == StatsMode::Full ? 1 : 0;
    if (g_mode.exchange(v) != v)
        log_line(Log::Lifecycle, "vitox stats: mode %s", v ? "Full Performance" : "FPS Only");
    if (!g_overridden)
        save_locked();
}

void stats_frame_presented() { g_frames.fetch_add(1, std::memory_order_relaxed); }

void stats_session_begin(uint32_t screen_h) {
    stats_session_end();
    {
        std::lock_guard<std::mutex> lock(g_m);
        g_stop = false;
        g_screen_h = screen_h ? screen_h : 1080;
    }
    {
        std::lock_guard<std::mutex> lock(g_px_m);
        g_px.clear();
        g_px_w = g_px_h = 0;
    }
    g_thread = std::thread(sampler);
}

void stats_session_end() {
    {
        std::lock_guard<std::mutex> lock(g_m);
        g_stop = true;
    }
    g_cv.notify_all();
    if (g_thread.joinable())
        g_thread.join();
}

bool stats_take_panel(uint32_t have_version, std::vector<uint8_t> &bgra, uint32_t &w, uint32_t &h, uint32_t &version) {
    std::lock_guard<std::mutex> lock(g_px_m);
    if (g_px_version == have_version || g_px.empty())
        return false;
    bgra = g_px;
    w = g_px_w;
    h = g_px_h;
    version = g_px_version;
    return true;
}

} // namespace vitox
