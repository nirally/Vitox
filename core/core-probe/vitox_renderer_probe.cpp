// Vitox M6C (0.0.13.0): first REAL Vita3K Vulkan renderer on Xbox, presented through the external presenter.
//
// All real Vita3K code unless noted:
//   EmuEnvState (+ cfg defaults), renderer::init(Backend::Vulkan) -> VKState::create (direct dzn ICD entry point,
//   M6B shim) -> ScreenRenderer::create/setup (M6C external-present substitution), VKState::late_init(cfg,
//   "VITOX0001", emuenv.mem), init(emuenv.mem) (Vita3K's app order: renderer, late_init, mem), the renderer half of
//   apply_renderer_config (no overlay manager), then per frame the real SceDisplay -> renderer display path:
//   DisplayState::next_rendered_frame -> VKState::render_frame (staging copy of the guest framebuffer -> vita_surface ->
//   screen filter -> ScreenRenderer image) -> VKState::swap_window -> host Wait/Copy/Present.
// The guest framebuffer is an animated test pattern the probe writes into real guest memory (no GXM commands yet).
#include "vitox_renderer_probe.h"

#include <vitox_external_present.h>
#include <vitox_swscale_disabled.h>
#include <vitox_sdl_shim_report.h>

#include <config/state.h>
#include <display/state.h>
#include <emuenv/state.h>
#include <gxm/state.h>
#include <mem/functions.h>
#include <mem/state.h>
#include <renderer/frame_host.h>
#include <renderer/functions.h>
#include <renderer/state.h>
#include <util/log.h>

#include <spdlog/sinks/callback_sink.h>
#include <spdlog/sinks/null_sink.h>

#include <chrono>
#include <cmath>

namespace {

const VitoxCoreHost *g_host = nullptr;

template <typename... Args>
void out(fmt::format_string<Args...> f, Args &&...args) {
    g_host->log(fmt::format(f, std::forward<Args>(args)...).c_str());
}
void snap(const char *tag) { g_host->mem_snapshot(tag); }
const char *pf(bool b) { return b ? "PASS" : "FAIL"; }

void install_log_bridge() {
    auto sink = std::make_shared<spdlog::sinks::callback_sink_mt>([](const spdlog::details::log_msg &msg) {
        const auto level = spdlog::level::to_string_view(msg.level);
        const std::string line = fmt::format("[vita3k {}] {}", std::string_view(level.data(), level.size()),
            std::string_view(msg.payload.data(), msg.payload.size()));
        g_host->log(line.c_str());
    });
    auto logger = std::make_shared<spdlog::logger>("vita3k", sink);
    logger->set_level(spdlog::level::info);
    spdlog::set_default_logger(logger);
}

// Vitox FrameHost: no native window handle and no VkSurfaceKHR provider - presentation is external (host D3D12).
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

const char *mapping_name(MappingMethod m) {
    switch (m) {
    case MappingMethod::Disabled: return "Disabled";
    case MappingMethod::DoubleBuffer: return "DoubleBuffer";
    case MappingMethod::ExernalHost: return "ExternalHost";
    case MappingMethod::PageTable: return "PageTable";
    case MappingMethod::NativeBuffer: return "NativeBuffer";
    }
    return "?";
}

constexpr uint32_t kFbWidth = 960, kFbHeight = 544;

// A8B8G8R8 (memory order R, G, B, A): 8 colour bars, a white band sweeping down (~4 s), bars drifting right (~8 s).
void draw_pattern(uint8_t *px, float t) {
    static const uint8_t bars[8][3] = { { 235, 235, 235 }, { 235, 235, 16 }, { 16, 235, 235 }, { 16, 235, 16 }, { 235, 16, 235 }, { 235, 16, 16 },
        { 16, 16, 235 }, { 64, 64, 64 } };
    const uint32_t band_y = static_cast<uint32_t>(std::fmod(t / 4.0f, 1.0f) * kFbHeight);
    const uint32_t shift = static_cast<uint32_t>(std::fmod(t / 8.0f, 1.0f) * kFbWidth);
    for (uint32_t y = 0; y < kFbHeight; ++y) {
        const bool band = y >= band_y && y < band_y + 24;
        uint8_t *row = px + static_cast<size_t>(y) * kFbWidth * 4;
        for (uint32_t x = 0; x < kFbWidth; ++x) {
            const uint8_t *c = bars[((x + shift) % kFbWidth) * 8 / kFbWidth];
            row[x * 4 + 0] = band ? 255 : c[0];
            row[x * 4 + 1] = band ? 255 : c[1];
            row[x * 4 + 2] = band ? 255 : c[2];
            row[x * 4 + 3] = 255;
        }
    }
}

} // namespace

