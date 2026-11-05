// Vitox M6B: direct dzn ICD entry point (see vitox_vk_entry.h).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "vitox_vk_entry.h"

#include <atomic>
#include <mutex>
#include <string>

namespace {

std::once_flag g_once;
PFN_vkGetInstanceProcAddr g_gipa = nullptr;
std::string g_source = "not loaded";
VitoxDznPrewarmApi g_prewarm; // Vitox 0.0.24.173

void load() {
    HMODULE dzn = LoadPackagedLibrary(L"vulkan_dzn.dll", 0);
    if (!dzn) {
        g_source = "vulkan_dzn.dll: LoadPackagedLibrary failed, error " + std::to_string(GetLastError());
        return;
    }
    using PFN_negotiate = VkResult(VKAPI_PTR *)(uint32_t *);
    auto negotiate = reinterpret_cast<PFN_negotiate>(GetProcAddress(dzn, "vk_icdNegotiateLoaderICDInterfaceVersion"));
    uint32_t version = 5;
    const VkResult nr = negotiate ? negotiate(&version) : VK_ERROR_INCOMPATIBLE_DRIVER;
    g_gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(dzn, "vk_icdGetInstanceProcAddr"));
    // Vitox 0.0.24.173: pipeline pre-warm hooks (absent in DLLs before 0.0.24.173)
    g_prewarm.set_variant_callback = reinterpret_cast<decltype(g_prewarm.set_variant_callback)>(GetProcAddress(dzn, "vitox_dzn_set_variant_callback"));
    g_prewarm.variant_key_size = reinterpret_cast<decltype(g_prewarm.variant_key_size)>(GetProcAddress(dzn, "vitox_dzn_variant_key_size"));
    g_prewarm.prebuild_variant = reinterpret_cast<decltype(g_prewarm.prebuild_variant)>(GetProcAddress(dzn, "vitox_dzn_prebuild_variant"));
    g_source = std::string("vulkan_dzn.dll!vk_icdGetInstanceProcAddr (LoadPackagedLibrary; ICD interface v") + std::to_string(version)
        + (nr == VK_SUCCESS ? "" : ", negotiate failed") + (g_gipa ? ")" : "; entry point missing)");
}

} // namespace

PFN_vkGetInstanceProcAddr vitox_vk_get_instance_proc_addr() {
    std::call_once(g_once, load);
    return g_gipa;
}

const char *vitox_vk_proc_addr_source() {
    std::call_once(g_once, load);
    return g_source.c_str();
}

const VitoxDznPrewarmApi &vitox_dzn_prewarm_api() {
    std::call_once(g_once, load);
    return g_prewarm;
}

// Vitox 0.0.28.3
static std::atomic<bool> g_prewarm_enabled{ true };
void vitox_prewarm_set_enabled(bool enabled) { g_prewarm_enabled.store(enabled); }
bool vitox_prewarm_enabled() { return g_prewarm_enabled.load(); }
