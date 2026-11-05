// Vitox M6C - first REAL Vita3K Vulkan renderer init on Xbox + its display path through the external presenter.
#pragma once

#include "vitox_core_probe.h" // VitoxCoreHost

#include <vitox_external_present.h> // VitoxXpVariant, vitox::ExternalPresenter

#include <cstdint>

namespace vitox {
class ExternalPresenter;
}

struct VitoxRendererResult {
    bool renderer_init;   // real renderer::init(Backend::Vulkan) -> vulkan::create (direct dzn ICD) -> ScreenRenderer::setup
    bool late_init;       // real VKState::late_init against the EmuEnvState's real MemState
    bool mapping_disabled; // mapping_method == Disabled (no bufferDeviceAddress on dzn/Xbox)
    bool bridge;          // N exportable images shared with the host + the GPU sync primitive
    bool gpu_sync;        // timeline semaphore exported as a shared D3D12 fence (false = labelled CPU-wait fallback)
    bool frames_ok;       // every submit + present succeeded for the whole run, no stall
    bool stalled;         // M6C.1: at least one host wait > 100 ms
    bool wedged;          // M6C.1: the stall never resolved and the Vulkan timeline is behind - teardown skipped (leaked)
    bool counters_zero;   // swscale-disabled hits == 0, SDL_GetTicks unexpected use == 0
    bool cleanup;         // VKState::cleanup + ~EmuEnvState
    uint64_t frames;
    double seconds;
};

// static_assets: directory holding shaders-builtin/ (the package install dir), as Vita3K's static_assets_path
VitoxRendererResult vitox_renderer_probe_run(const VitoxCoreHost *host, vitox::ExternalPresenter *presenter, void (*pump)(), const wchar_t *local_state,
    const wchar_t *dev_root, const wchar_t *static_assets, unsigned seconds, VitoxXpVariant variant, unsigned cycle);
