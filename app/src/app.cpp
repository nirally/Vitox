// Vitox M1 Probe - CoreApplication shell: lifecycle logging, probe runner and a
// D3D11 + Direct2D status screen on the CoreWindow.
#include "common.h"
#include "launcher_ui.h" // 0.0.25.0: the Vitox launcher (library + settings)
#include "library_install.h" // 0.0.29.0: Settings > Library (show homebrew)
#include "game_config.h"     // 0.0.30.0: per-game settings (game menu)

#include <vitox_homebrew_launcher.h>

#include <d2d1_1.h>
#include <d3d11_1.h>
#include <dwrite.h>
#include <dxgi1_3.h>
#include <wincodec.h>

#include <eh.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>

#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.ApplicationModel.Activation.h>
#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Display.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.System.Profile.h>
#include <winrt/Windows.UI.Core.h>

using namespace winrt;
using namespace winrt::Windows::ApplicationModel;
using namespace winrt::Windows::ApplicationModel::Activation;
using namespace winrt::Windows::ApplicationModel::Core;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::UI::Core;

namespace vitox {

struct SehFault {
    unsigned code;
    void *address;
    ULONG_PTR info0; // access violation: 0 read, 1 write, 8 execute (DEP)
    ULONG_PTR info1; // access violation: target address
};

Status &status() {
    static Status s;
    return s;
}

void set_status(std::string Status::*field, const std::string &value) {
    Status &s = status();
    std::lock_guard<std::mutex> lock(s.m);
    s.*field = value;
}

} // namespace vitox

using namespace vitox;

namespace {

LARGE_INTEGER g_process_start{};

const char *activation_kind(ActivationKind k) {
    switch (k) {
    case ActivationKind::Launch: return "Launch";
    case ActivationKind::Protocol: return "Protocol";
    case ActivationKind::File: return "File";
    default: return "Other";
    }
}

const char *exec_state(ApplicationExecutionState s) {
    switch (s) {
    case ApplicationExecutionState::NotRunning: return "NotRunning";
    case ApplicationExecutionState::Running: return "Running";
    case ApplicationExecutionState::Suspended: return "Suspended";
    case ApplicationExecutionState::Terminated: return "Terminated";
    case ApplicationExecutionState::ClosedByUser: return "ClosedByUser";
    default: return "?";
    }
}

// session_state.txt records the last lifecycle state, so the next launch can
// tell whether the previous session exited cleanly or was terminated/crashed.
std::wstring g_state_path;

void write_session_state(const char *state) {
    if (g_state_path.empty())
        return;
    std::ofstream f(g_state_path, std::ios::trunc);
    f << state << " pid=" << GetCurrentProcessId() << "\n";
}

std::string read_session_state() {
    std::ifstream f(g_state_path);
    std::string line;
    if (!f || !std::getline(f, line))
        return "none (first run or file missing)";
    return line;
}

class Graphics {
public:
    bool init(CoreWindow const &window) {
        dpi_ = Windows::Graphics::Display::DisplayInformation::GetForCurrentView().LogicalDpi();

        const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
        D3D_FEATURE_LEVEL got{};
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, d3d_.put(), &got, ctx_.put());
        log_line(Log::Lifecycle, "gfx: D3D11CreateDevice hr=0x%08lX feature_level=0x%04X", static_cast<unsigned long>(hr), static_cast<unsigned>(got));
        if (FAILED(hr))
            return false;

        com_ptr<IDXGIDevice1> dxgi_device = d3d_.as<IDXGIDevice1>();
        com_ptr<IDXGIAdapter> adapter;
        dxgi_device->GetAdapter(adapter.put());
        DXGI_ADAPTER_DESC ad{};
        adapter->GetDesc(&ad);
        log_line(Log::Lifecycle, "gfx: adapter=\"%s\" vendor=0x%04X device=0x%04X dedicated_vram=%zu",
            narrow(ad.Description).c_str(), ad.VendorId, ad.DeviceId, ad.DedicatedVideoMemory);
        com_ptr<IDXGIFactory2> factory;
        adapter->GetParent(__uuidof(IDXGIFactory2), factory.put_void());

