// Vitox M8A - USER-SELECTABLE HOMEBREW LOADER. A homebrew is selected at RUN time from an external, user-managed root
// (no app is compiled into the package or named in the binary) and booted through the real Vita3K loader / HLE /
// renderer, presented by the accepted P_Production external presenter.
//
//   root:      <dev_root>\homebrew\<folder>\{eboot.bin, sce_sys\param.sfo, ...}   (D:\DevelopmentFiles\Vitox\homebrew)
//   config:    <dev_root>\homebrew\vitox-launch.txt   key=value lines: app=<folder or TITLE_ID>, presenter=visual|headless,
//              timeout_s=<5..600>. No app= and exactly one valid candidate -> that one; anything else fails closed.
//   install:   real fs_utils::copy_directory_contents(<root>\<folder> -> vita-fs\ux0\app\<TITLE_ID from param.sfo>)
//   imports:   every NID the real loader binds is inventoried; an import outside the selected REAL dispatcher stops the
//              launch before the guest runs (classified against Vita3K's own export table).
#pragma once

#include "vitox_core_probe.h" // VitoxCoreHost

#include <vitox_external_present.h>

#include <cstdint>

struct VitoxLaunchConfig {
    bool found;        // vitox-launch.txt present and readable
    bool headless;     // presenter=headless (non-visual precheck); default visual
    unsigned timeout_s; // guest run limit; default 60
    bool trap;         // M8B.1 unsupported_imports=trap: runtime-exact discovery (DIAGNOSTIC only, never an acceptance run)
    bool fail_closed;  // M8E unsupported_imports=fail-closed: the guest runs; a CALL to an unselected NID stops it (same
                       // trap mechanism, nothing is returned) and fails the run - an acceptance mode, not discovery
    bool staged_pgf;   // M8C.3 firmware=staging-pgf: firmware devices from firmware-staging\vita-fs (read-only), MANUAL
                       // modules mode with lle-modules=[libpgf], REAL decrypt_fself; ux0 stays the active vita-fs
    char app[64];      // app= value (folder name or TITLE_ID), empty = auto (single candidate only)
    char app_root[16];    // M9A.2A app_root=homebrew|app-staging: the discovery root under <dev_root> (default homebrew);
                          // vitox-launch.txt and ime_input stay in <dev_root>\homebrew
    char lle_modules[64]; // M9A.2A lle_modules=<comma list from {libc, libfios2}>: Vita3K MANUAL lle-modules; upstream's
                          // load_app_impl() preload then loads those found in app0:sce_module after the main module
    char ime_input[64]; // M8F.2A ime_input=<file in the homebrew root>: DIAGNOSTIC scripted IME dialog input, one line per
                        // dialog session, actions split by '|': "enter:<text>" (ime_commit_text + submit_current_ime) or
                        // "cancel" (dismiss_current_ime) - Vita3K's frontend-neutral helpers, the SDL frontend's calls
    bool licensed_load; // M9A.3A licensed_load=staged-license (default off): only with app_root=app-staging,
                        // unsupported_imports=fail-closed and presenter=headless (else a config error). This build only
                        // VALIDATES the app + its staged license and then STOPs: protected SELF execution is not supported
    bool sync_pipelines; // M9A.6 async_pipelines=off: upstream's async-pipeline-compilation=false (default: config value, on)
    // 0.0.24.60 (M9A.6 DIAGNOSTIC) auto_touch=<x>,<y>,<from_s>,<to_s>: scripted front-panel taps at Vita pixel (x, y), a
    // 150 ms tap every second between from_s and to_s after the guest starts, through upstream's mouse-as-touch input -
    // reproduces a touch-gated scene without a person at the console (the L3 cursor takes over while it is on)
    // 0.0.24.74 (M9A.6 DIAGNOSTIC) auto_stick=<from_s>,<to_s>,<lx>,<ly>[;...] (up to 6 segments): scripted left-stick input on
    // pad slot 0 during each window (seconds after the guest starts, -1..1, SDL convention +y = down), layered over the real
    // controller through a wrapping VitoxHostInput - reaches gameplay-only states (Gravity Rush driver crash) unattended
    char auto_stick[128];
    // 0.0.24.76: several segments "x,y,from,to[,period_ms];..." (up to 6), e.g. the apple, then the comic dialogs
    char auto_touch[192]; // empty = off; each tap is down for 45% of its period (default 1000 ms), at most 150 ms
    char error[128];   // config problem (unknown key, bad value) - the launch fails closed when set
    // 0.0.25.0 (Vitox launcher UI)
    bool ui_direct;     // ui=direct: a visual run boots app= at once (harness runs); default ui=launcher: the library UI
                        // opens and app= only preselects. Headless runs and the diagnostic keys (auto_touch/auto_stick/
                        // ime_input) always boot directly.
    bool no_timeout;    // set by the library UI for a user session: the guest runs until it exits or the user quits
    bool lle_set;       // lle_modules came from the config or a profile (else the library derives it from sce_module)
    char ui_script[64]; // ui_script=<file in the homebrew root>: DIAGNOSTIC scripted pad buttons ("<ms> <buttons>" lines)
    // 0.0.29.3: a LoadExec relaunch (upstream AppLaunchRequest::self_path / argv): the main module is app0:<exec_self>
    // instead of app0:eboot.bin, and the main thread gets exec_args ('\n'-separated) as its argument block
    char exec_self[64];
    char exec_args[256];
};

