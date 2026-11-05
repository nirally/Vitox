// Vitox M6A (0.0.11.0) - standalone graphics platform probe: Vulkan -> Mesa Dozen (dzn) -> D3D12 -> Xbox GPU.
// No Vita3K renderer code. VISUAL test (run only behind the harness "Please confirm" gate).
//
// Path:
//   vulkan_dzn.dll (Mesa 26.2.5, built for WindowsStore, packaged) loaded with LoadPackagedLibrary; its
//   ICD entry points are used directly (there is no Vulkan loader on Xbox).
//   dzn renders a full-screen clear into an OFFSCREEN VkImage that is exported as a D3D12 resource
//   (VK_KHR_external_memory_win32, D3D12_RESOURCE handle type). Mesa's Win32 WSI is HWND/DirectComposition
//   only (no CoreWindow), so the host owns presentation: a D3D12 swapchain for the app's CoreWindow, on the
//   adapter dzn reports (deviceLUID), opens the shared resource and copies it to the back buffer.
//   Fallback if export/open fails: CPU readback (vkCmdCopyImageToBuffer -> host upload heap); logged.
// Colours alternate every second (red <-> blue) for 35 s; every frame is presented.
#include "common.h"

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include <d3d12.h>
#include <dxgi1_4.h>

#include <winrt/base.h>

#include <chrono>
#include <cstring>
#include <string>
#include <vector>

using winrt::com_ptr;

namespace vitox {

namespace {

void dlog(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    log_line(Log::Core, "m6a %s", buf);
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
    case VK_ERROR_INVALID_EXTERNAL_HANDLE: return "VK_ERROR_INVALID_EXTERNAL_HANDLE";
    default: return "VkResult(other)";
    }
}

struct Vk {
    PFN_vkGetInstanceProcAddr gipa = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice pdev = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t qfam = 0;
#define VKFN(n) PFN_##n n = nullptr;
    VKFN(vkCreateInstance) VKFN(vkDestroyInstance) VKFN(vkEnumeratePhysicalDevices) VKFN(vkGetPhysicalDeviceProperties2)
    VKFN(vkEnumerateDeviceExtensionProperties) VKFN(vkGetPhysicalDeviceQueueFamilyProperties) VKFN(vkGetPhysicalDeviceMemoryProperties)
    VKFN(vkGetDeviceProcAddr) VKFN(vkCreateDevice) VKFN(vkDestroyDevice) VKFN(vkGetDeviceQueue) VKFN(vkCreateImage) VKFN(vkDestroyImage)
    VKFN(vkGetImageMemoryRequirements) VKFN(vkAllocateMemory) VKFN(vkFreeMemory) VKFN(vkBindImageMemory) VKFN(vkCreateCommandPool)
    VKFN(vkDestroyCommandPool) VKFN(vkAllocateCommandBuffers) VKFN(vkBeginCommandBuffer) VKFN(vkEndCommandBuffer) VKFN(vkCmdPipelineBarrier)
    VKFN(vkCmdClearColorImage) VKFN(vkCmdCopyImageToBuffer) VKFN(vkQueueSubmit) VKFN(vkQueueWaitIdle) VKFN(vkDeviceWaitIdle)
    VKFN(vkResetCommandBuffer) VKFN(vkGetMemoryWin32HandleKHR) VKFN(vkCreateBuffer) VKFN(vkDestroyBuffer)
    VKFN(vkGetBufferMemoryRequirements) VKFN(vkBindBufferMemory) VKFN(vkMapMemory) VKFN(vkUnmapMemory)
#undef VKFN
};

template <typename T>
bool inst_fn(Vk &v, T &fn, const char *name) {
    fn = reinterpret_cast<T>(v.gipa(v.instance, name));
    return fn != nullptr;
}

} // namespace

