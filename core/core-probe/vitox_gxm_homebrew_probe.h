// Vitox M7A - FIRST REAL GUEST SceGxm VISUAL HOMEBREW: VITOX0002 through the real Vita3K loader, HLE and renderer,
// presented by the accepted P_Production external presenter.
#pragma once

#include "vitox_core_probe.h" // VitoxCoreHost

#include <vitox_external_present.h>

#include <cstdint>

struct VitoxGxmHomebrewResult {
    bool installed;     // packaged VITOX0002 copied into vita-fs\ux0\app\VITOX0002 (real fs_utils::copy_directory_contents)
    bool sfo;           // real vfs::read_app_file + sfo::load, TITLE_ID == VITOX0002
    bool renderer;      // real renderer::init(Vulkan) + late_init + init(mem), mapping_method Disabled, external presenter bridge
    bool loaded;        // real module_parent load_module("app0:eboot.bin") -> load_self / relocation / imports
    bool imports_bound; // the real loader bound exactly the 14 expected NIDs (M7A.1), all in the selected set
    bool thread;        // real guest main thread created + started, real vblank thread + real render thread started
    bool exited;        // guest called the real sceKernelExitProcess
    bool exit_ok;       // exit code 0
    bool imports_real;  // every invoked NID dispatched to a REAL ImportFn; 0 unknown; missing_nids empty
    bool gxm_calls;     // the guest's SceGxm + SceDisplay calls happened (call counts from the real dispatcher)
    bool frames;        // the renderer presented >= 30 s worth of frames through the external presenter, 0 failures
    bool sentinel;      // ux0:data/VitoxGxm/result.txt written by the guest, FRAMES=2100 and every failure count 0
    bool counters_zero; // swscale-disabled hits 0, SDL_GetTicks calls 0
    bool cleanup;       // render/vblank threads stopped, guest threads deleted, unload_module, ~VKState, ~EmuEnvState
    bool root_safety;   // the sentinel dir is removed again; the app install is kept
    uint64_t presents;  // external presenter frames
    double present_seconds;
    uint64_t setframebuf_calls, fill_calls, vblank_calls; // VITOX0003: fill_calls = guest sceGxmDraw calls
    // M7B Phase B (VITOX0003): GPU-side evidence from the counting hooks + Vita3K's own log
    bool gpu;                   // vkCmdDrawIndexed > 0, pipelines created > 0, 0 pipeline / shader-module failures, 0 vita3k errors
    uint64_t gpu_draws, pipelines, shader_modules, vita3k_errors;
};

// headless = the non-visual precheck: the host presenter copies into an offscreen texture and never Presents.
// homebrew: 2 = VITOX0002 (M7A transfer fills), 3 = VITOX0003 (M7B the first real GXM draw)
extern "C" VitoxGxmHomebrewResult vitox_gxm_homebrew_probe_run(const VitoxCoreHost *host, vitox::ExternalPresenter *presenter, void (*pump)(),
    const wchar_t *install_src, const wchar_t *local_state, const wchar_t *dev_root, const wchar_t *static_assets, unsigned timeout_s, unsigned homebrew);