// 0.0.25.0: one launchable app found by the same discovery the launcher uses (param.sfo parsed generically, eboot.bin
// present, TITLE_ID valid), in either root. Strings are UTF-8 / wide paths.
struct VitoxAppInfo {
    char title_id[16];
    char title[128];
    char folder[128];
    char app_root[16]; // homebrew | app-staging
    char app_ver[16];
    char category[8];  // param.sfo CATEGORY (gd = game, ...)
    wchar_t dir[260];  // <dev_root>\<app_root>\<folder>
};

// 0.0.25.0: every valid app under <dev_root>\homebrew and <dev_root>\app-staging (no side effects). Returns the count
// (at most max are written), sorted by title.
extern "C" unsigned vitox_launcher_discover(const wchar_t *dev_root, VitoxAppInfo *out, unsigned max);
// 0.0.25.0: the launch config for one app picked in the library UI: the global keys of vitox-launch.txt, then the app's
// [TITLE_ID] section of <dev_root>\homebrew\vitox-profiles.txt, then app/app_root/visual/no timeout. lle_modules set
// nowhere is derived from the app's sce_module folder (the launcher allowlist).
extern "C" VitoxLaunchConfig vitox_launcher_config_for(const wchar_t *dev_root, const VitoxAppInfo *app);

struct VitoxLaunchResult {
    bool discovered;    // the root exists and >= 1 valid candidate (param.sfo parsed generically, eboot.bin present)
    bool selected;      // exactly one candidate matched the config (or the single-candidate rule), no ambiguity
    bool installed;     // copied into ux0:app/<TITLE_ID>; every source file present with the same size, eboot/sfo byte-equal
    bool sfo;           // the real vfs::read_app_file + sfo::load of the installed app gives the same TITLE_ID
    bool renderer;      // real renderer::init(Vulkan) + late_init + init(mem), external presenter bridge
    bool loaded;        // real load_module("app0:eboot.bin")
    bool imports_supported; // every bound NID is in the selected REAL dispatcher (else STOP before the guest runs)
    bool thread;        // guest main thread + vblank thread + render thread started
    bool exited;        // guest called sceKernelExitProcess
    bool exit_ok;       // ... with exit code 0
    bool timed_out;     // still running at timeout_s (stopped by the host)
    bool user_quit;     // 0.0.25.0: stopped by the host because the user chose Return to Launcher in the quick menu
    bool trapped;       // M8B.1: the guest called an unselected NID and was stopped (run = FAIL / NEEDS_CLOSURE)
    unsigned traps;
    bool unknown_zero;  // 0 invoked NIDs without a REAL ImportFn, emuenv.missing_nids empty
    bool cleanup;       // render/vblank threads stopped, guest threads deleted, unload_module, ~VKState, ~EmuEnvState
    bool root_safety;   // no other ux0:app entry changed, no ux0:data entry removed, the homebrew root untouched
    bool root_safety_staging; // M8C.3: the staged firmware tree is unchanged (path + size) after a firmware=staging-pgf run
    bool content_checked;  // M9A.2C app_root=app-staging: eboot.bin + every sce_module SELF checked before anything is installed
    bool content_readable; // ... and all of them are directly loader-readable (plain ELF / SELF with plain segments); when
                           // not, the launch fails closed (no supported licensed-load mode in this build), no guest code
    unsigned content_protected; // number of required executables that are protected (not loader-readable)
    bool licensed_requested; // M9A.3A: licensed_load=staged-license was in effect for this run
    bool licensed_valid;     // ... app identity + staged license discovery/structure/identity all passed
    bool staging_unchanged;  // ... the app-staging tree is identical (path + size) after the validation
    char stop_reason[200];   // M9A.3A: why the run stopped before guest code (always set when licensed_requested)
    char title_id[16], title[128], folder[128];
    unsigned candidates, rejected, bound_imports, unsupported_imports;
    uint64_t presents, gpu_draws, pipelines, pipeline_failures, vita3k_errors, guest_calls;
    long pso_failures;  // dzn diagnostic counter (draws without a D3D12 PSO)
    double run_seconds, present_seconds;
    // 0.0.29.3: the guest asked for a LoadExec relaunch (sceAppMgrLoadExec -> upstream request_process_exit with an
    // AppLaunchRequest): the host runs the same app again with config exec_self / exec_args set from these
    bool load_exec;
    char exec_self[64];
    char exec_args[256];
};