// Returns a short status for the on-screen/lifecycle summary. `pump` processes window events.
std::string probe_dzn(void *core_window, unsigned width, unsigned height, void (*pump)()) {
    using clock = std::chrono::steady_clock;
    dlog("begin: standalone Vulkan(dzn)->D3D12 present probe, %ux%u, 35 s, red<->blue every 1 s", width, height);
    mem_snapshot("m6a_before_device");

    // ---- 1. dzn ICD, loaded directly from the package ----
    HMODULE dzn = LoadPackagedLibrary(L"vulkan_dzn.dll", 0);
    if (!dzn) {
        dlog("LoadPackagedLibrary(vulkan_dzn.dll) FAILED err=%lu", GetLastError());
        return "FAIL load vulkan_dzn.dll";
    }
    using PFN_negotiate = VkResult(VKAPI_PTR *)(uint32_t *);
    auto negotiate = reinterpret_cast<PFN_negotiate>(GetProcAddress(dzn, "vk_icdNegotiateLoaderICDInterfaceVersion"));
    auto icd_gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(dzn, "vk_icdGetInstanceProcAddr"));
    auto exp_gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(dzn, "vkGetInstanceProcAddr"));
    uint32_t icd_version = 5;
    const VkResult nr = negotiate ? negotiate(&icd_version) : VK_ERROR_INCOMPATIBLE_DRIVER;
    Vk v;
    v.gipa = icd_gipa ? icd_gipa : exp_gipa;
    dlog("vulkan_dzn.dll loaded: vk_icdNegotiateLoaderICDInterfaceVersion=%s (%s, version %u) vk_icdGetInstanceProcAddr=%s vkGetInstanceProcAddr=%s",
        negotiate ? "yes" : "no", vkr(nr), icd_version, icd_gipa ? "yes" : "no", exp_gipa ? "yes" : "no");
    if (!v.gipa)
        return "FAIL no GetInstanceProcAddr";

    // ---- 2. instance + physical device ----
    inst_fn(v, v.vkCreateInstance, "vkCreateInstance");
    VkApplicationInfo app{ VK_STRUCTURE_TYPE_APPLICATION_INFO, nullptr, "Vitox M6A", 1, "Vitox", 1, VK_API_VERSION_1_1 };
    VkInstanceCreateInfo ici{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, nullptr, 0, &app, 0, nullptr, 0, nullptr };
    VkResult r = v.vkCreateInstance ? v.vkCreateInstance(&ici, nullptr, &v.instance) : VK_ERROR_INITIALIZATION_FAILED;
    dlog("vkCreateInstance(apiVersion 1.1, no WSI extensions) -> %s", vkr(r));
    if (r != VK_SUCCESS)
        return std::string("FAIL vkCreateInstance ") + vkr(r);
    inst_fn(v, v.vkDestroyInstance, "vkDestroyInstance");
    inst_fn(v, v.vkEnumeratePhysicalDevices, "vkEnumeratePhysicalDevices");
    inst_fn(v, v.vkGetPhysicalDeviceProperties2, "vkGetPhysicalDeviceProperties2");
    inst_fn(v, v.vkEnumerateDeviceExtensionProperties, "vkEnumerateDeviceExtensionProperties");
    inst_fn(v, v.vkGetPhysicalDeviceQueueFamilyProperties, "vkGetPhysicalDeviceQueueFamilyProperties");
    inst_fn(v, v.vkGetPhysicalDeviceMemoryProperties, "vkGetPhysicalDeviceMemoryProperties");
    inst_fn(v, v.vkGetDeviceProcAddr, "vkGetDeviceProcAddr");
    inst_fn(v, v.vkCreateDevice, "vkCreateDevice");
    uint32_t n = 0;
    v.vkEnumeratePhysicalDevices(v.instance, &n, nullptr);
    std::vector<VkPhysicalDevice> pdevs(n);
    if (n)
        v.vkEnumeratePhysicalDevices(v.instance, &n, pdevs.data());
    dlog("vkEnumeratePhysicalDevices -> %u device(s)", n);
    if (!n)
        return "FAIL no physical device";
    v.pdev = pdevs[0];
    VkPhysicalDeviceIDProperties idp{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
    VkPhysicalDeviceProperties2 p2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &idp };
    v.vkGetPhysicalDeviceProperties2(v.pdev, &p2);
    const auto &pp = p2.properties;
    dlog("physical device: \"%s\" vendor=0x%04X device=0x%04X api=%u.%u.%u driver=0x%08X luid_valid=%u", pp.deviceName, pp.vendorID, pp.deviceID,
        VK_API_VERSION_MAJOR(pp.apiVersion), VK_API_VERSION_MINOR(pp.apiVersion), VK_API_VERSION_PATCH(pp.apiVersion), pp.driverVersion, idp.deviceLUIDValid);
    uint32_t ne = 0;
    v.vkEnumerateDeviceExtensionProperties(v.pdev, nullptr, &ne, nullptr);
    std::vector<VkExtensionProperties> exts(ne);
    v.vkEnumerateDeviceExtensionProperties(v.pdev, nullptr, &ne, exts.data());
    bool has_ext_mem = false;
    for (const auto &e : exts)
        has_ext_mem |= strcmp(e.extensionName, VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME) == 0;
    dlog("device extensions: %u exposed; %s=%s", ne, VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME, has_ext_mem ? "yes" : "no");

    // ---- 3. logical device + graphics queue ----
    uint32_t nq = 0;
    v.vkGetPhysicalDeviceQueueFamilyProperties(v.pdev, &nq, nullptr);
    std::vector<VkQueueFamilyProperties> qf(nq);
    v.vkGetPhysicalDeviceQueueFamilyProperties(v.pdev, &nq, qf.data());
    for (uint32_t i = 0; i < nq; ++i)
        if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            v.qfam = i;
            break;
        }
    const float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{ VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, nullptr, 0, v.qfam, 1, &prio };
    const char *dev_exts[] = { VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME };
    VkDeviceCreateInfo dci{ VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, nullptr, 0, 1, &qci, 0, nullptr, has_ext_mem ? 1u : 0u, dev_exts, nullptr };
    r = v.vkCreateDevice(v.pdev, &dci, nullptr, &v.device);
    dlog("vkCreateDevice(queue family %u of %u, ext_mem=%d) -> %s", v.qfam, nq, has_ext_mem, vkr(r));
    if (r != VK_SUCCESS)
        return std::string("FAIL vkCreateDevice ") + vkr(r);
