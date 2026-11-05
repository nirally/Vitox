// Vitox M6B (0.0.12.0), M6B.2 (0.0.12.4/0.0.12.5: first VISUAL shader draw with the view-instance-mask-fixed dzn) - standalone SHADER PIPELINE proof: Vulkan -> Mesa Dozen (dzn) -> D3D12 -> Xbox GPU.
// No Vita3K renderer code. VISUAL test (run only behind the harness "Please confirm" gate).
//
// Same platform path as M6A (direct dzn ICD; offscreen image exported as a D3D12 resource; host-owned
// D3D12 CoreWindow swapchain copies + presents), now with a REAL graphics pipeline:
//   SPIR-V vertex + fragment shader (HLSL -> SPIR-V, Microsoft DXC 1.9.2609.5; vitox_m6b_shaders.h)
//   -> dzn: SPIR-V -> NIR -> DXIL, validated by the packaged dxil.dll -> D3D12 PSO
//   render pass (clear + draw 3 vertices, RGB triangle) -> TRANSFER_SRC -> host CopyResource -> Present.
// Background colour drifts slowly and a push constant pulses the triangle: continued rendering is visible.
// Also logs, without linking Vita3K, every device capability Vita3K's Vulkan backend checks (M6B parts B/D).
#include "common.h"

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include <d3d12.h>
#include <dxgi1_4.h>

#include <winrt/base.h>
#include <winrt/Windows.System.h>

#include "vitox_m6b_shaders.h"

#include <chrono>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

using winrt::com_ptr;