        const Size px = pixel_size(window);
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = static_cast<UINT>(px.Width);
        desc.Height = static_cast<UINT>(px.Height);
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        desc.Scaling = DXGI_SCALING_STRETCH;
        desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        hr = factory->CreateSwapChainForCoreWindow(d3d_.get(), get_unknown(window), &desc, nullptr, swap_.put());
        log_line(Log::Lifecycle, "gfx: CreateSwapChainForCoreWindow %ux%u dpi=%.0f hr=0x%08lX", desc.Width, desc.Height, dpi_, static_cast<unsigned long>(hr));
        if (FAILED(hr))
            return false;

        D2D1_FACTORY_OPTIONS opts{};
        hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &opts, d2d_factory_.put_void());
        if (SUCCEEDED(hr))
            hr = d2d_factory_->CreateDevice(dxgi_device.get(), d2d_device_.put());
        if (SUCCEEDED(hr))
            hr = d2d_device_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, dc_.put());
        if (SUCCEEDED(hr))
            hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<::IUnknown **>(dwrite_.put()));
        if (SUCCEEDED(hr))
            hr = dwrite_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL, 40.0f, L"en-us", title_.put());
        if (SUCCEEDED(hr))
            hr = dwrite_->CreateTextFormat(L"Consolas", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL, 20.0f, L"en-us", body_.put());
        if (SUCCEEDED(hr))
            hr = dc_->CreateSolidColorBrush(D2D1::ColorF(0.92f, 0.95f, 1.0f), brush_.put());
        log_line(Log::Lifecycle, "gfx: D2D/DWrite init hr=0x%08lX", static_cast<unsigned long>(hr));
        if (FAILED(hr))
            return false;
        load_logo();
        return make_target();
    }

    void resize(CoreWindow const &window) {
        if (!swap_)
            return;
        dc_->SetTarget(nullptr);
        target_ = nullptr;
        const Size px = pixel_size(window);
        HRESULT hr = swap_->ResizeBuffers(0, static_cast<UINT>(px.Width), static_cast<UINT>(px.Height), DXGI_FORMAT_UNKNOWN, 0);
        log_line(Log::Lifecycle, "gfx: ResizeBuffers %.0fx%.0f hr=0x%08lX", px.Width, px.Height, static_cast<unsigned long>(hr));
        make_target();
    }

    void draw(const std::wstring &title, const std::wstring &body) {
        if (!target_)
            return;
        const D2D1_SIZE_F size = dc_->GetSize();
        dc_->BeginDraw();
        // 0.0.31.0: the start-up screen is the launcher's dark violet only (the splash colour): the old blue probe status
        // page is gone from the screen; every probe result is still in the logs
        dc_->Clear(D2D1::ColorF(0x0B0518));
        // 0.0.31.2: the big start-up logo: the splash image (1240x600 px) at about 65% of the screen width, centred. The
        // system splash (620x300 at scale 100 on Xbox) is only up for ~0.1 s before this first frame.
        if (logo_) {
            const float w = std::min(size.width * 0.65f, 1240.0f * size.width / 1920.0f), h = w * 300.0f / 620.0f;
            dc_->DrawBitmap(logo_.get(), D2D1::RectF((size.width - w) / 2, (size.height - h) / 2, (size.width + w) / 2, (size.height + h) / 2), 1.0f,
                D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
        }
        (void)title, (void)body;
        HRESULT hr = dc_->EndDraw();
        if (SUCCEEDED(hr))
            hr = swap_->Present(1, 0);
        if (FAILED(hr) && !present_error_logged_) {
            present_error_logged_ = true;
            log_line(Log::Lifecycle, "gfx: draw/present failed hr=0x%08lX", static_cast<unsigned long>(hr));
        }
    }

    void trim() {
        if (d3d_) {
            ctx_->ClearState();
            d3d_.as<IDXGIDevice3>()->Trim();
        }
    }

    // M6A: a CoreWindow holds one swapchain at a time; drop everything so the graphics probe can
    // create its own D3D12 swapchain for the window (init() recreates the status display after).
    void release() {
        if (dc_)
            dc_->SetTarget(nullptr);
        target_ = nullptr;
        brush_ = nullptr;
        title_ = nullptr;
        body_ = nullptr;
        dwrite_ = nullptr;
        dc_ = nullptr;
        d2d_device_ = nullptr;
        d2d_factory_ = nullptr;
        swap_ = nullptr;
        if (ctx_) {
            ctx_->ClearState();
            ctx_->Flush();
        }
        ctx_ = nullptr;
        d3d_ = nullptr;
    }

    unsigned width_px(CoreWindow const &window) const { return static_cast<unsigned>(pixel_size(window).Width); }
    unsigned height_px(CoreWindow const &window) const { return static_cast<unsigned>(pixel_size(window).Height); }

private:
    Size pixel_size(CoreWindow const &window) const {
        const Rect b = window.Bounds();
        return { std::max(1.0f, b.Width * dpi_ / 96.0f), std::max(1.0f, b.Height * dpi_ / 96.0f) };
    }

    // 0.0.31.1: the packaged splash image at the display's scale (Assets\SplashScreen.scale-NNN.png)
    void load_logo() {
        const float sc = dpi_ / 96.0f;
        const int pick = 200; (void)sc; // 0.0.31.2: drawn big (see draw); no scale-400 (manifest image size limit)
        HRESULT hr = E_FAIL;
        try {
            const std::wstring path = std::wstring(Windows::ApplicationModel::Package::Current().InstalledLocation().Path()) + L"\\Assets\\SplashScreen.scale-" +
                std::to_wstring(pick) + L".png";
            com_ptr<IWICImagingFactory> wic;
            hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(wic.put()));
            com_ptr<IWICBitmapDecoder> dec;
            com_ptr<IWICBitmapFrameDecode> frame;
            com_ptr<IWICFormatConverter> conv;
            if (SUCCEEDED(hr))
                hr = wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, dec.put());
            if (SUCCEEDED(hr))
                hr = dec->GetFrame(0, frame.put());
            if (SUCCEEDED(hr))
                hr = wic->CreateFormatConverter(conv.put());
            if (SUCCEEDED(hr))
                hr = conv->Initialize(frame.get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeMedianCut);
            if (SUCCEEDED(hr))
                hr = dc_->CreateBitmapFromWicBitmap(conv.get(), nullptr, logo_.put());
        } catch (...) {
            hr = E_UNEXPECTED;
        }
        log_line(Log::Lifecycle, "gfx: start-up logo (scale-%d) hr=0x%08lX", pick, static_cast<unsigned long>(hr));
    }

    bool make_target() {
        com_ptr<IDXGISurface> surface;
        HRESULT hr = swap_->GetBuffer(0, __uuidof(IDXGISurface), surface.put_void());
        if (SUCCEEDED(hr)) {
            const D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
                D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), dpi_, dpi_);
            hr = dc_->CreateBitmapFromDxgiSurface(surface.get(), &props, target_.put());
        }
        if (FAILED(hr)) {
            log_line(Log::Lifecycle, "gfx: make_target failed hr=0x%08lX", static_cast<unsigned long>(hr));
            return false;
        }
        dc_->SetTarget(target_.get());
        dc_->SetDpi(dpi_, dpi_);
        return true;
    }

    float dpi_ = 96.0f;
    bool present_error_logged_ = false;
    com_ptr<ID3D11Device> d3d_;
    com_ptr<ID3D11DeviceContext> ctx_;
    com_ptr<IDXGISwapChain1> swap_;
    com_ptr<ID2D1Factory1> d2d_factory_;
    com_ptr<ID2D1Device> d2d_device_;
    com_ptr<ID2D1DeviceContext> dc_;
    com_ptr<ID2D1Bitmap1> logo_; // 0.0.31.1
    com_ptr<ID2D1Bitmap1> target_;
    com_ptr<IDWriteFactory> dwrite_;
    com_ptr<IDWriteTextFormat> title_, body_;
    com_ptr<ID2D1SolidColorBrush> brush_;
};

