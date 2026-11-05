// Vitox M6B: Vulkan entry point for Vita3K's Vulkan backend on WindowsStore/Xbox. There is no Vulkan
// loader (vulkan-1.dll) on Xbox: the packaged Mesa Dozen ICD (vulkan_dzn.dll) is used directly, as
// proven in M6A/M6B. Mirrors the Android/adreno injection Vita3K already has (renderer.cpp).
#pragma once

#include <vulkan/vulkan_core.h>

// vk_icdGetInstanceProcAddr of the packaged dzn ICD (loaded and negotiated once), or nullptr.
PFN_vkGetInstanceProcAddr vitox_vk_get_instance_proc_addr();
// Human-readable source of the pointer above, for logging.
const char *vitox_vk_proc_addr_source();

// Vitox 0.0.24.173 (stutter, option 1): pipeline pre-warm hooks exported by the patched dzn (dzn_pipeline.c). Any member is
// nullptr when the packaged DLL does not export it (older DLL): pre-warm then rebuilds pipelines without their variants.
struct VitoxDznPrewarmApi {
    using VariantCallback = void (*)(uint64_t pipeline, const void *key, uint32_t size);
    void (*set_variant_callback)(VariantCallback cb) = nullptr;
    uint32_t (*variant_key_size)() = nullptr;
    int (*prebuild_variant)(uint64_t pipeline, const void *key, uint32_t size) = nullptr;
};
const VitoxDznPrewarmApi &vitox_dzn_prewarm_api();

// Vitox 0.0.28.3: launcher Settings > Shader Compilation "Pipeline Pre-warm at Launch" (default on). Off: run() still loads the
// recipe store (new pipelines keep being recorded and saved) but builds nothing at boot.
void vitox_prewarm_set_enabled(bool enabled);
bool vitox_prewarm_enabled();