namespace vitox {

namespace {

void blog(const char *fmt, ...) {
    char buf[1400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    log_line(Log::Core, "m6b %s", buf);
}

const char *vkr(VkResult r) {
    switch (r) {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_INVALID_SHADER_NV: return "VK_ERROR_INVALID_SHADER_NV";
    case VK_ERROR_UNKNOWN: return "VK_ERROR_UNKNOWN";
    default: return "VkResult(other)";
    }
}

#define VKFN(n) PFN_##n n = nullptr;
struct Fns {
    VKFN(vkCreateInstance) VKFN(vkDestroyInstance) VKFN(vkEnumeratePhysicalDevices) VKFN(vkGetPhysicalDeviceProperties2)
    VKFN(vkGetPhysicalDeviceFeatures2) VKFN(vkEnumerateDeviceExtensionProperties) VKFN(vkGetPhysicalDeviceQueueFamilyProperties)
    VKFN(vkGetPhysicalDeviceMemoryProperties) VKFN(vkGetDeviceProcAddr) VKFN(vkCreateDevice)
    VKFN(vkDestroyDevice) VKFN(vkGetDeviceQueue) VKFN(vkCreateImage) VKFN(vkDestroyImage) VKFN(vkGetImageMemoryRequirements)
    VKFN(vkAllocateMemory) VKFN(vkFreeMemory) VKFN(vkBindImageMemory) VKFN(vkCreateImageView) VKFN(vkDestroyImageView)
    VKFN(vkCreateRenderPass) VKFN(vkDestroyRenderPass) VKFN(vkCreateFramebuffer) VKFN(vkDestroyFramebuffer)
    VKFN(vkCreateShaderModule) VKFN(vkDestroyShaderModule) VKFN(vkCreatePipelineLayout) VKFN(vkDestroyPipelineLayout)
    VKFN(vkCreateGraphicsPipelines) VKFN(vkDestroyPipeline) VKFN(vkCreateCommandPool) VKFN(vkDestroyCommandPool)
    VKFN(vkAllocateCommandBuffers) VKFN(vkBeginCommandBuffer) VKFN(vkEndCommandBuffer) VKFN(vkResetCommandBuffer)
    VKFN(vkCmdBeginRenderPass) VKFN(vkCmdEndRenderPass) VKFN(vkCmdBindPipeline) VKFN(vkCmdPushConstants) VKFN(vkCmdDraw)
    VKFN(vkQueueSubmit) VKFN(vkQueueWaitIdle) VKFN(vkDeviceWaitIdle) VKFN(vkGetMemoryWin32HandleKHR)
    VKFN(vkCreateDebugUtilsMessengerEXT) VKFN(vkDestroyDebugUtilsMessengerEXT)
};
#undef VKFN

bool has(const std::vector<VkExtensionProperties> &exts, const char *name) {
    for (const auto &e : exts)
        if (strcmp(e.extensionName, name) == 0)
            return true;
    return false;
}

// M6B.2: VK_EXT_debug_utils capture (Mesa vk_error*() messages carry file:line). Never enable the D3D12 debug layer
// on Xbox: it is process-global and makes every D3D12 device creation fail (0.0.12.1).
unsigned g_msgs = 0;

VKAPI_ATTR VkBool32 VKAPI_CALL messenger_cb(VkDebugUtilsMessageSeverityFlagBitsEXT sev, VkDebugUtilsMessageTypeFlagsEXT type,
    const VkDebugUtilsMessengerCallbackDataEXT *d, void *) {
    const char *s = sev & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT ? "ERROR"
        : sev & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT       ? "WARNING"
        : sev & VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT          ? "INFO"
                                                                      : "VERBOSE";
    if (++g_msgs <= 50)
        blog("debug_utils #%u %s type=0x%X at %s: %s", g_msgs, s, static_cast<unsigned>(type), d && d->pMessageIdName ? d->pMessageIdName : "?",
            d && d->pMessage ? d->pMessage : "");
    return VK_FALSE;
}

double app_mem_mib() {
    return static_cast<double>(winrt::Windows::System::MemoryManager::AppMemoryUsage()) / (1024.0 * 1024.0);
}

} // namespace

std::string probe_m6b(void *core_window, unsigned width, unsigned height, void (*pump)()) {
    using clock = std::chrono::steady_clock;
    blog("begin: M6B.2 first visual shader draw (RGB triangle, SPIR-V via DXC 1.9.2609.5), %ux%u, 35 s", width, height);
    blog("SPIR-V: vs %zu B sha256 %s | ps %zu B sha256 %s", sizeof(kVitoxTriVs), kVitoxTriVsSha256, sizeof(kVitoxTriPs), kVitoxTriPsSha256);
    mem_snapshot("m6b_before_device");

    // ---- 1. dzn ICD, loaded directly from the package (as M6A) ----
    HMODULE dzn = LoadPackagedLibrary(L"vulkan_dzn.dll", 0);
    if (!dzn) {
        blog("LoadPackagedLibrary(vulkan_dzn.dll) FAILED err=%lu", GetLastError());
        return "FAIL load vulkan_dzn.dll";
    }
    using PFN_negotiate = VkResult(VKAPI_PTR *)(uint32_t *);
    auto negotiate = reinterpret_cast<PFN_negotiate>(GetProcAddress(dzn, "vk_icdNegotiateLoaderICDInterfaceVersion"));
    auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(dzn, "vk_icdGetInstanceProcAddr"));
    uint32_t icd_version = 5;
    const VkResult nr = negotiate ? negotiate(&icd_version) : VK_ERROR_INCOMPATIBLE_DRIVER;
    blog("vulkan_dzn.dll (Mesa 26.2.5 + VITOX_UWP patch incl. view-instance-mask tier-0 fix) negotiate=%s v%u vk_icdGetInstanceProcAddr=%s", vkr(nr), icd_version, gipa ? "yes" : "no");
    if (!gipa)
        return "FAIL no vk_icdGetInstanceProcAddr";
    Fns f;
    VkInstance instance = VK_NULL_HANDLE;
    f.vkCreateInstance = reinterpret_cast<PFN_vkCreateInstance>(gipa(nullptr, "vkCreateInstance"));
    VkDebugUtilsMessengerCreateInfoEXT mci{ VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT };
    mci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT
        | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    mci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
        | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    mci.pfnUserCallback = messenger_cb;
    const char *inst_exts[] = { VK_EXT_DEBUG_UTILS_EXTENSION_NAME };
    VkApplicationInfo app{ VK_STRUCTURE_TYPE_APPLICATION_INFO, nullptr, "Vitox M6B.2", 1, "Vitox", 1, VK_API_VERSION_1_1 };
    VkInstanceCreateInfo ici{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, &mci, 0, &app, 0, nullptr, 1, inst_exts };
    VkResult r = f.vkCreateInstance ? f.vkCreateInstance(&ici, nullptr, &instance) : VK_ERROR_INITIALIZATION_FAILED;
    blog("vkCreateInstance(1.1, +VK_EXT_debug_utils) -> %s", vkr(r));
    if (r != VK_SUCCESS)
        return std::string("FAIL vkCreateInstance ") + vkr(r);
#define IFN(n) f.n = reinterpret_cast<PFN_##n>(gipa(instance, #n))
    IFN(vkDestroyInstance); IFN(vkEnumeratePhysicalDevices); IFN(vkGetPhysicalDeviceProperties2); IFN(vkGetPhysicalDeviceFeatures2);
    IFN(vkEnumerateDeviceExtensionProperties); IFN(vkGetPhysicalDeviceQueueFamilyProperties); IFN(vkGetPhysicalDeviceMemoryProperties);
    IFN(vkGetDeviceProcAddr); IFN(vkCreateDevice); IFN(vkCreateDebugUtilsMessengerEXT); IFN(vkDestroyDebugUtilsMessengerEXT);
#undef IFN
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    r = f.vkCreateDebugUtilsMessengerEXT ? f.vkCreateDebugUtilsMessengerEXT(instance, &mci, nullptr, &messenger) : VK_ERROR_EXTENSION_NOT_PRESENT;
    blog("vkCreateDebugUtilsMessengerEXT -> %s", vkr(r));
    uint32_t n = 0;
    f.vkEnumeratePhysicalDevices(instance, &n, nullptr);
    std::vector<VkPhysicalDevice> pdevs(n);
    if (n)
        f.vkEnumeratePhysicalDevices(instance, &n, pdevs.data());
    if (!n)
        return "FAIL no physical device";
    VkPhysicalDevice pdev = pdevs[0];
    VkPhysicalDeviceExternalMemoryHostPropertiesEXT emh{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT };
    VkPhysicalDeviceIDProperties idp{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES, &emh };
    VkPhysicalDeviceProperties2 p2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &idp };
    f.vkGetPhysicalDeviceProperties2(pdev, &p2);
    const auto &pp = p2.properties;
    blog("physical device: \"%s\" api=%u.%u.%u driver=0x%08X (%u devices)", pp.deviceName, VK_API_VERSION_MAJOR(pp.apiVersion),
        VK_API_VERSION_MINOR(pp.apiVersion), VK_API_VERSION_PATCH(pp.apiVersion), pp.driverVersion, n);
    uint32_t ne = 0;
    f.vkEnumerateDeviceExtensionProperties(pdev, nullptr, &ne, nullptr);
    std::vector<VkExtensionProperties> exts(ne);
    f.vkEnumerateDeviceExtensionProperties(pdev, nullptr, &ne, exts.data());