struct App : implements<App, IFrameworkViewSource, IFrameworkView> {
    IFrameworkView CreateView() { return *this; }

    void Initialize(CoreApplicationView const &view) {
        local_state_ = std::wstring(Windows::Storage::ApplicationData::Current().LocalFolder().Path());
        SetEnvironmentVariableW(L"VITOX_LOCALSTATE", local_state_.c_str()); // 0.0.24.98: dzn XBSC bisect progress file
        const bool logs_ok = log_open(local_state_);
        crash_install(local_state_);
        g_state_path = local_state_ + L"\\session_state.txt";

        LARGE_INTEGER f, now;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&now);
        const double since_main = (now.QuadPart - g_process_start.QuadPart) * 1000.0 / f.QuadPart;

        for (int i = 0; i < static_cast<int>(Log::Count); ++i)
            log_line(static_cast<Log>(i), "===== Vitox M4 Probe session start pid=%lu =====", GetCurrentProcessId());
        log_line(Log::Lifecycle, "process_start: wWinMain entered %.1f ms before Initialize; logs_ok=%d", since_main, logs_ok ? 1 : 0);
        log_line(Log::Lifecycle, "build: version=%s compiled=%s %s config=%s", VITOX_M1_VERSION, __DATE__, __TIME__,
#ifdef NDEBUG
            "Release"
#else
            "Debug"
#endif
        );
        log_package_info();
        log_line(Log::Lifecycle, "previous_session_state=%s", read_session_state().c_str());
        write_session_state("RUNNING");