VitoxRendererResult vitox_renderer_probe_run(const VitoxCoreHost *host, vitox::ExternalPresenter *presenter, void (*pump)(), const wchar_t *local_state,
    const wchar_t *dev_root, const wchar_t *static_assets, unsigned seconds, VitoxXpVariant variant, unsigned cycle) {
    g_host = host;
    VitoxRendererResult r{};
    install_log_bridge();
    const fs::path ls(local_state);
    const std::string tag = fmt::format("m6c1_c{}", cycle);
    const auto snapf = [&](const char *point) { snap((tag + "_" + point).c_str()); };
    vitox_xp_set_variant(variant);
    out("M6C.1 renderer cycle {} variant {} begin: real renderer::init(Vulkan) + late_init + display path, external presenter {}x{} x{} images, {} s", cycle,
        vitox_xp_variant_name(variant), presenter->width(), presenter->height(), presenter->image_count(), seconds);
    snapf("before_renderer");

    auto *emuenv = new EmuEnvState();
    emuenv->config_path = ls;
    emuenv->log_path = ls / "log";
    emuenv->cache_path = ls / "cache";
    emuenv->vita_fs_path = fs::path(dev_root) / "vita-fs";
    emuenv->static_assets_path = fs::path(static_assets);
    // ScreenFilter (Bilinear) + overlay renderer load these through vkutil::load_shader; a missing one would yield a null module
    bool assets_ok = true;
    for (const char *rel : { "shaders-builtin/vulkan/render_main.vert.spv", "shaders-builtin/vulkan/render_main.frag.spv",
             "shaders-builtin/overlay/overlay.vert.spv", "shaders-builtin/overlay/overlay.frag.spv" }) {
        const bool present = fs::exists(emuenv->static_assets_path / rel);
        assets_ok &= present;
        out("m6c static asset {} -> {}", rel, present ? "present" : "MISSING");
    }
    if (!assets_ok) {
        out("M6C end: built-in shader assets missing - renderer not started");
        delete emuenv;
        spdlog::set_default_logger(std::make_shared<spdlog::logger>("vita3k-m6c-off", std::make_shared<spdlog::sinks::null_sink_mt>()));
        return r;
    }
    const auto &cc = emuenv->cfg.current_config;
    out("m6c config (defaults): memory_mapping='{}' high_accuracy={} validation_layer={} screen_filter='{}' resolution_multiplier={} v_sync={}", cc.memory_mapping,
        cc.high_accuracy, emuenv->cfg.validation_layer, cc.screen_filter, cc.resolution_multiplier, cc.v_sync);

    // ---- renderer::init (Vita3K app: init_renderer) ----
    vitox_set_external_presenter(presenter);
    VitoxFrameHost frame_host(*presenter);
    const auto t_init = std::chrono::steady_clock::now();
    r.renderer_init = renderer::init(frame_host, emuenv->renderer, renderer::Backend::Vulkan, emuenv->cfg, emuenv->get_root_paths());
    const double init_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_init).count();
    snapf("after_renderer_init");
    const VitoxExternalPresentStats s0 = vitox_external_present_stats();
    r.bridge = r.renderer_init && s0.images >= 2 && s0.image_handles_ok == s0.images;
    r.gpu_sync = s0.gpu_sync;
    out("m6c renderer::init(Backend::Vulkan) -> {} in {:.1f} ms | GPU '{}' | ScreenRenderer external images={} shared={} sync={} -> bridge {}", r.renderer_init,
        init_ms, r.renderer_init ? std::string(emuenv->renderer->get_gpu_name()) : std::string("-"), s0.images, s0.image_handles_ok,
        s0.gpu_sync ? "timeline semaphore = shared D3D12 fence (GPU-GPU)" : "CPU-wait FALLBACK", pf(r.bridge));

    Address fb = 0;
    if (r.renderer_init) {
        // ---- late_init + mem (Vita3K app order) ----
        emuenv->renderer->late_init(emuenv->cfg, "VITOX0001", emuenv->mem);
        const bool need_page_table = emuenv->renderer->mapping_method == MappingMethod::PageTable || emuenv->renderer->mapping_method == MappingMethod::NativeBuffer;
        r.late_init = init(emuenv->mem, need_page_table);
        r.mapping_disabled = emuenv->renderer->mapping_method == MappingMethod::Disabled;
        snapf("after_late_init");
        out("m6c late_init(cfg, 'VITOX0001', emuenv.mem) + init(mem, page_table={}) -> {} | mapping_method={} -> {}", need_page_table, r.late_init,
            mapping_name(emuenv->renderer->mapping_method), pf(r.mapping_disabled));

        // ---- renderer half of apply_renderer_config (no overlay manager) ----
        auto &rs = *emuenv->renderer;
        rs.res_multiplier = cc.resolution_multiplier;
        rs.set_vsync_state(cc.v_sync);
        rs.set_surface_sync_state(cc.disable_surface_sync);
        rs.set_screen_filter(cc.screen_filter);
        rs.set_anisotropic_filtering(cc.anisotropic_filtering);
        rs.set_stretch_display(cc.stretch_the_display_area);
        rs.stretch_hd_pixel_perfect(cc.fullscreen_hd_res_pixel_perfect);
        // M6C.1 variant D: async pipeline compilation OFF; every other variant keeps the config default
        const bool async = variant == VitoxXpVariant::D_AsyncOff ? false : cc.async_pipeline_compilation;
        rs.set_async_compilation(async);
        out("m6c async pipeline compilation: config default={} -> set {}", cc.async_pipeline_compilation, async);

        if (r.late_init)
            fb = alloc(emuenv->mem, kFbWidth * kFbHeight * 4, "vitox_m6c_framebuffer");
        out("m6c guest framebuffer {}x{} A8B8G8R8 at 0x{:08X} (real alloc in guest memory)", kFbWidth, kFbHeight, fb);
    }

    // ---- frames: real SceDisplay -> renderer display path ----
    if (fb) {
        const auto t0 = std::chrono::steady_clock::now();
        double secs = 0;
        bool snap_10s = false;
        while (secs < seconds) {
            pump();
            secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            draw_pattern(Ptr<uint8_t>(fb).get(emuenv->mem), static_cast<float>(secs));
            {
                std::lock_guard<std::mutex> guard(emuenv->display.display_info_mutex);
                emuenv->display.next_rendered_frame = DisplayFrameInfo{ Ptr<const void>(fb), kFbWidth, SCE_DISPLAY_PIXELFORMAT_A8B8G8R8,
                    { static_cast<int>(kFbWidth), static_cast<int>(kFbHeight) } };
            }
            emuenv->renderer->render_frame(emuenv->display, emuenv->gxm, emuenv->mem);
            emuenv->renderer->swap_window();
            ++r.frames;
            if (!snap_10s && secs >= 10.0) {
                snap_10s = true;
                snapf("after_10s");
            }
            if (r.frames % 300 == 0) {
                const auto s = vitox_external_present_stats();
                out("m6c frame {} t={:.1f}s submits ok={} failed={} presents ok={} failed={} timeline={} (completed {}) cpu_wait_fallback={} stalls={}", r.frames,
                    secs, s.submits_ok, s.submits_failed, s.presents_ok, s.presents_failed, s.last_value, s.completed_value, s.cpu_wait_fallback, s.stalls);
            }
            // M6C.1: "until it clearly stalls" - two acquisitions that gave up (3 s each) end the variant
            if (vitox_external_present_stats().stall_unresolved >= 2) {
                out("m6c variant {} CLEARLY STALLED at t={:.1f}s - ending the variant early", vitox_xp_variant_name(variant), secs);
                break;
            }
        }
        r.seconds = secs;
    }
    const VitoxExternalPresentStats s1 = vitox_external_present_stats();
    r.stalled = s1.stalls > 0;
    // wedged: the Vulkan timeline itself never caught up -> device waitIdle in cleanup could block forever
    r.wedged = s1.stall_unresolved > 0 && s1.gpu_sync && s1.completed_value < s1.last_value;
    r.frames_ok = r.frames > 0 && !r.stalled && s1.submits_failed == 0 && s1.presents_failed == 0 && s1.presents_ok == r.frames;
    out("m6c render end [{}]: frames={} in {:.1f} s ({:.1f} fps) acquires={} submits ok={} failed={} presents ok={} failed={} last timeline value={} "
        "completed={} order_errors={} | stalls={} unresolved={} first stall at acquire #{} | other vkQueueSubmit={} | LABELLED cpu_wait={} (avg {:.2f} ms, "
        "max {:.2f} ms) | semaphores created (process)={} -> {}",
        vitox_xp_variant_name(variant), r.frames, r.seconds, r.seconds > 0 ? r.frames / r.seconds : 0.0, s1.acquires, s1.submits_ok, s1.submits_failed,
        s1.presents_ok, s1.presents_failed, s1.last_value, s1.completed_value, s1.value_order_errors, s1.stalls, s1.stall_unresolved, s1.first_stall_frame,
        s1.other_submits, s1.cpu_wait_fallback, s1.cpu_wait_fallback ? s1.cpu_wait_fallback_ms / s1.cpu_wait_fallback : 0.0, s1.cpu_wait_fallback_max_ms,
        s1.semaphores_created, r.wedged ? "WEDGED (Vulkan timeline behind)" : pf(r.frames_ok));

    // ---- counters ----
    const unsigned sws_yuv = vitox_swscale_disabled_count(VITOX_SWS_YUV420_TO_RGB), sws_surf = vitox_swscale_disabled_count(VITOX_SWS_SURFACE_SYNC_RGB24),
                   sws_xfer = vitox_swscale_disabled_count(VITOX_SWS_TRANSFER_DOWNSCALE), sws_ctx = vitox_swscale_disabled_count(VITOX_SWS_UNEXPECTED_CONTEXT);
    const unsigned ticks = vitox_sdl_timer_report().get_ticks;
    r.counters_zero = sws_yuv + sws_surf + sws_xfer + sws_ctx == 0;
    out("m6c counters: swscale-disabled hits yuv420={} surface_rgb24={} transfer_downscale={} unexpected_context={} | SDL_GetTicks calls={} (only common_dialog; no other SDL timer API exists in the shim) -> {}",
        sws_yuv, sws_surf, sws_xfer, sws_ctx, ticks, pf(r.counters_zero));

    // ---- teardown ----
    if (r.wedged) {
        // M6C.1: VKState::cleanup starts with a device waitIdle that would never return; leak the cycle instead
        out("m6c teardown SKIPPED: Vulkan timeline wedged (completed {} < submitted {}) - EmuEnvState + renderer leaked on purpose", s1.completed_value,
            s1.last_value);
        vitox_set_external_presenter(nullptr);
        spdlog::set_default_logger(std::make_shared<spdlog::logger>("vita3k-m6c-off", std::make_shared<spdlog::sinks::null_sink_mt>()));
        return r;
    }
    if (fb)
        free(emuenv->mem, fb);
    if (emuenv->renderer) {
        emuenv->renderer->cleanup();
        snapf("after_vkstate_cleanup");
        emuenv->renderer.reset(); // ~VKState: device + instance (the dzn DLL stays loaded by the loader-less direct ICD path)
        snapf("after_vkstate_destroyed");
    }
    vitox_set_external_presenter(nullptr);
    deinit_mem(emuenv->mem);
    delete emuenv;
    snapf("after_emuenv_destroyed");
    // Vita3K's process-wide VEH captured the freed MemState: re-anchor it (as after M4.1/M5B); one per cycle
    auto *veh_anchor = new MemState;
    if (init(*veh_anchor, false))
        deinit_mem(*veh_anchor);
    r.cleanup = true;
    out("M6C.1 cycle {} end [{}]: renderer_init={} late_init={} mapping_disabled={} bridge={} gpu_sync={} frames_ok={} stalled={} counters_zero={} cleanup={}",
        cycle, vitox_xp_variant_name(variant), r.renderer_init, r.late_init, r.mapping_disabled, r.bridge, r.gpu_sync, r.frames_ok, r.stalled, r.counters_zero,
        r.cleanup);
    spdlog::set_default_logger(std::make_shared<spdlog::logger>("vita3k-m6c-off", std::make_shared<spdlog::sinks::null_sink_mt>()));
    return r;
}
