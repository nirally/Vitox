// Vitox 0.0.25.0: the Vitox launcher UI (see launcher_ui.h).
//
// Frame = D3D11 full-screen background shader (shaders/launcher_bg.hlsl: gradient field, light ribbons, glow clouds,
// particles, floating shapes, the selected card's halo) + Direct2D on the same back buffer (cards, artwork, text).
// Presented with vsync (60 Hz). The library comes from vitox_launcher_discover (the launcher's own param.sfo discovery
// of <dev_root>\homebrew and <dev_root>\app-staging), refreshed in the background every few seconds and on Y.
//
// 0.0.26.0: Settings is a menu of pages: Input (live controller view, button remap and stick dead zones for games),
// Artwork (SteamGridDB API key + a per-game browser that replaces the card / background art) and About.
#include "launcher_ui.h"
#include "library_install.h" // 0.0.29.0: Settings > Library (show homebrew) + the "+" tile's USB installer
#include "game_config.h"     // 0.0.30.0: the game menu (Menu on a library card)
#include "steamgriddb.h"
#include "firmware_setup.h" // 0.0.33.0: first-run firmware setup

#include <vitox_host_input.h>
#include <vitox_host_text.h>

#include <d2d1_1.h>
#include <d3d11_1.h>
#include <dwrite_1.h>
#include <dxgi1_3.h>
#include <wincodec.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Display.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <cmath>
#include <ctime>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "vitox_ui_shaders.h" // shaders/launcher_bg.hlsl (fxc)

using winrt::com_ptr;
using namespace winrt::Windows::UI::Core;