#define DEVFN(fn) v.fn = reinterpret_cast<PFN_##fn>(v.vkGetDeviceProcAddr(v.device, #fn))
    DEVFN(vkDestroyDevice); DEVFN(vkGetDeviceQueue); DEVFN(vkCreateImage); DEVFN(vkDestroyImage); DEVFN(vkGetImageMemoryRequirements);
    DEVFN(vkAllocateMemory); DEVFN(vkFreeMemory); DEVFN(vkBindImageMemory); DEVFN(vkCreateCommandPool); DEVFN(vkDestroyCommandPool);
    DEVFN(vkAllocateCommandBuffers); DEVFN(vkBeginCommandBuffer); DEVFN(vkEndCommandBuffer); DEVFN(vkCmdPipelineBarrier);
    DEVFN(vkCmdClearColorImage); DEVFN(vkCmdCopyImageToBuffer); DEVFN(vkQueueSubmit); DEVFN(vkQueueWaitIdle); DEVFN(vkDeviceWaitIdle);
    DEVFN(vkResetCommandBuffer); DEVFN(vkGetMemoryWin32HandleKHR); DEVFN(vkCreateBuffer); DEVFN(vkDestroyBuffer);
    DEVFN(vkGetBufferMemoryRequirements); DEVFN(vkBindBufferMemory); DEVFN(vkMapMemory); DEVFN(vkUnmapMemory);