// Reads <dev_root>\homebrew\vitox-launch.txt (no side effects). Called by the host first: the presenter mode decides
// whether the CoreWindow is yielded.
extern "C" VitoxLaunchConfig vitox_launcher_read_config(const wchar_t *dev_root);

extern "C" VitoxLaunchResult vitox_homebrew_launch_run(const VitoxCoreHost *host, vitox::ExternalPresenter *presenter, void (*pump)(),
    const VitoxLaunchConfig *config, const wchar_t *local_state, const wchar_t *dev_root, const wchar_t *static_assets);

// 0.0.27.0: internal resolution upscaling (launcher Settings > Graphics). The NEXT game session renders at
// (960 x 544) * multiplier: Vita3K's own res_multiplier, applied where upstream applies it (apply_renderer_config, after
// renderer init and before any GXM context / surface exists), so every render target, colour/depth surface, viewport and
// scissor is allocated and rasterised at the scaled size; the screen renderer then scales the result to the output size.
// A running session keeps the multiplier it started with (upstream: restart-required setting). Values are snapped to
// 0.5 steps and clamped to [1, 8].
extern "C" void vitox_launcher_set_resolution_multiplier(float multiplier);
extern "C" float vitox_launcher_resolution_multiplier();
// 0.0.28.3: launcher Settings > Shader Compilation (both default on, applied at the next game start).
// async: Vita3K's async-pipeline-compilation. A library session uses this value alone; a direct (harness) run also honours
// vitox-launch.txt async_pipelines=off. prewarm: the boot pipeline pre-warm (vitox_pipeline_prewarm.h) and its loading screen.
extern "C" void vitox_launcher_set_pipeline_options(bool async, bool prewarm);
extern "C" bool vitox_launcher_async_pipelines();
extern "C" bool vitox_launcher_pipeline_prewarm();