        view.Activated({ this, &App::OnActivated });
        CoreApplication::Suspending({ this, &App::OnSuspending });
        CoreApplication::Resuming({ this, &App::OnResuming });
        CoreApplication::EnteredBackground([](IInspectable const &, EnteredBackgroundEventArgs const &) {
            log_line(Log::Lifecycle, "EnteredBackground");
            mem_snapshot("entered_background");
            log_checkpoint("m9a4 cp lifecycle: EnteredBackground");
        });
        CoreApplication::LeavingBackground([](IInspectable const &, LeavingBackgroundEventArgs const &) {
            log_line(Log::Lifecycle, "LeavingBackground");
            mem_snapshot("leaving_background");
            log_checkpoint("m9a4 cp lifecycle: LeavingBackground");
        });
        CoreApplication::Exiting([](IInspectable const &, IInspectable const &) {
            log_line(Log::Lifecycle, "Exiting");
            write_session_state("EXITED (Exiting)");
            log_checkpoint("m9a4 cp lifecycle: Exiting");
        });
        CoreApplication::UnhandledErrorDetected([](IInspectable const &, UnhandledErrorDetectedEventArgs const &a) {
            log_line(Log::Lifecycle, "UnhandledErrorDetected handled=%d", a.UnhandledError().Handled() ? 1 : 0);
        });

        using winrt::Windows::System::MemoryManager;
        MemoryManager::AppMemoryUsageLimitChanging([](IInspectable const &, Windows::System::AppMemoryUsageLimitChangingEventArgs const &a) {
            log_line(Log::Mem, "AppMemoryUsageLimitChanging old=%llu new=%llu", a.OldLimit(), a.NewLimit());
            log_line(Log::Lifecycle, "AppMemoryUsageLimitChanging old=%llu new=%llu", a.OldLimit(), a.NewLimit());
        });
        MemoryManager::AppMemoryUsageIncreased([](IInspectable const &, IInspectable const &) { mem_snapshot("usage_level_increased"); });
        MemoryManager::AppMemoryUsageDecreased([](IInspectable const &, IInspectable const &) { mem_snapshot("usage_level_decreased"); });

