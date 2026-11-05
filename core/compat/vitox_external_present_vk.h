// Vitox M6C: the renderer side of external presentation (VITOX_UWP). Called only from the guarded
// substitutions in renderer/src/vulkan/{renderer,screen_renderer}.cpp, and only when a presenter is registered.
#pragma once

#include <vitox_external_present.h>

#include <vkutil/objects.h>

#include <vector>

namespace renderer::vulkan {
class ScreenRenderer;
}

// Device creation: add the external-memory/semaphore + timeline extensions (fails if one is missing) and chain
// the timeline-semaphore feature. The chain call must come after every StructureChain unlink.
bool vitox_xp_add_device_extensions(vk::PhysicalDevice physical_device, std::vector<const char *> &device_extensions);
void vitox_xp_chain_device_features(vk::Instance instance, vk::PhysicalDevice physical_device, vk::DeviceCreateInfo &info);

// ScreenRenderer: synthesized capabilities, exportable images instead of swapchain images, round-robin acquire,
// submit + present through the host.
vk::SurfaceCapabilitiesKHR vitox_xp_capabilities();
std::vector<vk::Image> vitox_xp_create_images(renderer::vulkan::ScreenRenderer &screen);
void vitox_xp_destroy_images(renderer::vulkan::ScreenRenderer &screen);
bool vitox_xp_acquire(renderer::vulkan::ScreenRenderer &screen);
void vitox_xp_submit_present(renderer::vulkan::ScreenRenderer &screen);