namespace vitox {

namespace {

using Clock = std::chrono::steady_clock;

D2D1_COLOR_F rgba(uint32_t rgb, float a = 1.0f) {
    return D2D1::ColorF(((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, a);
}

float approach(float v, float target, float dt, float rate) { return v + (target - v) * (1.0f - std::exp(-dt * rate)); }
float ease_out(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return 1 - (1 - t) * (1 - t) * (1 - t);
}
float ease_in_out(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return t < 0.5f ? 4 * t * t * t : 1 - std::pow(-2 * t + 2, 3.0f) / 2;
}

struct BgConstants {
    float screen[2], time, k;
    float parallax[2], glow, hue;
    float card[4];
    float radius, fade, pad[2];
};
static_assert(sizeof(BgConstants) % 16 == 0, "cbuffer size");

// controller buttons (VitoxPadReading SDL bit order)
enum : uint32_t {
    kA = 1u << 0, kB = 1u << 1, kX = 1u << 2, kY = 1u << 3, kView = 1u << 4, kMenu = 1u << 6, kL3 = 1u << 7, kR3 = 1u << 8, kLB = 1u << 9, kRB = 1u << 10,
    kDUp = 1u << 11, kDDown = 1u << 12, kDLeft = 1u << 13, kDRight = 1u << 14,
};

// Xbox face-button colours
uint32_t button_color(uint32_t bit) {
    switch (bit) {
    case kA: return 0x3E9A4F;
    case kB: return 0xB23A48;
    case kX: return 0x2F6FD0;
    case kY: return 0xC08A1E;
    default: return 0x4B2E8F;
    }
}
const wchar_t *button_glyph(uint32_t bit) {
    switch (bit) {
    case kA: return L"A";
    case kB: return L"B";
    case kX: return L"X";
    case kY: return L"Y";
    case kLB: return L"LB";
    case kRB: return L"RB";
    case kView: return L"⧉";
    case kMenu: return L"≡";
    case kDUp: return L"▲";
    case kDDown: return L"▼";
    case kDLeft: return L"◀";
    case kDRight: return L"▶";
    default: return L"?";
    }
}

std::wstring clean_title(const std::wstring &t) {
    std::wstring s;
    for (wchar_t c : t)
        if (c != L'™' && c != L'®' && c != L'©')
            s += c;
    while (!s.empty() && s.back() == L' ')
        s.pop_back();
    return s;
}

// a setting row: label, description, current value (the architecture for more settings later)
struct SettingItem {
    std::wstring label, description;
    std::function<std::wstring()> value;
};

struct AppEntry {
    VitoxAppInfo info{};
    std::wstring title, title_id, kind, version;
    com_ptr<ID2D1Bitmap> icon, hero;
    bool icon_tried = false, hero_tried = false; // requested from the art worker
    bool icon_done = false, hero_done = false;
    float scale = 0.86f, lift = 0, focus = 0; // animated
    float hue = 0;
    int art_gen = 0;                              // 0.0.26.0: bumped when custom artwork changes (reload)
    bool custom_card = false, custom_hero = false; // a SteamGridDB override is saved in LocalState
};

std::wstring hint_kind(const VitoxAppInfo &a) {
    if (strcmp(a.app_root, "homebrew") == 0)
        return L"Homebrew";
    return L"Game";
}

std::wstring pretty_version(const char *v) {
    std::string s = v;
    if (s.empty())
        return {};
    return L"v" + widen(s);
}

float title_hue(const std::string &id) {
    uint32_t h = 2166136261u;
    for (char c : id)
        h = (h ^ static_cast<uint8_t>(c)) * 16777619u;
    return (h % 1000) / 1000.0f;
}

} // namespace

struct LauncherUi::Impl {
    // ---- devices ----
    float dpi = 96.0f;
    uint32_t W = 1920, H = 1080;
    float k = 1.0f;
    com_ptr<ID3D11Device> d3d;
    com_ptr<ID3D11DeviceContext> ctx;
    com_ptr<IDXGISwapChain1> swap;
    com_ptr<ID3D11RenderTargetView> rtv;
    com_ptr<ID3D11VertexShader> vs;
    com_ptr<ID3D11PixelShader> ps;
    com_ptr<ID3D11Buffer> cb;
    com_ptr<ID2D1Factory1> d2d_factory;
    com_ptr<ID2D1Device> d2d_device;
    com_ptr<ID2D1DeviceContext> dc;
    com_ptr<ID2D1Bitmap1> target;
    com_ptr<IDWriteFactory> dw;
    com_ptr<IWICImagingFactory> wic;
    std::map<std::wstring, com_ptr<IDWriteTextFormat>> formats;
    std::unordered_map<std::wstring, com_ptr<IDWriteTextLayout>> layouts;
    com_ptr<ID2D1SolidColorBrush> brush;
    com_ptr<ID2D1StrokeStyle> round_stroke;
    bool ok = false;

    // ---- library ----
    std::vector<AppEntry> apps;
    int sel = 0;
    float scroll = 0;          // animated x of the selected card (pixels)
    float sel_anim_hue = 0;
    float parallax_x = 0;
    float change_pulse = 0;    // 1 on a selection change, decays
    AppEntry *hero_prev = nullptr;
    float hero_mix = 1;
    int hero_index = -1;
    std::mutex scan_m;
    std::vector<VitoxAppInfo> scan_result;
    bool scan_ready = false;
    std::atomic<bool> scan_busy{ false }, scan_stop{ false };
    std::thread scan_thread;
    Clock::time_point next_scan{};
    bool scanned_once = false;
    // 0.0.29.0: the last scan unfiltered (Settings > Library hides homebrew from `apps`), and the "+" tile after the last
    // card (sel == apps.size()) that opens the USB installer
    std::vector<VitoxAppInfo> all_found;
    unsigned hidden_homebrew = 0, shown_homebrew = 0;
    float plus_focus = 0;
    std::string pending_select; // a title just installed: selected once a scan lists it
    Clock::time_point pending_until{};
    bool on_plus() const { return sel >= static_cast<int>(apps.size()); }

    // ---- navigation ----
    enum class Tab { Library, Settings };
    enum class Zone { Tabs, Content };
    Tab tab = Tab::Library;
    Zone zone = Zone::Content;
    float tab_anim = 0;     // 0 Library .. 1 Settings (indicator)
    float content_anim = 1; // tab switch fade
    int setting_sel = 0;
    std::vector<SettingItem> settings;
    uint32_t prev_buttons = 0;
    bool armed = false; // every button released once since the launcher appeared
    uint32_t held_dir = 0;
    Clock::time_point dir_next{};
    int dir_repeats = 0;
    VitoxPadReading pad{}; // this frame's raw reading (the Input page draws it)

    // ---- 0.0.26.0 settings pages ----
    enum class Page { Menu, Input, Artwork, Browser, Graphics, Stats, Shaders, Library };
    Page page = Page::Menu;
    float page_anim = 1;
    int menu_sel = 0; // kMenuInput, kMenuGraphics, kMenuShaders, kMenuStats, kMenuLibrary, kMenuArtwork, kMenuAbout
    static constexpr int kMenuInput = 0, kMenuGraphics = 1, kMenuShaders = 2, kMenuStats = 3, kMenuLibrary = 4, kMenuArtwork = 5, kMenuAbout = 6,
                         kMenuRows = 7;
    int lib_force = -1; // prerender only: draw the Library page as if homebrew were shown (1) / hidden (0)
    // ---- 0.0.29.0 the USB installer (a modal over the library) ----
    std::unique_ptr<install::Installer> inst;
    install::Snapshot is;
    uint64_t is_ver = 0;
    bool inst_open = false;
    int inst_sel = 0;
    float inst_scroll = 0, inst_anim = 0;
    // ---- 0.0.33.0 first-run firmware setup (a modal shown while the firmware is not installed) ----
    std::unique_ptr<firmware::Setup> fw;
    firmware::Snapshot fws;
    uint64_t fws_ver = 0;
    bool fw_open = false;
    float fw_anim = 0;
    bool fw_busy() const { return fw_open && (fws.phase == firmware::Phase::Scanning || fws.phase == firmware::Phase::Copying || fws.phase == firmware::Phase::Installing); }
    void fw_buttons(uint32_t edge) {
        using firmware::Phase;
        if (fw_busy())
            return;
        if (edge & kA) {
            if (fws.phase == Phase::Done)
                fw_open = false;
            else
                fw->start();
        } else if (edge & kB) {
            fw_open = false; // skipped: shown again at the next start
        }
    }
    void fw_update(float dt) {
        if (!fw_open || !fw)
            return;
        fw_anim = std::min(1.0f, fw_anim + dt / 0.25f);
        if (fw->poll(fws_ver, fws))
            fws_ver = fws.version;
    }
    void draw_fw_modal(float a) {
        if (!fw_open)
            return;
        using firmware::Phase;
        const float t = ease_out(fw_anim), ma = a * t;
        brush->SetColor(rgba(0x05010C, 0.80f * ma));
        dc->FillRectangle(R(0, 0, 1920, 1080), brush.get());
        const float x0 = 260, x1 = 1660, y0 = 110 + (1 - t) * 30, y1 = 940 + (1 - t) * 30;
        fill_gradient_round(x0, y0, x1, y1, 34, rgba(0x2A1458, 0.98f * ma), rgba(0x170A30, 0.98f * ma));
        stroke_round(x0, y0, x1, y1, 34, rgba(0xC9A2FF, 0.5f * ma), 1.5f);
        text(L"WELCOME TO VITOX", x0 + 48, y0 + 34, 16, DWRITE_FONT_WEIGHT_BOLD, rgba(0xB07CFF, ma), 600, 5);
        text(L"Install the PS Vita firmware", x0 + 48, y0 + 62, 36, DWRITE_FONT_WEIGHT_SEMI_BOLD, rgba(0xFFFFFF, ma), 1100);
        text(L"Vitox needs these three firmware files to run games correctly. Copy the .PUP files to a USB drive, connect it to the "
             L"console and press A.",
            x0 + 48, y0 + 120, 21, DWRITE_FONT_WEIGHT_NORMAL, rgba(0xCDB8F5, ma), 1300, 0, true);
        float ry = y0 + 200;
        for (int i = 0; i < firmware::kPackages; ++i, ry += 112) {
            const firmware::PackageInfo &p = firmware::package(i);
            row_frame(x0 + 40, ry, x1 - 40, ry + 96, false, ma, 20);
            const bool have = fws.have[i];
            glyph_badge(x0 + 92, ry + 48, 24, have ? L"✓" : std::to_wstring(i + 1), have ? 0x3E9A4F : 0x6B2FE6, ma);
            text(p.name, x0 + 140, ry + 16, 26, DWRITE_FONT_WEIGHT_SEMI_BOLD, rgba(0xFFFFFF, ma), 900);
            text(p.detail, x0 + 140, ry + 54, 19, DWRITE_FONT_WEIGHT_NORMAL, rgba(0x8F7DB8, ma), 1000);
            const std::wstring st = have ? L"READY" : L"NEEDED";
            text(st, x1 - 80 - measure(st, 17, DWRITE_FONT_WEIGHT_BOLD, 3), ry + 36, 17, DWRITE_FONT_WEIGHT_BOLD, rgba(have ? 0x8FE3A0 : 0xF2C46B, ma), 300, 3);
        }
        const float my = ry + 30;
        if (fw_busy()) {
            spinner(x0 + 80, my + 22, 18, ma);
            std::wstring m = fws.message;
            if (fws.phase == Phase::Copying && !fws.current.empty()) {
                wchar_t pct[32];
                swprintf_s(pct, L"  %d%%", fws.total_bytes ? static_cast<int>(fws.done_bytes * 100 / fws.total_bytes) : 0);
                m += L"  " + fws.current + pct;
            }
            text(m, x0 + 120, my + 8, 22, DWRITE_FONT_WEIGHT_NORMAL, rgba(0xEADFFF, ma), 1300);
        } else if (!fws.message.empty()) {
            text(fws.message, x0 + 48, my + 8, 22, DWRITE_FONT_WEIGHT_SEMI_BOLD, rgba(fws.phase == Phase::Done ? 0x8FE3A0 : 0xF2C46B, ma), 1300, 0, true);
        }
    }
    // ---- 0.0.30.0 the game menu (Menu on a library card): per-game settings + uninstall ----
    bool gm_open = false;
    int gm_row = 0;
    float gm_anim = 0;
    VitoxAppInfo gm_app{};
    std::wstring gm_title;
    GameConfig gm_cfg;
    std::shared_ptr<std::atomic<int64_t>> gm_size; // measured off the UI thread (-1 while measuring)
    bool gm_confirm = false;      // the uninstall confirmation is up
    std::thread gm_thread;
    std::atomic<int> gm_state{ 0 }; // 0 idle, 1 uninstalling, 2 finished (taken by gm_update)
    std::wstring gm_error;
    uint64_t gm_freed = 0;
    bool gm_shared = false;
    static constexpr int kGmRows = 6; // resolution, async, pre-warm, overlay, reset, uninstall
    // ---- 0.1.1.0 Settings > Library: the USB install folder (row 1) and its folder picker ----
    int lib_row = 0; // 0 = Show Homebrew, 1 = USB Install Folder
    bool fp_open = false;
    float fp_anim = 0;
    std::unique_ptr<install::FolderBrowser> fp;
    install::FolderView fpv;
    int fp_sel = 0, fp_scroll = 0;
    std::wstring fp_return; // after "up": the entry to select again (the folder we came from)
    int shader_row = 0; // 0.0.28.3 Shader Compilation page: 0 = asynchronous compilation, 1 = pre-warm at launch
    int stats_row = 0; // 0.0.28.0 Statistics page: 0 = the toggle, 1 = the display mode
    float gfx_thumb = -1; // animated slider thumb position 0..1 (-1 = snap on first draw)
    float gfx_bump = 0;   // 1 when an adjustment hits an end of the range, decays
    std::wstring local_state, version;
    // Input page
    InputConfig icfg{};
    int input_sel = 0;
    float input_scroll = 0;
    float b_hold = 0; // B held on the Input page (hold to go back)
    static constexpr int kInputRows = 12 + 3; // 12 buttons, 2 dead zones, restore defaults
    // Artwork page
    std::unique_ptr<sgdb::Client> sgdb_client;
    std::wstring sgdb_key;
    enum class KeyState { Unset, Checking, Valid, Invalid, Offline } key_state = KeyState::Unset;
    std::wstring key_msg;
    sgdb::Handle key_check;
    int art_sel = 0; // 0 = the key row, 1.. = apps
    float art_scroll = 0;
    // text entry (system keyboard through host_text.cpp)
    enum class EditTarget { None, Key, Search } editing = EditTarget::None;
    std::wstring edit_text;
    float edit_t = 0;
    bool edit_seen_visible = false;
    // artwork browser
    struct Thumb {
        sgdb::Handle h;
        com_ptr<ID2D1Bitmap> bmp;
        bool failed = false;
    };
    struct Browser {
        std::wstring tid; // the app being edited (by TITLE_ID: a library rescan may reorder the apps)
        std::wstring term;
        sgdb::Handle search_h, images_h, apply_h;
        std::vector<sgdb::Game> games;
        int game_sel = 0;
        sgdb::Kind kind = sgdb::Kind::Card;
        std::map<std::pair<int64_t, int>, std::vector<sgdb::Image>> cache;
        std::vector<sgdb::Image> images;
        bool images_ready = false;
        int sel = 0;
        float scroll = 0;
        bool zone_games = false;
        std::unordered_map<std::wstring, Thumb> thumbs;
        int apply_index = -1;
        sgdb::Kind apply_kind = sgdb::Kind::Card;
        std::wstring status;
        bool load_pending = false;
        Clock::time_point load_at{};
        std::wstring in_use[2]; // source URL of the saved card / hero
    } br;

    // ---- transitions ----
    float intro = 0;       // 0..1 fade-in
    bool launching = false;
    float launch_t = 0;    // 0..1 launch animation
    float time_s = 0;
    std::wstring toast;
    float toast_t = -1;
    Clock::time_point t0 = Clock::now(), last = Clock::now();
    uint64_t frames = 0;
    double frame_ms_max = 0;

    ~Impl() {
        stop_scan();
        art_end();
        if (gm_thread.joinable())
            gm_thread.join();
    }

    // ================================================================ devices
    bool init(CoreWindow const &window) {
        dpi = winrt::Windows::Graphics::Display::DisplayInformation::GetForCurrentView().LogicalDpi();
        const auto b = window.Bounds();
        W = static_cast<uint32_t>(std::max(1.0f, b.Width * dpi / 96.0f));
        H = static_cast<uint32_t>(std::max(1.0f, b.Height * dpi / 96.0f));
        k = H / 1080.0f;
        const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
            d3d.put(), nullptr, ctx.put());
        com_ptr<IDXGIFactory2> factory;
        if (SUCCEEDED(hr)) {
            com_ptr<IDXGIAdapter> adapter;
            d3d.as<IDXGIDevice1>()->GetAdapter(adapter.put());
            adapter->GetParent(__uuidof(IDXGIFactory2), factory.put_void());
            DXGI_SWAP_CHAIN_DESC1 sd{};
            sd.Width = W;
            sd.Height = H;
            sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            sd.SampleDesc.Count = 1;
            sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            sd.BufferCount = 2;
            sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
            sd.Scaling = DXGI_SCALING_STRETCH;
            sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
            hr = factory->CreateSwapChainForCoreWindow(d3d.get(), winrt::get_unknown(window), &sd, nullptr, swap.put());
        }
        com_ptr<ID3D11Texture2D> bb;
        if (SUCCEEDED(hr))
            hr = swap->GetBuffer(0, __uuidof(ID3D11Texture2D), bb.put_void());
        if (SUCCEEDED(hr))
            hr = d3d->CreateRenderTargetView(bb.get(), nullptr, rtv.put());
        if (SUCCEEDED(hr))
            hr = d3d->CreateVertexShader(kVitoxLauncherBgVs, sizeof(kVitoxLauncherBgVs), nullptr, vs.put());
        if (SUCCEEDED(hr))
            hr = d3d->CreatePixelShader(kVitoxLauncherBgPs, sizeof(kVitoxLauncherBgPs), nullptr, ps.put());
        if (SUCCEEDED(hr)) {
            D3D11_BUFFER_DESC bd{ sizeof(BgConstants), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER };
            hr = d3d->CreateBuffer(&bd, nullptr, cb.put());
        }
        D2D1_FACTORY_OPTIONS opts{};
        if (SUCCEEDED(hr))
            hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &opts, d2d_factory.put_void());
        if (SUCCEEDED(hr))
            hr = d2d_factory->CreateDevice(d3d.as<IDXGIDevice>().get(), d2d_device.put());
        if (SUCCEEDED(hr))
            hr = d2d_device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, dc.put());
        if (SUCCEEDED(hr)) {
            com_ptr<IDXGISurface> surface;
            hr = swap->GetBuffer(0, __uuidof(IDXGISurface), surface.put_void());
            const D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96.0f, 96.0f);
            if (SUCCEEDED(hr))
                hr = dc->CreateBitmapFromDxgiSurface(surface.get(), &props, target.put());
        }
        if (SUCCEEDED(hr)) {
            dc->SetTarget(target.get());
            dc->SetDpi(96.0f, 96.0f);
            dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
            hr = dc->CreateSolidColorBrush(rgba(0xFFFFFF), brush.put());
        }
        if (SUCCEEDED(hr))
            hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<::IUnknown **>(dw.put()));
        if (SUCCEEDED(hr))
            hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IWICImagingFactory), wic.put_void());
        if (SUCCEEDED(hr))
            hr = d2d_factory->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND),
                nullptr, 0, round_stroke.put());
        if (SUCCEEDED(hr)) {
            warm_up();
            art_start();
        }
        log_line(Log::Lifecycle, "launcher ui: init %ux%u dpi=%.0f (D3D11 background shader + Direct2D) hr=0x%08lX", W, H, dpi, static_cast<unsigned long>(hr));
        ok = SUCCEEDED(hr);
        return ok;
    }

    void release() {
        stop_scan();
        art_end();
        inst.reset(); // (an open installer blocks launching; this only matters when the window closes)
        inst_open = false;
        fp.reset();
        fp_open = false;
        for (auto &a : apps)
            a.icon = a.hero = nullptr, a.icon_tried = a.hero_tried = a.icon_done = a.hero_done = false;
        layouts.clear();
        formats.clear();
        if (dc)
            dc->SetTarget(nullptr);
        target = nullptr;
        brush = nullptr;
        round_stroke = nullptr;
        dc = nullptr;
        d2d_device = nullptr;
        d2d_factory = nullptr;
        rtv = nullptr;
        cb = nullptr;
        vs = nullptr;
        ps = nullptr;
        swap = nullptr;
        if (ctx) {
            ctx->ClearState();
            ctx->Flush();
        }
        ctx = nullptr;
        d3d = nullptr;
        wic = nullptr;
        dw = nullptr;
        ok = false;
        log_line(Log::Lifecycle, "launcher ui: released (%llu frames, worst frame %.1f ms)", static_cast<unsigned long long>(frames), frame_ms_max);
    }

    // ================================================================ library discovery
    void start_scan() {
        if (scan_busy.exchange(true))
            return;
        if (scan_thread.joinable())
            scan_thread.join();
        scan_thread = std::thread([this] {
            std::vector<VitoxAppInfo> found(256);
            const unsigned n = vitox_launcher_discover(kDevRoot, found.data(), static_cast<unsigned>(found.size()));
            found.resize(std::min<unsigned>(n, 256));
            {
                std::lock_guard<std::mutex> lock(scan_m);
                scan_result = std::move(found);
                scan_ready = true;
            }
            scan_busy = false;
        });
    }
    void stop_scan() {
        if (scan_thread.joinable())
            scan_thread.join();
        scan_busy = false;
    }

    // apply a finished scan: keeps the selection on the same app when it still exists
    void take_scan(const std::string &preselect) {
        std::vector<VitoxAppInfo> found;
        {
            std::lock_guard<std::mutex> lock(scan_m);
            if (!scan_ready)
                return;
            scan_ready = false;
            found = std::move(scan_result);
        }
        const bool first = !scanned_once;
        scanned_once = true;
        all_found = std::move(found);
        apply_library(first, preselect);
    }

    // 0.0.29.0: `apps` = the last scan minus what Settings > Library hides (homebrew); called by a scan and by the toggle
    void apply_library(bool first, const std::string &preselect) {
        const bool show_hb = library_show_homebrew();
        std::vector<VitoxAppInfo> found;
        hidden_homebrew = shown_homebrew = 0;
        for (const VitoxAppInfo &f : all_found) {
            const bool hb = strcmp(f.app_root, "homebrew") == 0;
            if (hb && !show_hb) {
                ++hidden_homebrew;
                continue;
            }
            shown_homebrew += hb;
            found.push_back(f);
        }
        if (!pending_select.empty() && Clock::now() > pending_until)
            pending_select.clear();
        const auto select_pending = [&] {
            for (size_t i = 0; i < apps.size(); ++i)
                if (!pending_select.empty() && pending_select == apps[i].info.title_id) {
                    sel = static_cast<int>(i);
                    change_pulse = 1;
                    pending_select.clear();
                    return;
                }
        };
        bool same = found.size() == apps.size();
        for (size_t i = 0; same && i < found.size(); ++i)
            same = strcmp(found[i].title_id, apps[i].info.title_id) == 0 && strcmp(found[i].folder, apps[i].info.folder) == 0
                && strcmp(found[i].app_root, apps[i].info.app_root) == 0 && strcmp(found[i].title, apps[i].info.title) == 0;
        if (same) {
            select_pending(); // a reinstall: the list did not change
            return;
        }
        const bool was_plus = !first && !apps.empty() && on_plus();
        const std::string keep = first ? preselect : (sel >= 0 && sel < static_cast<int>(apps.size()) ? std::string(apps[sel].info.title_id) : std::string());
        std::vector<AppEntry> next;
        for (const VitoxAppInfo &f : found) {
            AppEntry e;
            // reuse the decoded artwork of an app that stays
            for (AppEntry &old : apps)
                if (strcmp(old.info.title_id, f.title_id) == 0 && strcmp(old.info.folder, f.folder) == 0) {
                    e = old;
                    break;
                }
            e.info = f;
            e.title = widen(f.title[0] ? f.title : f.title_id);
            // 0.0.26.1: param.sfo titles may hold line breaks (e.g. a subtitle on its own line): one line in the UI
            for (wchar_t &c : e.title)
                if (c == L'\n' || c == L'\r')
                    c = L' ';
            while (e.title.find(L"  ") != std::wstring::npos)
                e.title.replace(e.title.find(L"  "), 2, L" ");
            e.title_id = widen(f.title_id);
            e.kind = hint_kind(f);
            e.version = pretty_version(f.app_ver);
            e.hue = title_hue(f.title_id);
            refresh_custom(e);
            next.push_back(std::move(e));
        }
        apps = std::move(next);
        preload_i = 0;
        hero_prev = nullptr;
        hero_index = -1;
        int found_sel = -1;
        for (size_t i = 0; i < apps.size(); ++i)
            if (!keep.empty() && (keep == apps[i].info.title_id || keep == apps[i].info.folder))
                found_sel = static_cast<int>(i);
        sel = found_sel >= 0 ? found_sel : was_plus ? static_cast<int>(apps.size()) : std::clamp(sel, 0, std::max(0, static_cast<int>(apps.size()) - 1));
        select_pending();
        if (first)
            scroll = target_scroll();
        log_line(Log::Lifecycle, "launcher ui: library %u app(s)%s, %u homebrew hidden", static_cast<unsigned>(apps.size()), first ? " (initial scan)" : " (changed)",
            hidden_homebrew);
        for (const AppEntry &a : apps)
            log_line(Log::Lifecycle, "launcher ui:   %s '%s' (%s/%s)", a.info.title_id, a.info.title, a.info.app_root, a.info.folder);
    }

    // ---- artwork: decoded (and downscaled) on a worker thread into WIC bitmaps; the UI thread only uploads them ----
    struct ArtJob {
        std::wstring key;
        std::vector<std::wstring> paths; // the first one that decodes wins
        uint32_t max_w;
    };
    std::mutex art_m;
    std::condition_variable art_cv;
    std::deque<ArtJob> art_jobs;
    std::unordered_map<std::wstring, com_ptr<IWICBitmap>> art_done; // null = no artwork
    std::thread art_thread;
    bool art_stop = false;

    com_ptr<IWICBitmap> decode(IWICImagingFactory *f, const std::wstring &path, uint32_t max_w) {
        com_ptr<IWICBitmap> out;
        const DWORD attr = GetFileAttributesW(path.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY))
            return out;
        com_ptr<IWICBitmapDecoder> dec;
        com_ptr<IWICBitmapFrameDecode> frame;
        com_ptr<IWICFormatConverter> conv;
        if (FAILED(f->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, dec.put())) || FAILED(dec->GetFrame(0, frame.put())))
            return out;
        com_ptr<IWICBitmapSource> src = frame;
        UINT fw = 0, fh = 0;
        frame->GetSize(&fw, &fh);
        com_ptr<IWICBitmapScaler> scaler;
        if (fw > max_w && SUCCEEDED(f->CreateBitmapScaler(scaler.put()))
            && SUCCEEDED(scaler->Initialize(frame.get(), max_w, std::max<UINT>(1, fh * max_w / fw), WICBitmapInterpolationModeFant)))
            src = scaler;
        if (FAILED(f->CreateFormatConverter(conv.put()))
            || FAILED(conv->Initialize(src.get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeMedianCut)))
            return out;
        f->CreateBitmapFromSource(conv.get(), WICBitmapCacheOnLoad, out.put()); // decodes here, on the worker
        return out;
    }
    void art_start() {
        art_stop = false;
        art_thread = std::thread([this] {
            const HRESULT ci = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            com_ptr<IWICImagingFactory> f;
            CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IWICImagingFactory), f.put_void());
            for (;;) {
                ArtJob job;
                {
                    std::unique_lock<std::mutex> lock(art_m);
                    art_cv.wait(lock, [this] { return art_stop || !art_jobs.empty(); });
                    if (art_stop)
                        break;
                    job = std::move(art_jobs.front());
                    art_jobs.pop_front();
                }
                com_ptr<IWICBitmap> bmp;
                for (const std::wstring &p : job.paths)
                    if (f && !bmp)
                        bmp = decode(f.get(), p, job.max_w);
                std::lock_guard<std::mutex> lock(art_m);
                art_done[job.key] = bmp;
            }
            f = nullptr;
            if (SUCCEEDED(ci))
                CoUninitialize();
        });
    }
    void art_end() {
        {
            std::lock_guard<std::mutex> lock(art_m);
            art_stop = true;
            art_jobs.clear();
        }
        art_cv.notify_all();
        if (art_thread.joinable())
            art_thread.join();
        art_done.clear();
    }
    // requests once; true + bitmap (may be null) when the worker has finished it
    bool art_get(const std::wstring &key, std::vector<std::wstring> paths, uint32_t max_w, bool urgent, bool &requested, com_ptr<ID2D1Bitmap> &out) {
        std::lock_guard<std::mutex> lock(art_m);
        auto it = art_done.find(key);
        if (it != art_done.end()) {
            com_ptr<ID2D1Bitmap> bmp;
            if (it->second)
                dc->CreateBitmapFromWicBitmap(it->second.get(), nullptr, bmp.put());
            ++perf_bitmaps;
            out = bmp; // replaces the previous artwork (0.0.26.0: custom art reloads)
            art_done.erase(it);
            return true;
        }
        if (!requested) {
            requested = true;
            ArtJob j{ key, std::move(paths), max_w };
            if (urgent)
                art_jobs.push_front(std::move(j));
            else
                art_jobs.push_back(std::move(j));
            art_cv.notify_one();
        }
        return false;
    }
    // 0.0.26.6: every app's card and background are decoded and uploaded ahead of time, one texture per frame, so no page
    // has to upload artwork when it opens (a 1280-px upload on the Artwork page dropped a frame)
    size_t preload_i = 0;
    void preload_tick() {
        if (preload_i >= apps.size())
            return;
        AppEntry &e = apps[preload_i];
        const int before = perf_bitmaps;
        if (!e.icon_done)
            ensure_art(e, false);
        if (perf_bitmaps != before)
            return; // one upload per frame
        if (!e.hero_done)
            ensure_art(e, true);
        if (e.icon_done && e.hero_done)
            ++preload_i;
    }
    void ensure_art(AppEntry &a, bool hero) {
        const std::wstring dir = a.info.dir;
        const std::wstring gen = L"|" + std::to_wstring(a.art_gen);
        // 0.0.26.0: a custom (SteamGridDB) image saved in LocalState wins over the game's own artwork
        if (!a.icon_done)
            a.icon_done = art_get(dir + L"|icon" + gen, { art_file(a, sgdb::Kind::Card), dir + L"\\sce_sys\\icon0.png" }, 512, false, a.icon_tried, a.icon);
        if (hero && !a.hero_done)
            a.hero_done = art_get(dir + L"|hero" + gen,
                { art_file(a, sgdb::Kind::Hero), dir + L"\\sce_sys\\pic0.png", dir + L"\\sce_sys\\livearea\\contents\\bg0.png",
                    dir + L"\\sce_sys\\livearea\\contents\\bg.png" },
                1280, true, a.hero_tried, a.hero);
    }

    // ---- 0.0.26.0 custom artwork files: LocalState\artwork\<TITLE_ID>\card.art | hero.art (+ .src = the source URL) ----
    std::wstring art_file(const AppEntry &a, sgdb::Kind k) const {
        if (local_state.empty() || a.title_id.empty())
            return {};
        return local_state + L"\\artwork\\" + a.title_id + (k == sgdb::Kind::Card ? L"\\card.art" : L"\\hero.art");
    }
    static bool file_exists(const std::wstring &p) {
        const DWORD attr = p.empty() ? INVALID_FILE_ATTRIBUTES : GetFileAttributesW(p.c_str());
        return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
    }
    void refresh_custom(AppEntry &a) {
        a.custom_card = file_exists(art_file(a, sgdb::Kind::Card));
        a.custom_hero = file_exists(art_file(a, sgdb::Kind::Hero));
    }
    void art_changed(AppEntry &a) {
        ++a.art_gen;
        a.icon_done = a.icon_tried = a.hero_done = a.hero_tried = false;
        refresh_custom(a);
    }
    std::wstring read_src(const AppEntry &a, sgdb::Kind k) const {
        std::wifstream f(art_file(a, k) + L".src");
        std::wstring s;
        std::getline(f, s);
        return s;
    }

    // ================================================================ text
    IDWriteTextFormat *format(float size, DWRITE_FONT_WEIGHT weight, const wchar_t *family = L"Segoe UI") {
        wchar_t key[96];
        swprintf_s(key, L"%s|%.1f|%d", family, size, static_cast<int>(weight));
        auto it = formats.find(key);
        if (it != formats.end())
            return it->second.get();
        com_ptr<IDWriteTextFormat> f;
        dw->CreateTextFormat(family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", f.put());
        if (f) {
            f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            DWRITE_TRIMMING trim{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
            com_ptr<IDWriteInlineObject> ellipsis;
            dw->CreateEllipsisTrimmingSign(f.get(), ellipsis.put());
            f->SetTrimming(&trim, ellipsis.get());
        }
        ++perf_formats;
        return (formats[key] = f).get();
    }
    IDWriteTextLayout *layout(const std::wstring &s, float size, DWRITE_FONT_WEIGHT weight, float max_w, float spacing = 0, bool wrap = false,
        const wchar_t *family = L"Segoe UI") {
        wchar_t keyb[128];
        swprintf_s(keyb, L"|%s|%.1f|%d|%.0f|%.1f|%d", family, size, static_cast<int>(weight), max_w, spacing, wrap ? 1 : 0);
        const std::wstring key = s + keyb;
        auto it = layouts.find(key);
        if (it != layouts.end())
            return it->second.get();
        if (layouts.size() > 600)
            layouts.clear();
        com_ptr<IDWriteTextLayout> l;
        IDWriteTextFormat *f = format(size, weight, family);
        if (f && SUCCEEDED(dw->CreateTextLayout(s.c_str(), static_cast<UINT32>(s.size()), f, max_w, size * 3, l.put()))) {
            if (wrap)
                l->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
            if (spacing != 0)
                if (auto l1 = l.try_as<IDWriteTextLayout1>())
                    l1->SetCharacterSpacing(0, spacing, 0, { 0, static_cast<UINT32>(s.size()) });
        }
        ++perf_layouts;
        if (perf_new.size() < 600)
            perf_new += narrow(key.substr(0, 48)) + " | ";
        return (layouts[key] = l).get();
    }
    float text_width(IDWriteTextLayout *l) {
        DWRITE_TEXT_METRICS m{};
        if (l)
            l->GetMetrics(&m);
        return m.widthIncludingTrailingWhitespace;
    }
    // draws text with its top-left at (x, y); returns the drawn width
    float text(const std::wstring &s, float x, float y, float size, DWRITE_FONT_WEIGHT weight, D2D1_COLOR_F color, float max_w = 1800, float spacing = 0,
        bool wrap = false, const wchar_t *family = L"Segoe UI") {
        IDWriteTextLayout *l = layout(s, size * k, weight, max_w * k, spacing * k, wrap, family);
        if (!l)
            return 0;
        brush->SetColor(color);
        dc->DrawTextLayout({ x * k, y * k }, l, brush.get());
        return text_width(l) / k;
    }
    float measure(const std::wstring &s, float size, DWRITE_FONT_WEIGHT weight, float spacing = 0, const wchar_t *family = L"Segoe UI") {
        return text_width(layout(s, size * k, weight, 1800 * k, spacing * k, false, family)) / k;
    }

    // ================================================================ shapes (layout units: 1920x1080 reference, scaled by k)
    D2D1_RECT_F R(float x0, float y0, float x1, float y1) const { return D2D1::RectF(x0 * k, y0 * k, x1 * k, y1 * k); }
    void fill_round(float x0, float y0, float x1, float y1, float r, D2D1_COLOR_F c) {
        brush->SetColor(c);
        dc->FillRoundedRectangle({ R(x0, y0, x1, y1), r * k, r * k }, brush.get());
    }
    void stroke_round(float x0, float y0, float x1, float y1, float r, D2D1_COLOR_F c, float w) {
        brush->SetColor(c);
        dc->DrawRoundedRectangle({ R(x0, y0, x1, y1), r * k, r * k }, brush.get(), w * k);
    }
    void fill_gradient_round(float x0, float y0, float x1, float y1, float r, D2D1_COLOR_F a, D2D1_COLOR_F b, bool vertical = true) {
        com_ptr<ID2D1GradientStopCollection> stops;
        const D2D1_GRADIENT_STOP gs[] = { { 0.0f, a }, { 1.0f, b } };
        dc->CreateGradientStopCollection(gs, 2, stops.put());
        com_ptr<ID2D1LinearGradientBrush> g;
        dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({ x0 * k, y0 * k }, vertical ? D2D1::Point2F(x0 * k, y1 * k) : D2D1::Point2F(x1 * k, y0 * k)),
            stops.get(), g.put());
        dc->FillRoundedRectangle({ R(x0, y0, x1, y1), r * k, r * k }, g.get());
    }
    // a controller button glyph + label; returns the width used
    float button_hint(float x, float y, const wchar_t *glyph, const std::wstring &label, uint32_t ring, float alpha = 1.0f) {
        const float r = 17;
        brush->SetColor(rgba(ring, 0.95f * alpha));
        const bool wide = wcslen(glyph) > 1;
        if (wide)
            dc->FillRoundedRectangle({ R(x, y, x + 2 * r + 14, y + 2 * r), r * k, r * k }, brush.get());
        else
            dc->FillEllipse(D2D1::Ellipse({ (x + r) * k, (y + r) * k }, r * k, r * k), brush.get());
        const float gw = wide ? 2 * r + 14 : 2 * r;
        const float tw = measure(glyph, wide ? 15.0f : 19.0f, DWRITE_FONT_WEIGHT_BOLD);
        text(glyph, x + (gw - tw) / 2, y + (wide ? 7.0f : 4.5f), wide ? 15.0f : 19.0f, DWRITE_FONT_WEIGHT_BOLD, rgba(0xFFFFFF, alpha));
        const float lw = text(label, x + gw + 12, y + 3, 22, DWRITE_FONT_WEIGHT_SEMI_BOLD, rgba(0xD9C9FF, alpha));
        return gw + 12 + lw + 34;
    }

    // ================================================================ layout
    static constexpr float kCard = 300, kCardSel = 372, kGap = 34, kRowY = 236, kLeft = 150;
    float card_x(int i) const {
        // selected card left-aligned at kLeft; neighbours spaced by their animated widths
        float x = 0;
        for (int j = 0; j < i; ++j)
            x += kCard * apps[j].scale / 0.86f * 0.86f + kGap + (kCardSel - kCard) * apps[j].focus;
        return x;
    }
    float target_scroll() const {
        float x = 0;
        for (int j = 0; j < sel; ++j)
            x += kCard + kGap;
        return x;
    }

    // ================================================================ input
    void move(int dx, int dy) {
        if (fw_open)
            return;
        if (inst_open) {
            install_move(dy);
            return;
        }
        if (gm_open) {
            gm_move(dx, dy);
            return;
        }
        if (fp_open) {
            fp_move(dy);
            return;
        }
        if (zone == Zone::Tabs) {
            if (dx) {
                const Tab t = dx > 0 ? Tab::Settings : Tab::Library;
                if (t != tab)
                    switch_tab(t);
            }
            if (dy > 0)
                zone = Zone::Content;
            return;
        }
        if (tab == Tab::Library) {
            if (dy < 0) {
                zone = Zone::Tabs;
            } else if (dx) {
                const int n = std::clamp(sel + dx, 0, static_cast<int>(apps.size())); // apps.size() = the "+" tile
                if (n != sel) {
                    sel = n;
                    change_pulse = 1;
                }
            }
            return;
        }
        switch (page) {
        case Page::Menu:
            if (dy < 0 && menu_sel == 0)
                zone = Zone::Tabs;
            else if (dy)
                menu_sel = std::clamp(menu_sel + dy, 0, kMenuRows - 1);
            break;
        case Page::Input:
            if (dy)
                input_sel = std::clamp(input_sel + dy, 0, kInputRows - 1);
            else if (dx)
                input_change(dx);
            break;
        case Page::Artwork:
            if (dy)
                art_sel = std::clamp(art_sel + dy, 0, static_cast<int>(apps.size()));
            break;
        case Page::Browser:
            browser_move(dx, dy);
            break;
        case Page::Graphics:
            if (dx)
                graphics_adjust(dx);
            break;
        case Page::Shaders:
            if (dy)
                shader_row = std::clamp(shader_row + dy, 0, 1);
            else if (dx)
                shaders_set(shader_row, dx > 0); // right = ON, left = OFF
            break;
        case Page::Library:
            if (dy)
                lib_row = std::clamp(lib_row + dy, 0, 1);
            else if (dx && lib_row == 0)
                library_set(dx > 0); // right = ON, left = OFF
            break;
        case Page::Stats:
            if (dy)
                stats_row = std::clamp(stats_row + dy, 0, 1);
            else if (dx && stats_row == 0)
                stats_set_enabled(dx > 0); // right = ON, left = OFF
            else if (dx)
                stats_set_mode(dx > 0 ? StatsMode::Full : StatsMode::FpsOnly);
            break;
        }
    }
    void switch_tab(Tab t) {
        tab = t;
        content_anim = 0;
        if (t == Tab::Settings)
            open_page(Page::Menu);
        log_line(Log::Lifecycle, "launcher ui: tab %s", t == Tab::Library ? "Library" : "Settings");
    }
    void open_page(Page p) {
        if (page == Page::Browser && p != Page::Browser)
            browser_close();
        page = p;
        page_anim = 0;
        content_anim = 0;
        b_hold = 0;
        if (p == Page::Input) {
            icfg = input_config();
            input_sel = 0;
        }
        if (p == Page::Artwork)
            for (AppEntry &a : apps)
                refresh_custom(a);
        if (p == Page::Graphics)
            gfx_thumb = -1;
        if (p == Page::Stats)
            stats_row = 0;
        if (p == Page::Shaders)
            shader_row = 0;
        static const char *const kNames[] = { "Menu", "Input", "Artwork", "Browser", "Graphics", "Stats", "Shaders", "Library" };
        log_line(Log::Lifecycle, "launcher ui: settings page %s", kNames[static_cast<int>(p)]);
    }

    // returns true when the selected app should launch
    bool input(float dt) {
        VitoxPadReading p{};
        host_pad_poll(&p);
        pad = p;
        if (editing != EditTarget::None) {
            edit_poll(dt);
            prev_buttons = p.buttons;
            return false;
        }
        uint32_t b = p.buttons;
        // the Input page shows the sticks live: there they do not navigate (the D-pad does)
        const bool live = tab == Tab::Settings && zone == Zone::Content && page == Page::Input;
        if (!live) {
            // the left stick as a d-pad (with hysteresis)
            const float th = (held_dir & (kDLeft | kDRight | kDUp | kDDown)) ? 0.35f : 0.55f;
            if (p.lx > th)
                b |= kDRight;
            else if (p.lx < -th)
                b |= kDLeft;
            if (p.ly > th)
                b |= kDDown;
            else if (p.ly < -th)
                b |= kDUp;
        }
        if (!armed) {
            // nothing held from the game or the quick menu may act here: wait for a full release
            if ((b & (kA | kB | kX | kY | kLB | kRB | kDUp | kDDown | kDLeft | kDRight)) == 0)
                armed = true;
            prev_buttons = b;
            return false;
        }
        const uint32_t edge = b & ~prev_buttons;
        prev_buttons = b;
        // directions: press, then auto-repeat (faster the longer it is held)
        const uint32_t dirs = b & (kDLeft | kDRight | kDUp | kDDown);
        const auto now = Clock::now();
        uint32_t fire = 0;
        if (dirs != held_dir) {
            held_dir = dirs;
            fire = dirs & edge;
            dir_repeats = 0;
            dir_next = now + std::chrono::milliseconds(380);
        } else if (dirs && now >= dir_next) {
            fire = dirs;
            ++dir_repeats;
            dir_next = now + std::chrono::milliseconds(dir_repeats > 6 ? 70 : 110);
        }
        if (fire & kDLeft)
            move(-1, 0);
        if (fire & kDRight)
            move(1, 0);
        if (fire & kDUp)
            move(0, -1);
        if (fire & kDDown)
            move(0, 1);
        if (fw_open) {
            fw_buttons(edge);
            return false;
        }
        if (inst_open) {
            install_buttons(edge);
            return false;
        }
        if (gm_open) {
            gm_buttons(edge);
            return false;
        }
        if (fp_open) {
            fp_buttons(edge);
            return false;
        }
        // 0.0.30.0: Menu on a library card opens that game's menu (its own settings + uninstall)
        if ((edge & kMenu) && tab == Tab::Library && zone == Zone::Content && !on_plus()) {
            open_game_menu();
            return false;
        }
        if (live) {
            // every button is live here (the controller test): only a held B leaves, A acts on "Restore defaults"
            if (b & kB) {
                b_hold += dt;
                if (b_hold >= 0.7f) {
                    open_page(Page::Menu);
                    armed = false;
                }
            } else {
                b_hold = 0;
            }
            if ((edge & kA) && input_sel == kInputRows - 1)
                input_change(0);
            return false;
        }
        const bool browser = tab == Tab::Settings && zone == Zone::Content && page == Page::Browser;
        if (browser && (edge & (kLB | kRB))) {
            browser_kind((edge & kRB) ? sgdb::Kind::Hero : sgdb::Kind::Card);
        } else {
            if (edge & kLB)
                switch_tab(Tab::Library);
            if (edge & kRB)
                switch_tab(Tab::Settings);
        }
        if (edge & kY) {
            if (tab == Tab::Library) {
                start_scan();
                next_scan = now + std::chrono::seconds(4);
                show_toast(L"Refreshing library");
            } else if (zone == Zone::Content && page == Page::Browser) {
                restore_original(br.kind);
            } else if (zone == Zone::Content && page == Page::Artwork && art_sel > 0) {
                restore_original(sgdb::Kind::Card, &apps[art_sel - 1]);
                restore_original(sgdb::Kind::Hero, &apps[art_sel - 1]);
            }
        }
        if (edge & kX) {
            if (zone == Zone::Content && page == Page::Library && tab == Tab::Settings && lib_row == 1 && !library_install_folder().empty()) {
                library_set_install_folder(std::wstring());
                show_toast(L"USB install folder cleared: the whole USB drive is searched again");
            } else if (zone == Zone::Content && page == Page::Browser && tab == Tab::Settings)
                start_edit(EditTarget::Search, br.term);
            else if (zone == Zone::Content && page == Page::Artwork && tab == Tab::Settings && art_sel == 0)
                start_edit(EditTarget::Key, sgdb_key);
        }
        if (edge & kB) {
            if (zone == Zone::Content) {
                if (tab == Tab::Settings && page == Page::Browser)
                    open_page(Page::Artwork);
                else if (tab == Tab::Settings && page != Page::Menu)
                    open_page(Page::Menu);
                else
                    zone = Zone::Tabs;
            }
        }
        if (edge & kA) {
            if (zone == Zone::Tabs) {
                zone = Zone::Content;
            } else if (tab == Tab::Library) {
                if (!on_plus())
                    return true;
                open_installer();
            } else if (tab == Tab::Settings) {
                if (page == Page::Menu) {
                    if (menu_sel == kMenuInput)
                        open_page(Page::Input);
                    else if (menu_sel == kMenuGraphics)
                        open_page(Page::Graphics);
                    else if (menu_sel == kMenuShaders)
                        open_page(Page::Shaders);
                    else if (menu_sel == kMenuStats)
                        open_page(Page::Stats);
                    else if (menu_sel == kMenuArtwork)
                        open_page(Page::Artwork);
                    else if (menu_sel == kMenuLibrary)
                        open_page(Page::Library);
                } else if (page == Page::Library) {
                    if (lib_row == 0)
                        library_set(!library_show_homebrew());
                    else
                        fp_open_picker();
                } else if (page == Page::Shaders) {
                    shaders_set(shader_row, !(shader_row == 0 ? graphics_async_pipelines() : graphics_pipeline_prewarm()));
                } else if (page == Page::Stats) {
                    // A: the toggle flips; on the mode row the other mode is chosen
                    if (stats_row == 0)
                        stats_set_enabled(!stats_enabled());
                    else
                        stats_set_mode(stats_mode() == StatsMode::Full ? StatsMode::FpsOnly : StatsMode::Full);
                } else if (page == Page::Graphics) {
                    // saved on every change; A just confirms what the next game will use
                    const float m = graphics_resolution_multiplier();
                    wchar_t buf[128];
                    swprintf_s(buf, L"Internal resolution %.1fx (%d \u00D7 %d) is saved for the next game", m, resolution_width(m), resolution_height(m));
                    show_toast(buf);
                } else if (page == Page::Artwork) {
                    if (art_sel == 0)
                        start_edit(EditTarget::Key, sgdb_key);
                    else
                        browser_open(apps[art_sel - 1]);
                } else if (page == Page::Browser) {
                    browser_apply();
                }
            }
        }
        return false;
    }

    // ---- Shader Compilation page (0.0.28.3): saved at once, used by the next game ----
    void shaders_set(int row, bool on) {
        const bool cur = row == 0 ? graphics_async_pipelines() : graphics_pipeline_prewarm();
        if (cur == on)
            return;
        if (row == 0)
            graphics_set_async_pipelines(on);
        else
            graphics_set_pipeline_prewarm(on);
        log_line(Log::Lifecycle, "launcher ui: %s %s, saved; applies to the next game", row == 0 ? "asynchronous pipeline compilation" : "pipeline pre-warm",
            on ? "ON" : "OFF");
    }

    // ---- Library page (0.0.29.0): saved at once, the library updates at once ----
    void library_set(bool show) {
        if (library_show_homebrew() == show)
            return;
        library_set_show_homebrew(show);
        apply_library(false, std::string());
        log_line(Log::Lifecycle, "launcher ui: show homebrew %s, saved (%u shown, %u hidden)", show ? "ON" : "OFF", shown_homebrew, hidden_homebrew);
    }

    // ---- 0.1.1.0 the USB install folder picker (Settings > Library, row 1) ----
    void fp_open_picker() {
        if (!fp)
            fp = std::make_unique<install::FolderBrowser>();
        fpv = install::FolderView{};
        fp_sel = fp_scroll = 0;
        fp_return.clear();
        fp_anim = 0;
        fp_open = true;
        armed = false; // the A that opened it must be released first
        fp->open_root();
        log_line(Log::Lifecycle, "launcher ui: install folder picker opened");
    }
    void fp_close() {
        fp_open = false;
        armed = false;
        log_line(Log::Lifecycle, "launcher ui: install folder picker closed");
    }
    void fp_move(int dy) {
        if (!fpv.loading && dy && !fpv.entries.empty())
            fp_sel = std::clamp(fp_sel + dy, 0, static_cast<int>(fpv.entries.size()) - 1);
    }
    void fp_buttons(uint32_t edge) {
        if (fpv.loading) {
            if (edge & kB)
                fp_close(); // (the worker finishes on its own; the next open waits for it)
            return;
        }
        if ((edge & kA) && fp_sel < static_cast<int>(fpv.entries.size())) {
            fp_return.clear();
            fp->enter(static_cast<size_t>(fp_sel));
        } else if ((edge & kX) && !fpv.at_drives) {
            library_set_install_folder(fpv.path);
            show_toast(L"USB install folder set to " + fpv.path + L" (on any USB drive)");
            fp_close();
        } else if (edge & kY) {
            fp_return.clear();
            fp->open_root();
        } else if (edge & kB) {
            if (fpv.at_drives) {
                fp_close();
            } else {
                // select the folder (or drive) we leave once its parent is listed
                const size_t cut = fpv.path.find_last_of(L'\\');
                fp_return = fpv.path == L"\\" ? fpv.drive : fpv.path.substr(cut + 1);
                fp->up();
            }
        }
    }
    void fp_update(float dt) {
        if (!fp_open || !fp)
            return;
        fp_anim = std::min(1.0f, fp_anim + dt / 0.25f);
        if (fp->poll(fpv.version, fpv)) {
            fp_sel = 0;
            if (!fp_return.empty())
                for (size_t i = 0; i < fpv.entries.size(); ++i)
                    if (fpv.entries[i] == fp_return)
                        fp_sel = static_cast<int>(i);
            fp_scroll = 0;
        }
    }

    // ---- the USB installer (0.0.29.0, the "+" tile) ----
    void open_installer() {
        if (!inst)
            inst = std::make_unique<install::Installer>(kDevRoot);
        inst->scan();
        inst_open = true;
        inst_sel = 0;
        inst_scroll = 0;
        inst_anim = 0;
        is_ver = 0;
        is = install::Snapshot{};
        armed = false; // the A that opened it must be released first
        log_line(Log::Lifecycle, "launcher ui: installer opened");
    }
    void close_installer() {
        // anything installed: the library picks it up (and selects the first new game) on the next scan
        if (is.installed > 0) {
            pending_select = is.select_title_id;
            pending_until = Clock::now() + std::chrono::seconds(15);
            stop_scan();
            start_scan();
            next_scan = Clock::now() + std::chrono::seconds(4);
            wchar_t buf[96];
            swprintf_s(buf, L"%u item%s installed", is.installed, is.installed == 1 ? L"" : L"s");
            show_toast(buf);
        }
        if (inst)
            inst->close();
        inst_open = false;
        armed = false;
        log_line(Log::Lifecycle, "launcher ui: installer closed (%u installed, %u failed)", is.installed, is.failed);
    }
    void install_move(int dy) {
        if (is.phase == install::Phase::Files && dy && !is.files.empty())
            inst_sel = std::clamp(inst_sel + dy, 0, static_cast<int>(is.files.size()) - 1);
    }
    bool install_ready() const {
        return is.space_ok && std::any_of(is.items.begin(), is.items.end(), [](const install::Item &i) { return i.installable; });
    }
    void install_buttons(uint32_t edge) {
        using install::Phase;
        switch (is.phase) {
        case Phase::Idle:
        case Phase::Scanning:
            if (edge & kB)
                close_installer();
            break;
        case Phase::Files:
            if ((edge & kA) && inst_sel >= 0 && inst_sel < static_cast<int>(is.files.size()))
                inst->analyze(static_cast<size_t>(inst_sel));
            else if (edge & kY)
                inst->scan(), inst_sel = 0;
            else if (edge & kB)
                close_installer();
            break;
        case Phase::Analyzing:
            break; // reading the archive's directory: a moment
        case Phase::Confirm:
            if ((edge & kA) && install_ready())
                inst->install();
            else if (edge & kB)
                inst->back();
            break;
        case Phase::Installing:
            if ((edge & kB) && !is.cancelling)
                inst->cancel();
            break;
        case Phase::Done:
            if (edge & kX) {
                inst->delete_archive(); // 0.0.30.0: only when everything in the archive is installed (checked there)
            } else if (edge & kA) {
                close_installer();
            } else if (edge & kB) {
                // back to the file list (another archive); what was installed is picked up when the installer closes
                const unsigned installed = is.installed;
                const std::string select = is.select_title_id;
                inst->back();
                if (installed) {
                    pending_select = select;
                    pending_until = Clock::now() + std::chrono::seconds(15);
                    start_scan();
                }
            }
            break;
        }
    }
    void install_update(float dt) {
        if (!inst_open || !inst)
            return;
        inst_anim = std::min(1.0f, inst_anim + dt / 0.25f);
        install::Snapshot n;
        if (inst->poll(is_ver, n)) {
            if (n.phase == install::Phase::Done && is.phase == install::Phase::Installing)
                log_line(Log::Lifecycle, "launcher ui: installation finished: %s", narrow(n.message).c_str());
            is = std::move(n);
            is_ver = is.version;
            inst_sel = std::clamp(inst_sel, 0, std::max(0, static_cast<int>(is.files.size()) - 1));
        }
        // the file list keeps the selection on screen
        const float row = 84, visible = 6;
        const float want = std::clamp(inst_sel * row - (visible - 1) * row * 0.5f, 0.0f, std::max(0.0f, (static_cast<float>(is.files.size()) - visible) * row));
        inst_scroll = approach(inst_scroll, want, dt, 14);
    }

    // ---- the game menu (0.0.30.0): Menu on a library card ----
    AppEntry *gm_entry() {
        for (AppEntry &e : apps)
            if (strcmp(e.info.title_id, gm_app.title_id) == 0 && strcmp(e.info.folder, gm_app.folder) == 0 && strcmp(e.info.app_root, gm_app.app_root) == 0)
                return &e;
        return nullptr;
    }
    void open_game_menu() {
        if (sel < 0 || sel >= static_cast<int>(apps.size()) || gm_state != 0)
            return;
        const AppEntry &e = apps[sel];
        gm_app = e.info;
        gm_title = e.title;
        gm_cfg = game_config(e.info.title_id);
        // the folder walk can take a while on a big game: never on the UI thread (the result lands when ready)
        gm_size = std::make_shared<std::atomic<int64_t>>(-1);
        std::thread([sz = gm_size, dir = std::wstring(e.info.dir)] { sz->store(static_cast<int64_t>(install::dir_size(dir))); }).detach();
        gm_row = 0;
        gm_confirm = false;
        gm_anim = 0;
        gm_open = true;
        armed = false;
        log_line(Log::Lifecycle, "launcher ui: game menu %s '%s' (%s/%s)", gm_app.title_id, gm_app.title, gm_app.app_root, gm_app.folder);
    }
    void close_game_menu() {
        gm_open = false;
        gm_confirm = false;
        armed = false;
    }
    // the values a row cycles through (row 0: multipliers, 0 = Global; rows 1-2: -1 Global, 1 on, 0 off; row 3: -1, 0, 1, 2)
    void gm_change(int d) {
        if (gm_row == 0) {
            std::vector<float> v{ 0 };
            for (float m = kResolutionMin; m <= graphics_max_multiplier() + 0.01f; m += kResolutionStep)
                v.push_back(m);
            int i = 0;
            for (size_t k = 0; k < v.size(); ++k)
                if (std::fabs(v[k] - gm_cfg.resolution) < 0.01f)
                    i = static_cast<int>(k);
            gm_cfg.resolution = v[static_cast<size_t>(std::clamp(i + d, 0, static_cast<int>(v.size()) - 1))];
        } else if (gm_row == 1 || gm_row == 2) {
            static const int order[3] = { -1, 1, 0 };
            int &val = gm_row == 1 ? gm_cfg.async : gm_cfg.prewarm;
            int i = 0;
            for (int k = 0; k < 3; ++k)
                if (order[k] == val)
                    i = k;
            val = order[(i + d + 3) % 3];
        } else if (gm_row == 3) {
            static const int order[4] = { -1, 0, 1, 2 };
            int i = 0;
            for (int k = 0; k < 4; ++k)
                if (order[k] == gm_cfg.stats)
                    i = k;
            gm_cfg.stats = order[(i + d + 4) % 4];
        } else {
            return;
        }
        game_config_set(gm_app.title_id, gm_cfg);
        log_line(Log::Lifecycle, "launcher ui: %s settings: resolution %.1f async %d pre-warm %d overlay %d (0/-1 = global), saved", gm_app.title_id,
            gm_cfg.resolution, gm_cfg.async, gm_cfg.prewarm, gm_cfg.stats);
    }
    void gm_move(int dx, int dy) {
        if (gm_confirm || gm_state != 0)
            return;
        if (dy)
            gm_row = std::clamp(gm_row + dy, 0, kGmRows - 1);
        else if (dx && gm_row < 4)
            gm_change(dx);
    }
    void gm_buttons(uint32_t edge) {
        if (gm_state != 0)
            return; // uninstalling
        if (gm_confirm) {
            if (edge & kA)
                gm_start_uninstall(false);
            else if (edge & kX)
                gm_start_uninstall(true);
            else if (edge & kB)
                gm_confirm = false;
            return;
        }
        if (edge & (kB | kMenu)) {
            close_game_menu();
        } else if (edge & kA) {
            if (gm_row < 4) {
                gm_change(1);
            } else if (gm_row == 4) {
                gm_cfg = GameConfig{};
                game_config_set(gm_app.title_id, gm_cfg);
                show_toast(L"Global settings restored for " + gm_title);
            } else {
                gm_confirm = true;
            }
        }
    }
    void gm_start_uninstall(bool delete_saves) {
        unsigned same = 0;
        for (const VitoxAppInfo &f : all_found)
            same += strcmp(f.title_id, gm_app.title_id) == 0;
        gm_shared = same > 1; // another installed app has this TITLE_ID: its ux0 data is not ours to remove
        gm_error.clear();
        gm_freed = 0;
        gm_state = 1;
        if (gm_thread.joinable())
            gm_thread.join();
        const std::string root = gm_app.app_root, folder = gm_app.folder, tid = gm_app.title_id;
        const std::wstring ls = local_state;
        const bool shared = gm_shared;
        log_line(Log::Lifecycle, "launcher ui: UNINSTALL %s '%s' (%s/%s) delete_saves=%d shared_title_id=%d", tid.c_str(), gm_app.title, root.c_str(), folder.c_str(),
            delete_saves ? 1 : 0, shared ? 1 : 0);
        stop_scan(); // no library scan reads the folder while it is removed
        gm_thread = std::thread([this, root, folder, tid, ls, shared, delete_saves] {
            uint64_t freed = 0;
            gm_error = install::uninstall(kDevRoot, ls, root, folder, tid, shared, delete_saves, &freed);
            gm_freed = freed;
            gm_state = 2;
        });
    }
    void gm_update(float dt) {
        if (!gm_open)
            return;
        gm_anim = std::min(1.0f, gm_anim + dt / 0.25f);
        if (gm_state == 2) {
            gm_thread.join();
            gm_state = 0;
            if (gm_error.empty()) {
                if (!gm_shared)
                    game_config_remove(gm_app.title_id);
                show_toast(gm_title + L" uninstalled (" + install::format_bytes(gm_freed) + L" freed)");
            } else {
                show_toast(L"Uninstall incomplete: " + gm_error);
            }
            close_game_menu();
            start_scan();
            next_scan = Clock::now() + std::chrono::seconds(4);
        }
    }

    // ---- Graphics page: one 0.5x step per press (auto-repeat only after a 380 ms hold, like every list) ----
    void graphics_adjust(int d) {
        const float cur = graphics_resolution_multiplier(), max = graphics_max_multiplier();
        const float next = resolution_snap(cur + d * kResolutionStep, max);
        if (next == cur) {
            gfx_bump = 1; // already at 1.0x / the maximum
            return;
        }
        graphics_set_resolution_multiplier(next);
        log_line(Log::Lifecycle, "launcher ui: internal resolution %.1fx -> %.1fx (%dx%d), saved; applies to the next game", cur, next, resolution_width(next),
            resolution_height(next));
    }

    // ---- Input page ----
    // the targets a button can send, in cycle order: the 12 remappable buttons, then "disabled"
    void input_change(int d) {
        const int nb = input_button_count();
        if (input_sel < nb) {
            const uint32_t bit = input_button_bit(input_sel);
            int t = icfg.map[bit] == kInputNone ? nb : input_button_index(icfg.map[bit]);
            if (t < 0)
                t = nb;
            t = (t + d + nb + 1) % (nb + 1);
            icfg.map[bit] = t == nb ? kInputNone : static_cast<uint8_t>(input_button_bit(t));
        } else if (input_sel == nb || input_sel == nb + 1) {
            float &dz = input_sel == nb ? icfg.dead_l : icfg.dead_r;
            dz = std::clamp(std::round((dz + d * 0.01f) * 100.0f) / 100.0f, 0.0f, 0.5f);
        } else {
            if (d != 0)
                return;
            icfg = input_config_default();
            show_toast(L"Controller settings restored to defaults");
        }
        input_config_set(icfg);
        const bool saved = !local_state.empty() && input_config_save(local_state + L"\\vitox-input.txt");
        log_line(Log::Lifecycle, "launcher ui: input row %d changed -> saved %d", input_sel, saved ? 1 : 0);
    }
    bool input_is_default() const {
        const InputConfig d = input_config_default();
        return memcmp(d.map, icfg.map, sizeof(d.map)) == 0 && d.dead_l == icfg.dead_l && d.dead_r == icfg.dead_r;
    }

    // ---- text entry: the system keyboard (host_text.cpp: CoreTextEditContext + CoreInputView / InputPane) ----
    void start_edit(EditTarget t, const std::wstring &seed) {
        const VitoxHostText *ht = vitox_host_text();
        if (!ht) {
            show_toast(L"The on-screen keyboard is not available");
            return;
        }
        editing = t;
        edit_text = seed;
        edit_t = 0;
        edit_seen_visible = false;
        const std::u16string u(seed.begin(), seed.end());
        const bool shown = ht->show(u.data(), static_cast<uint32_t>(u.size()), 128);
        log_line(Log::Lifecycle, "launcher ui: text entry %s (keyboard show -> %d)", t == EditTarget::Key ? "API key" : "search", shown ? 1 : 0);
    }
    void edit_poll(float dt) {
        const VitoxHostText *ht = vitox_host_text();
        if (!ht) {
            editing = EditTarget::None;
            return;
        }
        edit_t += dt;
        char16_t buf[256];
        const uint32_t n = ht->text(buf, 256);
        edit_text.assign(buf, buf + n);
        const bool vis = ht->keyboard_visible();
        if (vis)
            edit_seen_visible = true;
        if (ht->take_enter() || (edit_seen_visible && !vis))
            finish_edit(true);
        else if (!edit_seen_visible && edit_t > 4.0f) {
            finish_edit(false);
            show_toast(L"The on-screen keyboard did not open");
        }
    }
    void finish_edit(bool commit) {
        const VitoxHostText *ht = vitox_host_text();
        if (ht)
            ht->hide();
        const EditTarget t = editing;
        editing = EditTarget::None;
        armed = false;
        std::wstring v;
        for (wchar_t c : edit_text)
            if (t == EditTarget::Search ? (c >= L' ') : (c > L' ' && c < 127))
                v += c;
        if (!commit)
            return;
        if (t == EditTarget::Key) {
            set_key(v, true);
            show_toast(v.empty() ? L"SteamGridDB API key removed" : L"SteamGridDB API key saved");
        } else if (t == EditTarget::Search && !v.empty()) {
            br.term = v;
            browser_search();
        }
    }

    // ---- SteamGridDB ----
    sgdb::Client &client() {
        if (!sgdb_client)
            sgdb_client = std::make_unique<sgdb::Client>();
        return *sgdb_client;
    }
    void set_key(const std::wstring &k, bool save) {
        sgdb_key = k;
        if (save && !local_state.empty())
            sgdb::save_key(local_state, k);
        key_check = nullptr;
        key_msg.clear();
        if (k.empty()) {
            key_state = KeyState::Unset;
            return;
        }
        client().set_key(k);
        key_state = KeyState::Checking;
        key_check = client().validate();
    }
    void poll_key() {
        if (!key_check || !key_check->done)
            return;
        key_state = key_check->ok ? KeyState::Valid : key_check->http == 401 || key_check->http == 403 ? KeyState::Invalid : KeyState::Offline;
        key_msg = key_check->ok ? L"" : key_check->error;
        key_check = nullptr;
    }

    AppEntry *br_app() {
        for (AppEntry &a : apps)
            if (a.title_id == br.tid)
                return &a;
        return nullptr;
    }
    void browser_open(AppEntry &a) {
        if (sgdb_key.empty()) {
            show_toast(L"Enter your SteamGridDB API key first");
            art_sel = 0;
            return;
        }
        br = Browser{};
        br.tid = a.title_id;
        br.term = clean_title(a.title);
        br.in_use[0] = read_src(a, sgdb::Kind::Card);
        br.in_use[1] = read_src(a, sgdb::Kind::Hero);
        open_page(Page::Browser);
        browser_search();
    }
    void browser_close() {
        if (sgdb_client)
            sgdb_client->cancel_pending();
        br.thumbs.clear();
        br.images.clear();
        br.cache.clear();
    }
    void browser_search() {
        br.games.clear();
        br.images.clear();
        br.images_ready = false;
        br.images_h = nullptr;
        br.game_sel = 0;
        br.sel = 0;
        br.scroll = 0;
        br.zone_games = false;
        br.status = L"Searching SteamGridDB for “" + br.term + L"”…";
        br.search_h = client().search(br.term);
    }
    void browser_load_images() {
        br.load_pending = false;
        br.images.clear();
        br.images_ready = false;
        br.sel = 0;
        br.scroll = 0;
        if (br.games.empty())
            return;
        const auto key = std::make_pair(br.games[br.game_sel].id, static_cast<int>(br.kind));
        auto it = br.cache.find(key);
        if (it != br.cache.end()) {
            br.images = it->second;
            br.images_ready = true;
            br.status = br.images.empty() ? no_images_text() : L"";
            br.images_h = nullptr;
            return;
        }
        br.status = L"Loading artwork…";
        br.images_h = client().images(br.games[br.game_sel].id, br.kind);
    }
    std::wstring no_images_text() const {
        return br.kind == sgdb::Kind::Card ? L"No square card art for this game on SteamGridDB yet" : L"No background art for this game on SteamGridDB yet";
    }
    void browser_kind(sgdb::Kind k) {
        if (k == br.kind)
            return;
        br.kind = k;
        if (sgdb_client)
            sgdb_client->cancel_pending();
        browser_load_images();
    }
    int browser_cols() const { return br.kind == sgdb::Kind::Card ? 8 : 3; }
    void browser_move(int dx, int dy) {
        if (br.zone_games) {
            if (dx && !br.games.empty()) {
                const int n = std::clamp(br.game_sel + dx, 0, static_cast<int>(br.games.size()) - 1);
                if (n != br.game_sel) {
                    br.game_sel = n;
                    br.load_pending = true;
                    br.load_at = Clock::now() + std::chrono::milliseconds(350);
                    br.images.clear();
                    br.images_ready = false;
                    br.status = L"Loading artwork…";
                }
            }
            if (dy > 0 && !br.images.empty())
                br.zone_games = false;
            return;
        }
        const int cols = browser_cols(), n = static_cast<int>(br.images.size());
        if (dy < 0 && br.sel < cols) {
            br.zone_games = true;
            return;
        }
        if (n == 0) {
            if (dy < 0)
                br.zone_games = true;
            return;
        }
        if (dx)
            br.sel = std::clamp(br.sel + dx, 0, n - 1);
        if (dy)
            br.sel = std::clamp(br.sel + dy * cols, 0, n - 1);
    }
    void browser_apply() {
        if (br.zone_games || br.sel < 0 || br.sel >= static_cast<int>(br.images.size()))
            return;
        if (br.apply_h && !br.apply_h->done)
            return;
        AppEntry *a = br_app();
        if (!a)
            return;
        br.apply_index = br.sel;
        br.apply_kind = br.kind;
        br.apply_h = client().download(br.images[br.sel].url, art_file(*a, br.kind));
        log_line(Log::Lifecycle, "launcher ui: apply %s image %lld for %s", br.kind == sgdb::Kind::Card ? "card" : "background",
            static_cast<long long>(br.images[br.sel].id), narrow(a->title_id).c_str());
    }
    void restore_original(sgdb::Kind k, AppEntry *a = nullptr) {
        if (!a)
            a = br_app();
        if (!a)
            return;
        const std::wstring f = art_file(*a, k);
        const bool had = file_exists(f);
        DeleteFileW(f.c_str());
        DeleteFileW((f + L".src").c_str());
        if (page == Page::Browser)
            br.in_use[k == sgdb::Kind::Card ? 0 : 1].clear();
        art_changed(*a);
        if (had)
            show_toast(std::wstring(k == sgdb::Kind::Card ? L"Original card art restored for " : L"Original background restored for ") + a->title);
        log_line(Log::Lifecycle, "launcher ui: restore original %s for %s (had custom %d)", k == sgdb::Kind::Card ? "card" : "background",
            narrow(a->title_id).c_str(), had ? 1 : 0);
    }
    // per frame: finished requests -> state; thumbnails for the visible rows
    void browser_update() {
        if (br.search_h && br.search_h->done) {
            sgdb::Handle h = br.search_h;
            br.search_h = nullptr;
            if (!h->ok) {
                br.status = h->error;
            } else if (h->games.empty()) {
                br.status = L"No SteamGridDB match for “" + br.term + L"”. Press X to search for a different name.";
            } else {
                br.games = h->games;
                if (br.games.size() > 8)
                    br.games.resize(8);
                br.game_sel = 0;
                browser_load_images();
            }
        }
        if (br.load_pending && Clock::now() >= br.load_at)
            browser_load_images();
        if (br.images_h && br.images_h->done) {
            sgdb::Handle h = br.images_h;
            br.images_h = nullptr;
            if (h->ok) {
                br.images = h->images;
                br.cache[std::make_pair(br.games[br.game_sel].id, static_cast<int>(br.kind))] = br.images;
                br.images_ready = true;
                br.status = br.images.empty() ? no_images_text() : L"";
                if (br.images.empty())
                    br.zone_games = true;
            } else {
                br.status = h->error;
            }
        }
        if (br.apply_h && br.apply_h->done) {
            sgdb::Handle h = br.apply_h;
            br.apply_h = nullptr;
            AppEntry *a = br_app();
            if (h->ok && a && br.apply_index >= 0 && br.apply_index < static_cast<int>(br.images.size())) {
                const std::wstring src = br.images[br.apply_index].url;
                std::wofstream(art_file(*a, br.apply_kind) + L".src", std::ios::trunc) << src << L"\n";
                br.in_use[br.apply_kind == sgdb::Kind::Card ? 0 : 1] = src;
                art_changed(*a);
                show_toast(std::wstring(br.apply_kind == sgdb::Kind::Card ? L"Card art updated for " : L"Background updated for ") + a->title);
            } else {
                show_toast(h->error.empty() ? L"Could not apply the artwork" : h->error);
            }
            br.apply_index = -1;
        }
        // thumbnails: the visible rows +1, in order
        const int cols = browser_cols(), n = static_cast<int>(br.images.size());
        const int first_row = static_cast<int>(br.scroll / thumb_row_h());
        const int last = std::min(n, (first_row + rows_visible() + 1) * cols);
        int uploads = 0; // 0.0.26.4: at most 4 texture uploads per frame (11 at once stalled Present)
        for (int i = std::max(0, first_row * cols); i < last; ++i) {
            const sgdb::Image &im = br.images[i];
            Thumb &t = br.thumbs[im.url];
            if (!t.h && !t.bmp && !t.failed)
                t.h = client().thumb(im.thumb.empty() ? im.url : im.thumb, br.kind == sgdb::Kind::Card ? 256 : 560, im.thumb.empty() ? std::wstring() : im.url);
            if (t.h && t.h->done && t.h->error == L"cancelled") {
                t.h = nullptr; // dropped from the queue by a kind / game switch: ask again
                continue;
            }
            if (t.h && t.h->done && uploads < 4) {
                ++uploads;
                if (t.h->bitmap)
                    dc->CreateBitmapFromWicBitmap(t.h->bitmap.get(), nullptr, t.bmp.put()), ++perf_bitmaps;
                t.failed = !t.bmp;
                t.h = nullptr;
            }
        }
    }
    float thumb_w() const { return br.kind == sgdb::Kind::Card ? 178.0f : 518.0f; }
    float thumb_h() const { return br.kind == sgdb::Kind::Card ? 178.0f : 167.0f; }
    float thumb_gap() const { return br.kind == sgdb::Kind::Card ? 25.0f : 33.0f; }
    float thumb_row_h() const { return thumb_h() + thumb_gap(); }
    int rows_visible() const { return static_cast<int>(std::ceil((kGridY1 - kGridY0) / thumb_row_h())); }
    static constexpr float kGridY0 = 470, kGridY1 = 948;
    void show_toast(const std::wstring &t) {
        toast = t;
        toast_t = 0;
    }

    // ================================================================ drawing
    void draw_background(float glow_rect[4], float glow) {
        BgConstants c{};
        c.screen[0] = static_cast<float>(W);
        c.screen[1] = static_cast<float>(H);
        c.time = std::fmod(time_s, 7200.0f);
        c.k = k;
        c.parallax[0] = parallax_x;
        c.parallax[1] = 0.25f * std::sin(time_s * 0.21f);
        c.glow = glow;
        c.hue = sel_anim_hue;
        memcpy(c.card, glow_rect, sizeof(c.card));
        c.radius = 34 * k;
        c.fade = ease_out(intro) * (1.0f - ease_in_out(launch_t));
        ctx->UpdateSubresource(cb.get(), 0, nullptr, &c, 0, 0);
        ID3D11RenderTargetView *rt = rtv.get();
        ctx->OMSetRenderTargets(1, &rt, nullptr);
        const D3D11_VIEWPORT vp{ 0, 0, static_cast<float>(W), static_cast<float>(H), 0, 1 };
        ctx->RSSetViewports(1, &vp);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->IASetInputLayout(nullptr);
        ctx->VSSetShader(vs.get(), nullptr, 0);
        ctx->PSSetShader(ps.get(), nullptr, 0);
        ID3D11Buffer *b = cb.get();
        ctx->PSSetConstantBuffers(0, 1, &b);
        ctx->Draw(3, 0);
    }

    void draw_header(float a) {
        // brand
        com_ptr<ID2D1GradientStopCollection> stops;
        const D2D1_GRADIENT_STOP gs[] = { { 0.0f, rgba(0xF4ECFF, a) }, { 0.5f, rgba(0xC594FF, a) }, { 1.0f, rgba(0x8C5BFF, a) } };
        dc->CreateGradientStopCollection(gs, 3, stops.put());
        com_ptr<ID2D1LinearGradientBrush> g;
        dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({ kLeft * k, 56 * k }, { (kLeft + 230) * k, 110 * k }), stops.get(), g.put());
        if (IDWriteTextLayout *l = layout(L"VITOX", 46 * k, DWRITE_FONT_WEIGHT_BLACK, 600 * k, 9 * k))
            dc->DrawTextLayout({ kLeft * k, 50 * k }, l, g.get());
        text(L"PLAYSTATION VITA RUNTIME", kLeft + 3, 108, 14, DWRITE_FONT_WEIGHT_SEMI_BOLD, rgba(0xA592CC, a), 600, 4.5f);

        // tabs: a glass capsule with a sliding highlight
        const float tw0 = measure(L"Library", 25, DWRITE_FONT_WEIGHT_SEMI_BOLD), tw1 = measure(L"Settings", 25, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        const float padx = 34, cx = 960, gap = 8;
        const float w0 = tw0 + padx * 2, w1 = tw1 + padx * 2, total = w0 + w1 + gap + 12;
        const float x0 = cx - total / 2, y0 = 58, y1 = 114;
        fill_round(x0, y0, x0 + total, y1, 28, rgba(0x1B0E33, 0.62f * a));
        stroke_round(x0, y0, x0 + total, y1, 28, rgba(0xB48CFF, 0.22f * a), 1.2f);
        const float hx0 = x0 + 6 + (w0 + gap) * tab_anim, hw = w0 + (w1 - w0) * tab_anim;
        const bool tabs_focus = zone == Zone::Tabs;
        fill_gradient_round(hx0, y0 + 6, hx0 + hw, y1 - 6, 22, rgba(0x9D5CFF, (tabs_focus ? 0.95f : 0.55f) * a), rgba(0x6A35E8, (tabs_focus ? 0.95f : 0.55f) * a), false);
        if (tabs_focus) {
            const float pulse = 0.55f + 0.45f * std::sin(time_s * 4.0f);
            stroke_round(hx0 - 2, y0 + 4, hx0 + hw + 2, y1 - 4, 24, rgba(0xE7D6FF, (0.5f + 0.4f * pulse) * a), 2.0f);
        }
        text(L"Library", x0 + 6 + padx, y0 + 12, 25, DWRITE_FONT_WEIGHT_SEMI_BOLD, rgba(tab == Tab::Library ? 0xFFFFFF : 0xBBA8E6, a));
        text(L"Settings", x0 + 6 + w0 + gap + padx, y0 + 12, 25, DWRITE_FONT_WEIGHT_SEMI_BOLD, rgba(tab == Tab::Settings ? 0xFFFFFF : 0xBBA8E6, a));

        // clock
        const time_t now = time(nullptr);
        tm lt{};
        localtime_s(&lt, &now);
        wchar_t clock[16];
        swprintf_s(clock, L"%02d:%02d", lt.tm_hour, lt.tm_min);
        const float cw = measure(clock, 30, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        text(clock, 1920 - kLeft - cw, 64, 30, DWRITE_FONT_WEIGHT_SEMI_BOLD, rgba(0xF2EBFF, a));
    }

    void draw_card(AppEntry &e, float x, float y, float size, float a, bool selected) {
        const float r = 34 * size / kCardSel;
        // drop shadow
        for (int i = 3; i >= 1; --i)
            fill_round(x - i * 3, y + i * 6, x + size + i * 3, y + size + i * 9, r + i * 4, rgba(0x05010C, 0.16f * a));
        ensure_art(e, false);
        const D2D1_ROUNDED_RECT rr{ R(x, y, x + size, y + size), r * k, r * k };
        if (e.icon) {
            com_ptr<ID2D1RoundedRectangleGeometry> clip;
            d2d_factory->CreateRoundedRectangleGeometry(rr, clip.put());
            dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), clip.get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), a), nullptr);
            dc->DrawBitmap(e.icon.get(), rr.rect, 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
            // a soft top light on the artwork
            com_ptr<ID2D1GradientStopCollection> stops;
            const D2D1_GRADIENT_STOP gs[] = { { 0.0f, rgba(0xFFFFFF, 0.16f) }, { 0.45f, rgba(0xFFFFFF, 0.0f) } };
            dc->CreateGradientStopCollection(gs, 2, stops.put());
            com_ptr<ID2D1LinearGradientBrush> g;
            dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({ x * k, y * k }, { x * k, (y + size) * k }), stops.get(), g.put());
            dc->FillRectangle(rr.rect, g.get());
            dc->PopLayer();
        } else {
            // fallback artwork: a violet gradient tile with the title's initials and its TITLE_ID
            const float h = e.hue;
            fill_gradient_round(x, y, x + size, y + size, r, rgba(h < 0.5f ? 0x6C2BD9 : 0x8B2FC9, a), rgba(0x1E0A3C, a));
            std::wstring initials;
            bool next = true;
            for (wchar_t c : e.title) {
                if (iswalnum(c) && next && initials.size() < 2)
                    initials += static_cast<wchar_t>(towupper(c));
                next = !iswalnum(c);
            }
            if (initials.empty())
                initials = L"V";
            const float fs = 120 * size / kCardSel;
            const float iw = measure(initials, fs, DWRITE_FONT_WEIGHT_BLACK);
            text(initials, x + (size - iw) / 2, y + size * 0.20f, fs, DWRITE_FONT_WEIGHT_BLACK, rgba(0xF3EAFF, 0.92f * a));
            const float ts = 22 * size / kCardSel;
            const float tw = measure(e.title_id, ts, DWRITE_FONT_WEIGHT_BOLD, 2, L"Consolas");
            text(e.title_id, x + (size - tw) / 2, y + size * 0.74f, ts, DWRITE_FONT_WEIGHT_BOLD, rgba(0xCDB8F5, 0.9f * a), 1800, 2, false, L"Consolas");
        }
        // border: bright for the selection, faint for the rest
        if (selected) {
            const float pulse = 0.5f + 0.5f * std::sin(time_s * 3.2f);
            stroke_round(x - 5, y - 5, x + size + 5, y + size + 5, r + 5, rgba(0xC9A2FF, (0.55f + 0.35f * pulse) * a), 3.0f);
            stroke_round(x - 1, y - 1, x + size + 1, y + size + 1, r + 1, rgba(0xFFFFFF, 0.85f * a), 2.0f);
        } else {
            stroke_round(x, y, x + size, y + size, r, rgba(0xB48CFF, 0.25f * a), 1.2f);
        }
    }

    void draw_hero(float a) {
        // the selected app's LiveArea / pic0 artwork, faded into the background on the right
        if (sel < 0 || sel >= static_cast<int>(apps.size()))
            return;
        if (hero_index != sel) {
            hero_prev = hero_index >= 0 && hero_index < static_cast<int>(apps.size()) ? &apps[hero_index] : nullptr;
            hero_index = sel;
            hero_mix = 0;
        }
        ensure_art(apps[sel], true);
        const auto draw_one = [&](AppEntry *e, float alpha) {
            if (!e || !e->hero || alpha <= 0.01f)
                return;
            const D2D1_SIZE_F s = e->hero->GetSize();
            const float hw = 1220, hh = hw * s.height / std::max(1.0f, s.width);
            const float x1 = 1920, x0 = x1 - hw, y0 = 0;
            // a radial opacity mask (a layer) melts every edge of the artwork into the animated background
            const D2D1_RECT_F rect = R(x0, y0, x1, y0 + hh);
            com_ptr<ID2D1GradientStopCollection> stops;
            const D2D1_GRADIENT_STOP gs[] = { { 0.0f, rgba(0xFFFFFF, 1.0f) }, { 0.5f, rgba(0xFFFFFF, 0.7f) }, { 1.0f, rgba(0xFFFFFF, 0.0f) } };
            dc->CreateGradientStopCollection(gs, 3, stops.put());
            com_ptr<ID2D1RadialGradientBrush> mask;
            dc->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties({ (x1 - hw * 0.34f) * k, (y0 + hh * 0.40f) * k }, { 0, 0 }, hw * 0.70f * k, hh * 0.66f * k),
                stops.get(), mask.put());
            dc->PushLayer(D2D1::LayerParameters1(rect, nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), alpha * 0.46f * a, mask.get()), nullptr);
            dc->DrawBitmap(e->hero.get(), rect, 1.0f, D2D1_INTERPOLATION_MODE_LINEAR);
            dc->PopLayer();
        };
        draw_one(hero_prev, 1.0f - ease_out(hero_mix));
        draw_one(&apps[sel], ease_out(hero_mix));
    }

    // 0.0.29.0: the "+" tile after the last card (opens the USB installer)
    void draw_plus_tile(float x, float y, float size, float a, bool selected) {
        const float r = 34 * size / kCardSel;
        for (int i = 3; i >= 1; --i)
            fill_round(x - i * 3, y + i * 6, x + size + i * 3, y + size + i * 9, r + i * 4, rgba(0x05010C, 0.10f * a));
        fill_gradient_round(x, y, x + size, y + size, r, rgba(0x2A1458, (selected ? 0.85f : 0.55f) * a), rgba(0x150A2C, (selected ? 0.85f : 0.55f) * a));
        // the plus: two rounded bars
        const float c = size / 2, len = size * 0.17f, th = size * 0.028f;
        const uint32_t pc = selected ? 0xFFFFFF : 0xC9A2FF;
        fill_round(x + c - len, y + c - th, x + c + len, y + c + th, th, rgba(pc, (selected ? 1.0f : 0.8f) * a));
        fill_round(x + c - th, y + c - len, x + c + th, y + c + len, th, rgba(pc, (selected ? 1.0f : 0.8f) * a));
        if (selected) {
            const float pulse = 0.5f + 0.5f * std::sin(time_s * 3.2f);
            stroke_round(x - 5, y - 5, x + size + 5, y + size + 5, r + 5, rgba(0xC9A2FF, (0.55f + 0.35f * pulse) * a), 3.0f);
            stroke_round(x - 1, y - 1, x + size + 1, y + size + 1, r + 1, rgba(0xFFFFFF, 0.85f * a), 2.0f);
        } else {
            stroke_round(x, y, x + size, y + size, r, rgba(0xB48CFF, 0.35f * a), 1.5f);
        }
    }

    void draw_library(float a, float glow_rect[4]) {
        wchar_t count[96];
        if (hidden_homebrew)
            swprintf_s(count, L"%u %s  ·  %u HOMEBREW HIDDEN", static_cast<unsigned>(apps.size()), apps.size() == 1 ? L"APPLICATION" : L"APPLICATIONS", hidden_homebrew);
        else
            swprintf_s(count, L"%u %s", static_cast<unsigned>(apps.size()), apps.size() == 1 ? L"APPLICATION" : L"APPLICATIONS");
        text(L"YOUR LIBRARY", kLeft, 176, 17, DWRITE_FONT_WEIGHT_BOLD, rgba(0xB07CFF, a), 600, 5);
        text(count, kLeft + measure(L"YOUR LIBRARY", 17, DWRITE_FONT_WEIGHT_BOLD, 5) + 22, 176, 17, DWRITE_FONT_WEIGHT_SEMI_BOLD, rgba(0x8F7DB8, a), 900, 4);

        // the card row: the selection sits at kLeft, the row scrolls smoothly
        const float base_y = kRowY;
        float x = kLeft - scroll;
        for (int i = 0; i < static_cast<int>(apps.size()); ++i) {
            AppEntry &e = apps[i];
            const float size = kCard + (kCardSel - kCard) * e.focus;
            const float y = base_y + (kCardSel - size) * 0.5f - 10 * e.focus;
            float ca = a;
            if (launching)
                ca *= i == sel ? 1.0f : 1.0f - ease_out(launch_t * 2);
            if (x + size > -40 && x < 1960) {
                float lx = x, ly = y, ls = size;
                if (launching && i == sel) {
                    const float g = ease_in_out(launch_t);
                    ls = size * (1 + 0.18f * g);
                    lx = x - (ls - size) / 2;
                    ly = y - (ls - size) / 2;
                }
                draw_card(e, lx, ly, ls, ca, i == sel);
                if (i == sel) {
                    glow_rect[0] = lx * k, glow_rect[1] = ly * k, glow_rect[2] = (lx + ls) * k, glow_rect[3] = (ly + ls) * k;
                }
            }
            x += size + kGap;
        }
        {
            const float size = kCard + (kCardSel - kCard) * plus_focus;
            const float y = base_y + (kCardSel - size) * 0.5f - 10 * plus_focus;
            const float pa = a * (launching ? 1.0f - ease_out(launch_t * 2) : 1.0f);
            if (x + size > -40 && x < 1960) {
                draw_plus_tile(x, y, size, pa, on_plus());
                if (on_plus())
                    glow_rect[0] = x * k, glow_rect[1] = y * k, glow_rect[2] = (x + size) * k, glow_rect[3] = (y + size) * k;
            }
        }

        const float dy = 660 + 16 * (1 - ease_out(std::min(1.0f, 1 - change_pulse + 0.25f)));
        const float da = a * (launching ? 1 - ease_out(launch_t * 2) : 1);
        if (on_plus()) {
            draw_plus_details(dy, da);
            return;
        }
        // details of the selection
        AppEntry &s = apps[sel];
        text(s.title, kLeft, dy, 54, DWRITE_FONT_WEIGHT_SEMI_BOLD, rgba(0xFFFFFF, da), 1400);
        float cx = kLeft;
        const auto chip = [&](const std::wstring &t, uint32_t fill, float fa, uint32_t tc, const wchar_t *family = L"Segoe UI") {
            const float w = measure(t, 19, DWRITE_FONT_WEIGHT_BOLD, 1.5f, family) + 30;
            fill_round(cx, dy + 84, cx + w, dy + 120, 18, rgba(fill, fa * da));
            text(t, cx + 15, dy + 89, 19, DWRITE_FONT_WEIGHT_BOLD, rgba(tc, da), 600, 1.5f, false, family);
            cx += w + 12;
        };
        chip(s.title_id, 0x8A4DFF, 0.30f, 0xEADFFF, L"Consolas");
        chip(s.kind, 0x5B2BB8, 0.30f, 0xD9C9FF);
        if (!s.version.empty())
            chip(s.version, 0x3A1B70, 0.45f, 0xC9B8EE);

        // the Start button
        const float bx = kLeft, by = dy + 152, bw = 244, bh = 70;
        const float pulse = 0.5f + 0.5f * std::sin(time_s * 3.0f);
        fill_round(bx - 6, by - 6, bx + bw + 6, by + bh + 6, 41, rgba(0xA66BFF, (0.12f + 0.12f * pulse) * da));
        fill_gradient_round(bx, by, bx + bw, by + bh, 35, rgba(0xA463FF, da), rgba(0x6B2FE6, da), false);
        stroke_round(bx, by, bx + bw, by + bh, 35, rgba(0xE6D5FF, 0.55f * da), 1.5f);
        brush->SetColor(rgba(0xFFFFFF, da));
        dc->FillEllipse(D2D1::Ellipse({ (bx + 44) * k, (by + bh / 2) * k }, 19 * k, 19 * k), brush.get());
        text(L"A", bx + 37, by + 21, 21, DWRITE_FONT_WEIGHT_BOLD, rgba(0x6B2FE6, da));
        text(L"Start", bx + 80, by + 15, 31, DWRITE_FONT_WEIGHT_SEMI_BOLD, rgba(0xFFFFFF, da));
        text(widen(s.info.app_root) + L"  /  " + widen(s.info.folder), bx + bw + 30, by + 22, 20, DWRITE_FONT_WEIGHT_NORMAL, rgba(0x8F7DB8, da), 900);
    }

    // 0.0.29.0: the details area while the "+" tile is selected (also what an empty library shows)
    void draw_plus_details(float dy, float da) {
        text(apps.empty() ? L"Your library is empty" : L"Add Games & DLC", kLeft, dy, 54, DWRITE_FONT_WEIGHT_SEMI_BOLD, rgba(0xFFFFFF, da), 1400);
        float cx = kLeft;
        const auto chip = [&](const std::wstring &t, uint32_t fill, float fa, uint32_t tc) {
            const float w = measure(t, 19, DWRITE_FONT_WEIGHT_BOLD, 1.5f) + 30;
            fill_round(cx, dy + 84, cx + w, dy + 120, 18, rgba(fill, fa * da));
            text(t, cx + 15, dy + 89, 19, DWRITE_FONT_WEIGHT_BOLD, rgba(tc, da), 600, 1.5f);
            cx += w + 12;
        };
        chip(L"USB DRIVE", 0x8A4DFF, 0.30f, 0xEADFFF);
        chip(L".ZIP  /  .VPK", 0x5B2BB8, 0.30f, 0xD9C9FF);
        chip(L"GAMES · UPDATES · DLC · HOMEBREW", 0x3A1B70, 0.45f, 0xC9B8EE);
        const float bx = kLeft, by = dy + 152, bw = 244, bh = 70;
        const float pulse = 0.5f + 0.5f * std::sin(time_s * 3.0f);
        fill_round(bx - 6, by - 6, bx + bw + 6, by + bh + 6, 41, rgba(0xA66BFF, (0.12f + 0.12f * pulse) * da));
        fill_gradient_round(bx, by, bx + bw, by + bh, 35, rgba(0xA463FF, da), rgba(0x6B2FE6, da), false);
        stroke_round(bx, by, bx + bw, by + bh, 35, rgba(0xE6D5FF, 0.55f * da), 1.5f);
        brush->SetColor(rgba(0xFFFFFF, da));
        dc->FillEllipse(D2D1::Ellipse({ (bx + 44) * k, (by + bh / 2) * k }, 19 * k, 19 * k), brush.get());
        text(L"A", bx + 37, by + 21, 21, DWRITE_FONT_WEIGHT_BOLD, rgba(0x6B2FE6, da));
        text(L"Install", bx + 80, by + 15, 31, DWRITE_FONT_WEIGHT_SEMI_BOLD, rgba(0xFFFFFF, da));
        text(L"Choose a .zip or .vpk archive on a USB drive connected to this console", bx + bw + 30, by + 22, 20, DWRITE_FONT_WEIGHT_NORMAL, rgba(0x8F7DB8, da), 1300);
    }

    // ================================================================ settings pages (0.0.26.0)
    float dt_ = 0;
    void section_title(const std::wstring &crumb, const std::wstring &title, float a) {
        text(crumb, kLeft, 176, 17, DWRITE_FONT_WEIGHT_BOLD, rgba(0xB07CFF, a), 1400, 5);
        text(title, kLeft, 204, 46, DWRITE_FONT_WEIGHT_SEMI_BOLD, rgba(0xFFFFFF, a), 1400);
    }
    void row_frame(float x0, float y0, float x1, float y1, bool focus, float a, float r = 26) {
        if (focus) {
            const float pulse = 0.5f + 0.5f * std::sin(time_s * 3.2f);
            fill_round(x0 - 6, y0 - 6, x1 + 6, y1 + 6, r + 4, rgba(0xA66BFF, (0.10f + 0.10f * pulse) * a));
            fill_gradient_round(x0, y0, x1, y1, r, rgba(0x3A1C72, 0.92f * a), rgba(0x24104A, 0.92f * a), false);
            stroke_round(x0, y0, x1, y1, r, rgba(0xC9A2FF, (0.6f + 0.3f * pulse) * a), 2.5f);
        } else {
            fill_round(x0, y0, x1, y1, r, rgba(0x1A0C33, 0.70f * a));
            stroke_round(x0, y0, x1, y1, r, rgba(0xB48CFF, 0.20f * a), 1.2f);
        }
    }
    // a value capsule right-aligned at x1; returns its left edge
    float pill(const std::wstring &v, float x1, float cy, float a, uint32_t fill = 0x8A4DFF, float fa = 0.32f, uint32_t tc = 0xF2EBFF, const wchar_t *family = L"Segoe UI",
        float size = 24) {
        const float vw = measure(v, size, DWRITE_FONT_WEIGHT_BOLD, 1, family) + 40;
        fill_round(x1 - vw, cy - 24, x1, cy + 24, 24, rgba(fill, fa * a));
        text(v, x1 - vw + 20, cy - size * 0.66f, size, DWRITE_FONT_WEIGHT_BOLD, rgba(tc, a), 900, 1, false, family);
        return x1 - vw;
    }
    void glyph_badge(float cx, float cy, float r, const std::wstring &g, uint32_t color, float a, float size = 0) {
        brush->SetColor(rgba(color, 0.95f * a));
        dc->FillEllipse(D2D1::Ellipse({ cx * k, cy * k }, r * k, r * k), brush.get());
        const float fs = size > 0 ? size : r * 1.05f;
        const float tw = measure(g, fs, DWRITE_FONT_WEIGHT_BOLD);
        text(g, cx - tw / 2, cy - fs * 0.68f, fs, DWRITE_FONT_WEIGHT_BOLD, rgba(0xFFFFFF, a));
    }
    void arc(float cx, float cy, float r, float frac, D2D1_COLOR_F c, float w) {
        if (frac <= 0.001f)
            return;
        frac = std::min(frac, 0.999f);
        com_ptr<ID2D1PathGeometry> g;
        d2d_factory->CreatePathGeometry(g.put());
        com_ptr<ID2D1GeometrySink> sink;
        g->Open(sink.put());
        const float a0 = -3.14159265f / 2, a1 = a0 + frac * 2 * 3.14159265f;
        sink->BeginFigure({ (cx + r * std::cos(a0)) * k, (cy + r * std::sin(a0)) * k }, D2D1_FIGURE_BEGIN_HOLLOW);
        sink->AddArc(D2D1::ArcSegment({ (cx + r * std::cos(a1)) * k, (cy + r * std::sin(a1)) * k }, { r * k, r * k }, 0, D2D1_SWEEP_DIRECTION_CLOCKWISE,
            frac > 0.5f ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
        sink->EndFigure(D2D1_FIGURE_END_OPEN);
        sink->Close();
        brush->SetColor(c);
        dc->DrawGeometry(g.get(), brush.get(), w * k, round_stroke.get());
    }
    // a bitmap cropped to fill the rounded rect ("cover")
    void draw_cover(ID2D1Bitmap *bmp, float x0, float y0, float x1, float y1, float r, float alpha) {
        const D2D1_SIZE_F s = bmp->GetSize();
        const float da = (x1 - x0) / std::max(1.0f, y1 - y0), sa = s.width / std::max(1.0f, s.height);
        D2D1_RECT_F src;
        if (sa > da) {
            const float w = s.height * da;
            src = { (s.width - w) / 2, 0, (s.width + w) / 2, s.height };
        } else {
            const float h = s.width / da;
            src = { 0, (s.height - h) / 2, s.width, (s.height + h) / 2 };
        }
        // 0.0.26.4: a bitmap brush filling the rounded rect (a masked layer per tile cost the GPU an offscreen pass each)
        const D2D1_ROUNDED_RECT rr{ R(x0, y0, x1, y1), r * k, r * k };
        com_ptr<ID2D1BitmapBrush1> bb;
        if (FAILED(dc->CreateBitmapBrush(bmp, D2D1::BitmapBrushProperties1(D2D1_EXTEND_MODE_CLAMP, D2D1_EXTEND_MODE_CLAMP, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC),
                D2D1::BrushProperties(alpha), bb.put())))
            return;
        const float sx = (rr.rect.right - rr.rect.left) / std::max(1.0f, src.right - src.left), sy = (rr.rect.bottom - rr.rect.top) / std::max(1.0f, src.bottom - src.top);
        bb->SetTransform(D2D1::Matrix3x2F::Translation(-src.left, -src.top) * D2D1::Matrix3x2F::Scale(sx, sy) * D2D1::Matrix3x2F::Translation(rr.rect.left, rr.rect.top));
        dc->FillRoundedRectangle(rr, bb.get());
    }

    // 0.0.26.5: once the library is known (during the intro fade), each settings page is drawn once offscreen at the real
    // scale: its text sizes, glyphs and artwork reach the GPU before the user opens it (the first Artwork open dropped a frame)
    int prerender_step = 0;
    void prerender(int step) {
        const auto t0 = Clock::now();
        const float save_dt = dt_;
        const int save_menu = menu_sel, save_art = art_sel;
        const Zone save_zone = zone;
        dt_ = 0;
        zone = Zone::Content;
        dc->BeginDraw(); // on the back buffer: the frame drawn next covers it with the full-screen background first
        switch (step) {
        case 0: {
            for (menu_sel = 0; menu_sel < kMenuRows; ++menu_sel)
                draw_settings_menu(1.0f);
            draw_header(1.0f);
            // 0.0.29.0: the "+" tile's details
            const int save_sel = sel;
            float g[4] = {};
            sel = static_cast<int>(apps.size());
            draw_library(1.0f, g);
            sel = save_sel;
            break;
        }
        case 1:
            draw_input_page(1.0f);
            draw_graphics_page(1.0f);
            {
                const int save_row = shader_row;
                for (shader_row = 0; shader_row < 2; ++shader_row)
                    draw_shaders_page(1.0f);
                shader_row = save_row;
            }
            {
                const int save_row = stats_row;
                for (stats_row = 0; stats_row < 2; ++stats_row)
                    draw_stats_page(1.0f);
                stats_row = save_row;
            }
            for (lib_force = 0; lib_force < 2; ++lib_force)
                draw_library_page(1.0f);
            lib_force = -1;
            break;
        case 4: {
            // 0.0.29.0: the installer modal in each phase (static labels, chips, status texts) + its footers
            using install::Phase;
            const install::Snapshot save_is = is;
            const bool save_open = inst_open;
            const float save_anim = inst_anim;
            inst_open = true;
            inst_anim = 1;
            install::Item it;
            it.title = L"Vitox";
            it.title_id = L"PCSX00000";
            it.version = L"01.00";
            it.bytes = 1u << 20;
            it.files = 2;
            std::vector<install::Item> items;
            for (int kd = 0; kd < 4; ++kd) {
                it.kind = static_cast<install::Kind>(kd);
                it.replaces = kd == 1;
                items.push_back(it);
            }
            items[2].note = L"Its game (PCSX00000) is not installed yet";
            int files_pass = 0;
            for (Phase p : { Phase::Scanning, Phase::Files, Phase::Files, Phase::Analyzing, Phase::Confirm, Phase::Installing, Phase::Done }) {
                is = install::Snapshot{};
                is.phase = p;
                is.archive = L"Vitox.zip";
                if (p == Phase::Files && files_pass++ == 0)
                    is.files.push_back({ L"Vitox.zip", L"USB Drive (E:)", L"\\", 1u << 20 });
                else if (p == Phase::Files)
                    is.message = L"No .zip or .vpk files on the USB drive. Copy your archives to it, then press Y to look again.";
                if (p >= Phase::Confirm) {
                    is.items = items;
                    is.need_bytes = 4u << 20;
                    is.free_bytes = 8u << 20;
                }
                if (p == Phase::Done) {
                    is.message = L"Installation complete";
                    is.installed = 4;
                    is.can_delete_archive = true;
                    for (install::Item &x : is.items)
                        x.done = x.ok = true, x.result = L"Installed";
                }
                draw_install_modal(1.0f);
                draw_footer(1.0f);
                if (p == Phase::Done) { // 0.0.30.0: the delete-archive line after deletion
                    is.archive_deleted = true;
                    draw_install_modal(1.0f);
                    draw_footer(1.0f);
                }
            }
            is = save_is;
            inst_open = save_open;
            inst_anim = save_anim;
            // 0.0.30.0: the game menu (every row focused, the confirmation) on the first app
            if (!apps.empty() && !gm_open) {
                const int save_sel = sel;
                sel = 0;
                open_game_menu();
                gm_anim = 1;
                for (gm_row = 0; gm_row < kGmRows; ++gm_row) {
                    draw_game_menu(1.0f);
                    draw_footer(1.0f);
                }
                gm_confirm = true;
                draw_game_menu(1.0f);
                draw_footer(1.0f);
                close_game_menu();
                sel = save_sel;
                tab = Tab::Library;
                draw_footer(1.0f); // the library footer with "Options"
            }
            break;
        }
        case 2: {
            // every key status chip (the key check may still be running now) and both row kinds
            const KeyState save_key = key_state;
            for (KeyState ks : { KeyState::Checking, KeyState::Valid, KeyState::Invalid, KeyState::Offline, save_key }) {
                key_state = ks;
                art_sel = 0;
                draw_artwork_page(1.0f);
            }
            key_state = save_key;
            if (!apps.empty()) {
                art_sel = 1;
                draw_artwork_page(1.0f);
            }
            break;
        }
        default: {
            const Tab save_tab = tab;
            const Page save_page = page;
            tab = Tab::Settings;
            const int save_input = input_sel;
            // every footer variant at its real position (a new label's first draw stalled Present for a frame)
            for (page = Page::Menu; page != Page::Browser; page = static_cast<Page>(static_cast<int>(page) + 1))
                for (int v = 0; v < 2; ++v) {
                    menu_sel = v ? kMenuAbout : 0;
                    art_sel = v && !apps.empty() ? 1 : 0;
                    input_sel = v ? kInputRows - 1 : 0;
                    draw_footer(1.0f);
                }
            input_sel = save_input;
            page = Page::Graphics;
            draw_footer(1.0f);
            page = Page::Stats;
            draw_footer(1.0f);
            page = Page::Shaders;
            draw_footer(1.0f);
            page = Page::Library;
            draw_footer(1.0f);
            {
                // the library footers: on a card and on the "+" tile
                const int save_sel = sel;
                tab = Tab::Library;
                for (int v = 0; v < 2; ++v) {
                    sel = v ? static_cast<int>(apps.size()) : 0;
                    draw_footer(1.0f);
                }
                sel = save_sel;
                tab = Tab::Settings;
            }
            br.zone_games = false;
            br.images.resize(1);
            page = Page::Browser;
            draw_footer(1.0f);
            br.images.clear();
            tab = save_tab;
            page = save_page;
            break;
        }
        }
        const HRESULT hr = dc->EndDraw();
        dt_ = save_dt;
        menu_sel = save_menu;
        art_sel = save_art;
        zone = save_zone;
        log_line(Log::Lifecycle, "launcher ui: page pre-render %d %.1f ms hr=0x%08lX", step, std::chrono::duration<double, std::milli>(Clock::now() - t0).count(),
            static_cast<unsigned long>(hr));
    }

    // 0.0.26.4: one offscreen pass at init through every kind of primitive the launcher draws, so the driver's one-time
    // shader/state setup and the font fallback for the symbol glyphs happen before the first visible frame (each was a
    // 25-90 ms Present the first time a page used it)
    void warm_up() {
        const auto t0 = Clock::now();
        std::vector<uint32_t> pixels(64 * 64, 0xFF8040C0u);
        com_ptr<ID2D1Bitmap> img;
        dc->CreateBitmap({ 64, 64 }, pixels.data(), 64 * 4, D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)), img.put());
        const float k0 = k;
        k = 1.0f;
        // 0.0.26.8: on the real back buffer (ALPHA_MODE_IGNORE): an offscreen premultiplied target did not warm the same
        // GPU states; the first frame's full-screen background covers whatever is drawn here
        dc->BeginDraw();
        fill_round(10, 10, 200, 60, 20, rgba(0x8A4DFF, 0.5f));
        stroke_round(10, 10, 200, 60, 20, rgba(0xC9A2FF, 0.6f), 2);
        fill_gradient_round(10, 70, 200, 120, 20, rgba(0x9D5CFF), rgba(0x6A35E8), false);
        fill_gradient_round(10, 130, 200, 180, 20, rgba(0x9D5CFF), rgba(0x6A35E8), true);
        glyph_badge(240, 40, 17, L"A", 0x3E9A4F, 1.0f);
        arc(300, 40, 20, 0.6f, rgba(0xFFFFFF), 3);
        text(L"Aa ‹›▲▼◀▶⧉≡⚿✓✗→•×·“”…✥▣▤%‹›", 10, 200, 22,
            DWRITE_FONT_WEIGHT_BOLD, rgba(0xFFFFFF));
        text(L"Aa 0123", 10, 240, 17, DWRITE_FONT_WEIGHT_NORMAL, rgba(0xFFFFFF), 400, 0, false, L"Consolas");
        if (img) {
            draw_cover(img.get(), 10, 280, 120, 390, 14, 1.0f);
            D2D1_RECT_F d{ 130, 280, 250, 400 };
            dc->DrawBitmap(img.get(), &d, 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr, nullptr);
            d = { 260, 280, 380, 400 };
            dc->DrawBitmap(img.get(), &d, 1.0f, D2D1_INTERPOLATION_MODE_LINEAR, nullptr, nullptr);
            // the library card (rounded-rect layer) and the hero art (radial opacity-mask layer)
            const D2D1_ROUNDED_RECT rr{ D2D1::RectF(390, 280, 500, 390), 20, 20 };
            com_ptr<ID2D1RoundedRectangleGeometry> clip;
            d2d_factory->CreateRoundedRectangleGeometry(rr, clip.put());
            dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), clip.get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), 1.0f), nullptr);
            dc->DrawBitmap(img.get(), &rr.rect, 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr, nullptr);
            dc->PopLayer();
            com_ptr<ID2D1GradientStopCollection> stops;
            const D2D1_GRADIENT_STOP gs[] = { { 0.0f, rgba(0xFFFFFF, 1.0f) }, { 1.0f, rgba(0xFFFFFF, 0.0f) } };
            dc->CreateGradientStopCollection(gs, 2, stops.put());
            com_ptr<ID2D1RadialGradientBrush> mask;
            dc->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties({ 100, 450 }, { 0, 0 }, 90, 50), stops.get(), mask.put());
            const D2D1_RECT_F hr{ 10, 400, 200, 500 };
            dc->PushLayer(D2D1::LayerParameters1(hr, nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), 0.5f, mask.get()), nullptr);
            dc->DrawBitmap(img.get(), &hr, 1.0f, D2D1_INTERPOLATION_MODE_LINEAR, nullptr, nullptr);
            dc->PopLayer();
        }
        dc->PushAxisAlignedClip(D2D1::RectF(220, 400, 500, 500), D2D1_ANTIALIAS_MODE_ALIASED);
        brush->SetColor(rgba(0xFFFFFF, 0.3f));
        dc->FillEllipse(D2D1::Ellipse({ 300, 450 }, 30, 30), brush.get());
        dc->DrawEllipse(D2D1::Ellipse({ 380, 450 }, 30, 30), brush.get(), 3);
        dc->DrawLine({ 220, 400 }, { 500, 500 }, brush.get(), 2, round_stroke.get());
        dc->PopAxisAlignedClip();
        draw_controller(256, 256, 0.5f, 1.0f, 0, false, 0);
        const HRESULT hr = dc->EndDraw();
        k = k0;
        ctx->Flush();
        layouts.clear(); // the warm-up strings were laid out at k = 1
        log_line(Log::Lifecycle, "launcher ui: warm-up pass %.1f ms hr=0x%08lX", std::chrono::duration<double, std::milli>(Clock::now() - t0).count(),
            static_cast<unsigned long>(hr));
    }
    std::wstring masked_key() const {
        if (sgdb_key.empty())
            return L"Not set";
        std::wstring m;
        for (size_t i = 0; i + 4 < sgdb_key.size() && i < 12; ++i)
            m += L'•';
        return m + sgdb_key.substr(sgdb_key.size() >= 4 ? sgdb_key.size() - 4 : 0);
    }

    // ---------------------------------------------------------------- menu
    void draw_settings_menu(float a) {
        section_title(L"SETTINGS", L"Vitox settings", a);
        const InputConfig cur = input_config(), def = input_config_default();
        const bool idef = memcmp(cur.map, def.map, sizeof(def.map)) == 0 && cur.dead_l == def.dead_l && cur.dead_r == def.dead_r;
        struct Row {
            const wchar_t *label, *desc;
            std::wstring value;
            const wchar_t *icon;
        };
        const float gm = graphics_resolution_multiplier();
        wchar_t gv[32];
        swprintf_s(gv, L"%.1fx", gm);
        const bool sa = graphics_async_pipelines(), sp = graphics_pipeline_prewarm();
        const Row rows[kMenuRows] = {
            { L"Input", L"Button mapping, stick dead zones and a live controller test", idef ? L"Default" : L"Custom", L"✥" },
            { L"Graphics", L"Image quality: internal resolution upscaling", gv, L"▩" },
            { L"Shader Compilation", L"Background compiling and pre-warm at launch",
                sa && sp ? L"Async · Pre-warm" : sa ? L"Async" : sp ? L"Pre-warm" : L"OFF", L"◈" },
            { L"Performance Statistics Overlay", L"FPS, memory and CPU over the running game", stats_enabled() ? (stats_mode() == StatsMode::Full ? L"ON · Full" : L"ON · FPS") : L"OFF",
                L"≡" },
            { L"Library", L"Show or hide homebrew apps; choose the USB install folder", library_show_homebrew() ? L"Homebrew shown" : L"Homebrew hidden", L"▤" },
            { L"Artwork", L"Custom card and background art from SteamGridDB", sgdb_key.empty() ? L"No API key" : L"SteamGridDB", L"▣" },
            { L"About", L"The installed Vitox package (Package.Current)", version, L"i" },
        };
        const float x0 = kLeft, x1 = kLeft + 1000, h = 80; // 0.0.29.0: seven rows (92 for six, 104 for five)
        float y = 284;
        for (int i = 0; i < kMenuRows; ++i) {
            const bool focus = zone == Zone::Content && i == menu_sel;
            row_frame(x0, y, x1, y + h, focus, a, 24);
            brush->SetColor(rgba(0x8A4DFF, 0.35f * a));
            dc->FillEllipse(D2D1::Ellipse({ (x0 + 58) * k, (y + h / 2) * k }, 26 * k, 26 * k), brush.get());
            const float iw = measure(rows[i].icon, 28, DWRITE_FONT_WEIGHT_BOLD);
            text(rows[i].icon, x0 + 58 - iw / 2, y + h / 2 - 20, 28, DWRITE_FONT_WEIGHT_BOLD, rgba(0xEADFFF, a));
            text(rows[i].label, x0 + 108, y + 8, 28, DWRITE_FONT_WEIGHT_SEMI_BOLD, rgba(0xFFFFFF, a), 600);
            text(rows[i].desc, x0 + 108, y + 46, 18, DWRITE_FONT_WEIGHT_NORMAL, rgba(0xB9A6E0, a), 600);
            float px = x1 - 36;
            if (i != kMenuAbout) {
                text(L"›", x1 - 52, y + h / 2 - 30, 40, DWRITE_FONT_WEIGHT_SEMI_BOLD, rgba(0xC9A2FF, a));
                px -= 30;
            }
            pill(rows[i].value, px, y + h / 2, a, 0x8A4DFF, 0.32f, 0xF2EBFF, i == kMenuAbout ? L"Consolas" : L"Segoe UI", 22);
            y += h + 10;
        }
        // a preview of the focused page on the right
        if (menu_sel == kMenuInput) {
            draw_controller(1480, 560, 0.62f, a * 0.95f, 0, false, 0);
        } else if (menu_sel == kMenuGraphics) {
            draw_resolution_frames(1250, 330, 520, 460, a);
        } else if (menu_sel == kMenuShaders) {
            draw_shaders_preview(1290, 400, a);
        } else if (menu_sel == kMenuStats) {
            draw_stats_preview(1330, 380, StatsMode::Full, a, true);
        } else if (menu_sel == kMenuLibrary) {
            draw_library_preview(1290, 400, a);
        } else if (menu_sel == kMenuArtwork) {
            const int n = std::min<int>(3, static_cast<int>(apps.size()));
            for (int i = n - 1; i >= 0; --i) {
                AppEntry &e = apps[(sel + i) % apps.size()];
                ensure_art(e, false);
                const float s = 230.0f - i * 26, x = 1340.0f + i * 120, yy = 420.0f + i * 30;
                fill_round(x - 4, yy + 10, x + s + 4, yy + s + 16, 30, rgba(0x05010C, 0.3f * a));
                if (e.icon)
                    draw_cover(e.icon.get(), x, yy, x + s, yy + s, 28, a * (1 - i * 0.18f));
                else
                    fill_gradient_round(x, yy, x + s, yy + s, 28, rgba(0x6C2BD9, a), rgba(0x1E0A3C, a));
                stroke_round(x, yy, x + s, yy + s, 28, rgba(0xC9A2FF, 0.35f * a), 1.5f);
            }
            text(L"steamgriddb.com", 1345, 760, 22, DWRITE_FONT_WEIGHT_SEMI_BOLD, rgba(0xB9A6E0, a));
        } else {
            com_ptr<ID2D1GradientStopCollection> stops;
            const D2D1_GRADIENT_STOP gs[] = { { 0.0f, rgba(0xF4ECFF, a) }, { 0.5f, rgba(0xC594FF, a) }, { 1.0f, rgba(0x8C5BFF, a) } };
            dc->CreateGradientStopCollection(gs, 3, stops.put());
            com_ptr<ID2D1LinearGradientBrush> g;
            dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({ 1330 * k, 440 * k }, { 1700 * k, 560 * k }), stops.get(), g.put());
            if (IDWriteTextLayout *l = layout(L"VITOX", 96 * k, DWRITE_FONT_WEIGHT_BLACK, 800 * k, 18 * k))
                dc->DrawTextLayout({ 1330 * k, 440 * k }, l, g.get());
            text(L"By Nirally with Claude", 1338, 566, 22, DWRITE_FONT_WEIGHT_SEMI_BOLD, rgba(0xD9C9FF, a), 600, 1); // 0.0.31.0
            text(version, 1336, 608, 28, DWRITE_FONT_WEIGHT_SEMI_BOLD, rgba(0xB9A6E0, a), 600, 2, false, L"Consolas");
        }
    }

    // ---------------------------------------------------------------- the controller drawing (Input page + menu preview)
    // 0.0.26.2: the "Neon Line" controller (the user's pick of 5 designs): a glowing outline only; pressed buttons fill
    // with their colour. The outline's lower notch is shallow so every control sits fully inside it (>= 25 units margin).
    // focus_bit: the button of the selected mapping row (pulsing ring); focus_stick: 1 left / 2 right dead-zone row
    void draw_controller(float cx, float cy, float s, float a, uint32_t focus_bit, bool live, int focus_stick) {
        const uint32_t b = live ? pad.buttons : 0;
        const auto Pt = [&](float x, float y) { return D2D1::Point2F((cx + x * s) * k, (cy + y * s) * k); };
        const auto Rc = [&](float x0, float y0, float x1, float y1) { return R(cx + x0 * s, cy + y0 * s, cx + x1 * s, cy + y1 * s); };
        const float pulse = 0.5f + 0.5f * std::sin(time_s * 4.0f);
        const float px = s * k; // one layout unit in pixels
        // a soft glow: wide faint strokes under the crisp one
        const auto glow_geom = [&](ID2D1Geometry *g, uint32_t rgb, float w, float strength = 1.0f) {
            const float ws[3] = { 13, 8, 4.5f }, as[3] = { 0.05f, 0.10f, 0.18f };
            for (int i = 0; i < 3; ++i) {
                brush->SetColor(rgba(rgb, as[i] * strength * a));
                dc->DrawGeometry(g, brush.get(), (w + ws[i]) * px, round_stroke.get());
            }
        };
        const auto glow_circle = [&](float x, float y, float r, uint32_t rgb, float strength = 1.0f) {
            const float ws[3] = { 13, 8, 4.5f }, as[3] = { 0.05f, 0.10f, 0.18f };
            for (int i = 0; i < 3; ++i) {
                brush->SetColor(rgba(rgb, as[i] * strength * a));
                dc->DrawEllipse(D2D1::Ellipse(Pt(x, y), r * px, r * px), brush.get(), (2.5f + ws[i]) * px);
            }
        };
        const auto glow_fill = [&](float x, float y, float r, uint32_t rgb) {
            for (int i = 3; i >= 1; --i) {
                brush->SetColor(rgba(rgb, 0.10f * a));
                dc->FillEllipse(D2D1::Ellipse(Pt(x, y), (r + i * 5) * px, (r + i * 5) * px), brush.get());
            }
            brush->SetColor(rgba(rgb, a));
            dc->FillEllipse(D2D1::Ellipse(Pt(x, y), r * px, r * px), brush.get());
        };
        const auto ring = [&](float x, float y, float r, uint32_t rgb, float alpha, float w) {
            brush->SetColor(rgba(rgb, alpha * a));
            dc->DrawEllipse(D2D1::Ellipse(Pt(x, y), r * px, r * px), brush.get(), w * px);
        };
        const auto focus_ring = [&](float x, float y, float r) {
            if (live)
                ring(x, y, r, 0xFFFFFF, 0.55f + 0.4f * pulse, 3);
        };
        const auto label = [&](const wchar_t *t, float x, float y, float size, uint32_t rgb, float alpha = 1.0f) {
            const float tw = measure(t, size * s, DWRITE_FONT_WEIGHT_BOLD);
            text(t, cx + x * s - tw / 2, cy + y * s - size * s * 0.68f, size * s, DWRITE_FONT_WEIGHT_BOLD, rgba(rgb, alpha * a));
        };

        // ---- triggers (level bars) and bumpers (arcs over the shoulders)
        const auto trigger = [&](float x0, float x1, float v, const wchar_t *lab) {
            const float y0 = -268, y1 = -228, r = 20;
            if (v > 0.01f) {
                const float xf = x0 + 4 + (x1 - x0 - 8) * v;
                for (int i = 2; i >= 1; --i) {
                    brush->SetColor(rgba(0xB07CFF, 0.10f * a));
                    dc->FillRoundedRectangle({ Rc(x0 + 4 - i * 4, y0 + 4 - i * 4, xf + i * 4, y1 - 4 + i * 4), (r + i * 4) * px, (r + i * 4) * px }, brush.get());
                }
                com_ptr<ID2D1GradientStopCollection> stops;
                const D2D1_GRADIENT_STOP gs[] = { { 0.0f, rgba(0x9D5CFF, a) }, { 1.0f, rgba(0xE2C8FF, a) } };
                dc->CreateGradientStopCollection(gs, 2, stops.put());
                com_ptr<ID2D1LinearGradientBrush> g;
                dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(Pt(x0, 0), Pt(x1, 0)), stops.get(), g.put());
                dc->FillRoundedRectangle({ Rc(x0 + 4, y0 + 4, xf, y1 - 4), (r - 4) * px, (r - 4) * px }, g.get());
            }
            brush->SetColor(rgba(0xC9A2FF, 0.6f * a));
            dc->DrawRoundedRectangle({ Rc(x0, y0, x1, y1), r * px, r * px }, brush.get(), 2 * px);
            label(lab, (x0 + x1) / 2, (y0 + y1) / 2, 17, 0xF2EBFF);
        };
        trigger(-300, -168, live ? pad.lt : 0, L"LT");
        trigger(168, 300, live ? pad.rt : 0, L"RT");
        const auto bumper = [&](float x0, float x1, uint32_t bit, const wchar_t *lab) {
            const bool on = (b & bit) != 0;
            com_ptr<ID2D1PathGeometry> g;
            d2d_factory->CreatePathGeometry(g.put());
            com_ptr<ID2D1GeometrySink> sink;
            g->Open(sink.put());
            sink->BeginFigure(Pt(x0, -182), D2D1_FIGURE_BEGIN_HOLLOW);
            sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(Pt((x0 + x1) / 2, -214), Pt(x1, -182)));
            sink->EndFigure(D2D1_FIGURE_END_OPEN);
            sink->Close();
            if (on)
                glow_geom(g.get(), 0xC9A2FF, 5, 1.6f);
            brush->SetColor(on ? rgba(0xFFFFFF, a) : rgba(0xC9A2FF, 0.75f * a));
            dc->DrawGeometry(g.get(), brush.get(), (on ? 6.0f : 5.0f) * px, round_stroke.get());
            label(lab, (x0 + x1) / 2, -210 + 1, 16, on ? 0xFFFFFF : 0xCDB8F5);
            if (focus_bit == bit && live) {
                brush->SetColor(rgba(0xFFFFFF, (0.45f + 0.4f * pulse) * a));
                dc->DrawRoundedRectangle({ Rc(x0 - 10, -224, x1 + 10, -168), 20 * px, 20 * px }, brush.get(), 2.5f * px);
            }
        };
        bumper(-318, -150, kLB, L"LB");
        bumper(150, 318, kRB, L"RB");

        // ---- the body outline (shallow lower notch: the D-pad and the right stick sit fully inside)
        com_ptr<ID2D1PathGeometry> body;
        d2d_factory->CreatePathGeometry(body.put());
        {
            com_ptr<ID2D1GeometrySink> sink;
            body->Open(sink.put());
            sink->BeginFigure(Pt(-250, -168), D2D1_FIGURE_BEGIN_FILLED);
            const float bz[][6] = {
                { -140, -196, 140, -196, 250, -168 }, { 340, -146, 366, -40, 384, 70 }, { 404, 186, 384, 262, 318, 262 }, { 264, 262, 226, 226, 188, 186 },
                { 140, 150, -140, 150, -188, 186 }, { -226, 226, -264, 262, -318, 262 }, { -384, 262, -404, 186, -384, 70 }, { -366, -40, -340, -146, -250, -168 },
            };
            for (const auto &c : bz)
                sink->AddBezier(D2D1::BezierSegment(Pt(c[0], c[1]), Pt(c[2], c[3]), Pt(c[4], c[5])));
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            sink->Close();
        }
        brush->SetColor(rgba(0x1A0B36, 0.35f * a));
        dc->FillGeometry(body.get(), brush.get());
        glow_geom(body.get(), 0xB07CFF, 3, 1.2f);
        {
            com_ptr<ID2D1GradientStopCollection> stops;
            const D2D1_GRADIENT_STOP gs[] = { { 0.0f, rgba(0xF0E2FF, a) }, { 0.5f, rgba(0xB07CFF, a) }, { 1.0f, rgba(0x6A35E8, a) } };
            dc->CreateGradientStopCollection(gs, 3, stops.put());
            com_ptr<ID2D1LinearGradientBrush> g;
            dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(Pt(-404, -196), Pt(404, 262)), stops.get(), g.put());
            dc->DrawGeometry(body.get(), g.get(), 3 * px, round_stroke.get());
        }

        // ---- guide, view, menu
        glow_circle(0, -118, 24, 0xB07CFF);
        ring(0, -118, 24, 0xD9C2FF, 1.0f, 3);
        label(L"V", 0, -118, 22, 0xF2EBFF);
        const auto small_btn = [&](float x, float y, uint32_t bit) {
            const bool on = (b & bit) != 0;
            if (on)
                glow_fill(x, y, 16, 0xB07CFF);
            ring(x, y, 16, on ? 0xFFFFFF : 0xC9A2FF, on ? 1.0f : 0.85f, 2);
            label(button_glyph(bit), x, y, 16, on ? 0xFFFFFF : 0xE7D6FF);
            if (focus_bit == bit)
                focus_ring(x, y, 25);
        };
        small_btn(-66, -54, kView);
        small_btn(66, -54, kMenu);

        // ---- sticks: the ring, the dead zone, the cap (moves with the stick) and the exact position dot
        const auto stick = [&](float x, float y, float sx, float sy, bool pressed, float dz, bool focus) {
            const float rb = 58;
            ring(x, y, rb, 0xC9A2FF, 0.55f, 2);
            if (dz > 0) {
                brush->SetColor(rgba(0xFF8A9A, 0.25f * a));
                dc->FillEllipse(D2D1::Ellipse(Pt(x, y), rb * dz * px, rb * dz * px), brush.get());
                ring(x, y, rb * dz, 0xFF8A9A, 0.8f, 1.5f);
            }
            const float m = std::sqrt(sx * sx + sy * sy);
            if (live && m > 0.02f) {
                brush->SetColor(rgba(0xFFFFFF, 0.5f * a));
                dc->DrawLine(Pt(x, y), Pt(x + sx * rb, y + sy * rb), brush.get(), 2 * px, round_stroke.get());
            }
            const float capx = x + sx * 22, capy = y + sy * 22;
            if (pressed)
                glow_fill(capx, capy, 34, 0x8A4DFF);
            glow_circle(capx, capy, 34, 0xC9A2FF, 0.9f);
            ring(capx, capy, 34, 0xE7D6FF, 1.0f, 2.5f);
            if (live) {
                const bool dead = dz > 0 && m <= dz; // red only inside a real dead zone
                glow_fill(x + sx * rb, y + sy * rb, 6, dead ? 0xFF8A9A : 0xFFFFFF);
            }
            if (focus)
                focus_ring(x, y, rb + 10);
        };
        stick(-206, -44, live ? pad.lx : 0, live ? pad.ly : 0, (b & kL3) != 0, live || focus_stick ? icfg.dead_l : 0, focus_stick == 1);
        stick(118, 82, live ? pad.rx : 0, live ? pad.ry : 0, (b & kR3) != 0, live || focus_stick ? icfg.dead_r : 0, focus_stick == 2);

        // ---- D-pad: a plus outline; a pressed arm fills with light
        {
            const float dx = -118, dy = 82, aw = 17, al = 52;
            const auto arm = [&](float x0, float y0, float x1, float y1, uint32_t bit) {
                if (b & bit) {
                    for (int i = 2; i >= 1; --i) {
                        brush->SetColor(rgba(0xB07CFF, 0.12f * a));
                        dc->FillRoundedRectangle({ Rc(dx + x0 - i * 4, dy + y0 - i * 4, dx + x1 + i * 4, dy + y1 + i * 4), (6 + i * 4) * px, (6 + i * 4) * px }, brush.get());
                    }
                    brush->SetColor(rgba(0xC9A2FF, a));
                    dc->FillRoundedRectangle({ Rc(dx + x0, dy + y0, dx + x1, dy + y1), 6 * px, 6 * px }, brush.get());
                }
                if (focus_bit == bit && live) {
                    brush->SetColor(rgba(0xFFFFFF, (0.55f + 0.4f * pulse) * a));
                    dc->DrawRoundedRectangle({ Rc(dx + x0 - 7, dy + y0 - 7, dx + x1 + 7, dy + y1 + 7), 12 * px, 12 * px }, brush.get(), 3 * px);
                }
            };
            arm(-aw + 3, -al + 3, aw - 3, -aw, kDUp);
            arm(-aw + 3, aw, aw - 3, al - 3, kDDown);
            arm(-al + 3, -aw + 3, -aw, aw - 3, kDLeft);
            arm(aw, -aw + 3, al - 3, aw - 3, kDRight);
            com_ptr<ID2D1PathGeometry> g;
            d2d_factory->CreatePathGeometry(g.put());
            com_ptr<ID2D1GeometrySink> sink;
            g->Open(sink.put());
            const float P[][2] = { { -aw, -al }, { aw, -al }, { aw, -aw }, { al, -aw }, { al, aw }, { aw, aw }, { aw, al }, { -aw, al }, { -aw, aw }, { -al, aw },
                { -al, -aw }, { -aw, -aw } };
            sink->BeginFigure(Pt(dx + P[0][0], dy + P[0][1]), D2D1_FIGURE_BEGIN_HOLLOW);
            for (int i = 1; i < 12; ++i)
                sink->AddLine(Pt(dx + P[i][0], dy + P[i][1]));
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            sink->Close();
            brush->SetColor(rgba(0xC9A2FF, 0.8f * a));
            dc->DrawGeometry(g.get(), brush.get(), 2.5f * px, round_stroke.get());
        }

        // ---- A B X Y
        const auto face = [&](float x, float y, uint32_t bit, uint32_t lit) {
            const bool on = (b & bit) != 0;
            if (on)
                glow_fill(x, y, 24, lit);
            ring(x, y, 24, lit, 1.0f, 2.5f);
            label(button_glyph(bit), x, y, 22, on ? 0x0B0418 : lit);
            if (focus_bit == bit)
                focus_ring(x, y, 32);
        };
        face(212, -96, kY, 0xFFD25A);
        face(164, -48, kX, 0x5AA2FF);
        face(260, -48, kB, 0xFF6B7D);
        face(212, 0, kA, 0x5BE07A);
    }

    // ---------------------------------------------------------------- Performance Statistics Overlay page (0.0.28.0)
    // a schematic of the in-game panel: labels only, no numbers (the real values exist only while a game runs)
    void draw_stats_preview(float x, float y, StatsMode mode, float a, bool big) {
        const float s = big ? 1.3f : 1.0f;
        if (mode == StatsMode::FpsOnly) {
            const float w = 156 * s, h = 54 * s;
            fill_round(x, y, x + w, y + h, 16 * s, rgba(0x0B0517, 0.85f * a));
            stroke_round(x, y, x + w, y + h, 16 * s, rgba(0xB07CFF, 0.6f * a), 1.5f);
            brush->SetColor(rgba(0xA66BFF, a));
            dc->FillEllipse(D2D1::Ellipse({ (x + 22 * s) * k, (y + h / 2) * k }, 5 * s * k, 5 * s * k), brush.get());
            text(L"FPS:", x + 36 * s, y + h / 2 - 13 * s, 18 * s, DWRITE_FONT_WEIGHT_BOLD, rgba(0xD9C9FF, a));
            const float dw = measure(L"\u2013\u2013", 24 * s, DWRITE_FONT_WEIGHT_BOLD);
            text(L"\u2013\u2013", x + w - 16 * s - dw, y + h / 2 - 17 * s, 24 * s, DWRITE_FONT_WEIGHT_BOLD, rgba(0xFFFFFF, a));
            return;
        }
        const float w = 316 * s, h = 174 * s;
        fill_round(x, y, x + w, y + h, 18 * s, rgba(0x0B0517, 0.85f * a));
        stroke_round(x, y, x + w, y + h, 18 * s, rgba(0xB07CFF, 0.6f * a), 1.5f);
        text(L"VITOX PERFORMANCE", x + 20 * s, y + 13 * s, 13 * s, DWRITE_FONT_WEIGHT_BOLD, rgba(0xB98BFF, a), 600, 3 * s);
        brush->SetColor(rgba(0xB48CFF, 0.25f * a));
        dc->FillRectangle(R(x + 20 * s, y + 40 * s, x + w - 20 * s, y + 41 * s), brush.get());
        const wchar_t *labels[] = { L"FPS", L"Memory", L"CPU" };
        for (int i = 0; i < 3; ++i) {
            const float ry = y + 48 * s + i * 39 * s;
            text(labels[i], x + 20 * s, ry + 8 * s, 18 * s, DWRITE_FONT_WEIGHT_SEMI_BOLD, rgba(0xD9C9FF, a));
            const wchar_t *v = L"\u2013\u2013";
            const float vw = measure(v, 21 * s, DWRITE_FONT_WEIGHT_BOLD);
            text(v, x + w - 20 * s - vw, y + 48 * s + i * 39 * s + 6 * s, 21 * s, DWRITE_FONT_WEIGHT_BOLD, rgba(0xFFFFFF, a));
        }
    }
    void draw_stats_page(float a) {
        section_title(L"SETTINGS  \u203A  PERFORMANCE", L"Performance Statistics Overlay", a);
        text(L"Real-time statistics in the top-right corner while a game runs. The in-game menu (R3) has the same switch.", kLeft, 266, 20,
            DWRITE_FONT_WEIGHT_NORMAL, rgba(0xB9A6E0, a), 1600);
        const bool on = stats_enabled();
        const StatsMode mode = stats_mode();
        // A. the toggle
        {
            const float x0 = kLeft, x1 = 1770, y0 = 318, y1 = 420;
            row_frame(x0, y0, x1, y1, zone == Zone::Content && stats_row == 0, a, 26);
            text(L"Enable Statistics Overlay", x0 + 40, y0 + 20, 29, DWRITE_FONT_WEIGHT_SEMI_BOLD, rgba(0xFFFFFF, a), 900);
            text(on ? L"Shown while a game or homebrew app is running" : L"Nothing is shown during gameplay", x0 + 40, y0 + 60, 19, DWRITE_FONT_WEIGHT_NORMAL,
                rgba(0xB9A6E0, a), 900);
            const float sw = 104, sh = 52, sx = x1 - 60 - sw - 90, sy = (y0 + y1) / 2 - sh / 2;
            if (on)
                fill_gradient_round(sx, sy, sx + sw, sy + sh, sh / 2, rgba(0x9D5CFF, a), rgba(0x6A35E8, a), false);
            else
                fill_round(sx, sy, sx + sw, sy + sh, sh / 2, rgba(0x2A1A4A, a));
            stroke_round(sx, sy, sx + sw, sy + sh, sh / 2, rgba(0xC9A2FF, (on ? 0.9f : 0.4f) * a), 1.5f);
            const float kx = on ? sx + sw - sh / 2 : sx + sh / 2;
            brush->SetColor(rgba(0xFFFFFF, a));
            dc->FillEllipse(D2D1::Ellipse({ kx * k, (sy + sh / 2) * k }, (sh / 2 - 6) * k, (sh / 2 - 6) * k), brush.get());
            text(on ? L"ON" : L"OFF", sx + sw + 22, sy + 9, 26, DWRITE_FONT_WEIGHT_BOLD, rgba(on ? 0xFFFFFF : 0x8F7DB8, a));
        }
        // B. the display mode: two cards, the selected one highlighted
        text(L"DISPLAY MODE", kLeft, 452, 15, DWRITE_FONT_WEIGHT_BOLD, rgba(0x8F7DB8, a), 600, 4);
        const float cy0 = 484, cy1 = 900, cw = (1770 - kLeft - 30) / 2;
        for (int i = 0; i < 2; ++i) {
            const StatsMode m = i == 0 ? StatsMode::FpsOnly : StatsMode::Full;
            const bool selected = m == mode, focus = zone == Zone::Content && stats_row == 1 && selected;
            const float x0 = kLeft + i * (cw + 30), x1 = x0 + cw;
            const float da = a * (on ? 1.0f : 0.75f); // still choosable (and saved) while the overlay is off
            if (selected) {
                const float pulse = 0.5f + 0.5f * std::sin(time_s * 3.2f);
                if (focus)
                    fill_round(x0 - 6, cy0 - 6, x1 + 6, cy1 + 6, 30, rgba(0xA66BFF, (0.12f + 0.10f * pulse) * da));
                fill_gradient_round(x0, cy0, x1, cy1, 26, rgba(0x3E1E7A, 0.95f * da), rgba(0x24104A, 0.95f * da), false);
                stroke_round(x0, cy0, x1, cy1, 26, rgba(0xC9A2FF, (focus ? 0.65f + 0.3f * pulse : 0.75f) * da), focus ? 3.0f : 2.0f);
                const float bw = measure(L"\u2713 Selected", 17, DWRITE_FONT_WEIGHT_BOLD) + 30;
                fill_round(x1 - 28 - bw, cy0 + 24, x1 - 28, cy0 + 58, 17, rgba(0x8A4DFF, 0.9f * da));
                text(L"\u2713 Selected", x1 - 28 - bw + 15, cy0 + 29, 17, DWRITE_FONT_WEIGHT_BOLD, rgba(0xFFFFFF, da));
            } else {
                fill_round(x0, cy0, x1, cy1, 26, rgba(0x1A0C33, 0.70f * da));
                stroke_round(x0, cy0, x1, cy1, 26, rgba(0xB48CFF, 0.22f * da), 1.2f);
            }
            text(i == 0 ? L"FPS Only" : L"Full Performance", x0 + 34, cy0 + 26, 30, DWRITE_FONT_WEIGHT_SEMI_BOLD, rgba(0xFFFFFF, da), 600);
            text(i == 0 ? L"A small frame-rate counter" : L"FPS, memory (Vitox) and CPU (Vitox)", x0 + 34, cy0 + 70, 19, DWRITE_FONT_WEIGHT_NORMAL,
                rgba(0xB9A6E0, da), 760);
            if (i == 0)
                draw_stats_preview(x0 + cw / 2 - 78, cy0 + 200, StatsMode::FpsOnly, da, false);
            else
                draw_stats_preview(x0 + cw / 2 - 158, cy0 + 150, StatsMode::Full, da, false);
        }
        text(L"Memory and CPU are those of the Vitox app process (what the running game uses), not the whole console.", kLeft, 918, 18,
            DWRITE_FONT_WEIGHT_NORMAL, rgba(0x8F7DB8, a), 1600);
    }

    // ---------------------------------------------------------------- Shader Compilation page (0.0.28.3)
    // an ON/OFF switch (the Statistics page's look), its label to the right
    void draw_switch(float sx, float sy, bool on, float a, float sw = 104, float sh = 52) {
        if (on)
            fill_gradient_round(sx, sy, sx + sw, sy + sh, sh / 2, rgba(0x9D5CFF, a), rgba(0x6A35E8, a), false);
        else
            fill_round(sx, sy, sx + sw, sy + sh, sh / 2, rgba(0x2A1A4A, a));
        stroke_round(sx, sy, sx + sw, sy + sh, sh / 2, rgba(0xC9A2FF, (on ? 0.9f : 0.4f) * a), 1.5f);
        const float kx = on ? sx + sw - sh / 2 : sx + sh / 2;
        brush->SetColor(rgba(0xFFFFFF, a));
        dc->FillEllipse(D2D1::Ellipse({ kx * k, (sy + sh / 2) * k }, (sh / 2 - 6) * k, (sh / 2 - 6) * k), brush.get());
    }
    // the settings menu preview: both switches as they are now
    void draw_shaders_preview(float x0, float y0, float a) {
        const bool v[2] = { graphics_async_pipelines(), graphics_pipeline_prewarm() };
        const wchar_t *names[2] = { L"Asynchronous compilation", L"Pre-warm at launch" };
        for (int i = 0; i < 2; ++i) {
            const float y = y0 + i * 130;
            fill_round(x0, y, x0 + 480, y + 104, 24, rgba(0x1A0C33, 0.70f * a));
            stroke_round(x0, y, x0 + 480, y + 104, 24, rgba(0xB48CFF, 0.22f * a), 1.2f);
            text(names[i], x0 + 28, y + 22, 22, DWRITE_FONT_WEIGHT_SEMI_BOLD, rgba(0xFFFFFF, a), 300);
            text(v[i] ? L"ON" : L"OFF", x0 + 28, y + 56, 19, DWRITE_FONT_WEIGHT_BOLD, rgba(v[i] ? 0xC9A2FF : 0x8F7DB8, a), 200);
            draw_switch(x0 + 480 - 28 - 88, y + 30, v[i], a, 88, 44);
        }
    }
    void draw_shaders_page(float a) {
        section_title(L"SETTINGS  \u203A  SHADER COMPILATION", L"Shader Compilation", a);
        text(L"How Vitox builds the graphics pipelines a game needs. Changes apply the next time a game starts.", kLeft, 266, 20,
            DWRITE_FONT_WEIGHT_NORMAL, rgba(0xB9A6E0, a), 1600);
        struct Item {
            const wchar_t *label, *on_desc, *off_desc;
            bool on;
        };
        const Item items[2] = {
            { L"Asynchronous Pipeline Compilation",
                L"New pipelines compile in the background while the game keeps running: less stutter, an object may appear a moment late",
                L"The game waits for every new pipeline: nothing pops in, but it stutters while pipelines compile", graphics_async_pipelines() },
            { L"Pipeline Pre-warm at Launch",
                L"Pipelines this game used before are built on a loading screen before it starts, so they are ready from the first frame",
                L"The game starts at once; its saved pipelines are built when it first needs them", graphics_pipeline_prewarm() },
        };
        for (int i = 0; i < 2; ++i) {
            const float x0 = kLeft, x1 = 1770, y0 = 318.0f + i * 136, y1 = y0 + 112;
            row_frame(x0, y0, x1, y1, zone == Zone::Content && shader_row == i, a, 26);
            text(items[i].label, x0 + 40, y0 + 22, 29, DWRITE_FONT_WEIGHT_SEMI_BOLD, rgba(0xFFFFFF, a), 1100);
            text(items[i].on ? items[i].on_desc : items[i].off_desc, x0 + 40, y0 + 64, 19, DWRITE_FONT_WEIGHT_NORMAL, rgba(0xB9A6E0, a), 1220);
            const float sw = 104, sh = 52, sx = x1 - 60 - sw - 90, sy = (y0 + y1) / 2 - sh / 2;
            draw_switch(sx, sy, items[i].on, a, sw, sh);
            text(items[i].on ? L"ON" : L"OFF", sx + sw + 22, sy + 9, 26, DWRITE_FONT_WEIGHT_BOLD, rgba(items[i].on ? 0xFFFFFF : 0x8F7DB8, a));
        }
        // what the combination means (one line each)
        text(L"HOW THEY WORK TOGETHER", kLeft, 628, 15, DWRITE_FONT_WEIGHT_BOLD, rgba(0x8F7DB8, a), 600, 4);
        const bool sa = graphics_async_pipelines(), sp = graphics_pipeline_prewarm();
        const wchar_t *lines[3] = {
            L"Pre-warm prepares the pipelines a game has already used; asynchronous compilation hides the cost of the new ones met during play.",
            L"Every pipeline a game builds is remembered, so the pre-warm list grows as you play and later launches stutter less.",
            sa && sp   ? L"Current: both ON \u2013 the smoothest play (recommended)."
            : sa       ? L"Current: asynchronous only \u2013 games start faster; the first minutes may show brief pop-in."
            : sp       ? L"Current: pre-warm only \u2013 familiar scenes are smooth; new pipelines still pause the game briefly."
                       : L"Current: both OFF \u2013 the game pauses briefly each time it meets a new pipeline.",
        };
        for (int i = 0; i < 3; ++i) {
            glyph_badge(kLeft + 16, 678 + i * 56 + 14, 14, i == 2 ? L"\u2713" : L"i", 0x6A35E8, a, 16);
            text(lines[i], kLeft + 46, 678 + i * 56, 20, DWRITE_FONT_WEIGHT_NORMAL, rgba(i == 2 ? 0xEADFFF : 0xB9A6E0, a), 1600);
        }
    }

    // ---------------------------------------------------------------- Library page (0.0.29.0)
    unsigned game_count() const {
        unsigned n = 0;
        for (const VitoxAppInfo &f : all_found)
            n += strcmp(f.app_root, "homebrew") != 0;
        return n;
    }
    void draw_library_preview(float x0, float y0, float a) {
        const bool on = library_show_homebrew();
        fill_round(x0, y0, x0 + 480, y0 + 104, 24, rgba(0x1A0C33, 0.70f * a));
        stroke_round(x0, y0, x0 + 480, y0 + 104, 24, rgba(0xB48CFF, 0.22f * a), 1.2f);
        text(L"Show Homebrew", x0 + 28, y0 + 22, 22, DWRITE_FONT_WEIGHT_SEMI_BOLD, rgba(0xFFFFFF, a), 300);
        text(on ? L"ON" : L"OFF", x0 + 28, y0 + 56, 19, DWRITE_FONT_WEIGHT_BOLD, rgba(on ? 0xC9A2FF : 0x8F7DB8, a), 200);
        draw_switch(x0 + 480 - 28 - 88, y0 + 30, on, a, 88, 44);
        const float y1 = y0 + 130;
        fill_round(x0, y1, x0 + 480, y1 + 104, 24, rgba(0x1A0C33, 0.70f * a));
        stroke_round(x0, y1, x0 + 480, y1 + 104, 24, rgba(0xB48CFF, 0.22f * a), 1.2f);
        draw_plus_tile(x0 + 22, y1 + 18, 68, a, false);
        text(L"Install from USB", x0 + 112, y1 + 22, 22, DWRITE_FONT_WEIGHT_SEMI_BOLD, rgba(0xFFFFFF, a), 340);
        wchar_t buf[96];
        swprintf_s(buf, L"%u game%s · %u homebrew", game_count(), game_count() == 1 ? L"" : L"s", shown_homebrew + hidden_homebrew);
        text(buf, x0 + 112, y1 + 56, 19, DWRITE_FONT_WEIGHT_NORMAL, rgba(0xB9A6E0, a), 340);
    }
    void draw_library_page(float a) {
        section_title(L"SETTINGS  ›  LIBRARY", L"Library", a);
        text(L"What the library lists, and how to add games to it.", kLeft, 266, 20, DWRITE_FONT_WEIGHT_NORMAL, rgba(0xB9A6E0, a), 1600);
        const bool on = lib_force >= 0 ? lib_force == 1 : library_show_homebrew();
        const unsigned hb = shown_homebrew + hidden_homebrew;
        {
            const float x0 = kLeft, x1 = 1770, y0 = 318, y1 = 430;
            row_frame(x0, y0, x1, y1, zone == Zone::Content && lib_row == 0, a, 26);
            text(L"Show Homebrew Applications", x0 + 40, y0 + 22, 29, DWRITE_FONT_WEIGHT_SEMI_BOLD, rgba(0xFFFFFF, a), 1100);
            wchar_t desc[160];
            if (on)
                swprintf_s(desc, L"Apps from the homebrew folder are listed with your games (%u homebrew app%s)", hb, hb == 1 ? L"" : L"s");
            else
                swprintf_s(desc, L"Only games are listed: %u homebrew app%s hidden (nothing is deleted)", hb, hb == 1 ? L" is" : L"s are");
            text(desc, x0 + 40, y0 + 66, 19, DWRITE_FONT_WEIGHT_NORMAL, rgba(0xB9A6E0, a), 1220);
            const float sw = 104, sh = 52, sx = x1 - 60 - sw - 90, sy = (y0 + y1) / 2 - sh / 2;
            draw_switch(sx, sy, on, a, sw, sh);
            text(on ? L"ON" : L"OFF", sx + sw + 22, sy + 9, 26, DWRITE_FONT_WEIGHT_BOLD, rgba(on ? 0xFFFFFF : 0x8F7DB8, a));
        }
        {
            // 0.1.1.0: the USB install folder
            const std::wstring folder = library_install_folder();
            const float x0 = kLeft, x1 = 1770, y0 = 446, y1 = 558;
            row_frame(x0, y0, x1, y1, zone == Zone::Content && lib_row == 1, a, 26);
            text(L"USB Install Folder", x0 + 40, y0 + 22, 29, DWRITE_FONT_WEIGHT_SEMI_BOLD, rgba(0xFFFFFF, a), 1000);
            text(folder.empty() ? L"The installer searches every folder of the USB drive for .zip and .vpk archives"
                                : L"The installer lists only the archives in this folder (on any USB drive), not its sub-folders",
                x0 + 40, y0 + 66, 19, DWRITE_FONT_WEIGHT_NORMAL, rgba(0xB9A6E0, a), 1000);
            const std::wstring v = folder.empty() ? L"Whole drive" : folder;
            const wchar_t *vf = folder.empty() ? L"Segoe UI" : L"Consolas";
            const float vw = std::min(560.0f, measure(v, 24, DWRITE_FONT_WEIGHT_BOLD, 0, vf) + 40);
            const float vx = x1 - 50 - vw, vy = (y0 + y1) / 2 - 24;
            fill_round(vx, vy, vx + vw, vy + 48, 24, rgba(folder.empty() ? 0x3A1B70 : 0x8A4DFF, (folder.empty() ? 0.45f : 0.30f) * a));
            text(v, vx + 20, vy + 9, 24, DWRITE_FONT_WEIGHT_BOLD, rgba(folder.empty() ? 0xC9B8EE : 0xFFFFFF, a), vw - 40, 0, false, vf);
        }
        text(L"INSTALLING GAMES AND DLC", kLeft, 604, 15, DWRITE_FONT_WEIGHT_BOLD, rgba(0x8F7DB8, a), 600, 4);
        draw_plus_tile(kLeft, 644, 150, a, false);
        const wchar_t *lines[4] = {
            L"Select the + tile at the end of the library, then choose a .zip or .vpk archive on a USB drive.",
            L"Games and homebrew appear in the library; updates merge into their game; DLC goes to the game's add-on content.",
            L"An archive may hold several items (a game with its update and DLC): each one is checked before anything is written.",
            L"Decrypted dumps install as they are; NoNpDrm dumps are decrypted with their license (work.bin) while installing.",
        };
        for (int i = 0; i < 4; ++i) {
            const float ly = 652.0f + i * 50.0f;
            glyph_badge(kLeft + 196, ly + 14, 14, i == 3 ? L"!" : L"i", 0x6A35E8, a, 16);
            text(lines[i], kLeft + 226, ly, 20, DWRITE_FONT_WEIGHT_NORMAL, rgba(i == 3 ? 0xEADFFF : 0xB9A6E0, a), 1400);
        }
    }

    // ---------------------------------------------------------------- 0.1.1.0 the USB install folder picker
    void draw_folder_picker(float a) {
        if (!fp_open)
            return;
        const float t = ease_out(fp_anim), ma = a * t;
        brush->SetColor(rgba(0x05010C, 0.72f * ma));
        dc->FillRectangle(R(0, 0, 1920, 1080), brush.get());
        const float x0 = 360, x1 = 1560, y0 = 104 + (1 - t) * 30, y1 = 952 + (1 - t) * 30;
        fill_gradient_round(x0, y0, x1, y1, 34, rgba(0x2A1458, 0.98f * ma), rgba(0x170A30, 0.98f * ma));
        stroke_round(x0, y0, x1, y1, 34, rgba(0xC9A2FF, 0.5f * ma), 1.5f);
        const float lx0 = x0 + 44, lx1 = x1 - 44;
        text(L"USB INSTALL FOLDER", lx0, y0 + 44, 15, DWRITE_FONT_WEIGHT_BOLD, rgba(0xB07CFF, ma), 600, 5);
        text(fpv.at_drives ? L"Choose a USB drive" : L"Choose a folder", lx0, y0 + 70, 36, DWRITE_FONT_WEIGHT_SEMI_BOLD, rgba(0xFFFFFF, ma), lx1 - lx0);
        // where we are
        const std::wstring where = fpv.at_drives ? L"USB drives" : fpv.drive + L"   " + fpv.path;
        fill_round(lx0, y0 + 130, lx1, y0 + 178, 20, rgba(0x1A0C33, 0.75f * ma));
        text(where, lx0 + 22, y0 + 140, 22, DWRITE_FONT_WEIGHT_SEMI_BOLD, rgba(0xEADFFF, ma), lx1 - lx0 - 44, 0, false, L"Consolas");
        const float ly = y0 + 204, rh = 64, gap = 10;
        constexpr int kRowsShown = 7;
        const std::wstring cur = library_install_folder();
        text(L"Now: " + (cur.empty() ? std::wstring(L"the whole USB drive is searched") : cur + L"  (only this folder's archives are listed)"), lx0 + 4,
            y1 - 70, 19, DWRITE_FONT_WEIGHT_NORMAL, rgba(0xB9A6E0, ma), lx1 - lx0 - 8);
        if (fpv.loading) {
            spinner(960, ly + 200, 26, ma);
            return;
        }
        if (fpv.entries.empty()) {
            const std::wstring m = !fpv.message.empty() ? fpv.message
                : fpv.at_drives                    ? L"No USB drive found."
                                                   : L"No sub-folders here. Press X to use this folder.";
            text(m, lx0 + 4, ly + 20, 22, DWRITE_FONT_WEIGHT_NORMAL, rgba(0xF2C46B, ma), lx1 - lx0 - 8, 0, true);
            return;
        }
        if (fp_sel < fp_scroll)
            fp_scroll = fp_sel;
        if (fp_sel >= fp_scroll + kRowsShown)
            fp_scroll = fp_sel - kRowsShown + 1;
        for (int r = 0; r < kRowsShown; ++r) {
            const int i = fp_scroll + r;
            if (i >= static_cast<int>(fpv.entries.size()))
                break;
            const float ry = ly + r * (rh + gap);
            row_frame(lx0, ry, lx1, ry + rh, i == fp_sel, ma, 20);
            glyph_badge(lx0 + 40, ry + rh / 2, 16, fpv.at_drives ? L"\u25A3" : L"\u25B8", fpv.at_drives ? 0x2F6FD0 : 0x6A35E8, ma, 16);
            text(fpv.entries[static_cast<size_t>(i)], lx0 + 74, ry + 16, 24, DWRITE_FONT_WEIGHT_SEMI_BOLD, rgba(0xFFFFFF, ma), lx1 - lx0 - 120);
        }
        if (fpv.entries.size() > kRowsShown) {
            wchar_t buf[64];
            swprintf_s(buf, L"%d / %u", fp_sel + 1, static_cast<unsigned>(fpv.entries.size()));
            text(buf, lx1 - measure(buf, 18, DWRITE_FONT_WEIGHT_NORMAL), ly + kRowsShown * (rh + gap) + 4, 18, DWRITE_FONT_WEIGHT_NORMAL, rgba(0x8F7DB8, ma), 200);
        }
    }

    // ---------------------------------------------------------------- the USB installer (0.0.29.0, over the library)
    void spinner(float cx, float cy, float r, float a) {
        dc->SetTransform(D2D1::Matrix3x2F::Rotation(std::fmod(time_s * 300.0f, 360.0f), { cx * k, cy * k }));
        arc(cx, cy, r, 0.28f, rgba(0xC9A2FF, a), 4.0f);
        dc->SetTransform(D2D1::Matrix3x2F::Identity());
    }
    static uint32_t kind_color(install::Kind kd) {
        switch (kd) {
        case install::Kind::Game: return 0x8A4DFF;
        case install::Kind::Update: return 0x2F6FD0;
        case install::Kind::Dlc: return 0xC08A1E;
        default: return 0x3E9A4F;
        }
    }
    // one item row of the confirm / progress / result lists
    void install_item_row(const install::Item &it, float x0, float x1, float y, float a, int mode /* 0 confirm, 1 installing, 2 done */, bool current) {
        const float h = 88;
        fill_round(x0, y, x1, y + h, 20, rgba(0x1A0C33, 0.75f * a));
        stroke_round(x0, y, x1, y + h, 20, rgba(0xB48CFF, (current ? 0.6f : 0.2f) * a), current ? 2.0f : 1.2f);
        const std::wstring kn = install::kind_name(it.kind);
        const float cw = measure(kn, 17, DWRITE_FONT_WEIGHT_BOLD, 1.5f) + 28;
        fill_round(x0 + 22, y + 26, x0 + 22 + cw, y + 60, 17, rgba(kind_color(it.kind), 0.85f * a));
        text(kn, x0 + 36, y + 31, 17, DWRITE_FONT_WEIGHT_BOLD, rgba(0xFFFFFF, a), 200, 1.5f);
        const float tx = x0 + 150;
        text(it.title, tx, y + 12, 25, DWRITE_FONT_WEIGHT_SEMI_BOLD, rgba(0xFFFFFF, a), 560);
        std::wstring sub = it.title_id;
        if (!it.version.empty())
            sub += L"  ·  v" + it.version;
        sub += L"  ·  " + install::format_bytes(it.bytes) + L"  ·  " + std::to_wstring(it.files) + (it.files == 1 ? L" file" : L" files");
        text(sub, tx, y + 50, 17, DWRITE_FONT_WEIGHT_NORMAL, rgba(0xB9A6E0, a), 560);
        // the status column
        const float sx = x1 - 450;
        std::wstring st;
        uint32_t sc = 0xB9A6E0;
        if (!it.installable) {
            st = it.problem, sc = 0xFF9AA8;
        } else if (mode == 2 || it.done) {
            st = it.result, sc = it.ok ? 0x8FE3A0 : 0xFF9AA8;
        } else if (mode == 1) {
            st = current ? L"Installing…" : L"Waiting", sc = current ? 0xEADFFF : 0x8F7DB8;
        } else {
            st = it.kind == install::Kind::Update ? L"Merges into the installed game" : it.replaces ? L"Replaces the installed copy" : L"New";
            sc = it.replaces ? 0xF2C46B : 0x8FE3A0;
            if (!it.note.empty())
                st += L"\n" + it.note;
        }
        const bool mark = mode != 0 && (it.done || !it.installable);
        if (mark)
            glyph_badge(sx + 14, y + h / 2, 14, it.ok ? L"✓" : L"✗", it.ok ? 0x3E9A4F : 0xB23A48, a, 16);
        else if (mode == 1 && current)
            spinner(sx + 14, y + h / 2, 12, a);
        text(st, sx + 40, y + (st.find(L'\n') != std::wstring::npos || measure(st, 17, DWRITE_FONT_WEIGHT_SEMI_BOLD) > 400 ? 20.0f : 32.0f), 17,
            DWRITE_FONT_WEIGHT_SEMI_BOLD, rgba(sc, a), 400, 0, true);
    }
    void draw_install_modal(float a) {
        if (!inst_open)
            return;
        using install::Phase;
        const float t = ease_out(inst_anim), ma = a * t;
        brush->SetColor(rgba(0x05010C, 0.72f * ma));
        dc->FillRectangle(R(0, 0, 1920, 1080), brush.get());
        const float x0 = 260, x1 = 1660, y0 = 120 + (1 - t) * 30, y1 = 930 + (1 - t) * 30;
        fill_gradient_round(x0, y0, x1, y1, 34, rgba(0x2A1458, 0.98f * ma), rgba(0x170A30, 0.98f * ma));
        stroke_round(x0, y0, x1, y1, 34, rgba(0xC9A2FF, 0.5f * ma), 1.5f);
        text(L"INSTALL FROM USB", x0 + 48, y0 + 34, 16, DWRITE_FONT_WEIGHT_BOLD, rgba(0xB07CFF, ma), 600, 5);
        std::wstring title;
        switch (is.phase) {
        case Phase::Idle:
        case Phase::Scanning: title = L"Looking for archives…"; break;
        case Phase::Files: title = L"Choose an archive"; break;
        case Phase::Analyzing: title = L"Reading the archive…"; break;
        case Phase::Confirm: title = is.archive; break;
        case Phase::Installing: title = is.cancelling ? L"Cancelling…" : L"Installing…"; break;
        case Phase::Done: title = is.items.empty() ? L"Cannot install this archive" : is.message; break;
        }
        text(title, x0 + 48, y0 + 62, 36, DWRITE_FONT_WEIGHT_SEMI_BOLD, rgba(0xFFFFFF, ma), 1100);
        const float lx0 = x0 + 40, lx1 = x1 - 40, ly0 = y0 + 140;
        switch (is.phase) {
        case Phase::Idle:
        case Phase::Scanning:
        case Phase::Analyzing: {
            spinner(960, 460, 26, ma);
            const std::wstring m = is.phase == Phase::Analyzing ? L"“" + is.archive + L"”" : L"Searching the USB drives for .zip and .vpk files";
            text(m, 960 - std::min(1200.0f, measure(m, 22, DWRITE_FONT_WEIGHT_NORMAL)) / 2, 520, 22, DWRITE_FONT_WEIGHT_NORMAL, rgba(0xCDB8F5, ma), 1200);
            break;
        }
        case Phase::Files: {
            if (is.files.empty()) {
                const std::wstring m = is.message.empty() ? L"No archives found." : is.message;
                text(m, x0 + 120, 430, 24, DWRITE_FONT_WEIGHT_NORMAL, rgba(0xCDB8F5, ma), 1160, 0, true);
                text(L"Archives are found anywhere on the drive (up to 400 per drive).", x0 + 120, 540, 19, DWRITE_FONT_WEIGHT_NORMAL, rgba(0x8F7DB8, ma), 1160);
                break;
            }
            wchar_t cnt[64];
            swprintf_s(cnt, L"%d of %u", inst_sel + 1, static_cast<unsigned>(is.files.size()));
            text(cnt, x1 - 48 - measure(cnt, 20, DWRITE_FONT_WEIGHT_SEMI_BOLD), y0 + 76, 20, DWRITE_FONT_WEIGHT_SEMI_BOLD, rgba(0x8F7DB8, ma));
            const float ly1 = y1 - 40, row = 84;
            dc->PushAxisAlignedClip(R(lx0 - 8, ly0 - 8, lx1 + 8, ly1), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            for (int i = 0; i < static_cast<int>(is.files.size()); ++i) {
                const float ry = ly0 + i * row - inst_scroll;
                if (ry + row < ly0 - 10 || ry > ly1)
                    continue;
                const install::ArchiveFile &f = is.files[i];
                row_frame(lx0, ry, lx1, ry + 74, i == inst_sel, ma, 20);
                const bool vpk = f.name.size() >= 4 && _wcsicmp(f.name.c_str() + f.name.size() - 4, L".vpk") == 0;
                fill_round(lx0 + 22, ry + 20, lx0 + 92, ry + 54, 17, rgba(vpk ? 0x3E9A4F : 0x8A4DFF, 0.85f * ma));
                const wchar_t *ext = vpk ? L"VPK" : L"ZIP";
                text(ext, lx0 + 57 - measure(ext, 16, DWRITE_FONT_WEIGHT_BOLD, 1.5f) / 2, ry + 26, 16, DWRITE_FONT_WEIGHT_BOLD, rgba(0xFFFFFF, ma), 100, 1.5f);
                text(f.name, lx0 + 118, ry + 8, 24, DWRITE_FONT_WEIGHT_SEMI_BOLD, rgba(0xFFFFFF, ma), 960);
                text(f.drive + L"  ·  " + f.folder, lx0 + 118, ry + 42, 17, DWRITE_FONT_WEIGHT_NORMAL, rgba(0xB9A6E0, ma), 960);
                pill(install::format_bytes(f.size), lx1 - 24, ry + 37, ma, 0x3A1B70, 0.6f, 0xEADFFF, L"Segoe UI", 19);
            }
            dc->PopAxisAlignedClip();
            break;
        }
        case Phase::Confirm:
        case Phase::Installing:
        case Phase::Done: {
            if (is.items.empty()) { // the archive could not be used
                text(is.message, x0 + 120, 400, 24, DWRITE_FONT_WEIGHT_NORMAL, rgba(0xFFB0BC, ma), 1160, 0, true);
                break;
            }
            const int mode = is.phase == Phase::Confirm ? 0 : is.phase == Phase::Installing ? 1 : 2;
            float y = ly0;
            if (mode == 1) {
                // the progress bar
                const double frac = is.total_bytes ? std::min(1.0, static_cast<double>(is.done_bytes) / static_cast<double>(is.total_bytes)) : 0.0;
                fill_round(lx0, y, lx1, y + 22, 11, rgba(0x0E061E, ma));
                if (frac > 0.002)
                    fill_gradient_round(lx0, y, lx0 + static_cast<float>((lx1 - lx0) * frac), y + 22, 11, rgba(0x9D5CFF, ma), rgba(0xE2C8FF, ma), false);
                stroke_round(lx0, y, lx1, y + 22, 11, rgba(0xC9A2FF, 0.5f * ma), 1.2f);
                wchar_t pct[96];
                swprintf_s(pct, L"%.0f%%   %s of %s", frac * 100.0, install::format_bytes(is.done_bytes).c_str(), install::format_bytes(is.total_bytes).c_str());
                text(pct, lx0, y + 34, 22, DWRITE_FONT_WEIGHT_SEMI_BOLD, rgba(0xEADFFF, ma), 600);
                text(is.current, lx0 + 620, y + 38, 17, DWRITE_FONT_WEIGHT_NORMAL, rgba(0x8F7DB8, ma), lx1 - lx0 - 620, 0, false, L"Consolas");
                y += 84;
            }
            int current = -1;
            if (mode == 1)
                for (int i = 0; i < static_cast<int>(is.items.size()) && current < 0; ++i)
                    if (is.items[i].installable && !is.items[i].done)
                        current = i;
            const int max_rows = mode == 1 ? 5 : 6;
            for (int i = 0; i < static_cast<int>(is.items.size()) && i < max_rows; ++i, y += 98)
                install_item_row(is.items[i], lx0, lx1, y, ma, mode, i == current);
            if (static_cast<int>(is.items.size()) > max_rows) {
                wchar_t more[64];
                swprintf_s(more, L"… and %u more", static_cast<unsigned>(is.items.size()) - max_rows);
                text(more, lx0 + 22, y, 19, DWRITE_FONT_WEIGHT_SEMI_BOLD, rgba(0x8F7DB8, ma));
            }
            if (mode == 0) {
                std::wstring sp;
                uint32_t sc = 0xB9A6E0;
                if (!std::any_of(is.items.begin(), is.items.end(), [](const install::Item &i) { return i.installable; }))
                    sp = L"Nothing in this archive can be installed", sc = 0xFF9AA8;
                else if (!is.space_ok)
                    sp = L"Not enough free space: needs " + install::format_bytes(is.need_bytes) + L", " + install::format_bytes(is.free_bytes) + L" free", sc = 0xFF9AA8;
                else
                    sp = L"Needs " + install::format_bytes(is.need_bytes) + L"  ·  " + install::format_bytes(is.free_bytes) + L" free on the console";
                text(sp, lx0 + 8, y1 - 92, 21, DWRITE_FONT_WEIGHT_SEMI_BOLD, rgba(sc, ma), 1300);
                text(L"Saves are kept when a game is replaced or updated.", lx0 + 8, y1 - 58, 18, DWRITE_FONT_WEIGHT_NORMAL, rgba(0x8F7DB8, ma), 1300);
            } else if (mode == 2) {
                wchar_t sum[96];
                // 0.0.30.0: everything installed -> the archive may be deleted from the USB drive with one button
                if (is.can_delete_archive) {
                    const float py = y1 - 150;
                    fill_round(lx0, py - 14, lx1, py + 46, 22, rgba(0x1A0C33, 0.8f * ma));
                    stroke_round(lx0, py - 14, lx1, py + 46, 22, rgba(0xB48CFF, 0.3f * ma), 1.2f);
                    if (is.archive_deleted) {
                        glyph_badge(lx0 + 34, py + 16, 14, L"✓", 0x3E9A4F, ma, 16);
                        text(L"“" + is.archive + L"” was deleted from the USB drive", lx0 + 60, py + 3, 21, DWRITE_FONT_WEIGHT_SEMI_BOLD, rgba(0x8FE3A0, ma), 1200);
                    } else if (is.deleting_archive) {
                        spinner(lx0 + 34, py + 16, 12, ma);
                        text(L"Deleting “" + is.archive + L"” from the USB drive…", lx0 + 60, py + 3, 21, DWRITE_FONT_WEIGHT_SEMI_BOLD, rgba(0xEADFFF, ma), 1200);
                    } else if (!is.delete_error.empty()) {
                        text(L"Could not delete the archive: " + is.delete_error, lx0 + 24, py + 3, 20, DWRITE_FONT_WEIGHT_SEMI_BOLD, rgba(0xFF9AA8, ma), 1240);
                    } else {
                        const float bw = button_hint(lx0 + 20, py - 1, L"X", L"", 0x2F6FD0, ma) - 34;
                        text(L"Delete “" + is.archive + L"” from the USB drive? It is installed and no longer needed.", lx0 + 20 + bw, py + 3, 21,
                            DWRITE_FONT_WEIGHT_SEMI_BOLD, rgba(0xEADFFF, ma), 1200);
                    }
                }
                const unsigned missed = static_cast<unsigned>(is.items.size()) - is.installed;
                swprintf_s(sum, L"%u installed  ·  %u not installed", is.installed, missed);
                text(sum, lx0 + 8, y1 - 76, 21, DWRITE_FONT_WEIGHT_SEMI_BOLD, rgba(missed ? 0xF2C46B : 0x8FE3A0, ma), 1300);
            }
            break;
        }
        }
    }

    // ---------------------------------------------------------------- the game menu (0.0.30.0)
    std::wstring gm_size_text() const {
        const int64_t v = gm_size ? gm_size->load() : -1;
        return v < 0 ? std::wstring(L"measuring…") : install::format_bytes(static_cast<uint64_t>(v));
    }
    std::wstring gm_value(int row) const {
        wchar_t b[64];
        switch (row) {
        case 0:
            if (gm_cfg.resolution > 0)
                swprintf_s(b, L"%.1fx  (%d × %d)", gm_cfg.resolution, resolution_width(gm_cfg.resolution), resolution_height(gm_cfg.resolution));
            else
                swprintf_s(b, L"Global  (%.1fx)", graphics_resolution_multiplier());
            return b;
        case 1:
        case 2: {
            const int v = row == 1 ? gm_cfg.async : gm_cfg.prewarm;
            const bool g = row == 1 ? graphics_async_pipelines() : graphics_pipeline_prewarm();
            return v < 0 ? (g ? L"Global  (ON)" : L"Global  (OFF)") : v ? L"ON" : L"OFF";
        }
        case 3: {
            const wchar_t *global = !stats_enabled() ? L"Global  (OFF)" : stats_mode() == StatsMode::Full ? L"Global  (Full)" : L"Global  (FPS Only)";
            return gm_cfg.stats < 0 ? global : gm_cfg.stats == 0 ? L"OFF" : gm_cfg.stats == 1 ? L"FPS Only" : L"Full Performance";
        }
        case 4: return gm_cfg.is_global() ? L"Using global settings" : L"Custom";
        default: return gm_size_text();
        }
    }
    void draw_game_menu(float a) {
        if (!gm_open)
            return;
        const float t = ease_out(gm_anim), ma = a * t;
        brush->SetColor(rgba(0x05010C, 0.72f * ma));
        dc->FillRectangle(R(0, 0, 1920, 1080), brush.get());
        const float x0 = 360, x1 = 1560, y0 = 104 + (1 - t) * 30, y1 = 952 + (1 - t) * 30;
        fill_gradient_round(x0, y0, x1, y1, 34, rgba(0x2A1458, 0.98f * ma), rgba(0x170A30, 0.98f * ma));
        stroke_round(x0, y0, x1, y1, 34, rgba(0xC9A2FF, 0.5f * ma), 1.5f);
        // the game: card art, title, identity, where it is and its size
        const float ax = x0 + 44, ay = y0 + 44, as = 150;
        AppEntry *e = gm_entry();
        if (e && e->icon)
            draw_cover(e->icon.get(), ax, ay, ax + as, ay + as, 22, ma);
        else
            fill_gradient_round(ax, ay, ax + as, ay + as, 22, rgba(0x6C2BD9, ma), rgba(0x1E0A3C, ma));
        stroke_round(ax, ay, ax + as, ay + as, 22, rgba(0xC9A2FF, 0.4f * ma), 1.5f);
        const float tx = ax + as + 34;
        text(L"GAME MENU", tx, ay + 2, 15, DWRITE_FONT_WEIGHT_BOLD, rgba(0xB07CFF, ma), 600, 5);
        text(gm_title, tx, ay + 26, 36, DWRITE_FONT_WEIGHT_SEMI_BOLD, rgba(0xFFFFFF, ma), x1 - tx - 44);
        float cx = tx;
        const auto chip = [&](const std::wstring &s, uint32_t fill, float fa, uint32_t tc, const wchar_t *family = L"Segoe UI") {
            const float w = measure(s, 17, DWRITE_FONT_WEIGHT_BOLD, 1.5f, family) + 28;
            fill_round(cx, ay + 84, cx + w, ay + 116, 16, rgba(fill, fa * ma));
            text(s, cx + 14, ay + 89, 17, DWRITE_FONT_WEIGHT_BOLD, rgba(tc, ma), 600, 1.5f, false, family);
            cx += w + 10;
        };
        chip(widen(gm_app.title_id), 0x8A4DFF, 0.30f, 0xEADFFF, L"Consolas");
        chip(strcmp(gm_app.app_root, "homebrew") == 0 ? L"Homebrew" : L"Game", 0x5B2BB8, 0.30f, 0xD9C9FF);
        if (gm_app.app_ver[0])
            chip(L"v" + widen(gm_app.app_ver), 0x3A1B70, 0.45f, 0xC9B8EE);
        text(widen(gm_app.app_root) + L"  /  " + widen(gm_app.folder) + L"   ·   " + gm_size_text() + L" on this console", tx, ay + 128, 18,
            DWRITE_FONT_WEIGHT_NORMAL, rgba(0x8F7DB8, ma), x1 - tx - 44);
        const float lx0 = x0 + 44, lx1 = x1 - 44, ly = y0 + 236;
        if (gm_confirm || gm_state != 0) {
            // the uninstall confirmation (or its progress)
            const bool hb = strcmp(gm_app.app_root, "homebrew") == 0;
            fill_round(lx0, ly, lx1, y1 - 44, 26, rgba(0x3A0E22, 0.55f * ma));
            stroke_round(lx0, ly, lx1, y1 - 44, 26, rgba(0xFF8FA3, 0.45f * ma), 1.5f);
            if (gm_state != 0) {
                spinner(960, ly + 220, 26, ma);
                const std::wstring m = L"Uninstalling " + gm_title + L"…";
                text(m, 960 - measure(m, 26, DWRITE_FONT_WEIGHT_SEMI_BOLD) / 2, ly + 280, 26, DWRITE_FONT_WEIGHT_SEMI_BOLD, rgba(0xFFFFFF, ma), 1000);
                return;
            }
            text(L"Uninstall " + gm_title + L"?", lx0 + 40, ly + 36, 32, DWRITE_FONT_WEIGHT_SEMI_BOLD, rgba(0xFFFFFF, ma), lx1 - lx0 - 80);
            const std::wstring what = hb ? L"Removes the app (" + gm_size_text() + L") from this console."
                                         : L"Removes the game (" + gm_size_text() + L"), its add-on content (DLC) and its license from this console.";
            text(what, lx0 + 40, ly + 96, 21, DWRITE_FONT_WEIGHT_NORMAL, rgba(0xEADFFF, ma), lx1 - lx0 - 80, 0, true);
            text(L"Your custom artwork and this game's own settings are removed too. Save data is kept unless you choose to delete it.", lx0 + 40, ly + 160, 19,
                DWRITE_FONT_WEIGHT_NORMAL, rgba(0xB9A6E0, ma), lx1 - lx0 - 80, 0, true);
            float bx = lx0 + 40;
            const float by = ly + 270;
            bx += button_hint(bx, by, L"A", L"Uninstall, keep save data", 0x3E9A4F, ma);
            bx += button_hint(bx, by, L"X", L"Uninstall and delete save data", 0x2F6FD0, ma);
            button_hint(bx, by, L"B", L"Cancel", 0xB23A48, ma);
            text(L"This cannot be undone. To play again, install the game again from its archive.", lx0 + 40, ly + 350, 18, DWRITE_FONT_WEIGHT_NORMAL, rgba(0xFF9AA8, ma),
                lx1 - lx0 - 80);
            return;
        }
        text(L"THIS GAME'S SETTINGS", lx0 + 4, ly - 30, 15, DWRITE_FONT_WEIGHT_BOLD, rgba(0x8F7DB8, ma), 600, 4);
        static const wchar_t *const kLabels[kGmRows] = { L"Internal Resolution", L"Asynchronous Shader Compilation", L"Pipeline Pre-warm at Launch",
            L"Performance Overlay", L"Reset to Global Settings", L"Uninstall" };
        for (int i = 0; i < kGmRows; ++i) {
            const float ry = ly + i * 82.0f + (i >= 4 ? 18.0f : 0.0f), rh = 70;
            const bool focus = i == gm_row;
            row_frame(lx0, ry, lx1, ry + rh, focus, ma, 22);
            text(kLabels[i], lx0 + 30, ry + 19, 25, DWRITE_FONT_WEIGHT_SEMI_BOLD, rgba(i == 5 ? 0xFFB0BC : 0xFFFFFF, ma), 560);
            const std::wstring v = gm_value(i);
            const bool custom = (i == 0 && gm_cfg.resolution > 0) || (i == 1 && gm_cfg.async >= 0) || (i == 2 && gm_cfg.prewarm >= 0) || (i == 3 && gm_cfg.stats >= 0);
            float px = lx1 - 24;
            if (focus && i < 4) {
                text(L"›", lx1 - 40, ry + 12, 34, DWRITE_FONT_WEIGHT_SEMI_BOLD, rgba(0xC9A2FF, ma));
                px -= 26;
            }
            const float left = pill(v, px, ry + rh / 2, ma, i == 5 ? 0x7A1E3A : custom ? 0x8A4DFF : 0x3A1B70, custom ? 0.55f : 0.5f, custom ? 0xFFFFFF : 0xC9B8EE,
                L"Segoe UI", 20);
            if (focus && i < 4)
                text(L"‹", left - 26, ry + 12, 34, DWRITE_FONT_WEIGHT_SEMI_BOLD, rgba(0xC9A2FF, ma));
        }
        text(L"Changes are saved at once and apply the next time this game starts. “Global” follows Settings.", lx0 + 4, y1 - 52, 18,
            DWRITE_FONT_WEIGHT_NORMAL, rgba(0x8F7DB8, ma), lx1 - lx0);
    }

    // ---------------------------------------------------------------- Graphics page (0.0.27.0)
    // nested frames, to scale: the native Vita frame, the selected internal resolution and the maximum
    void draw_resolution_frames(float x0, float y0, float w, float h, float a) {
        const float m = graphics_resolution_multiplier(), max = graphics_max_multiplier();
        const float sc = std::min(w / (960.0f * max), h / (544.0f * max));
        const auto frame = [&](float mult, uint32_t rgb, float fill_a, float stroke_a, float width) {
            const float fw = 960.0f * mult * sc, fh = 544.0f * mult * sc;
            const float fx = x0 + (w - 960.0f * max * sc) / 2, fy = y0 + h - fh; // bottom-left aligned
            if (fill_a > 0)
                fill_round(fx, fy, fx + fw, fy + fh, 10, rgba(rgb, fill_a * a));
            stroke_round(fx, fy, fx + fw, fy + fh, 10, rgba(rgb, stroke_a * a), width);
            return D2D1::RectF(fx, fy, fx + fw, fy + fh);
        };
        frame(max, 0xB48CFF, 0.0f, 0.25f, 1.2f);
        const D2D1_RECT_F cur = frame(m, 0x9D5CFF, 0.22f, 0.95f, 2.5f);
        const D2D1_RECT_F nat = frame(1.0f, 0xFFFFFF, 0.10f, 0.7f, 1.5f);
        wchar_t buf[48];
        swprintf_s(buf, L"%d \u00D7 %d", resolution_width(m), resolution_height(m));
        text(buf, cur.left + 14, cur.top + 10, 18, DWRITE_FONT_WEIGHT_BOLD, rgba(0xFFFFFF, a), 600, 0, false, L"Consolas");
        if (m > 1.0f && nat.right - nat.left > 70)
            text(L"Vita", nat.left + 10, nat.top + 6, 15, DWRITE_FONT_WEIGHT_BOLD, rgba(0xEADFFF, a), 200);
    }
    void draw_graphics_page(float a) {
        section_title(L"SETTINGS  \u203A  GRAPHICS", L"Graphics", a);
        text(L"Image quality for Vita games and homebrew.", kLeft, 266, 20, DWRITE_FONT_WEIGHT_NORMAL, rgba(0xB9A6E0, a), 1000);
        text(L"IMAGE QUALITY", kLeft, 330, 15, DWRITE_FONT_WEIGHT_BOLD, rgba(0x8F7DB8, a), 600, 4);
        const float m = graphics_resolution_multiplier(), max = graphics_max_multiplier();
        const float x0 = kLeft, x1 = 1770, y0 = 362, y1 = 820;
        row_frame(x0, y0, x1, y1, zone == Zone::Content, a, 30);
        text(L"Internal Resolution Upscaling", x0 + 48, y0 + 34, 32, DWRITE_FONT_WEIGHT_SEMI_BOLD, rgba(0xFFFFFF, a), 1200);
        text(L"Renders games at a higher internal resolution for sharper 3D, edges and text. Higher values need more GPU power and memory.",
            x0 + 48, y0 + 82, 20, DWRITE_FONT_WEIGHT_NORMAL, rgba(0xB9A6E0, a), 1500);
        // the multiplier, big, and the resulting internal resolution under it (both computed from 960 x 544)
        wchar_t mb[16];
        swprintf_s(mb, L"%.1fx", m);
        const float cx = 960;
        {
            const float sz = 92;
            const float mw = text_width(layout(mb, sz * k, DWRITE_FONT_WEIGHT_BLACK, 800 * k, 2 * k)) / k;
            com_ptr<ID2D1GradientStopCollection> stops;
            const D2D1_GRADIENT_STOP gs[] = { { 0.0f, rgba(0xF4ECFF, a) }, { 0.55f, rgba(0xC594FF, a) }, { 1.0f, rgba(0x8C5BFF, a) } };
            dc->CreateGradientStopCollection(gs, 3, stops.put());
            com_ptr<ID2D1LinearGradientBrush> g;
            dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({ (cx - mw / 2) * k, (y0 + 128) * k }, { (cx + mw / 2) * k, (y0 + 238) * k }), stops.get(),
                g.put());
            if (IDWriteTextLayout *l = layout(mb, sz * k, DWRITE_FONT_WEIGHT_BLACK, 800 * k, 2 * k))
                dc->DrawTextLayout({ (cx - mw / 2) * k, (y0 + 120) * k }, l, g.get());
        }
        wchar_t rb[48];
        swprintf_s(rb, L"%d \u00D7 %d", resolution_width(m), resolution_height(m));
        const float rw = measure(rb, 34, DWRITE_FONT_WEIGHT_BOLD, 1, L"Consolas");
        text(rb, cx - rw / 2, y0 + 248, 34, DWRITE_FONT_WEIGHT_BOLD, rgba(0xF2EBFF, a), 900, 1, false, L"Consolas");
        wchar_t cap[96];
        if (m == 1.0f)
            swprintf_s(cap, L"Native PlayStation Vita resolution");
        else
            swprintf_s(cap, L"%g\u00D7 the pixels of the native 960 \u00D7 544", static_cast<double>(m * m));
        const float cw = measure(cap, 19, DWRITE_FONT_WEIGHT_NORMAL);
        text(cap, cx - cw / 2, y0 + 298, 19, DWRITE_FONT_WEIGHT_NORMAL, rgba(0xA592CC, a));
        // the slider: one tick per 0.5x step, a filled track up to the thumb
        const float tx0 = x0 + 150, tx1 = x1 - 150, ty = y0 + 372;
        const int steps = static_cast<int>(std::lround((max - kResolutionMin) / kResolutionStep));
        const float target = steps > 0 ? (m - kResolutionMin) / (max - kResolutionMin) : 0.0f;
        gfx_thumb = gfx_thumb < 0 ? target : approach(gfx_thumb, target, dt_, 18);
        gfx_bump = std::max(0.0f, gfx_bump - dt_ * 4);
        const float shake = gfx_bump * 6 * std::sin(time_s * 60);
        fill_round(tx0, ty - 6, tx1, ty + 6, 6, rgba(0x0E061E, a));
        stroke_round(tx0, ty - 6, tx1, ty + 6, 6, rgba(0xB48CFF, 0.25f * a), 1.0f);
        const float thx = tx0 + (tx1 - tx0) * gfx_thumb + shake;
        fill_gradient_round(tx0, ty - 6, std::max(tx0 + 12, thx), ty + 6, 6, rgba(0x7A3CF0, a), rgba(0xC9A2FF, a), false);
        for (int i = 0; i <= steps; ++i) {
            const float v = kResolutionMin + i * kResolutionStep;
            const float x = tx0 + (tx1 - tx0) * (steps ? static_cast<float>(i) / steps : 0);
            const bool on = v <= m + 0.001f;
            brush->SetColor(rgba(on ? 0xE7D6FF : 0x5A4884, a));
            dc->FillEllipse(D2D1::Ellipse({ x * k, ty * k }, 4 * k, 4 * k), brush.get());
            wchar_t tl[16];
            swprintf_s(tl, L"%.1fx", v);
            const float tw = measure(tl, 17, DWRITE_FONT_WEIGHT_SEMI_BOLD);
            text(tl, x - tw / 2, ty + 26, 17, DWRITE_FONT_WEIGHT_SEMI_BOLD, rgba(v == m ? 0xFFFFFF : 0x8F7DB8, a));
        }
        // the thumb: a clearly visible white knob with a violet glow (pulsing)
        const float pulse = 0.5f + 0.5f * std::sin(time_s * 3.2f);
        for (int i = 3; i >= 1; --i) {
            brush->SetColor(rgba(0xA66BFF, (0.10f + 0.05f * pulse) * a));
            dc->FillEllipse(D2D1::Ellipse({ thx * k, ty * k }, (22 + i * 7) * k, (22 + i * 7) * k), brush.get());
        }
        brush->SetColor(rgba(0xFFFFFF, a));
        dc->FillEllipse(D2D1::Ellipse({ thx * k, ty * k }, 20 * k, 20 * k), brush.get());
        brush->SetColor(rgba(0x8A4DFF, a));
        dc->FillEllipse(D2D1::Ellipse({ thx * k, ty * k }, 8 * k, 8 * k), brush.get());
        // when it applies, and the crash guard's message
        float ny = y1 + 26;
        glyph_badge(kLeft + 16, ny + 14, 14, L"i", 0x6A35E8, a, 16);
        text(L"Applies the next time a game starts. A game that is already running keeps the resolution it started with.", kLeft + 42, ny, 21,
            DWRITE_FONT_WEIGHT_NORMAL, rgba(0xD9C9FF, a), 1600);
        const std::wstring notice = graphics_notice();
        if (!notice.empty()) {
            ny += 44;
            const float nw = measure(notice, 20, DWRITE_FONT_WEIGHT_SEMI_BOLD) + 40;
            fill_round(kLeft, ny - 6, kLeft + nw, ny + 34, 20, rgba(0xA8741A, 0.85f * a));
            text(notice, kLeft + 20, ny + 1, 20, DWRITE_FONT_WEIGHT_SEMI_BOLD, rgba(0xFFFFFF, a), 1600);
        }
    }

    // ---------------------------------------------------------------- Input page
    float input_row_y(int i) const {
        // content coordinates: a "BUTTON MAPPING" header, 12 rows, a "STICKS" header, 2 rows, a gap, "Restore defaults"
        const float row = 61;
        if (i < 12)
            return 36 + i * row;
        if (i < 14)
            return 36 + 12 * row + 54 + (i - 12) * row;
        return 36 + 12 * row + 54 + 2 * row + 16;
    }
    void draw_input_page(float a) {
        section_title(L"SETTINGS  ›  INPUT", L"Input", a);
        text(L"Remap the controller for games and tune the stick dead zones. The Vitox menus always keep the standard layout.", kLeft, 266, 20,
            DWRITE_FONT_WEIGHT_NORMAL, rgba(0xB9A6E0, a), 860, 0, true);
        const int nb = input_button_count();
        const uint32_t fbit = input_sel < nb ? input_button_bit(input_sel) : 0;
        const int fstick = input_sel == nb ? 1 : input_sel == nb + 1 ? 2 : 0;
        const float ccx = 560, ccy = 620;
        // a soft stage under the controller
        fill_round(kLeft, 324, 970, 948, 34, rgba(0x12082A, 0.55f * a));
        stroke_round(kLeft, 324, 970, 948, 34, rgba(0xB48CFF, 0.16f * a), 1.2f);
        draw_controller(ccx, 596, 0.84f, a, fbit, true, fstick); // 0.0.26.2: smaller, clear of the readouts below
        // what the game receives right now
        uint32_t phys = pad.buttons;
        uint32_t remappable = 0;
        for (int i = 0; i < nb; ++i)
            remappable |= 1u << input_button_bit(i);
        const uint32_t out = input_remap(phys & remappable, icfg);
        float x = kLeft + 30;
        const float ry = 884;
        x += text(L"GAME RECEIVES", x, ry + 4, 15, DWRITE_FONT_WEIGHT_BOLD, rgba(0x8F7DB8, a), 600, 4) + 18;
        bool any = false;
        for (int i = 0; i < nb; ++i) {
            const uint32_t bit = input_button_bit(i);
            if (!(out & (1u << bit)))
                continue;
            any = true;
            const std::wstring n = input_button_vita(i);
            const float w = measure(n, 18, DWRITE_FONT_WEIGHT_BOLD) + 28;
            if (x + w > 950)
                break;
            fill_round(x, ry - 2, x + w, ry + 30, 16, rgba(0x8A4DFF, 0.55f * a));
            text(n, x + 14, ry + 2, 18, DWRITE_FONT_WEIGHT_BOLD, rgba(0xFFFFFF, a));
            x += w + 8;
        }
        if (!any)
            text(L"nothing pressed", x, ry + 2, 18, DWRITE_FONT_WEIGHT_NORMAL, rgba(0x6F5F96, a));
        // stick readouts (raw -> what the game gets after the dead zone)
        const auto readout = [&](float x0, const wchar_t *name, float sx, float sy, float dz) {
            float gx = sx, gy = sy;
            const float m = std::sqrt(sx * sx + sy * sy);
            if (m <= dz)
                gx = gy = 0;
            else if (dz > 0) {
                const float t = std::min(1.0f, (m - dz) / (1 - dz));
                gx = sx / m * t;
                gy = sy / m * t;
            }
            wchar_t buf[96];
            swprintf_s(buf, L"%s  %+.2f %+.2f  →  %+.2f %+.2f", name, sx, -sy, gx, -gy);
            text(buf, x0, 842, 17, DWRITE_FONT_WEIGHT_NORMAL, rgba(0xB9A6E0, a), 420, 0, false, L"Consolas");
        };
        readout(kLeft + 30, L"L", pad.lx, pad.ly, icfg.dead_l);
        readout(kLeft + 430, L"R", pad.rx, pad.ry, icfg.dead_r);

        // the settings list (scrolls)
        const float lx0 = 1030, lx1 = 1770, top = 300, bottom = 948, rh = 54;
        const float target = std::clamp(input_row_y(input_sel) + rh / 2 - (bottom - top) / 2, 0.0f, std::max(0.0f, input_row_y(kInputRows - 1) + rh + 8 - (bottom - top)));
        input_scroll = approach(input_scroll, target, dt_, 12);
        dc->PushAxisAlignedClip(R(lx0 - 14, top - 4, lx1 + 14, bottom), D2D1_ANTIALIAS_MODE_ALIASED);
        const float base = top - input_scroll;
        text(L"BUTTON MAPPING  ·  FOR GAMES", lx0, base + 4, 15, DWRITE_FONT_WEIGHT_BOLD, rgba(0x8F7DB8, a), 700, 4);
        text(L"STICK DEAD ZONES", lx0, base + input_row_y(12) - 32, 15, DWRITE_FONT_WEIGHT_BOLD, rgba(0x8F7DB8, a), 700, 4);
        const InputConfig def = input_config_default();
        for (int i = 0; i < kInputRows; ++i) {
            const float y0 = base + input_row_y(i), y1 = y0 + rh;
            if (y1 < top - 10 || y0 > bottom + 10)
                continue;
            const bool focus = zone == Zone::Content && i == input_sel;
            row_frame(lx0, y0, lx1, y1, focus, a, 20);
            const float cyr = (y0 + y1) / 2;
            if (i < nb) {
                const uint32_t bit = input_button_bit(i);
                const bool on = (pad.buttons & (1u << bit)) != 0;
                glyph_badge(lx0 + 34, cyr, 17, button_glyph(1u << bit), on ? 0xB07CFF : button_color(1u << bit), a, bit >= 11 ? 13 : 0);
                text(input_button_xbox(i), lx0 + 66, cyr - 16, 23, DWRITE_FONT_WEIGHT_SEMI_BOLD, rgba(0xFFFFFF, a), 260);
                const uint8_t t = icfg.map[bit];
                const bool changed = t != def.map[bit];
                std::wstring v;
                if (t == kInputNone) {
                    v = L"Disabled";
                } else {
                    const int ti = input_button_index(t);
                    v = std::wstring(input_button_vita(ti)) + (changed ? std::wstring(L"  ·  ") + input_button_xbox(ti) : L"");
                }
                const float vw = measure(v, 22, DWRITE_FONT_WEIGHT_BOLD);
                const float vx = lx1 - 60 - vw;
                if (focus) {
                    text(L"‹", vx - 34, cyr - 22, 32, DWRITE_FONT_WEIGHT_SEMI_BOLD, rgba(0xE7D6FF, a));
                    text(L"›", lx1 - 44, cyr - 22, 32, DWRITE_FONT_WEIGHT_SEMI_BOLD, rgba(0xE7D6FF, a));
                }
                text(v, vx, cyr - 15, 22, DWRITE_FONT_WEIGHT_BOLD, rgba(t == kInputNone ? 0xFF8A9A : changed ? 0xFFD27A : 0xD9C9FF, a), 420);
                if (changed) {
                    brush->SetColor(rgba(0xFFD27A, a));
                    dc->FillEllipse(D2D1::Ellipse({ (lx0 + 300) * k, cyr * k }, 4.5f * k, 4.5f * k), brush.get());
                }
            } else if (i < nb + 2) {
                const float dz = i == nb ? icfg.dead_l : icfg.dead_r;
                text(i == nb ? L"Left stick" : L"Right stick", lx0 + 26, cyr - 16, 23, DWRITE_FONT_WEIGHT_SEMI_BOLD, rgba(0xFFFFFF, a), 260);
                const float sx0 = lx0 + 230, sx1 = lx1 - 150;
                fill_round(sx0, cyr - 5, sx1, cyr + 5, 5, rgba(0x0E061E, a));
                const float f = dz / 0.5f;
                fill_gradient_round(sx0, cyr - 5, sx0 + (sx1 - sx0) * f + 1, cyr + 5, 5, rgba(0x9D5CFF, a), rgba(0xC9A2FF, a), false);
                brush->SetColor(rgba(0xFFFFFF, a));
                dc->FillEllipse(D2D1::Ellipse({ (sx0 + (sx1 - sx0) * f) * k, cyr * k }, (focus ? 12.0f : 9.0f) * k, (focus ? 12.0f : 9.0f) * k), brush.get());
                wchar_t buf[16];
                swprintf_s(buf, dz <= 0 ? L"Off" : L"%d%%", static_cast<int>(std::lround(dz * 100)));
                const float vw = measure(buf, 22, DWRITE_FONT_WEIGHT_BOLD, 0, L"Consolas");
                text(buf, lx1 - 40 - vw, cyr - 15, 22, DWRITE_FONT_WEIGHT_BOLD, rgba(dz != 0 ? 0xFFD27A : 0xD9C9FF, a), 200, 0, false, L"Consolas");
            } else {
                const bool d = input_is_default();
                const std::wstring t = d ? L"Restore defaults (already default)" : L"Restore defaults";
                text(t, lx0 + 26, cyr - 16, 23, DWRITE_FONT_WEIGHT_SEMI_BOLD, rgba(d ? 0x8F7DB8 : 0xFFFFFF, a), 600);
                if (focus && !d)
                    glyph_badge(lx1 - 40, cyr, 17, L"A", 0x3E9A4F, a);
            }
        }
        dc->PopAxisAlignedClip();
    }

    // ---------------------------------------------------------------- Artwork page
    void key_status_chip(float x1, float cy, float a) {
        std::wstring t;
        uint32_t c = 0;
        switch (key_state) {
        case KeyState::Unset: return;
        case KeyState::Checking: t = L"Checking…"; c = 0x6A4FB0; break;
        case KeyState::Valid: t = L"✓ Connected"; c = 0x2F8A4A; break;
        case KeyState::Invalid: t = L"Invalid key"; c = 0xB23A48; break;
        case KeyState::Offline: t = L"Not checked: offline"; c = 0xA8741A; break;
        }
        pill(t, x1, cy, a, c, 0.85f, 0xFFFFFF, L"Segoe UI", 19);
    }
    void draw_artwork_page(float a) {
        section_title(L"SETTINGS  ›  ARTWORK", L"Artwork", a);
        text(L"Replace a game’s card and background with community artwork from SteamGridDB.", kLeft, 266, 20, DWRITE_FONT_WEIGHT_NORMAL,
            rgba(0xB9A6E0, a), 1000);
        const float x0 = kLeft, x1 = kLeft + 1040;
        // the API key row
        {
            const float y = 318, h = 112;
            row_frame(x0, y, x1, y + h, zone == Zone::Content && art_sel == 0, a);
            glyph_badge(x0 + 60, y + h / 2, 28, L"⚿", 0x6A35E8, a, 26);
            text(L"SteamGridDB API key", x0 + 112, y + 22, 29, DWRITE_FONT_WEIGHT_SEMI_BOLD, rgba(0xFFFFFF, a), 500);
            text(L"Free at steamgriddb.com › Preferences › API", x0 + 112, y + 63, 20, DWRITE_FONT_WEIGHT_NORMAL, rgba(0xB9A6E0, a), 520);
            const float px = pill(masked_key(), x1 - 30, y + h / 2, a, 0x8A4DFF, 0.32f, sgdb_key.empty() ? 0xB9A6E0 : 0xF2EBFF,
                sgdb_key.empty() ? L"Segoe UI" : L"Consolas", 22);
            if (key_state != KeyState::Unset)
                key_status_chip(px - 12, y + h / 2, a);
        }
        // the games
        text(L"YOUR GAMES", kLeft, 462, 15, DWRITE_FONT_WEIGHT_BOLD, rgba(0x8F7DB8, a), 600, 4);
        const float top = 494, bottom = 948, rh = 88, gap = 10;
        const int n = static_cast<int>(apps.size());
        const int fi = std::max(0, art_sel - 1);
        const float target = std::clamp(fi * (rh + gap) + rh / 2 - (bottom - top) / 2, 0.0f, std::max(0.0f, n * (rh + gap) - gap - (bottom - top)));
        art_scroll = approach(art_scroll, target, dt_, 12);
        dc->PushAxisAlignedClip(R(x0 - 14, top - 8, x1 + 14, bottom), D2D1_ANTIALIAS_MODE_ALIASED);
        for (int i = 0; i < n; ++i) {
            const float y0 = top + i * (rh + gap) - art_scroll, y1 = y0 + rh;
            if (y1 < top - 10 || y0 > bottom + 10)
                continue;
            AppEntry &e = apps[i];
            ensure_art(e, false);
            const bool focus = zone == Zone::Content && art_sel == i + 1;
            row_frame(x0, y0, x1, y1, focus, a, 22);
            if (e.icon)
                draw_cover(e.icon.get(), x0 + 12, y0 + 12, x0 + 76, y0 + 76, 14, a);
            else
                fill_gradient_round(x0 + 12, y0 + 12, x0 + 76, y0 + 76, 14, rgba(0x6C2BD9, a), rgba(0x1E0A3C, a));
            text(e.title, x0 + 96, y0 + 14, 25, DWRITE_FONT_WEIGHT_SEMI_BOLD, rgba(0xFFFFFF, a), 640);
            std::wstring st;
            if (e.custom_card && e.custom_hero)
                st = L"Custom card art  ·  custom background";
            else if (e.custom_card)
                st = L"Custom card art";
            else if (e.custom_hero)
                st = L"Custom background";
            else
                st = L"Original artwork";
            text(st, x0 + 96, y0 + 50, 18, DWRITE_FONT_WEIGHT_NORMAL, rgba(e.custom_card || e.custom_hero ? 0xFFD27A : 0x8F7DB8, a), 640);
            const float tw = measure(e.title_id, 17, DWRITE_FONT_WEIGHT_BOLD, 1, L"Consolas");
            text(e.title_id, x1 - 70 - tw, y0 + 32, 17, DWRITE_FONT_WEIGHT_BOLD, rgba(0x8F7DB8, a), 300, 1, false, L"Consolas");
            text(L"›", x1 - 46, y0 + 16, 40, DWRITE_FONT_WEIGHT_SEMI_BOLD, rgba(focus ? 0xFFFFFF : 0x8F7DB8, a));
        }
        dc->PopAxisAlignedClip();
        // the preview / help panel
        const float px0 = 1262, px1 = 1770;
        fill_round(px0, 318, px1, 948, 30, rgba(0x12082A, 0.6f * a));
        stroke_round(px0, 318, px1, 948, 30, rgba(0xB48CFF, 0.18f * a), 1.2f);
        if (art_sel == 0 || apps.empty()) {
            text(L"HOW TO GET A KEY", px0 + 32, 346, 15, DWRITE_FONT_WEIGHT_BOLD, rgba(0x8F7DB8, a), 500, 4);
            const wchar_t *steps[] = { L"Sign in at steamgriddb.com (free)", L"Open Preferences › API", L"Generate an API key",
                L"Press A here and type it with the on-screen keyboard" };
            float yy = 390;
            for (int i = 0; i < 4; ++i) {
                glyph_badge(px0 + 50, yy + 16, 17, std::to_wstring(i + 1), 0x6A35E8, a);
                text(steps[i], px0 + 82, yy, 21, DWRITE_FONT_WEIGHT_NORMAL, rgba(0xEADFFF, a), 400, 0, true);
                yy += i == 3 ? 92 : 64;
            }
            text(L"Typing a long key with a controller is slow: you can also save it as a text file", px0 + 32, yy + 10, 18, DWRITE_FONT_WEIGHT_NORMAL,
                rgba(0xB9A6E0, a), 450, 0, true);
            text(L"DevelopmentFiles\\Vitox\\steamgriddb-key.txt", px0 + 32, yy + 66, 17, DWRITE_FONT_WEIGHT_NORMAL, rgba(0xD9C9FF, a), 470, 0, false, L"Consolas");
            text(L"and Vitox picks it up on the next start.", px0 + 32, yy + 94, 18, DWRITE_FONT_WEIGHT_NORMAL, rgba(0xB9A6E0, a), 450, 0, true);
            if (!key_msg.empty())
                text(key_msg, px0 + 32, 880, 18, DWRITE_FONT_WEIGHT_SEMI_BOLD, rgba(0xFF8A9A, a), 450, 0, true);
        } else {
            AppEntry &e = apps[art_sel - 1];
            ensure_art(e, true);
            text(L"CARD", px0 + 32, 346, 15, DWRITE_FONT_WEIGHT_BOLD, rgba(0x8F7DB8, a), 300, 4);
            if (e.custom_card)
                pill(L"Custom", px1 - 32, 354, a, 0xA8741A, 0.85f, 0xFFFFFF, L"Segoe UI", 16);
            const float cs = 236, cxx = (px0 + px1) / 2 - cs / 2;
            if (e.icon)
                draw_cover(e.icon.get(), cxx, 384, cxx + cs, 384 + cs, 28, a);
            else
                fill_gradient_round(cxx, 384, cxx + cs, 384 + cs, 28, rgba(0x6C2BD9, a), rgba(0x1E0A3C, a));
            text(L"BACKGROUND", px0 + 32, 650, 15, DWRITE_FONT_WEIGHT_BOLD, rgba(0x8F7DB8, a), 300, 4);
            if (e.custom_hero)
                pill(L"Custom", px1 - 32, 658, a, 0xA8741A, 0.85f, 0xFFFFFF, L"Segoe UI", 16);
            if (e.hero)
                draw_cover(e.hero.get(), px0 + 32, 690, px1 - 32, 690 + 160, 20, a);
            else {
                fill_round(px0 + 32, 690, px1 - 32, 850, 20, rgba(0x0E061E, a));
                text(L"No background art", px0 + 160, 756, 20, DWRITE_FONT_WEIGHT_NORMAL, rgba(0x6F5F96, a));
            }
            text(L"Press A to choose new artwork", px0 + 32, 880, 19, DWRITE_FONT_WEIGHT_NORMAL, rgba(0xB9A6E0, a), 450);
        }
    }

    // ---------------------------------------------------------------- the SteamGridDB browser
    void draw_browser(float a) {
        AppEntry *e = br_app();
        section_title(L"SETTINGS  ›  ARTWORK  ›  STEAMGRIDDB", e ? e->title : std::wstring(), a);
        // the search line
        float x = kLeft;
        x += text(L"Search", x, 272, 20, DWRITE_FONT_WEIGHT_SEMI_BOLD, rgba(0x8F7DB8, a)) + 14;
        x += text(L"“" + br.term + L"”", x, 272, 20, DWRITE_FONT_WEIGHT_SEMI_BOLD, rgba(0xFFFFFF, a), 700) + 22;
        glyph_badge(x + 13, 286, 13, L"X", 0x2F6FD0, a, 15);
        text(L"Edit search", x + 34, 274, 18, DWRITE_FONT_WEIGHT_NORMAL, rgba(0xB9A6E0, a));
        // the matching SteamGridDB games
        x = kLeft;
        const float gy = 316;
        for (int i = 0; i < static_cast<int>(br.games.size()); ++i) {
            const sgdb::Game &g = br.games[i];
            std::wstring t = g.name;
            if (g.year)
                t += L"  (" + std::to_wstring(g.year) + L")";
            if (g.verified)
                t += L"  ✓";
            float w = std::min(380.0f, measure(t, 19, DWRITE_FONT_WEIGHT_SEMI_BOLD)) + 36;
            if (x + w > 1770)
                break;
            const bool cur = i == br.game_sel;
            if (cur)
                fill_gradient_round(x, gy, x + w, gy + 44, 22, rgba(0x9D5CFF, (br.zone_games ? 0.95f : 0.6f) * a), rgba(0x6A35E8, (br.zone_games ? 0.95f : 0.6f) * a), false);
            else
                fill_round(x, gy, x + w, gy + 44, 22, rgba(0x1A0C33, 0.8f * a));
            stroke_round(x, gy, x + w, gy + 44, 22, rgba(0xB48CFF, (cur ? 0.6f : 0.22f) * a), 1.2f);
            if (cur && br.zone_games) {
                const float pulse = 0.5f + 0.5f * std::sin(time_s * 4.0f);
                stroke_round(x - 3, gy - 3, x + w + 3, gy + 47, 25, rgba(0xFFFFFF, (0.5f + 0.4f * pulse) * a), 2.0f);
            }
            text(t, x + 18, gy + 10, 19, DWRITE_FONT_WEIGHT_SEMI_BOLD, rgba(cur ? 0xFFFFFF : 0xCDB8F5, a), 380);
            x += w + 10;
        }
        // card / background switch
        {
            const float tw0 = measure(L"Card art", 21, DWRITE_FONT_WEIGHT_SEMI_BOLD), tw1 = measure(L"Background", 21, DWRITE_FONT_WEIGHT_SEMI_BOLD);
            const float bx = kLeft + 58, by = 384, w0 = tw0 + 48, w1 = tw1 + 48;
            glyph_badge(kLeft + 22, by + 23, 19, L"LB", 0x4B2E8F, a, 14);
            fill_round(bx, by, bx + w0 + w1 + 12, by + 46, 23, rgba(0x1B0E33, 0.7f * a));
            const bool card = br.kind == sgdb::Kind::Card;
            const float hx = card ? bx + 6 : bx + 6 + w0;
            fill_gradient_round(hx, by + 5, hx + (card ? w0 : w1), by + 41, 18, rgba(0x9D5CFF, a), rgba(0x6A35E8, a), false);
            text(L"Card art", bx + 6 + 24, by + 10, 21, DWRITE_FONT_WEIGHT_SEMI_BOLD, rgba(card ? 0xFFFFFF : 0xBBA8E6, a));
            text(L"Background", bx + 6 + w0 + 24, by + 10, 21, DWRITE_FONT_WEIGHT_SEMI_BOLD, rgba(!card ? 0xFFFFFF : 0xBBA8E6, a));
            glyph_badge(bx + w0 + w1 + 12 + 34, by + 23, 19, L"RB", 0x4B2E8F, a, 14);
            // details of the selected image
            if (!br.zone_games && br.sel >= 0 && br.sel < static_cast<int>(br.images.size())) {
                const sgdb::Image &im = br.images[br.sel];
                std::wstring d = std::to_wstring(im.width) + L" × " + std::to_wstring(im.height);
                if (!im.style.empty())
                    d += L"  ·  " + im.style;
                if (!im.author.empty())
                    d = L"by " + im.author + L"  ·  " + d;
                if (im.icon)
                    d += L"  ·  icon";
                const float dw = measure(d, 19, DWRITE_FONT_WEIGHT_NORMAL);
                text(d, 1770 - dw, by + 11, 19, DWRITE_FONT_WEIGHT_NORMAL, rgba(0xB9A6E0, a), 900);
            }
        }
        // the grid
        const int cols = browser_cols(), n = static_cast<int>(br.images.size());
        const float tw = thumb_w(), th = thumb_h(), tg = thumb_gap(), rowh = thumb_row_h();
        const int srow = br.sel / std::max(1, cols);
        const float view = kGridY1 - kGridY0;
        float target = br.scroll;
        if (srow * rowh < target)
            target = srow * rowh;
        if (srow * rowh + th > target + view)
            target = srow * rowh + th - view;
        br.scroll = approach(br.scroll, std::max(0.0f, target), dt_, 12);
        dc->PushAxisAlignedClip(R(kLeft - 14, kGridY0 - 10, 1784, kGridY1 + 4), D2D1_ANTIALIAS_MODE_ALIASED);
        for (int i = 0; i < n; ++i) {
            const int c = i % cols, r = i / cols;
            const float x0 = kLeft + c * (tw + tg), y0 = kGridY0 + r * rowh - br.scroll;
            if (y0 + th < kGridY0 - 20 || y0 > kGridY1 + 20)
                continue;
            const sgdb::Image &im = br.images[i];
            auto it = br.thumbs.find(im.url);
            const Thumb *t = it != br.thumbs.end() ? &it->second : nullptr;
            const float rr = br.kind == sgdb::Kind::Card ? 22.0f : 16.0f;
            const bool focus = !br.zone_games && i == br.sel;
            if (focus)
                fill_round(x0 - 8, y0 - 8, x0 + tw + 8, y0 + th + 8, rr + 6, rgba(0xA66BFF, (0.18f + 0.12f * std::sin(time_s * 3.2f)) * a));
            if (t && t->bmp) {
                draw_cover(t->bmp.get(), x0, y0, x0 + tw, y0 + th, rr, a);
            } else {
                const float sh = 0.5f + 0.5f * std::sin(time_s * 3.0f - i * 0.45f);
                fill_round(x0, y0, x0 + tw, y0 + th, rr, rgba(0x1E0F3C, (0.65f + 0.25f * sh) * a));
                if (t && t->failed)
                    text(L"×", x0 + tw / 2 - 10, y0 + th / 2 - 22, 34, DWRITE_FONT_WEIGHT_NORMAL, rgba(0x6F5F96, a));
            }
            const bool in_use = !im.url.empty() && im.url == br.in_use[br.kind == sgdb::Kind::Card ? 0 : 1];
            if (in_use) {
                const float bw = measure(L"IN USE", 14, DWRITE_FONT_WEIGHT_BOLD, 2) + 20;
                fill_round(x0 + 10, y0 + 10, x0 + 10 + bw, y0 + 36, 13, rgba(0x2F8A4A, 0.95f * a));
                text(L"IN USE", x0 + 20, y0 + 13, 14, DWRITE_FONT_WEIGHT_BOLD, rgba(0xFFFFFF, a), 200, 2);
            }
            if (im.icon) {
                const float bw = measure(L"ICON", 13, DWRITE_FONT_WEIGHT_BOLD, 2) + 18;
                fill_round(x0 + tw - 10 - bw, y0 + th - 34, x0 + tw - 10, y0 + th - 10, 12, rgba(0x05010C, 0.7f * a));
                text(L"ICON", x0 + tw - 1 - bw, y0 + th - 31, 13, DWRITE_FONT_WEIGHT_BOLD, rgba(0xD9C9FF, a), 200, 2);
            }
            if (br.apply_index == i) {
                fill_round(x0, y0, x0 + tw, y0 + th, rr, rgba(0x05010C, 0.6f * a));
                arc(x0 + tw / 2, y0 + th / 2, 22, 0.25f + 0.2f * std::sin(time_s * 5), rgba(0xFFFFFF, a), 4);
                brush->SetColor(rgba(0xFFFFFF, 0.25f * a));
                dc->DrawEllipse(D2D1::Ellipse({ (x0 + tw / 2) * k, (y0 + th / 2) * k }, 22 * k, 22 * k), brush.get(), 4 * k);
            }
            if (focus) {
                const float pulse = 0.5f + 0.5f * std::sin(time_s * 3.2f);
                stroke_round(x0 - 4, y0 - 4, x0 + tw + 4, y0 + th + 4, rr + 4, rgba(0xC9A2FF, (0.6f + 0.35f * pulse) * a), 3.0f);
                stroke_round(x0, y0, x0 + tw, y0 + th, rr, rgba(0xFFFFFF, 0.8f * a), 1.5f);
            } else {
                stroke_round(x0, y0, x0 + tw, y0 + th, rr, rgba(0xB48CFF, 0.2f * a), 1.0f);
            }
        }
        dc->PopAxisAlignedClip();
        if (!br.status.empty()) {
            const bool busy = br.search_h || br.images_h || br.load_pending;
            const float sw = measure(br.status, 24, DWRITE_FONT_WEIGHT_SEMI_BOLD);
            const float sy = n == 0 ? 640 : kGridY1 - 40;
            if (busy)
                arc(960 - sw / 2 - 34, sy + 16, 13, 0.3f + 0.2f * std::sin(time_s * 5), rgba(0xC9A2FF, a), 3.5f);
            text(br.status, 960 - sw / 2, sy, 24, DWRITE_FONT_WEIGHT_SEMI_BOLD, rgba(busy ? 0xD9C9FF : 0xFFB0BC, a), 1600);
        }
    }

    // ---------------------------------------------------------------- the text entry box (over everything)
    void draw_edit_modal(float a) {
        if (editing == EditTarget::None)
            return;
        brush->SetColor(rgba(0x05010C, 0.62f * a));
        dc->FillRectangle(R(0, 0, 1920, 1080), brush.get());
        const float x0 = 420, x1 = 1500, y0 = 110, y1 = 360;
        fill_gradient_round(x0, y0, x1, y1, 34, rgba(0x2A1458, 0.98f * a), rgba(0x170A30, 0.98f * a));
        stroke_round(x0, y0, x1, y1, 34, rgba(0xC9A2FF, 0.5f * a), 1.5f);
        const bool key = editing == EditTarget::Key;
        text(key ? L"SteamGridDB API key" : L"Search SteamGridDB", x0 + 44, y0 + 30, 30, DWRITE_FONT_WEIGHT_SEMI_BOLD, rgba(0xFFFFFF, a));
        const float fx0 = x0 + 44, fx1 = x1 - 44, fy0 = y0 + 92, fy1 = y0 + 156;
        fill_round(fx0, fy0, fx1, fy1, 18, rgba(0x0E061E, a));
        stroke_round(fx0, fy0, fx1, fy1, 18, rgba(0xB07CFF, 0.8f * a), 2.0f);
        const wchar_t *family = key ? L"Consolas" : L"Segoe UI";
        const float tw = edit_text.empty() ? 0 : text(edit_text, fx0 + 22, fy0 + 15, 26, DWRITE_FONT_WEIGHT_SEMI_BOLD, rgba(0xFFFFFF, a), fx1 - fx0 - 50, 0, false, family);
        if (std::fmod(time_s, 1.0f) < 0.6f) {
            brush->SetColor(rgba(0xE7D6FF, a));
            dc->FillRectangle(R(fx0 + 24 + tw, fy0 + 14, fx0 + 27 + tw, fy1 - 14), brush.get());
        }
        text(key ? L"Type with the on-screen keyboard. Press Enter (or ≡ Menu) to save; leave it empty to remove the key."
                 : L"Type a game name with the on-screen keyboard. Press Enter (or ≡ Menu) to search.",
            x0 + 44, y0 + 180, 19, DWRITE_FONT_WEIGHT_NORMAL, rgba(0xB9A6E0, a), x1 - x0 - 88);
    }

    void draw_settings(float a) {
        switch (page) {
        case Page::Menu: draw_settings_menu(a); break;
        case Page::Input: draw_input_page(a); break;
        case Page::Artwork: draw_artwork_page(a); break;
        case Page::Browser: draw_browser(a); break;
        case Page::Graphics: draw_graphics_page(a); break;
        case Page::Stats: draw_stats_page(a); break;
        case Page::Shaders: draw_shaders_page(a); break;
        case Page::Library: draw_library_page(a); break;
        }
    }

    void draw_footer(float a) {
        float x = kLeft;
        const float y = 1080 - 92;
        brush->SetColor(rgba(0xB48CFF, 0.16f * a));
        dc->FillRectangle(R(kLeft, y - 26, 1920 - kLeft, y - 25), brush.get());
        std::wstring right;
        if (editing != EditTarget::None) {
            right = L"Enter saves  ·  the keyboard's close button keeps what you typed";
        } else if (fw_open) {
            if (!fw_busy()) {
                x += button_hint(x, y, L"A", fws.phase == firmware::Phase::Done ? L"Continue" : L"Install from USB", 0x3E9A4F, a);
                if (fws.phase != firmware::Phase::Done)
                    x += button_hint(x, y, L"B", L"Skip for now", 0xB23A48, a);
            }
            right = L"Files are recognised by their contents, any name or folder on the drive";
        } else if (inst_open) {
            using install::Phase;
            switch (is.phase) {
            case Phase::Files:
                if (!is.files.empty())
                    x += button_hint(x, y, L"▲▼", L"Select", 0x4B2E8F, a), x += button_hint(x, y, L"A", L"Open", 0x3E9A4F, a);
                x += button_hint(x, y, L"Y", L"Look again", 0xC08A1E, a);
                x += button_hint(x, y, L"B", L"Close", 0xB23A48, a);
                break;
            case Phase::Confirm:
                if (install_ready())
                    x += button_hint(x, y, L"A", L"Install", 0x3E9A4F, a);
                x += button_hint(x, y, L"B", L"Back", 0xB23A48, a);
                break;
            case Phase::Installing:
                if (!is.cancelling)
                    x += button_hint(x, y, L"B", L"Cancel", 0xB23A48, a);
                break;
            case Phase::Done:
                x += button_hint(x, y, L"A", L"Done", 0x3E9A4F, a);
                if (is.can_delete_archive && !is.archive_deleted && !is.deleting_archive)
                    x += button_hint(x, y, L"X", L"Delete archive from USB", 0x2F6FD0, a);
                x += button_hint(x, y, L"B", L"Another archive", 0xB23A48, a);
                break;
            default:
                if (is.phase != Phase::Analyzing)
                    x += button_hint(x, y, L"B", L"Close", 0xB23A48, a);
                break;
            }
            right = !is.folder.empty() && is.phase == Phase::Files ? L"Install folder: " + is.folder + L"  (Settings \u203A Library)"
                                                                   : L"Decrypted dumps, and NoNpDrm dumps with their license (work.bin)";
        } else if (fp_open) {
            if (!fpv.loading) {
                if (!fpv.entries.empty())
                    x += button_hint(x, y, L"\u25B2\u25BC", L"Select", 0x4B2E8F, a), x += button_hint(x, y, L"A", L"Open", 0x3E9A4F, a);
                if (!fpv.at_drives)
                    x += button_hint(x, y, L"X", L"Use this folder", 0x2F6FD0, a);
                x += button_hint(x, y, L"Y", L"Drives", 0xC08A1E, a);
            }
            x += button_hint(x, y, L"B", fpv.at_drives || fpv.loading ? L"Close" : L"Up", 0xB23A48, a);
            right = L"The same folder is used on any USB drive";
        } else if (gm_open) {
            if (gm_state != 0) {
                right = L"Removing files…";
            } else if (gm_confirm) {
                x += button_hint(x, y, L"A", L"Uninstall", 0x3E9A4F, a);
                x += button_hint(x, y, L"X", L"Uninstall + delete saves", 0x2F6FD0, a);
                x += button_hint(x, y, L"B", L"Cancel", 0xB23A48, a);
            } else {
                x += button_hint(x, y, L"▲▼", L"Select", 0x4B2E8F, a);
                if (gm_row < 4)
                    x += button_hint(x, y, L"◀▶", L"Change", 0x4B2E8F, a);
                x += button_hint(x, y, L"A", gm_row < 4 ? L"Next value" : gm_row == 4 ? L"Reset" : L"Uninstall…", 0x3E9A4F, a);
                x += button_hint(x, y, L"B", L"Close", 0xB23A48, a);
                right = L"Settings for this game only";
            }
        } else if (tab == Tab::Library || zone == Zone::Tabs) {
            if (tab == Tab::Library && zone == Zone::Content)
                x += button_hint(x, y, L"A", on_plus() ? L"Install from USB" : L"Start", 0x3E9A4F, a);
            if (tab == Tab::Library && zone == Zone::Content && !on_plus())
                x += button_hint(x, y, L"≡", L"Options", 0x4B2E8F, a);
            if (zone == Zone::Tabs)
                x += button_hint(x, y, L"A", L"Open", 0x3E9A4F, a);
            if (zone == Zone::Content)
                x += button_hint(x, y, L"B", L"Back", 0xB23A48, a);
            if (tab == Tab::Library)
                x += button_hint(x, y, L"Y", L"Refresh", 0xC08A1E, a);
            x += button_hint(x, y, L"LB", L"", 0x4B2E8F, a) - 34 + 8;
            button_hint(x, y, L"RB", L"Switch tab", 0x4B2E8F, a);
            right = L"In game: press R3 for the Vitox menu";
        } else if (page == Page::Stats) {
            x += button_hint(x, y, L"\u25B2\u25BC", L"Select", 0x4B2E8F, a);
            x += button_hint(x, y, L"\u25C0\u25B6", L"Change", 0x4B2E8F, a);
            x += button_hint(x, y, L"A", stats_row == 0 ? L"Toggle" : L"Switch mode", 0x3E9A4F, a);
            x += button_hint(x, y, L"B", L"Back", 0xB23A48, a);
            right = L"In game: R3 menu \u203A Statistics Overlay toggles it too";
        } else if (page == Page::Shaders) {
            x += button_hint(x, y, L"\u25B2\u25BC", L"Select", 0x4B2E8F, a);
            x += button_hint(x, y, L"\u25C0\u25B6", L"Change", 0x4B2E8F, a);
            x += button_hint(x, y, L"A", L"Toggle", 0x3E9A4F, a);
            x += button_hint(x, y, L"B", L"Back", 0xB23A48, a);
            right = L"Applies the next time a game starts";
        } else if (page == Page::Library) {
            x += button_hint(x, y, L"\u25B2\u25BC", L"Select", 0x4B2E8F, a);
            if (lib_row == 0) {
                x += button_hint(x, y, L"\u25C0\u25B6", L"Change", 0x4B2E8F, a);
                x += button_hint(x, y, L"A", L"Toggle", 0x3E9A4F, a);
            } else {
                x += button_hint(x, y, L"A", L"Choose folder", 0x3E9A4F, a);
                if (!library_install_folder().empty())
                    x += button_hint(x, y, L"X", L"Whole drive", 0x2F6FD0, a);
            }
            x += button_hint(x, y, L"B", L"Back", 0xB23A48, a);
            right = lib_row == 0 ? L"The library updates at once" : L"Used the next time you open the + tile";
        } else if (page == Page::Graphics) {
            x += button_hint(x, y, L"\u25C0\u25B6", L"Adjust", 0x4B2E8F, a);
            x += button_hint(x, y, L"A", L"Confirm", 0x3E9A4F, a);
            x += button_hint(x, y, L"B", L"Back", 0xB23A48, a);
            right = L"Applies the next time a game starts";
        } else if (page == Page::Menu) {
            if (menu_sel != kMenuAbout)
                x += button_hint(x, y, L"A", L"Open", 0x3E9A4F, a);
            x += button_hint(x, y, L"B", L"Back", 0xB23A48, a);
            x += button_hint(x, y, L"LB", L"", 0x4B2E8F, a) - 34 + 8;
            button_hint(x, y, L"RB", L"Switch tab", 0x4B2E8F, a);
        } else if (page == Page::Input) {
            x += button_hint(x, y, L"▲▼", L"Select", 0x4B2E8F, a);
            if (input_sel < kInputRows - 1)
                x += button_hint(x, y, L"◀▶", L"Change", 0x4B2E8F, a);
            else if (!input_is_default())
                x += button_hint(x, y, L"A", L"Restore", 0x3E9A4F, a);
            const float bx = x;
            x += button_hint(x, y, L"B", L"Hold to go back", 0xB23A48, a);
            arc(bx + 17, y + 17, 22, b_hold / 0.7f, rgba(0xFFFFFF, a), 3.5f);
            right = L"Buttons and sticks are live on this page · use the D-pad to navigate";
        } else if (page == Page::Artwork) {
            x += button_hint(x, y, L"A", art_sel == 0 ? L"Edit key" : L"Choose artwork", 0x3E9A4F, a);
            if (art_sel > 0 && (apps[art_sel - 1].custom_card || apps[art_sel - 1].custom_hero))
                x += button_hint(x, y, L"Y", L"Restore original", 0xC08A1E, a);
            x += button_hint(x, y, L"B", L"Back", 0xB23A48, a);
            right = L"Artwork from steamgriddb.com";
        } else {
            if (!br.zone_games && !br.images.empty())
                x += button_hint(x, y, L"A", L"Apply", 0x3E9A4F, a);
            x += button_hint(x, y, L"X", L"Edit search", 0x2F6FD0, a);
            x += button_hint(x, y, L"Y", L"Restore original", 0xC08A1E, a);
            x += button_hint(x, y, L"LB", L"", 0x4B2E8F, a) - 34 + 8;
            x += button_hint(x, y, L"RB", L"Card / Background", 0x4B2E8F, a);
            button_hint(x, y, L"B", L"Back", 0xB23A48, a);
            right = L"Artwork from steamgriddb.com";
        }
        if (!right.empty())
            text(right, 1920 - kLeft - measure(right, 20, DWRITE_FONT_WEIGHT_NORMAL), y + 5, 20, DWRITE_FONT_WEIGHT_NORMAL, rgba(0x8F7DB8, a));
    }

    void draw_toast(float a) {
        if (toast_t < 0 || toast.empty())
            return;
        const float life = 4.5f;
        if (toast_t > life) {
            toast_t = -1;
            return;
        }
        const float t = std::min(ease_out(toast_t / 0.3f), 1 - ease_out((toast_t - (life - 0.4f)) / 0.4f));
        const float w = measure(toast, 22, DWRITE_FONT_WEIGHT_SEMI_BOLD) + 60;
        const float x1 = 1920 - kLeft + (1 - t) * 40, x0 = x1 - w, y0 = 140, y1 = 196;
        fill_round(x0, y0, x1, y1, 28, rgba(0x24104A, 0.92f * t * a));
        stroke_round(x0, y0, x1, y1, 28, rgba(0xC9A2FF, 0.5f * t * a), 1.5f);
        text(toast, x0 + 30, y0 + 13, 22, DWRITE_FONT_WEIGHT_SEMI_BOLD, rgba(0xFFFFFF, t * a));
    }

    // one frame; returns true when the launch animation finished
    bool frame(float dt) {
        time_s += dt;
        dt_ = dt;
        art_sel = std::clamp(art_sel, 0, static_cast<int>(apps.size())); // the library may shrink under the Artwork page
        poll_key();
        if (page == Page::Browser)
            browser_update();
        intro = std::min(1.0f, intro + dt / 0.6f);
        if (toast_t >= 0)
            toast_t += dt;
        if (launching)
            launch_t = std::min(1.0f, launch_t + dt / 0.55f);
        tab_anim = approach(tab_anim, tab == Tab::Settings ? 1.0f : 0.0f, dt, 14);
        content_anim = std::min(1.0f, content_anim + dt / 0.28f);
        change_pulse = std::max(0.0f, change_pulse - dt * 2.2f);
        hero_mix = std::min(1.0f, hero_mix + dt / 0.5f);
        for (int i = 0; i < static_cast<int>(apps.size()); ++i) {
            AppEntry &e = apps[i];
            e.focus = approach(e.focus, i == sel && tab == Tab::Library ? 1.0f : 0.0f, dt, 13);
        }
        plus_focus = approach(plus_focus, on_plus() && tab == Tab::Library ? 1.0f : 0.0f, dt, 13);
        scroll = approach(scroll, target_scroll(), dt, 11);
        if (!apps.empty()) {
            sel_anim_hue = approach(sel_anim_hue, on_plus() ? 0.0f : apps[sel].hue * 0.35f, dt, 3);
            parallax_x = approach(parallax_x, 2.0f * sel / apps.size() - 1.0f, dt, 2.5f); // apps.size() + 1 tiles
        }
        install_update(dt);
        gm_update(dt);
        fp_update(dt);
        fw_update(dt);

        float glow_rect[4] = { 0, 0, 0, 0 };
        // Direct2D first records into the layer below; the background is drawn by D3D11 into the back buffer first, so the
        // halo rect is the previous frame's (the cards move smoothly: no visible lag)
        const auto pt0 = Clock::now();
        draw_background(last_glow_, tab == Tab::Library ? (0.85f + 0.15f * std::sin(time_s * 2.6f) + 0.5f * change_pulse) * content_vis() : 0.0f);
        dc->BeginDraw();
        const float a = ease_out(intro) * (1 - ease_in_out(std::max(0.0f, launch_t * 1.4f - 0.4f)));
        if (tab == Tab::Library)
            draw_hero(a * content_vis());
        draw_header(a);
        const float ca = a * content_vis();
        dc->SetTransform(D2D1::Matrix3x2F::Translation(0, (1 - ease_out(content_anim)) * 26 * k));
        if (tab == Tab::Library)
            draw_library(ca, glow_rect);
        else
            draw_settings(ca);
        dc->SetTransform(D2D1::Matrix3x2F::Identity());
        draw_footer(a * (launching ? 1 - ease_out(launch_t * 2) : 1));
        draw_install_modal(a);
        draw_game_menu(a);
        draw_folder_picker(a);
        draw_fw_modal(a);
        draw_toast(a);
        draw_edit_modal(a);
        if (launching) {
            // the final fade to black before the game takes the screen
            brush->SetColor(rgba(0x000000, ease_in_out(std::max(0.0f, launch_t * 1.6f - 0.6f))));
            dc->FillRectangle(R(0, 0, 1920, 1080), brush.get());
        }
        const auto pt1 = Clock::now();
        HRESULT hr = dc->EndDraw();
        const auto pt2 = Clock::now();
        memcpy(last_glow_, glow_rect, sizeof(glow_rect));
        if (SUCCEEDED(hr))
            hr = swap->Present(1, 0);
        const auto pt3 = Clock::now();
        perf_d2d = std::chrono::duration<double, std::milli>(pt1 - pt0).count();
        perf_end = std::chrono::duration<double, std::milli>(pt2 - pt1).count();
        perf_present = std::chrono::duration<double, std::milli>(pt3 - pt2).count();
        if (FAILED(hr) && !present_error_logged_) {
            present_error_logged_ = true;
            log_line(Log::Lifecycle, "launcher ui: draw/present failed hr=0x%08lX", static_cast<unsigned long>(hr));
        }
        ++frames;
        return launching && launch_t >= 1.0f;
    }
    float content_vis() const { return ease_out(content_anim); }
    float last_glow_[4] = { 0, 0, 0, 0 };
    // 0.0.26.4 DIAGNOSTIC: per-frame cost breakdown, logged for slow frames
    int perf_layouts = 0, perf_formats = 0, perf_bitmaps = 0;
    std::string perf_new;
    double perf_d2d = 0, perf_end = 0, perf_present = 0;
    bool present_error_logged_ = false;
};