        mem_snapshot("startup");
    }

    void SetWindow(CoreWindow const &window) {
        log_line(Log::Lifecycle, "SetWindow bounds=%.0fx%.0f", window.Bounds().Width, window.Bounds().Height);
        window.VisibilityChanged([this](CoreWindow const &, VisibilityChangedEventArgs const &a) {
            visible_ = a.Visible();
            log_line(Log::Lifecycle, "VisibilityChanged visible=%d", visible_ ? 1 : 0);
            log_checkpoint(visible_ ? "m9a4 cp lifecycle: VisibilityChanged visible=1" : "m9a4 cp lifecycle: VisibilityChanged visible=0");
        });
        window.Activated([](CoreWindow const &, WindowActivatedEventArgs const &a) {
            log_line(Log::Lifecycle, "WindowActivated state=%d", static_cast<int>(a.WindowActivationState()));
        });
        window.Closed([this](CoreWindow const &, CoreWindowEventArgs const &) {
            log_line(Log::Lifecycle, "WindowClosed");
            log_checkpoint("m9a4 cp lifecycle: WindowClosed");
            closed_ = true;
        });
        window.SizeChanged([this](CoreWindow const &w, WindowSizeChangedEventArgs const &a) {
            log_line(Log::Lifecycle, "SizeChanged %.0fx%.0f", a.Size().Width, a.Size().Height);
            gfx_.resize(w);
        });
        // Keep the B button from navigating away; this probe has no back stack.
        Windows::UI::Core::SystemNavigationManager::GetForCurrentView().BackRequested(
            [](IInspectable const &, BackRequestedEventArgs const &a) { a.Handled(true); });

        gfx_ok_ = gfx_.init(window);
        log_line(Log::Lifecycle, "gfx init %s", gfx_ok_ ? "ok" : "FAILED (probes still run; screen may stay on splash)");
    }

    void Load(hstring const &entry) { log_line(Log::Lifecycle, "Load entry=%s", to_string(entry).c_str()); }

    void Run() {
        log_line(Log::Lifecycle, "Run begin");
        CoreWindow window = CoreWindow::GetForCurrentThread();
        window.Activate();
        render(window);

        run_probe("API", [] { probe_api(); });
        render(window);
        run_probe("JIT", [] { probe_jit(); });
        render(window);
        run_probe("FS", [this] { probe_fs(local_state_); });
        render(window);
        run_probe("DYNARMIC", [] { probe_dynarmic(); });
        render(window);
        run_probe("VEH", [] { probe_veh(); });
        render(window);
        run_probe("UNWIND", [] { probe_unwind(); });
        render(window);
        run_probe("FASTMEM", [] { probe_fastmem(); });
        render(window);
        // M4.2: real Vita3K IO + config (no guest memory, so no VEH): before CORE.
        run_probe("IO", [this] { probe_io(local_state_); });
        render(window);
        // Last on purpose: Vita3K's mem layer installs a process-wide VEH that is never removed.
        run_probe("CORE", [] { probe_core(); });
        render(window);
        // M8A (0.0.17.x): the user-selectable homebrew launcher replaces the packaged bring-up apps (M5B VITOX0001 probe,
        // M7A/M7B VITOX0002/3 - all still in the tree, no longer run or packaged). The app and the presenter mode are read
        // at run time from D:\DevelopmentFiles\Vitox\homebrew\vitox-launch.txt. Headless = non-visual precheck (the status
        // display keeps the CoreWindow); visual yields the CoreWindow and recreates the status display afterwards.
        // M8C.1: staged firmware install (once; only when firmware-staging\input exists and nothing is staged yet)
        run_probe("M8C", [] { probe_m8c(); });
        run_probe("M8C2", [this] { probe_m8c2(local_state_); }); // M8C.2: only when the staging is complete
        render(window);
        // 0.0.25.0: the Vitox launcher. A visual start opens the library (app= of vitox-launch.txt only preselects); a
        // headless run, ui=direct and the diagnostic scripted-input keys keep the direct boot of app= (harness runs). After
        // a direct visual run, and after every game, the library comes back.
        const VitoxLaunchConfig base = vitox_launcher_read_config(kDevRoot);
        // 0.0.26.0: the controller settings (Settings > Input) apply to every game session, direct boots included
        if (!input_config_load(local_state_ + L"\\vitox-input.txt"))
            log_line(Log::Core, "vitox input config: none saved (defaults)");
        // 0.0.27.0: internal resolution upscaling (Settings > Graphics); validated, and reverted by the crash guard
        graphics_load(local_state_);
        // 0.0.28.0: the performance statistics overlay (Settings page + R3 menu share it)
        stats_load(local_state_);
        // 0.0.29.0: Settings > Library (homebrew shown or hidden in the library)
        library_load(local_state_);
        game_config_load(local_state_); // 0.0.30.0
        const bool m8a_headless = base.headless;
        const bool direct = m8a_headless || base.ui_direct || base.auto_touch[0] || base.auto_stick[0] || base.ime_input[0];
        log_line(Log::Lifecycle, "launcher: presenter=%s ui=%s%s", m8a_headless ? "headless" : "visual", direct ? "direct (boot app=)" : "launcher (library)",
            base.ui_script[0] ? " | ui_script (DIAGNOSTIC)" : "");
        if (base.ui_script[0])
            ui_script_load(std::wstring(kDevRoot) + L"\\homebrew\\" + widen(base.ui_script));
        if (!m8a_headless)
            gfx_.release();
        if (direct) {
            graphics_session_begin();
            run_probe("M8A", [this, &window] {
                set_status(&Status::m6c, probe_m8a(winrt::get_unknown(window), 1920, 1080, [] {
                    CoreWindow::GetForCurrentThread().Dispatcher().ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
                }, local_state_));
            });
            graphics_session_end();
        }
        if (!m8a_headless)
            run_launcher(window, base);
        if (m8a_headless)
            render(window);
        log_line(Log::Lifecycle, "probes complete: jit=%s api=%s fs_local=%s fs_dev=%s dyn=%s veh=%s unwind=%s fastmem=%s io=%s core=%s hb=%s m6c=%s",
            status().jit.c_str(), status().api.c_str(), status().fs_local.c_str(), status().fs_dev.c_str(),
            status().dyn.c_str(), status().veh.c_str(), status().unwind.c_str(), status().fastmem.c_str(), status().io.c_str(),
            status().core.c_str(), status().hb.c_str(), status().m6c.c_str());
        log_checkpoint("m9a4 cp app: probes complete, entering the idle loop");

        mem_thread_start();
        while (!closed_) {
            if (visible_) {
                window.Dispatcher().ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
                render(window);
            } else {
                window.Dispatcher().ProcessEvents(CoreProcessEventsOption::ProcessOneAndAllPending);
            }
        }
        mem_thread_stop();
        log_line(Log::Lifecycle, "Run end");
    }

    void Uninitialize() {
        log_line(Log::Lifecycle, "Uninitialize");
        write_session_state("EXITED (Uninitialize)");
    }

    void OnActivated(CoreApplicationView const &, IActivatedEventArgs const &args) {
        log_line(Log::Lifecycle, "Activated kind=%s(%d) previous_execution_state=%s", activation_kind(args.Kind()),
            static_cast<int>(args.Kind()), exec_state(args.PreviousExecutionState()));
        CoreWindow::GetForCurrentThread().Activate();
    }

    void OnSuspending(IInspectable const &, SuspendingEventArgs const &args) {
        log_line(Log::Lifecycle, "Suspending deadline_in_ms=%lld",
            static_cast<long long>((args.SuspendingOperation().Deadline() - clock::now()).count() / 10000));
        mem_snapshot("suspending");
        write_session_state("SUSPENDED");
        gfx_.trim();
        log_line(Log::Lifecycle, "Suspending done");
        log_checkpoint("m9a4 cp lifecycle: Suspending done");
    }

    void OnResuming(IInspectable const &, IInspectable const &) {
        log_line(Log::Lifecycle, "Resuming");
        write_session_state("RUNNING (resumed)");
        mem_snapshot("resuming");
        log_checkpoint("m9a4 cp lifecycle: Resuming");
    }