    // ---- 2. Vita3K Vulkan backend capability report (vita3k/renderer/src/vulkan/renderer.cpp create()) ----
    {
        VkPhysicalDeviceBufferDeviceAddressFeatures bda{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES };
        VkPhysicalDeviceUniformBufferStandardLayoutFeatures ubsl{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_UNIFORM_BUFFER_STANDARD_LAYOUT_FEATURES, &bda };
        VkPhysicalDeviceShaderFloat16Int8Features f16{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES, &ubsl };
        VkPhysicalDeviceFeatures2 feats{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f16 };
        f.vkGetPhysicalDeviceFeatures2(pdev, &feats);
        const auto &fb = feats.features;
        const bool req_swapchain = has(exts, "VK_KHR_swapchain"), req_ssbo = has(exts, "VK_KHR_storage_buffer_storage_class"),
                   req_m1 = has(exts, "VK_KHR_maintenance1");
        blog("vita3k required device exts: VK_KHR_swapchain=%d VK_KHR_storage_buffer_storage_class=%d VK_KHR_maintenance1=%d -> %s", req_swapchain,
            req_ssbo, req_m1, req_swapchain && req_ssbo && req_m1 ? "all present" : "MISSING (vita3k device creation would fail)");
        const char *opt[] = { "VK_KHR_get_memory_requirements2", "VK_KHR_dedicated_allocation", "VK_EXT_global_priority", "VK_KHR_image_format_list",
            "VK_KHR_external_memory", "VK_KHR_device_group", "VK_EXT_external_memory_host", "VK_KHR_buffer_device_address",
            "VK_KHR_uniform_buffer_standard_layout", "VK_KHR_shader_float16_int8", "VK_EXT_fragment_shader_interlock",
            "VK_EXT_rasterization_order_attachment_access" };
        std::string o;
        for (const char *e : opt)
            o += std::string(e) + "=" + (has(exts, e) ? "1 " : "0 ");
        blog("vita3k optional device exts: %s", o.c_str());
        blog("vita3k features: bufferDeviceAddress=%u uniformBufferStandardLayout=%u shaderFloat16=%u shaderInt16=%u fillModeNonSolid=%u wideLines=%u "
             "samplerAnisotropy=%u occlusionQueryPrecise=%u fragmentStoresAndAtomics=%u shaderStorageImageExtendedFormats=%u minImportedHostPointerAlignment=%llu",
            bda.bufferDeviceAddress, ubsl.uniformBufferStandardLayout, f16.shaderFloat16, fb.shaderInt16, fb.fillModeNonSolid, fb.wideLines,
            fb.samplerAnisotropy, fb.occlusionQueryPrecise, fb.fragmentStoresAndAtomics, fb.shaderStorageImageExtendedFormats,
            static_cast<unsigned long long>(emh.minImportedHostPointerAlignment));
        // Same decision as renderer.cpp: memory mapping needs BDA ext+feature and standard layout ext+feature;
        // ExternalHost additionally needs VK_EXT_external_memory_host with alignment <= 4096. Default request
        // (config memory-mapping) is "double-buffer".
        const bool mm = has(exts, "VK_KHR_buffer_device_address") && bda.bufferDeviceAddress && has(exts, "VK_KHR_uniform_buffer_standard_layout")
            && ubsl.uniformBufferStandardLayout;
        const bool ext_host = mm && has(exts, "VK_EXT_external_memory_host") && emh.minImportedHostPointerAlignment <= 4096;
        blog("vita3k mapping_method (predicted from renderer.cpp logic): support_memory_mapping=%d supported={Disabled%s%s} -> with default "
             "'double-buffer' request: %s (page_table needed by late_init: no)",
            mm, mm ? ",DoubleBuffer,PageTable" : "", ext_host ? ",ExternalHost" : "", mm ? "DoubleBuffer" : "Disabled");
    }

