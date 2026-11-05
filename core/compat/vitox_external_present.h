// Vitox M6C: external presentation for Vita3K's Vulkan renderer on Xbox (VITOX_UWP only).
//
// There is no VkSurfaceKHR / VkSwapchainKHR on Xbox UWP (dzn has no CoreWindow WSI). Instead the renderer's
// ScreenRenderer renders into N exportable VkImages (D3D12_RESOURCE handles) and signals an exportable timeline
// semaphore (a shared D3D12 fence); the host (the Vitox app) opens both on the same adapter, makes its D3D12
// queue Wait() for the frame's value, copies the image into its CoreWindow back buffer and presents.
// The host registers its presenter once, before renderer::init; nothing changes when none is registered.
#pragma once

#include <cstdint>

namespace vitox {

class ExternalPresenter {
public:
    virtual ~ExternalPresenter() = default;

    virtual uint32_t width() const = 0;
    virtual uint32_t height() const = 0;
    virtual uint32_t image_count() const = 0;

    // Adapter LUID of the Vulkan physical device (VkPhysicalDeviceIDProperties::deviceLUID); the host opens
    // every shared handle on this adapter. Called once, before any import.
    virtual bool bind(const uint8_t luid[8]) = 0;
    // NT handle of image `index`'s D3D12 resource (VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT). The host
    // takes its own reference; the renderer closes the handle afterwards.
    virtual bool import_image(uint32_t index, void *nt_handle) = 0;
    // NT handle of the shared D3D12 fence behind the exported timeline semaphore
    // (VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT). False -> the renderer uses the labelled CPU-wait fallback.
    virtual bool import_fence(void *nt_handle) = 0;
    // Waits up to `timeout_ms` for the host's last use of image `index` to complete (the image may be rendered again).
    virtual bool wait_image_free(uint32_t index, uint32_t timeout_ms) = 0;
    // M6C.1: log the host half of a stall snapshot (fence values, expected values, back buffer, HRESULTs).
    virtual void snapshot(uint32_t id, const char *reason) = 0;
    // Present image `index` once the timeline reaches `value` (GPU wait on the host queue). In the CPU-wait
    // fallback the renderer has already waited and passes value 0.
    virtual bool present(uint32_t index, uint64_t value) = 0;
    // Drop every imported image/fence (host idle first). Called before the renderer frees them.
    virtual void release() = 0;
    // 0.0.24.198: draw + present one loading-screen frame while the boot pipeline pre-warm runs (no game frame exists
    // yet). `title` is ASCII (may be empty). False = not supported (headless / no overlay pipeline): nothing presented.
    virtual bool present_loading(uint32_t done, uint32_t total, const char *title) { (void)done; (void)total; (void)title; return false; }
    // 0.0.25.0: the Vitox quick-menu overlay (R3). While it is visible and the guest is paused (no game presents),
    // the launcher thread calls this ~every 2 ms: the presenter re-presents the last game frame with the overlay on
    // top when no game frame came for a while (paced by the flip queue). False = nothing presented.
    virtual bool present_overlay() { return false; }
};

} // namespace vitox

// M6C.1 control variants (renderer side; the host presenter is told the same variant by the probe).
enum class VitoxXpVariant : uint32_t {
    A_GpuWait = 0, // unchanged: host queue Wait(shared fence, v) -> copy -> Present -> Signal
    B_CpuWait = 1, // LABELLED CPU WAIT: vkWaitSemaphores(v) on the CPU, then present(index, 0) (no host queue Wait)
    C_Paced = 2,   // A, but the host waits for the previous frame's completion before the next Present
    D_AsyncOff = 3, // A with Vita3K async pipeline compilation disabled
    // M6C.2 production default: GPU-GPU, CoreWindow frame-latency waitable (MaxFrameLatency 1), image reuse gated on
    // the host copy fence, labelled watchdog -> CPU-wait fallback (counted; acceptance needs 0)
    P_Production = 4
};
const char *vitox_xp_variant_name(VitoxXpVariant v);

void vitox_set_external_presenter(vitox::ExternalPresenter *presenter);
void vitox_xp_set_variant(VitoxXpVariant v);
vitox::ExternalPresenter *vitox_external_presenter();
// 0.0.24.198 pre-warm loading screen: the launcher sets the title (ASCII) and its window pump once; the pre-warm then
// calls vitox_loading_progress() from the launcher thread (~30 Hz) while its workers compile.
void vitox_loading_configure(const char *title, void (*pump)());
void vitox_loading_progress(uint32_t done, uint32_t total);

// Counters for the probe/report (vitox_external_present_vk.cpp)
struct VitoxExternalPresentStats {
    uint32_t images;            // exportable images created
    uint32_t image_handles_ok;  // images whose D3D12_RESOURCE handle the host imported
    bool gpu_sync;              // timeline semaphore exported + imported by the host as a D3D12 fence
    uint64_t acquires;          // round-robin acquisitions
    uint64_t submits_ok, submits_failed;
    uint64_t presents_ok, presents_failed;
    uint64_t cpu_wait_fallback; // frames presented through the labelled CPU-wait fallback
    uint64_t last_value;        // last timeline value signalled
    double cpu_wait_fallback_ms;
    double cpu_wait_fallback_max_ms;
    // M6C.1
    uint32_t semaphores_created; // timeline semaphores created over the process lifetime (one per ScreenRenderer image set)
    uint32_t stalls;             // acquisitions whose host wait exceeded 100 ms
    uint32_t stall_unresolved;   // acquisitions that gave up (3 s)
    uint64_t first_stall_frame;  // frame number (acquire count) of the first stall, 0 = none
    uint64_t other_submits;      // vkQueueSubmit calls NOT made by the external presenter (counting dispatcher hook)
    uint64_t value_order_errors; // signalled timeline values that were not strictly increasing
    uint64_t completed_value;    // vkGetSemaphoreCounterValue at the time of the stats call (0 = no timeline)
    // M7B Phase B: counting hooks on the default dispatcher (everything Vita3K does through vulkan-hpp)
    uint64_t cmd_draw_indexed;   // vkCmdDrawIndexed (Vita3K's GXM draws: renderer/src/vulkan/scene.cpp)
    uint64_t cmd_draw;           // vkCmdDraw (screen filter / overlay)
    uint64_t pipelines_created;  // vkCreateGraphicsPipelines pipelines that returned VK_SUCCESS
    uint64_t pipeline_failures;  // vkCreateGraphicsPipelines calls that did not
    uint64_t shader_modules;     // vkCreateShaderModule successes (SHA-256 of each module's code is logged)
    uint64_t shader_module_failures;
    // M7B visual (diagnostic only): vkCmdDrawIndexed total seen at each presenter submit
    uint64_t presents_without_new_draw; // presenter submits with no vkCmdDrawIndexed recorded since the previous one
    uint64_t first_no_draw_frame, last_no_draw_frame; // frame numbers of the first/last such submit (0 = none)
    uint64_t draws_at_last_present;     // vkCmdDrawIndexed total at the last presenter submit
    uint64_t last_present_frame;        // frame number of the last presenter submit
};
VitoxExternalPresentStats vitox_external_present_stats();