#undef DEVFN
    v.vkGetDeviceQueue(v.device, v.qfam, 0, &v.queue);
    mem_snapshot("m6a_after_device");

    // ---- 4. host D3D12 on the same adapter + CoreWindow swapchain ----
    com_ptr<IDXGIFactory4> factory;
    HRESULT hr = CreateDXGIFactory2(0, __uuidof(IDXGIFactory4), factory.put_void());
    com_ptr<IDXGIAdapter1> adapter;
    if (SUCCEEDED(hr) && idp.deviceLUIDValid) {
        LUID luid;
        memcpy(&luid, idp.deviceLUID, sizeof(luid));
        hr = factory->EnumAdapterByLuid(luid, __uuidof(IDXGIAdapter1), adapter.put_void());
    }
    DXGI_ADAPTER_DESC1 ad{};
    if (adapter)
        adapter->GetDesc1(&ad);
    com_ptr<ID3D12Device> d12;
    if (SUCCEEDED(hr))
        hr = D3D12CreateDevice(adapter.get(), D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), d12.put_void());
    D3D12_FEATURE_DATA_FEATURE_LEVELS fl{};
    const D3D_FEATURE_LEVEL want[] = { D3D_FEATURE_LEVEL_12_2, D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    fl.NumFeatureLevels = ARRAYSIZE(want);
    fl.pFeatureLevelsRequested = want;
    if (d12)
        d12->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &fl, sizeof(fl));
    dlog("host D3D12: adapter by LUID \"%s\" hr=0x%08lX D3D12CreateDevice max_feature_level=0x%04X", narrow(ad.Description).c_str(),
        static_cast<unsigned long>(hr), static_cast<unsigned>(fl.MaxSupportedFeatureLevel));
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
    dlog("host swapchain: CreateSwapChainForCoreWindow %ux%u B8G8R8A8_UNORM 2 buffers FLIP_DISCARD present interval 1 -> hr=0x%08lX", width, height,
        static_cast<unsigned long>(hr));
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

    // ---- 5. dzn offscreen image (exportable), command buffer ----
    VkPhysicalDeviceMemoryProperties mp{};
    v.vkGetPhysicalDeviceMemoryProperties(v.pdev, &mp);
    auto mem_type = [&](uint32_t bits, VkMemoryPropertyFlags want_flags) -> uint32_t {
        for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
            if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want_flags) == want_flags)
                return i;
        return UINT32_MAX;
    };
    VkExternalMemoryImageCreateInfo emi{ VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, nullptr, VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT };
    VkImageCreateInfo ii{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, has_ext_mem ? &emi : nullptr, 0, VK_IMAGE_TYPE_2D, VK_FORMAT_B8G8R8A8_UNORM, { width, height, 1 },
        1, 1, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, nullptr,
        VK_IMAGE_LAYOUT_UNDEFINED };
    VkImage image = VK_NULL_HANDLE;
    r = v.vkCreateImage(v.device, &ii, nullptr, &image);
    VkMemoryRequirements mr{};
    v.vkGetImageMemoryRequirements(v.device, image, &mr);
    VkExportMemoryAllocateInfo emai{ VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, nullptr, VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT };
    VkMemoryDedicatedAllocateInfo dai{ VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, has_ext_mem ? &emai : nullptr, image, VK_NULL_HANDLE };
    VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &dai, mr.size, mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
    VkDeviceMemory image_mem = VK_NULL_HANDLE;
    VkResult ra = v.vkAllocateMemory(v.device, &mai, nullptr, &image_mem);
    VkResult rb = ra == VK_SUCCESS ? v.vkBindImageMemory(v.device, image, image_mem, 0) : ra;
    dlog("offscreen VkImage %ux%u B8G8R8A8 create=%s alloc(%llu B, dedicated, export D3D12_RESOURCE=%d)=%s bind=%s", width, height, vkr(r),
        static_cast<unsigned long long>(mr.size), has_ext_mem, vkr(ra), vkr(rb));
    if (r != VK_SUCCESS || rb != VK_SUCCESS)
        return "FAIL offscreen image";

    // Shared path: export the D3D12 resource handle, open it on the host device.
    com_ptr<ID3D12Resource> shared;
    HANDLE handle = nullptr;
    if (has_ext_mem && v.vkGetMemoryWin32HandleKHR) {
        VkMemoryGetWin32HandleInfoKHR gh{ VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR, nullptr, image_mem, VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT };
        r = v.vkGetMemoryWin32HandleKHR(v.device, &gh, &handle);
        hr = r == VK_SUCCESS ? d12->OpenSharedHandle(handle, __uuidof(ID3D12Resource), shared.put_void()) : E_FAIL;
        dlog("shared path: vkGetMemoryWin32HandleKHR(D3D12_RESOURCE)=%s handle=%p -> host OpenSharedHandle hr=0x%08lX", vkr(r), handle, static_cast<unsigned long>(hr));
    }
    // CPU readback fallback buffer
    VkBuffer rb_buf = VK_NULL_HANDLE;
    VkDeviceMemory rb_mem = VK_NULL_HANDLE;
    com_ptr<ID3D12Resource> upload;
    const uint64_t row = static_cast<uint64_t>(width) * 4, size = row * height;
    if (!shared) {
        VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, nullptr, 0, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, nullptr };
        v.vkCreateBuffer(v.device, &bci, nullptr, &rb_buf);
        VkMemoryRequirements bmr{};
        v.vkGetBufferMemoryRequirements(v.device, rb_buf, &bmr);
        VkMemoryAllocateInfo bmai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, bmr.size,
            mem_type(bmr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
        v.vkAllocateMemory(v.device, &bmai, nullptr, &rb_mem);
        v.vkBindBufferMemory(v.device, rb_buf, rb_mem, 0);
        D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_UPLOAD };
        const UINT pitch = static_cast<UINT>((row + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1));
        D3D12_RESOURCE_DESC rd{ D3D12_RESOURCE_DIMENSION_BUFFER, 0, static_cast<UINT64>(pitch) * height, 1, 1, 1, DXGI_FORMAT_UNKNOWN, { 1, 0 },
            D3D12_TEXTURE_LAYOUT_ROW_MAJOR };
        d12->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, __uuidof(ID3D12Resource), upload.put_void());
        dlog("fallback path: CPU readback (vkCmdCopyImageToBuffer -> host upload heap -> CopyTextureRegion)");
    }
    mem_snapshot("m6a_after_resources");

    VkCommandPoolCreateInfo cpi{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, v.qfam };
    VkCommandPool pool = VK_NULL_HANDLE;
    v.vkCreateCommandPool(v.device, &cpi, nullptr, &pool);
    VkCommandBufferAllocateInfo cbai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 };
    VkCommandBuffer cb = VK_NULL_HANDLE;
    v.vkAllocateCommandBuffers(v.device, &cbai, &cb);

    // ---- 6. render loop ----
    const auto t0 = clock::now();
    uint64_t frames = 0, present_fail = 0, color_switches = 0;
    int last_phase = -1;
    HRESULT first_bad = S_OK;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    while (clock::now() - t0 < std::chrono::seconds(35)) {
        pump();
        const int phase = static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(clock::now() - t0).count() % 2);
        if (phase != last_phase) {
            ++color_switches;
            last_phase = phase;
        }
        const VkClearColorValue color = phase ? VkClearColorValue{ { 0.0f, 0.2f, 1.0f, 1.0f } } : VkClearColorValue{ { 1.0f, 0.1f, 0.0f, 1.0f } };
        // dzn: clear the offscreen image (and read it back on the fallback path)
        v.vkResetCommandBuffer(cb, 0);
        VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
        v.vkBeginCommandBuffer(cb, &bi);
        VkImageSubresourceRange rng{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        VkImageMemoryBarrier to_dst{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr, 0, VK_ACCESS_TRANSFER_WRITE_BIT, layout,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, image, rng };
        v.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &to_dst);
        v.vkCmdClearColorImage(cb, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &rng);
        VkImageMemoryBarrier to_src{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, image, rng };
        v.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &to_src);
        layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        if (!shared) {
            VkBufferImageCopy bic{ 0, 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, 0, 0 }, { width, height, 1 } };
            v.vkCmdCopyImageToBuffer(cb, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb_buf, 1, &bic);
        }
        v.vkEndCommandBuffer(cb);
        VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO, nullptr, 0, nullptr, nullptr, 1, &cb, 0, nullptr };
        r = v.vkQueueSubmit(v.queue, 1, &si, VK_NULL_HANDLE);
        if (r == VK_SUCCESS)
            r = v.vkQueueWaitIdle(v.queue); // dzn work complete before the host reads the image
        if (r != VK_SUCCESS) {
            dlog("frame %llu: vkQueueSubmit/WaitIdle -> %s; stopping", static_cast<unsigned long long>(frames), vkr(r));
            break;
        }
        // host: copy to the back buffer and present
        com_ptr<ID3D12Resource> back;
        sc->GetBuffer(sc->GetCurrentBackBufferIndex(), __uuidof(ID3D12Resource), back.put_void());
        alloc->Reset();
        cl->Reset(alloc.get(), nullptr);
        D3D12_RESOURCE_BARRIER b{ D3D12_RESOURCE_BARRIER_TYPE_TRANSITION };
        b.Transition = { back.get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST };
        cl->ResourceBarrier(1, &b);
        if (shared) {
            cl->CopyResource(back.get(), shared.get());
        } else {
            void *src = nullptr, *dst = nullptr;
            v.vkMapMemory(v.device, rb_mem, 0, size, 0, &src);
            D3D12_RANGE none{ 0, 0 };
            upload->Map(0, &none, &dst);
            const UINT pitch = static_cast<UINT>((row + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1));
            for (unsigned y = 0; y < height; ++y)
                memcpy(static_cast<uint8_t *>(dst) + static_cast<size_t>(y) * pitch, static_cast<const uint8_t *>(src) + y * row, static_cast<size_t>(row));
            upload->Unmap(0, nullptr);
            v.vkUnmapMemory(v.device, rb_mem);
            D3D12_TEXTURE_COPY_LOCATION dl{ back.get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
            dl.SubresourceIndex = 0;
            D3D12_TEXTURE_COPY_LOCATION sl{ upload.get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
            sl.PlacedFootprint = { 0, { DXGI_FORMAT_B8G8R8A8_UNORM, width, height, 1, pitch } };
            cl->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
        }
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
        if (frames % 300 == 1)
            dlog("frame %llu t=%.1fs color=%s Present hr=0x%08lX", static_cast<unsigned long long>(frames),
                std::chrono::duration<double>(clock::now() - t0).count(), phase ? "BLUE" : "RED", static_cast<unsigned long>(hr));
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
            break;
    }
    const double secs = std::chrono::duration<double>(clock::now() - t0).count();
    const HRESULT removed = d12->GetDeviceRemovedReason();
    mem_snapshot("m6a_after_render");
    dlog("render end: frames=%llu in %.1f s (%.1f fps) color_switches=%llu present_failures=%llu first_failure=0x%08lX device_removed_reason=0x%08lX path=%s",
        static_cast<unsigned long long>(frames), secs, frames / (secs > 0 ? secs : 1), static_cast<unsigned long long>(color_switches),
        static_cast<unsigned long long>(present_fail), static_cast<unsigned long>(first_bad), static_cast<unsigned long>(removed),
        shared ? "shared D3D12 resource" : "CPU readback");

    // ---- 7. teardown ----
    v.vkDeviceWaitIdle(v.device);
    host_wait();
    v.vkDestroyCommandPool(v.device, pool, nullptr);
    if (rb_buf)
        v.vkDestroyBuffer(v.device, rb_buf, nullptr);
    if (rb_mem)
        v.vkFreeMemory(v.device, rb_mem, nullptr);
    shared = nullptr;
    if (handle)
        CloseHandle(handle);
    v.vkDestroyImage(v.device, image, nullptr);
    v.vkFreeMemory(v.device, image_mem, nullptr);
    v.vkDestroyDevice(v.device, nullptr);
    v.vkDestroyInstance(v.instance, nullptr);
    upload = nullptr;
    cl = nullptr;
    alloc = nullptr;
    sc = nullptr;
    sc1 = nullptr;
    hq = nullptr;
    fence = nullptr;
    CloseHandle(fence_event);
    d12 = nullptr;
    FreeLibrary(dzn);
    mem_snapshot("m6a_after_teardown");
    const bool ok = frames > 0 && secs >= 30.0 && present_fail == 0 && removed == S_OK && color_switches >= 30;
    dlog("end -> %s", ok ? "PASS" : "FAIL");
    char s[200];
    snprintf(s, sizeof(s), "M6A %s (frames=%llu %.0fs switches=%llu path=%s)", ok ? "PASS" : "FAIL", static_cast<unsigned long long>(frames), secs,
        static_cast<unsigned long long>(color_switches), shared ? "shared" : "readback");
    return s;
}

} // namespace vitox