    // ---- 3. device + queue ----
    uint32_t nq = 0, qfam = 0;
    f.vkGetPhysicalDeviceQueueFamilyProperties(pdev, &nq, nullptr);
    std::vector<VkQueueFamilyProperties> qf(nq);
    f.vkGetPhysicalDeviceQueueFamilyProperties(pdev, &nq, qf.data());
    for (uint32_t i = 0; i < nq; ++i)
        if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            qfam = i;
            break;
        }
    const float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{ VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, nullptr, 0, qfam, 1, &prio };
    const char *dev_exts[] = { VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME };
    VkDeviceCreateInfo dci{ VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, nullptr, 0, 1, &qci, 0, nullptr, 1, dev_exts, nullptr };
    VkDevice dev = VK_NULL_HANDLE;
    r = f.vkCreateDevice(pdev, &dci, nullptr, &dev);
    blog("vkCreateDevice -> %s", vkr(r));
    if (r != VK_SUCCESS)
        return std::string("FAIL vkCreateDevice ") + vkr(r);
#define DFN(n) f.n = reinterpret_cast<PFN_##n>(f.vkGetDeviceProcAddr(dev, #n))
    DFN(vkDestroyDevice); DFN(vkGetDeviceQueue); DFN(vkCreateImage); DFN(vkDestroyImage); DFN(vkGetImageMemoryRequirements); DFN(vkAllocateMemory);
    DFN(vkFreeMemory); DFN(vkBindImageMemory); DFN(vkCreateImageView); DFN(vkDestroyImageView); DFN(vkCreateRenderPass); DFN(vkDestroyRenderPass);
    DFN(vkCreateFramebuffer); DFN(vkDestroyFramebuffer); DFN(vkCreateShaderModule); DFN(vkDestroyShaderModule); DFN(vkCreatePipelineLayout);
    DFN(vkDestroyPipelineLayout); DFN(vkCreateGraphicsPipelines); DFN(vkDestroyPipeline); DFN(vkCreateCommandPool); DFN(vkDestroyCommandPool);
    DFN(vkAllocateCommandBuffers); DFN(vkBeginCommandBuffer); DFN(vkEndCommandBuffer); DFN(vkResetCommandBuffer); DFN(vkCmdBeginRenderPass);
    DFN(vkCmdEndRenderPass); DFN(vkCmdBindPipeline); DFN(vkCmdPushConstants); DFN(vkCmdDraw); DFN(vkQueueSubmit); DFN(vkQueueWaitIdle);
    DFN(vkDeviceWaitIdle); DFN(vkGetMemoryWin32HandleKHR);