LauncherUi::LauncherUi() : impl_(std::make_unique<Impl>()) {}
LauncherUi::~LauncherUi() = default;

bool LauncherUi::init(CoreWindow const &window) { return impl_->init(window); }
void LauncherUi::release() { impl_->release(); }

LauncherChoice LauncherUi::run(CoreWindow const &window, const LauncherHost &host, const std::string &preselect, const std::wstring &toast) {
    Impl &m = *impl_;
    LauncherChoice choice;
    if (!m.ok)
        return choice;
    m.settings.clear();
    m.version = host.version;
    m.local_state = host.local_state;
    m.icfg = input_config();
    // 0.0.26.0: the system keyboard for the SteamGridDB key / search (the game session stops it again when it ends)
    host_text_start();
    if (m.sgdb_key.empty() && m.key_state == LauncherUi::Impl::KeyState::Unset) {
        std::wstring key = m.local_state.empty() ? std::wstring() : sgdb::load_key(m.local_state);
        if (key.empty()) {
            // a key saved as a text file in DevelopmentFiles (typing 32 characters with a controller is slow)
            for (const std::wstring &dir : { std::wstring(kDevRoot), std::wstring(kDevRoot) + L"\\homebrew" }) {
                key = sgdb::load_key(dir);
                if (!key.empty()) {
                    log_line(Log::Lifecycle, "launcher ui: SteamGridDB key imported from %s\\steamgriddb-key.txt", narrow(dir).c_str());
                    break;
                }
            }
            if (!key.empty())
                m.set_key(key, true);
        } else {
            m.set_key(key, false);
        }
    }
    m.armed = false;
    m.launching = false;
    m.launch_t = 0;
    m.intro = 0;
    m.zone = LauncherUi::Impl::Zone::Content;
    m.scanned_once = false;
    if (!toast.empty())
        m.show_toast(toast);
    if (!m.fw && !vitox::firmware::installed(kDevRoot)) {
        m.fw = std::make_unique<vitox::firmware::Setup>(kDevRoot);
        m.fw_open = true;
        m.fw_anim = 0;
        m.fw->poll(0, m.fws);
    }
    m.start_scan();
    m.next_scan = Clock::now() + std::chrono::seconds(4);
    m.last = Clock::now();
    log_line(Log::Lifecycle, "launcher ui: run (preselect '%s')", preselect.c_str());
    while (!host.closed()) {
        if (!host.visible()) {
            window.Dispatcher().ProcessEvents(CoreProcessEventsOption::ProcessOneAndAllPending);
            m.last = Clock::now();
            continue;
        }
        window.Dispatcher().ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
        const auto now = Clock::now();
        const float dt = std::min(0.1f, std::chrono::duration<float>(now - m.last).count());
        m.last = now;
        m.take_scan(preselect);
        if (now >= m.next_scan) {
            m.start_scan();
            m.next_scan = now + std::chrono::seconds(4);
        }
        if (!m.launching && m.input(dt) && m.sel >= 0 && m.sel < static_cast<int>(m.apps.size())) {
            m.launching = true;
            m.launch_t = 0;
            log_line(Log::Lifecycle, "launcher ui: START %s '%s' (%s/%s)", m.apps[m.sel].info.title_id, m.apps[m.sel].info.title, m.apps[m.sel].info.app_root,
                m.apps[m.sel].info.folder);
        }
        if (m.prerender_step < 5 && m.scanned_once && m.frames >= 6 && m.page == LauncherUi::Impl::Page::Menu && m.tab == LauncherUi::Impl::Tab::Library)
            m.prerender(m.prerender_step++);
        else if (m.prerender_step >= 5)
            m.preload_tick();
        const auto f0 = Clock::now();
        const double pre_ms = std::chrono::duration<double, std::milli>(f0 - now).count();
        const bool done = m.frame(dt);
        const double ms = std::chrono::duration<double, std::milli>(Clock::now() - f0).count();
        if (m.frames > 30 && ms + pre_ms > 25)
            log_line(Log::Lifecycle,
                "launcher perf: slow frame %.1f ms | scan+input %.1f, draw %.1f, EndDraw %.1f, Present %.1f | tab %d page %d | new layouts %d formats %d bitmaps %d",
                ms + pre_ms, pre_ms, m.perf_d2d, m.perf_end, m.perf_present, static_cast<int>(m.tab), static_cast<int>(m.page), m.perf_layouts, m.perf_formats,
                m.perf_bitmaps);
        if (m.frames > 30 && ms + pre_ms > 25 && !m.perf_new.empty())
            log_line(Log::Lifecycle, "launcher perf:   new text: %s", m.perf_new.c_str());
        m.perf_new.clear();
        m.perf_layouts = m.perf_formats = m.perf_bitmaps = 0;
        if (m.frames > 30)
            m.frame_ms_max = std::max(m.frame_ms_max, ms);
        if (m.frames % 1800 == 0)
            log_line(Log::Lifecycle, "launcher ui: %llu frames, worst frame %.1f ms", static_cast<unsigned long long>(m.frames), m.frame_ms_max);
        if (done) {
            choice.launch = true;
            choice.app = m.apps[m.sel].info;
            break;
        }
    }
    m.stop_scan();
    return choice;
}

} // namespace vitox