private:
    // 0.0.25.0: library -> game -> library, until the window closes. The CoreWindow holds one swapchain at a time: the
    // launcher releases its swapchain before a game's presenter takes the window, and recreates it afterwards.
    void run_launcher(CoreWindow const &window, const VitoxLaunchConfig &base) {
        const std::wstring last_path = local_state_ + L"\\launcher_last.txt";
        std::string preselect;
        {
            std::ifstream f(last_path);
            std::getline(f, preselect);
        }
        if (preselect.empty())
            preselect = base.app;
        std::wstring toast = graphics_notice(); // the crash guard reverted the multiplier: say so once at the start
        LauncherHost host;
        host.version = widen(package_version_);
        host.local_state = local_state_;
        host.closed = [this] { return closed_; };
        host.visible = [this] { return visible_; };
        unsigned sessions = 0;
        while (!closed_) {
            LauncherUi ui;
            if (!ui.init(window)) {
                log_line(Log::Lifecycle, "launcher: UI init FAILED - no library");
                break;
            }
            const LauncherChoice c = ui.run(window, host, preselect, toast);
            ui.release();
            if (!c.launch)
                break;
            preselect = c.app.title_id;
            {
                std::ofstream f(last_path, std::ios::trunc);
                f << preselect << "\n";
            }
            const VitoxLaunchConfig cfg = vitox_launcher_config_for(kDevRoot, &c.app);
            const std::wstring title = widen(c.app.title[0] ? c.app.title : c.app.title_id);
            ++sessions;
            log_line(Log::Lifecycle, "launcher: session %u: %s '%s' from %s/%s | lle_modules=%s firmware=%s fail_closed=%d async_pipelines=%s%s%s", sessions,
                c.app.title_id, c.app.title, c.app.app_root, c.app.folder, cfg.lle_modules[0] ? cfg.lle_modules : "-", cfg.staged_pgf ? "staging-pgf" : "none",
                cfg.fail_closed ? 1 : 0, cfg.sync_pipelines ? "off" : "on", cfg.error[0] ? " | CONFIG ERROR: " : "", cfg.error);
            log_checkpoint("m9a4 cp app: launcher session start");
            if (cfg.error[0]) {
                toast = L"Cannot start " + title + L": " + widen(cfg.error);
                continue;
            }
            overlay_set_info({ title, widen(c.app.title_id), std::wstring(c.app.dir) + L"\\sce_sys\\icon0.png" });
            bool quit = false;
            std::string result;
            // 0.0.30.0: the game's own settings (game menu) for this session; the global ones stay saved as they are
            const GameConfig gc = game_config(c.app.title_id);
            graphics_session_override(gc.resolution, gc.async, gc.prewarm);
            stats_session_override(gc.stats);
            graphics_session_begin();
            run_probe("M8A", [&] {
                result = probe_m8a_with(winrt::get_unknown(window), 1920, 1080, [] {
                    CoreWindow::GetForCurrentThread().Dispatcher().ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
                }, local_state_, &cfg, &quit);
                set_status(&Status::m6c, result);
            });
            graphics_session_end();
            stats_session_override(-1);
            if (quit)
                toast = L"Closed " + title;
            else if (result.find("TRAPPED") != std::string::npos)
                toast = title + L" stopped: it needs a system function Vitox does not support yet";
            else if (result.find("exit=0") != std::string::npos)
                toast = title + L" exited";
            else if (result.find("not-run") != std::string::npos)
                // 0.0.27.1: the guest never ran (a module or import check failed before its first instruction): say so
                toast = title + L" could not start - see the Vitox log for the reason";
            else
                toast = title + L" stopped";
            log_line(Log::Lifecycle, "launcher: session %u ended (%s) -> back to the library%s", sessions, quit ? "Return to Launcher" : "guest stop",
                result.find("not-run") != std::string::npos ? " | the guest never started" : "");
            log_checkpoint("m9a4 cp app: launcher session end, back to the library");
        }
    }

    template <typename F>
    void run_probe(const char *name, F &&fn) {
        log_line(Log::Lifecycle, "probe %s begin", name);
        // /EHa: translate SEH faults so the log records the code and fault location.
        const _se_translator_function prev = _set_se_translator([](unsigned code, EXCEPTION_POINTERS *p) {
            const EXCEPTION_RECORD *r = p->ExceptionRecord;
            throw SehFault{ code, r->ExceptionAddress, r->NumberParameters > 0 ? r->ExceptionInformation[0] : 0,
                r->NumberParameters > 1 ? r->ExceptionInformation[1] : 0 };
        });
        try {
            fn();
        } catch (SehFault const &f) {
            HMODULE mod = nullptr;
            wchar_t path[MAX_PATH] = L"?";
            if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    static_cast<LPCWSTR>(f.address), &mod))
                GetModuleFileNameW(mod, path, MAX_PATH);
            else
                wcscpy_s(path, L"<no module: JIT/heap>");
            log_line(Log::Lifecycle, "probe %s SEH code=0x%08X address=%p module=%s rva=0x%llX info0=0x%llX info1=0x%llX",
                name, f.code, f.address, narrow(path).c_str(),
                mod ? static_cast<unsigned long long>(static_cast<const char *>(f.address) - reinterpret_cast<const char *>(mod)) : 0ull,
                static_cast<unsigned long long>(f.info0), static_cast<unsigned long long>(f.info1));
        } catch (hresult_error const &e) {
            log_line(Log::Lifecycle, "probe %s threw hresult=0x%08lX %s", name, static_cast<unsigned long>(e.code()), to_string(e.message()).c_str());
        } catch (std::exception const &e) {
            log_line(Log::Lifecycle, "probe %s threw std::exception %s", name, e.what());
        } catch (...) {
            log_line(Log::Lifecycle, "probe %s threw unknown exception", name);
        }
        _set_se_translator(prev);
        log_line(Log::Lifecycle, "probe %s end", name);
    }

    void log_package_info() {
        try {
            const Package pkg = Package::Current();
            const PackageId id = pkg.Id();
            const PackageVersion v = id.Version();
            char ver[64];
            snprintf(ver, sizeof(ver), "%u.%u.%u.%u", v.Major, v.Minor, v.Build, v.Revision);
            log_line(Log::Lifecycle, "package: name=%s publisher=%s version=%s family=%s full=%s", to_string(id.Name()).c_str(),
                to_string(id.Publisher()).c_str(), ver, to_string(id.FamilyName()).c_str(), to_string(id.FullName()).c_str());
            log_line(Log::Lifecycle, "package: install_path=%s", to_string(pkg.InstalledLocation().Path()).c_str());
            package_version_ = ver;
        } catch (hresult_error const &e) {
            log_line(Log::Lifecycle, "package: query failed hr=0x%08lX", static_cast<unsigned long>(e.code()));
        }
        log_line(Log::Lifecycle, "local_state=%s", narrow(local_state_).c_str());

        try {
            const auto vi = Windows::System::Profile::AnalyticsInfo::VersionInfo();
            const uint64_t raw = std::stoull(to_string(vi.DeviceFamilyVersion()));
            char os[64];
            snprintf(os, sizeof(os), "%llu.%llu.%llu.%llu", raw >> 48, (raw >> 32) & 0xFFFF, (raw >> 16) & 0xFFFF, raw & 0xFFFF);
            const std::string family = to_string(vi.DeviceFamily());
            log_line(Log::Lifecycle, "device: family=%s os_version=%s", family.c_str(), os);
            set_status(&Status::device, family + " " + os);
        } catch (...) {
            log_line(Log::Lifecycle, "device: AnalyticsInfo query failed");
        }

        SYSTEM_INFO si{};
        GetNativeSystemInfo(&si);
        log_line(Log::Lifecycle, "system: cpus=%lu page_size=%lu alloc_granularity=%lu min_addr=%p max_addr=%p",
            si.dwNumberOfProcessors, si.dwPageSize, si.dwAllocationGranularity, si.lpMinimumApplicationAddress,
            si.lpMaximumApplicationAddress);
    }

    void render(CoreWindow const &window) {
        // 0.0.31.3: process window events between the start-up probes: the system splash stays up until the window is
        // activated / visible, which needs the dispatcher; otherwise the big start-up logo below is never seen
        window.Dispatcher().ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
        if (!gfx_ok_)
            return;
        std::wstring body;
        {
            Status &s = status();
            std::lock_guard<std::mutex> lock(s.m);
            std::ostringstream o;
            o << "Version " << package_version_ << "   device " << s.device << "\n\n"
              << "JIT (probe 4)       " << s.jit << "\n"
              << "  RW->RX execute    " << s.jit_rx << "\n"
              << "  patch cycle       " << s.jit_patch << "\n"
              << "  direct RWX        " << s.jit_rwx << "\n"
              << "API availability    " << s.api << "\n"
              << "LocalState FS       " << s.fs_local << "\n"
              << "DevelopmentFiles FS " << s.fs_dev << "\n"
              << "Memory              " << s.mem << "\n"
              << "Dynarmic A32 (1-5)  " << s.dyn << "\n"
              << "VEH (6)             " << s.veh << "\n"
              << "Unwind (7)          " << s.unwind << "\n"
              << "Fastmem (M2.5)      " << s.fastmem << "\n"
              << "Vita3K IO/config    " << s.io << "\n"
              << "Vita3K core (M3)    " << s.core << "\n"
              << "Homebrew (M5B)      " << s.hb << "\n"
              << "Vita3K GXM triangle M7B " << s.m6c << "\n\n"
              << "Logs: " << narrow(local_state_) << "\n"
              << "  lifecycle_log.txt  mem_log.txt  probe_log.txt  fs_log.txt  crash_log.txt\n"
              << "  dynarmic_log.txt  veh_log.txt  unwind_log.txt  fastmem_log.txt  core_log.txt";
            body = widen(o.str());
        }
        gfx_.draw(L"Vitox", body);
    }

    std::wstring local_state_;
    std::string package_version_ = "?";
    Graphics gfx_;
    bool gfx_ok_ = false;
    bool visible_ = true;
    bool closed_ = false;
};

} // namespace

int __stdcall wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    QueryPerformanceCounter(&g_process_start);
    CoreApplication::Run(make<App>());
    return 0;
}