#undef DFN
    VkQueue queue = VK_NULL_HANDLE;
    f.vkGetDeviceQueue(dev, qfam, 0, &queue);
    // dzn loads the DXIL validator at instance creation (dxil_create_validator)
    wchar_t dxil_path[MAX_PATH] = {};
    HMODULE dxil = GetModuleHandleW(L"dxil.dll");
    if (dxil)
        GetModuleFileNameW(dxil, dxil_path, MAX_PATH);
    blog("DXIL validator module: %s %s", dxil ? "loaded" : "NOT loaded", dxil ? narrow(dxil_path).c_str() : "");
    mem_snapshot("m6b_after_device");

    // ---- 4. host D3D12 + CoreWindow swapchain (as M6A) ----
    com_ptr<IDXGIFactory4> factory;
    HRESULT hr = CreateDXGIFactory2(0, __uuidof(IDXGIFactory4), factory.put_void());
    com_ptr<IDXGIAdapter1> adapter;
    LUID luid;
    memcpy(&luid, idp.deviceLUID, sizeof(luid));
    if (SUCCEEDED(hr))
        hr = factory->EnumAdapterByLuid(luid, __uuidof(IDXGIAdapter1), adapter.put_void());
    com_ptr<ID3D12Device> d12;
    if (SUCCEEDED(hr))
        hr = D3D12CreateDevice(adapter.get(), D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), d12.put_void());
    if (!d12)
        return "FAIL host D3D12 device";
    D3D12_COMMAND_QUEUE_DESC qd{ D3D12_COMMAND_LIST_TYPE_DIRECT };
    com_ptr<ID3D12CommandQueue> hq;
    d12->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), hq.put_void());
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = width;
    sd.Height = height;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.Scaling = DXGI_SCALING_STRETCH;
    sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    com_ptr<IDXGISwapChain1> sc1;
    hr = factory->CreateSwapChainForCoreWindow(hq.get(), static_cast<::IUnknown *>(core_window), &sd, nullptr, sc1.put());
    blog("host D3D12 device + CoreWindow swapchain %ux%u -> hr=0x%08lX", width, height, static_cast<unsigned long>(hr));
    if (FAILED(hr))
        return "FAIL CreateSwapChainForCoreWindow";
    com_ptr<IDXGISwapChain3> sc = sc1.as<IDXGISwapChain3>();
    com_ptr<ID3D12CommandAllocator> alloc;
    d12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), alloc.put_void());
    com_ptr<ID3D12GraphicsCommandList> cl;
    d12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.get(), nullptr, __uuidof(ID3D12GraphicsCommandList), cl.put_void());
    cl->Close();
    com_ptr<ID3D12Fence> fence;
    d12->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), fence.put_void());
    HANDLE fence_event = CreateEventExW(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
    uint64_t fence_value = 0;
    auto host_wait = [&] {
        hq->Signal(fence.get(), ++fence_value);
        if (fence->GetCompletedValue() < fence_value) {
            fence->SetEventOnCompletion(fence_value, fence_event);
            WaitForSingleObjectEx(fence_event, 5000, FALSE);
        }
    };

    // ---- 5. exportable render target + view ----
    VkPhysicalDeviceMemoryProperties mp{};
    f.vkGetPhysicalDeviceMemoryProperties(pdev, &mp);
    uint32_t local_type = UINT32_MAX;
    VkExternalMemoryImageCreateInfo emi{ VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, nullptr, VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT };
    VkImageCreateInfo ii{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &emi, 0, VK_IMAGE_TYPE_2D, VK_FORMAT_B8G8R8A8_UNORM, { width, height, 1 }, 1, 1,
        VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_SHARING_MODE_EXCLUSIVE,
        0, nullptr, VK_IMAGE_LAYOUT_UNDEFINED };
    VkImage image = VK_NULL_HANDLE;
    r = f.vkCreateImage(dev, &ii, nullptr, &image);
    VkMemoryRequirements mr{};
    f.vkGetImageMemoryRequirements(dev, image, &mr);
    for (uint32_t i = 0; i < mp.memoryTypeCount && local_type == UINT32_MAX; ++i)
        if ((mr.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
            local_type = i;
    VkExportMemoryAllocateInfo emai{ VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, nullptr, VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT };
    VkMemoryDedicatedAllocateInfo dai{ VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, &emai, image, VK_NULL_HANDLE };
    VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &dai, mr.size, local_type };
    VkDeviceMemory image_mem = VK_NULL_HANDLE;
    const VkResult ra = r == VK_SUCCESS ? f.vkAllocateMemory(dev, &mai, nullptr, &image_mem) : r;
    const VkResult rb = ra == VK_SUCCESS ? f.vkBindImageMemory(dev, image, image_mem, 0) : ra;
    VkImageViewCreateInfo vci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, nullptr, 0, image, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_B8G8R8A8_UNORM, {},
        { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    VkImageView view = VK_NULL_HANDLE;
    const VkResult rv = rb == VK_SUCCESS ? f.vkCreateImageView(dev, &vci, nullptr, &view) : rb;
    blog("render target VkImage %ux%u B8G8R8A8 (COLOR_ATTACHMENT|TRANSFER_SRC, export D3D12_RESOURCE) create=%s alloc=%s bind=%s view=%s", width, height, vkr(r),
        vkr(ra), vkr(rb), vkr(rv));
    if (rv != VK_SUCCESS)
        return "FAIL render target";
    HANDLE handle = nullptr;
    VkMemoryGetWin32HandleInfoKHR gh{ VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR, nullptr, image_mem, VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT };
    r = f.vkGetMemoryWin32HandleKHR(dev, &gh, &handle);
    com_ptr<ID3D12Resource> shared;
    hr = r == VK_SUCCESS ? d12->OpenSharedHandle(handle, __uuidof(ID3D12Resource), shared.put_void()) : E_FAIL;
    blog("shared render target: vkGetMemoryWin32HandleKHR=%s -> host OpenSharedHandle hr=0x%08lX", vkr(r), static_cast<unsigned long>(hr));
    if (!shared)
        return "FAIL shared render target";

    // ---- 6. render pass, framebuffer, shaders, pipeline ----
    VkAttachmentDescription att{ 0, VK_FORMAT_B8G8R8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
        VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL };
    VkAttachmentReference ref{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sub{ 0, VK_PIPELINE_BIND_POINT_GRAPHICS, 0, nullptr, 1, &ref, nullptr, nullptr, 0, nullptr };
    // make the colour writes available to the transfer (host copy) after the pass
    VkSubpassDependency dep{ 0, VK_SUBPASS_EXTERNAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, 0 };
    VkRenderPassCreateInfo rpci{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, nullptr, 0, 1, &att, 1, &sub, 1, &dep };
    VkRenderPass rp = VK_NULL_HANDLE;
    const VkResult rrp = f.vkCreateRenderPass(dev, &rpci, nullptr, &rp);
    VkFramebufferCreateInfo fci{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, nullptr, 0, rp, 1, &view, width, height, 1 };
    VkFramebuffer fbo = VK_NULL_HANDLE;
    const VkResult rfb = rrp == VK_SUCCESS ? f.vkCreateFramebuffer(dev, &fci, nullptr, &fbo) : rrp;
    VkShaderModuleCreateInfo smv{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr, 0, sizeof(kVitoxTriVs), kVitoxTriVs };
    VkShaderModuleCreateInfo smp{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr, 0, sizeof(kVitoxTriPs), kVitoxTriPs };
    VkShaderModule vs = VK_NULL_HANDLE, ps = VK_NULL_HANDLE;
    const VkResult rvs = f.vkCreateShaderModule(dev, &smv, nullptr, &vs);
    const VkResult rps = f.vkCreateShaderModule(dev, &smp, nullptr, &ps);
    blog("render pass=%s framebuffer=%s | vkCreateShaderModule vs=%s ps=%s", vkr(rrp), vkr(rfb), vkr(rvs), vkr(rps));
    VkPushConstantRange pcr{ VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(float) };
    VkPipelineLayoutCreateInfo plci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, nullptr, 0, 0, nullptr, 1, &pcr };
    VkPipelineLayout layout = VK_NULL_HANDLE;
    const VkResult rpl = f.vkCreatePipelineLayout(dev, &plci, nullptr, &layout);
    VkPipelineShaderStageCreateInfo stages[2] = {
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "vs_main", nullptr },
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, ps, "ps_main", nullptr },
    };
    VkPipelineVertexInputStateCreateInfo vi{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, nullptr, 0, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkViewport vp{ 0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f };
    VkRect2D sc_rect{ { 0, 0 }, { width, height } };
    VkPipelineViewportStateCreateInfo vps{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, nullptr, 0, 1, &vp, 1, &sc_rect };
    VkPipelineRasterizationStateCreateInfo rs{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb_state{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cb_state.attachmentCount = 1;
    cb_state.pAttachments = &cba;
    VkGraphicsPipelineCreateInfo gp{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    gp.stageCount = 2;
    gp.pStages = stages;
    gp.pVertexInputState = &vi;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vps;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms;
    gp.pColorBlendState = &cb_state;
    gp.layout = layout;
    gp.renderPass = rp;
    VkPipeline pipe = VK_NULL_HANDLE;
    const auto tp = clock::now();
    const VkResult rgp = (rvs == VK_SUCCESS && rps == VK_SUCCESS && rpl == VK_SUCCESS && rfb == VK_SUCCESS)
        ? f.vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gp, nullptr, &pipe)
        : VK_ERROR_INITIALIZATION_FAILED;
    const long long pipe_us = std::chrono::duration_cast<std::chrono::microseconds>(clock::now() - tp).count();
    blog("pipeline layout=%s | vkCreateGraphicsPipelines (dzn: SPIR-V -> NIR -> DXIL -> dxil.dll validation -> D3D12 PSO) -> %s in %lld us", vkr(rpl),
        vkr(rgp), pipe_us);
    if (rgp != VK_SUCCESS)
        return std::string("FAIL graphics pipeline ") + vkr(rgp);
    mem_snapshot("m6b_after_pipeline");

    VkCommandPoolCreateInfo cpi{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, qfam };
    VkCommandPool pool = VK_NULL_HANDLE;
    f.vkCreateCommandPool(dev, &cpi, nullptr, &pool);
    VkCommandBufferAllocateInfo cbai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 };
    VkCommandBuffer cb = VK_NULL_HANDLE;
    f.vkAllocateCommandBuffers(dev, &cbai, &cb);

    // ---- 7. render loop: clear (slow background drift) + draw RGB triangle (pulsing) ----
    const unsigned msgs_before_loop = g_msgs;
    const double mem_loop0 = app_mem_mib();
    double mem_peak = mem_loop0;
    const auto t0 = clock::now();
    uint64_t frames = 0, present_fail = 0, draw_fail = 0;
    HRESULT first_bad = S_OK;
    while (clock::now() - t0 < std::chrono::seconds(35)) {
        pump();
        const float t = std::chrono::duration<float>(clock::now() - t0).count();
        const float bg = 0.5f + 0.5f * std::sin(t * 0.5f);       // ~12.6 s period
        const float pulse = 0.5f + 0.5f * std::sin(t * 1.5708f); // ~4 s period
        VkClearValue clear{};
        clear.color = { { 0.05f + 0.15f * bg, 0.05f, 0.20f - 0.15f * bg, 1.0f } };
        f.vkResetCommandBuffer(cb, 0);
        VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
        f.vkBeginCommandBuffer(cb, &bi);
        VkRenderPassBeginInfo rpbi{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, nullptr, rp, fbo, { { 0, 0 }, { width, height } }, 1, &clear };
        f.vkCmdBeginRenderPass(cb, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
        f.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        f.vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(float), &pulse);
        f.vkCmdDraw(cb, 3, 1, 0, 0);
        f.vkCmdEndRenderPass(cb); // finalLayout TRANSFER_SRC_OPTIMAL
        r = f.vkEndCommandBuffer(cb);
        if (r != VK_SUCCESS) {
            ++draw_fail;
            blog("frame %llu: vkEndCommandBuffer -> %s; stopping", static_cast<unsigned long long>(frames), vkr(r));
            break;
        }
        VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO, nullptr, 0, nullptr, nullptr, 1, &cb, 0, nullptr };
        r = f.vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
        if (r == VK_SUCCESS)
            r = f.vkQueueWaitIdle(queue); // dzn work complete before the host copies the image
        if (r != VK_SUCCESS) {
            ++draw_fail;
            blog("frame %llu: vkQueueSubmit/WaitIdle -> %s; stopping", static_cast<unsigned long long>(frames), vkr(r));
            break;
        }
        com_ptr<ID3D12Resource> back;
        sc->GetBuffer(sc->GetCurrentBackBufferIndex(), __uuidof(ID3D12Resource), back.put_void());
        alloc->Reset();
        cl->Reset(alloc.get(), nullptr);
        D3D12_RESOURCE_BARRIER b{ D3D12_RESOURCE_BARRIER_TYPE_TRANSITION };
        b.Transition = { back.get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST };
        cl->ResourceBarrier(1, &b);
        cl->CopyResource(back.get(), shared.get());
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        cl->ResourceBarrier(1, &b);
        cl->Close();
        ID3D12CommandList *lists[] = { cl.get() };
        hq->ExecuteCommandLists(1, lists);
        hr = sc->Present(1, 0);
        host_wait();
        ++frames;
        if (FAILED(hr)) {
            ++present_fail;
            if (first_bad == S_OK)
                first_bad = hr;
        }
        if (frames % 300 == 1) {
            const double m = app_mem_mib();
            mem_peak = m > mem_peak ? m : mem_peak;
            blog("frame %llu t=%.1fs bg=%.2f pulse=%.2f Present hr=0x%08lX AppMemoryUsage=%.1f MiB debug_utils=%u", static_cast<unsigned long long>(frames),
                t, bg, pulse, static_cast<unsigned long>(hr), m, g_msgs);
        }
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
            break;
    }
    const double secs = std::chrono::duration<double>(clock::now() - t0).count();
    const HRESULT removed = d12->GetDeviceRemovedReason();
    mem_snapshot("m6b_after_render");
    const double fps = frames / (secs > 0 ? secs : 1);
    const double mem_loop1 = app_mem_mib();
    mem_peak = mem_loop1 > mem_peak ? mem_loop1 : mem_peak;
    blog("render end: frames=%llu in %.1f s (%.1f fps) draw_failures=%llu present_failures=%llu first_failure=0x%08lX device_removed_reason=0x%08lX "
         "debug_utils total=%u (during render loop %u) AppMemoryUsage loop start %.1f end %.1f peak(sampled) %.1f MiB",
        static_cast<unsigned long long>(frames), secs, fps, static_cast<unsigned long long>(draw_fail), static_cast<unsigned long long>(present_fail),
        static_cast<unsigned long>(first_bad), static_cast<unsigned long>(removed), g_msgs, g_msgs - msgs_before_loop, mem_loop0, mem_loop1, mem_peak);

    // ---- 8. teardown ----
    f.vkDeviceWaitIdle(dev);
    host_wait();
    f.vkDestroyCommandPool(dev, pool, nullptr);
    f.vkDestroyPipeline(dev, pipe, nullptr);
    f.vkDestroyPipelineLayout(dev, layout, nullptr);
    f.vkDestroyShaderModule(dev, vs, nullptr);
    f.vkDestroyShaderModule(dev, ps, nullptr);
    f.vkDestroyFramebuffer(dev, fbo, nullptr);
    f.vkDestroyRenderPass(dev, rp, nullptr);
    f.vkDestroyImageView(dev, view, nullptr);
    shared = nullptr;
    if (handle)
        CloseHandle(handle);
    f.vkDestroyImage(dev, image, nullptr);
    f.vkFreeMemory(dev, image_mem, nullptr);
    f.vkDestroyDevice(dev, nullptr);
    if (messenger)
        f.vkDestroyDebugUtilsMessengerEXT(instance, messenger, nullptr);
    f.vkDestroyInstance(instance, nullptr);
    cl = nullptr;
    alloc = nullptr;
    sc = nullptr;
    sc1 = nullptr;
    hq = nullptr;
    fence = nullptr;
    CloseHandle(fence_event);
    d12 = nullptr;
    FreeLibrary(dzn);
    mem_snapshot("m6b_after_teardown");
    const bool ok = frames > 0 && secs >= 30.0 && present_fail == 0 && draw_fail == 0 && removed == S_OK;
    blog("end -> %s", ok ? "PASS" : "FAIL");
    char s[200];
    snprintf(s, sizeof(s), "M6B.2 %s (frames=%llu %.0fs %.1f fps present_fail=%llu debug_utils=%u removed=0x%08lX)", ok ? "PASS" : "FAIL",
        static_cast<unsigned long long>(frames), secs, fps, static_cast<unsigned long long>(present_fail), g_msgs, static_cast<unsigned long>(removed));
    return s;
}

} // namespace vitox
