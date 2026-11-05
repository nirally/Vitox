// Vitox M6B.1 (0.0.12.1, 0.0.12.2, 0.0.12.3) - dzn DRAW FAILURE DIAGNOSTIC. NON-VISUAL: log/return-code determined, no present.
//
// 0.0.12.0 compiled the pipeline but its first draw submit returned VK_ERROR_OUT_OF_DEVICE_MEMORY, and the
// host D3D12 device (same adapter) then reported GetDeviceRemovedReason = DXGI_ERROR_INVALID_CALL.
// This probe pins the failure:
//   - VK_EXT_debug_utils messenger from vkCreateInstance onward (Mesa vk_error*() -> message + "file:line")
//   - CRT stderr -> LocalState\m6b1_stderr.txt before dzn loads (Mesa's mesa_log file sink; dzn shares the UCRT)
//   - D3D12 debug layer + DRED requested before any D3D12 device exists (logged whether available)
//   - the packaged vulkan_dzn.dll carries VITOX_UWP logging-only instrumentation (HRESULTs of the D3D12 calls
//     behind dzn's generic VkResults; vitox-uwp-mesa-26.2.5.patch)
//   - raw D3D12 capability report + a SetViewInstanceMask/Close check (dzn calls it on every draw)
//   - bisect A..F, each case with its own VkInstance/VkDevice/resources so a removed device cannot poison
//     later cases; every Vulkan return code logged; after a successful submit a 2-pixel readback proves what
//     the GPU actually wrote.
#include "common.h"

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include <d3d12.h>
#include <dxgi1_4.h>

#include <winrt/base.h>
#include <winrt/Windows.System.h>

#include "vitox_m6b_shaders.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

using winrt::com_ptr;

namespace vitox {

namespace {

void blog(const char *fmt, ...) {
    char buf[2000];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    log_line(Log::Core, "m6b1 %s", buf);
}

const char *vkr(VkResult r) {
    switch (r) {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_NOT_READY: return "VK_NOT_READY";
    case VK_TIMEOUT: return "VK_TIMEOUT";
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

double app_mem_mib() {
    return static_cast<double>(winrt::Windows::System::MemoryManager::AppMemoryUsage()) / (1024.0 * 1024.0);
}

// ---- debug-utils capture (shared by all cases; `g_case` tags each message) ----
const char *g_case = "init";
unsigned g_msgs = 0;

VKAPI_ATTR VkBool32 VKAPI_CALL messenger_cb(VkDebugUtilsMessageSeverityFlagBitsEXT sev, VkDebugUtilsMessageTypeFlagsEXT type,
    const VkDebugUtilsMessengerCallbackDataEXT *d, void *) {
    const char *s = sev & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT ? "ERROR"
        : sev & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT       ? "WARNING"
        : sev & VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT          ? "INFO"
                                                                      : "VERBOSE";
    ++g_msgs;
    blog("[%s] debug_utils %s type=0x%X at %s: %s", g_case, s, static_cast<unsigned>(type), d && d->pMessageIdName ? d->pMessageIdName : "?",
        d && d->pMessage ? d->pMessage : "");
    return VK_FALSE;
}

#define VKFN(n) PFN_##n n = nullptr;
struct Fns {
    VKFN(vkCreateInstance) VKFN(vkDestroyInstance) VKFN(vkEnumeratePhysicalDevices) VKFN(vkGetPhysicalDeviceProperties2)
    VKFN(vkGetPhysicalDeviceQueueFamilyProperties) VKFN(vkGetPhysicalDeviceMemoryProperties) VKFN(vkGetDeviceProcAddr)
    VKFN(vkCreateDevice) VKFN(vkCreateDebugUtilsMessengerEXT) VKFN(vkDestroyDebugUtilsMessengerEXT)
    VKFN(vkDestroyDevice) VKFN(vkGetDeviceQueue) VKFN(vkCreateImage) VKFN(vkDestroyImage) VKFN(vkGetImageMemoryRequirements)
    VKFN(vkCreateBuffer) VKFN(vkDestroyBuffer) VKFN(vkGetBufferMemoryRequirements) VKFN(vkBindBufferMemory)
    VKFN(vkMapMemory) VKFN(vkUnmapMemory)
    VKFN(vkAllocateMemory) VKFN(vkFreeMemory) VKFN(vkBindImageMemory) VKFN(vkCreateImageView) VKFN(vkDestroyImageView)
    VKFN(vkCreateRenderPass) VKFN(vkDestroyRenderPass) VKFN(vkCreateFramebuffer) VKFN(vkDestroyFramebuffer)
    VKFN(vkCreateShaderModule) VKFN(vkDestroyShaderModule) VKFN(vkCreatePipelineLayout) VKFN(vkDestroyPipelineLayout)
    VKFN(vkCreateGraphicsPipelines) VKFN(vkDestroyPipeline) VKFN(vkCreateCommandPool) VKFN(vkDestroyCommandPool)
    VKFN(vkAllocateCommandBuffers) VKFN(vkBeginCommandBuffer) VKFN(vkEndCommandBuffer)
    VKFN(vkCmdBeginRenderPass) VKFN(vkCmdEndRenderPass) VKFN(vkCmdBindPipeline) VKFN(vkCmdPushConstants) VKFN(vkCmdDraw)
    VKFN(vkCmdCopyImageToBuffer)
    VKFN(vkQueueSubmit) VKFN(vkQueueWaitIdle) VKFN(vkDeviceWaitIdle) VKFN(vkGetMemoryWin32HandleKHR)
};
#undef VKFN

enum class Kind { ClearOnly, BindNoDraw, Draw };

struct Case {
    const char *id;
    const char *what;
    Kind kind;
    bool push_constants; // pipeline with a fragment push-constant range + vkCmdPushConstants (the 0.0.12.0 path)
    bool exported;       // VkImage + memory exportable as D3D12_RESOURCE (the M6A/M6B sharing path)
    unsigned w, h;
};

struct CaseResult {
    std::string summary;
    bool ok = false;
};

LUID g_luid{};
bool g_have_luid = false;

std::string hr_hex(HRESULT hr) {
    char b[16];
    snprintf(b, sizeof(b), "0x%08lX", static_cast<unsigned long>(hr));
    return b;
}

const char *dred_op(D3D12_AUTO_BREADCRUMB_OP op) {
    switch (op) {
    case D3D12_AUTO_BREADCRUMB_OP_SETMARKER: return "SetMarker";
    case D3D12_AUTO_BREADCRUMB_OP_BEGINEVENT: return "BeginEvent";
    case D3D12_AUTO_BREADCRUMB_OP_ENDEVENT: return "EndEvent";
    case D3D12_AUTO_BREADCRUMB_OP_DRAWINSTANCED: return "DrawInstanced";
    case D3D12_AUTO_BREADCRUMB_OP_DRAWINDEXEDINSTANCED: return "DrawIndexedInstanced";
    case D3D12_AUTO_BREADCRUMB_OP_EXECUTEINDIRECT: return "ExecuteIndirect";
    case D3D12_AUTO_BREADCRUMB_OP_DISPATCH: return "Dispatch";
    case D3D12_AUTO_BREADCRUMB_OP_COPYBUFFERREGION: return "CopyBufferRegion";
    case D3D12_AUTO_BREADCRUMB_OP_COPYTEXTUREREGION: return "CopyTextureRegion";
    case D3D12_AUTO_BREADCRUMB_OP_COPYRESOURCE: return "CopyResource";
    case D3D12_AUTO_BREADCRUMB_OP_RESOLVESUBRESOURCE: return "ResolveSubresource";
    case D3D12_AUTO_BREADCRUMB_OP_CLEARRENDERTARGETVIEW: return "ClearRenderTargetView";
    case D3D12_AUTO_BREADCRUMB_OP_CLEARUNORDEREDACCESSVIEW: return "ClearUnorderedAccessView";
    case D3D12_AUTO_BREADCRUMB_OP_CLEARDEPTHSTENCILVIEW: return "ClearDepthStencilView";
    case D3D12_AUTO_BREADCRUMB_OP_RESOURCEBARRIER: return "ResourceBarrier";
    default: return "op(other)";
    }
}

// Opens the adapter's D3D12 device (D3D12 returns the existing device object for the same adapter, so while dzn holds its
// device this is normally that same object: refcount > 1 shows it). Logs removal reason, DRED breadcrumbs, debug-layer messages.
HRESULT host_check(const char *tag, bool dump) {
    if (!g_have_luid)
        return S_OK;
    com_ptr<IDXGIFactory4> factory;
    com_ptr<IDXGIAdapter1> adapter;
    com_ptr<ID3D12Device> d;
    HRESULT hr = CreateDXGIFactory2(0, __uuidof(IDXGIFactory4), factory.put_void());
    if (SUCCEEDED(hr))
        hr = factory->EnumAdapterByLuid(g_luid, __uuidof(IDXGIAdapter1), adapter.put_void());
    if (SUCCEEDED(hr))
        hr = D3D12CreateDevice(adapter.get(), D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), d.put_void());
    if (!d) {
        blog("[%s] host check: D3D12CreateDevice hr=%s", tag, hr_hex(hr).c_str());
        return hr;
    }
    d->AddRef();
    const ULONG refs = d->Release(); // testing-only count: > 1 means another holder (dzn) shares this device object
    const HRESULT removed = d->GetDeviceRemovedReason();
    blog("[%s] host check: adapter D3D12 device refcount=%lu (%s) GetDeviceRemovedReason=%s%s", tag, refs,
        refs > 1 ? "shared with dzn" : "not shared", hr_hex(removed).c_str(),
        removed == S_OK ? "" : removed == DXGI_ERROR_INVALID_CALL ? " (DXGI_ERROR_INVALID_CALL: removed for an invalid API call)" : " (REMOVED)");
    if (dump || FAILED(removed)) {
        if (auto dred = d.try_as<ID3D12DeviceRemovedExtendedData>()) {
            D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT bc{};
            const HRESULT hb = dred->GetAutoBreadcrumbsOutput(&bc);
            blog("[%s] DRED GetAutoBreadcrumbsOutput hr=%s", tag, hr_hex(hb).c_str());
            if (SUCCEEDED(hb)) {
                unsigned nodes = 0;
                for (const D3D12_AUTO_BREADCRUMB_NODE *n = bc.pHeadAutoBreadcrumbNode; n && nodes < 16; n = n->pNext, ++nodes) {
                    const UINT done = n->pLastBreadcrumbValue ? *n->pLastBreadcrumbValue : 0;
                    std::string ops;
                    for (UINT i = 0; i < n->BreadcrumbCount && i < 64; ++i) {
                        ops += dred_op(n->pCommandHistory[i]);
                        ops += i + 1 == done ? " <-last-completed " : " ";
                    }
                    blog("[%s] DRED node %u: list=%s queue=%s ops=%u completed=%u: %s", tag, nodes,
                        n->pCommandListDebugNameA ? n->pCommandListDebugNameA : "-", n->pCommandQueueDebugNameA ? n->pCommandQueueDebugNameA : "-",
                        n->BreadcrumbCount, done, ops.c_str());
                }
                if (!nodes)
                    blog("[%s] DRED: no breadcrumb nodes (no command list reached the GPU)", tag);
            }
            D3D12_DRED_PAGE_FAULT_OUTPUT pf{};
            const HRESULT hp = dred->GetPageFaultAllocationOutput(&pf);
            blog("[%s] DRED GetPageFaultAllocationOutput hr=%s va=0x%llX", tag, hr_hex(hp).c_str(), static_cast<unsigned long long>(pf.PageFaultVA));
        } else {
            blog("[%s] DRED: ID3D12DeviceRemovedExtendedData not available", tag);
        }
        if (auto iq = d.try_as<ID3D12InfoQueue>()) {
            const UINT64 nm = iq->GetNumStoredMessages();
            blog("[%s] D3D12 debug layer: %llu stored messages", tag, static_cast<unsigned long long>(nm));
            for (UINT64 i = 0; i < nm && i < 64; ++i) {
                SIZE_T len = 0;
                iq->GetMessage(i, nullptr, &len);
                std::vector<char> mb(len ? len : 1);
                auto *m = reinterpret_cast<D3D12_MESSAGE *>(mb.data());
                if (len && SUCCEEDED(iq->GetMessage(i, m, &len)))
                    blog("[%s] D3D12 msg %llu sev=%d id=%d: %.*s", tag, static_cast<unsigned long long>(i), m->Severity, m->ID,
                        static_cast<int>(m->DescriptionByteLength), m->pDescription);
            }
            iq->ClearStoredMessages();
        }
    }
    return removed;
}

// Raw D3D12 on the adapter: what dzn's draw path depends on, plus SetViewInstanceMask/Close (dzn's update_pipeline calls
// SetViewInstanceMask(1) on every pipeline bind at draw time, also when view instancing is unsupported).
void raw_d3d12_report() {
    com_ptr<IDXGIFactory4> factory;
    com_ptr<IDXGIAdapter1> adapter;
    com_ptr<ID3D12Device> d;
    HRESULT hr = CreateDXGIFactory2(0, __uuidof(IDXGIFactory4), factory.put_void());
    if (SUCCEEDED(hr))
        hr = factory->EnumAdapterByLuid(g_luid, __uuidof(IDXGIAdapter1), adapter.put_void());
    if (SUCCEEDED(hr))
        hr = D3D12CreateDevice(adapter.get(), D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), d.put_void());
    if (!d) {
        blog("raw D3D12: device hr=%s", hr_hex(hr).c_str());
        return;
    }
    const D3D_FEATURE_LEVEL fls[] = { D3D_FEATURE_LEVEL_12_2, D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    D3D12_FEATURE_DATA_FEATURE_LEVELS fl{ static_cast<UINT>(std::size(fls)), fls };
    d->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &fl, sizeof(fl));
    D3D12_FEATURE_DATA_D3D12_OPTIONS o{};
    d->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &o, sizeof(o));
    D3D12_FEATURE_DATA_D3D12_OPTIONS3 o3{};
    const HRESULT h3 = d->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS3, &o3, sizeof(o3));
    D3D12_FEATURE_DATA_SHADER_MODEL sm{ D3D_SHADER_MODEL_6_6 };
    d->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm));
    D3D12_FEATURE_DATA_ARCHITECTURE arch{};
    d->CheckFeatureSupport(D3D12_FEATURE_ARCHITECTURE, &arch, sizeof(arch));
    blog("raw D3D12: MaxSupportedFeatureLevel=0x%X HighestShaderModel=0x%X ResourceBindingTier=%d ResourceHeapTier=%d TiledResourcesTier=%d "
         "ConservativeRaster=%d ROVs=%d UMA=%d CacheCoherentUMA=%d | OPTIONS3 hr=%s ViewInstancingTier=%d CopyQueueTimestamp=%d",
        fl.MaxSupportedFeatureLevel, sm.HighestShaderModel, o.ResourceBindingTier, o.ResourceHeapTier, o.TiledResourcesTier,
        o.ConservativeRasterizationTier, o.ROVsSupported, arch.UMA, arch.CacheCoherentUMA, hr_hex(h3).c_str(), o3.ViewInstancingTier);

    com_ptr<ID3D12CommandAllocator> alloc;
    com_ptr<ID3D12GraphicsCommandList> cl;
    d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), alloc.put_void());
    if (alloc)
        d->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.get(), nullptr, __uuidof(ID3D12GraphicsCommandList), cl.put_void());
    auto cl1 = cl ? cl.try_as<ID3D12GraphicsCommandList1>() : nullptr;
    if (!cl1) {
        blog("raw D3D12: ID3D12GraphicsCommandList1 not available");
        return;
    }
    // A closed-with-error list is never executed (executing it is what removes a device).
    const HRESULT empty_close = cl->Close();
    alloc->Reset();
    cl->Reset(alloc.get(), nullptr);
    cl1->SetViewInstanceMask(1);
    const HRESULT vim_close = cl->Close();
    blog("raw D3D12: empty list Close=%s | SetViewInstanceMask(1) then Close=%s (%s) - not executed", hr_hex(empty_close).c_str(),
        hr_hex(vim_close).c_str(), SUCCEEDED(vim_close) ? "accepted" : "REJECTED: recording it invalidates the command list");
    host_check("raw", true);
}

CaseResult run_case(HMODULE dzn, PFN_vkGetInstanceProcAddr gipa, const Case &c) {
    using clock = std::chrono::steady_clock;
    g_case = c.id;
    const unsigned msgs0 = g_msgs;
    const double mem0 = app_mem_mib();
    blog("==== case %s: %s | %ux%u %s image | %s ====", c.id, c.what, c.w, c.h, c.exported ? "EXPORTED (D3D12_RESOURCE)" : "non-exported",
        c.kind == Kind::ClearOnly ? "render-pass clear only" : c.kind == Kind::BindNoDraw ? "bind pipeline, no draw"
                                   : c.push_constants             ? "draw + push constants"
                                                                  : "draw, no push constants");
    CaseResult res;
    Fns f;
    std::string fail;
    auto stop = [&](const char *step, VkResult r) {
        fail = std::string(step) + "=" + vkr(r);
        blog("[%s] FIRST FAILURE: %s -> %s", c.id, step, vkr(r));
    };

    // -- instance + messenger (chained into vkCreateInstance so instance creation itself is covered) --
    VkDebugUtilsMessengerCreateInfoEXT mci{ VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT };
    mci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT
        | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    mci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
        | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    mci.pfnUserCallback = messenger_cb;
    const char *inst_exts[] = { VK_EXT_DEBUG_UTILS_EXTENSION_NAME };
    VkApplicationInfo app{ VK_STRUCTURE_TYPE_APPLICATION_INFO, nullptr, "Vitox M6B.1", 1, "Vitox", 1, VK_API_VERSION_1_1 };
    VkInstanceCreateInfo ici{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, &mci, 0, &app, 0, nullptr, 1, inst_exts };
    f.vkCreateInstance = reinterpret_cast<PFN_vkCreateInstance>(gipa(nullptr, "vkCreateInstance"));
    VkInstance instance = VK_NULL_HANDLE;
    VkResult r = f.vkCreateInstance(&ici, nullptr, &instance);
    blog("[%s] vkCreateInstance(+VK_EXT_debug_utils) -> %s", c.id, vkr(r));
    if (r != VK_SUCCESS) {
        res.summary = std::string(c.id) + " FAIL vkCreateInstance " + vkr(r);
        return res;
    }
#define IFN(n) f.n = reinterpret_cast<PFN_##n>(gipa(instance, #n))
    IFN(vkDestroyInstance); IFN(vkEnumeratePhysicalDevices); IFN(vkGetPhysicalDeviceProperties2); IFN(vkGetPhysicalDeviceQueueFamilyProperties);
    IFN(vkGetPhysicalDeviceMemoryProperties); IFN(vkGetDeviceProcAddr); IFN(vkCreateDevice); IFN(vkCreateDebugUtilsMessengerEXT);
    IFN(vkDestroyDebugUtilsMessengerEXT);
#undef IFN
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    r = f.vkCreateDebugUtilsMessengerEXT ? f.vkCreateDebugUtilsMessengerEXT(instance, &mci, nullptr, &messenger) : VK_ERROR_EXTENSION_NOT_PRESENT;
    blog("[%s] vkCreateDebugUtilsMessengerEXT -> %s", c.id, vkr(r));

    uint32_t n = 1;
    VkPhysicalDevice pdev = VK_NULL_HANDLE;
    r = f.vkEnumeratePhysicalDevices(instance, &n, &pdev);
    blog("[%s] vkEnumeratePhysicalDevices -> %s count=%u", c.id, vkr(r), n);
    if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || n == 0 || !pdev) {
        if (messenger)
            f.vkDestroyDebugUtilsMessengerEXT(instance, messenger, nullptr);
        f.vkDestroyInstance(instance, nullptr);
        res.summary = std::string(c.id) + " FAIL no physical device (" + vkr(r) + ")";
        return res;
    }
    VkPhysicalDeviceIDProperties idp{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
    VkPhysicalDeviceProperties2 p2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &idp };
    f.vkGetPhysicalDeviceProperties2(pdev, &p2);
    if (idp.deviceLUIDValid && !g_have_luid) {
        memcpy(&g_luid, idp.deviceLUID, sizeof(g_luid));
        g_have_luid = true;
    }
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
    blog("[%s] vkCreateDevice -> %s", c.id, vkr(r));
    if (r != VK_SUCCESS) {
        if (messenger)
            f.vkDestroyDebugUtilsMessengerEXT(instance, messenger, nullptr);
        f.vkDestroyInstance(instance, nullptr);
        res.summary = std::string(c.id) + " FAIL vkCreateDevice " + vkr(r);
        return res;
    }
#define DFN(n) f.n = reinterpret_cast<PFN_##n>(f.vkGetDeviceProcAddr(dev, #n))
    DFN(vkDestroyDevice); DFN(vkGetDeviceQueue); DFN(vkCreateImage); DFN(vkDestroyImage); DFN(vkGetImageMemoryRequirements); DFN(vkCreateBuffer);
    DFN(vkDestroyBuffer); DFN(vkGetBufferMemoryRequirements); DFN(vkBindBufferMemory); DFN(vkMapMemory); DFN(vkUnmapMemory); DFN(vkAllocateMemory);
    DFN(vkFreeMemory); DFN(vkBindImageMemory); DFN(vkCreateImageView); DFN(vkDestroyImageView); DFN(vkCreateRenderPass); DFN(vkDestroyRenderPass);
    DFN(vkCreateFramebuffer); DFN(vkDestroyFramebuffer); DFN(vkCreateShaderModule); DFN(vkDestroyShaderModule); DFN(vkCreatePipelineLayout);
    DFN(vkDestroyPipelineLayout); DFN(vkCreateGraphicsPipelines); DFN(vkDestroyPipeline); DFN(vkCreateCommandPool); DFN(vkDestroyCommandPool);
    DFN(vkAllocateCommandBuffers); DFN(vkBeginCommandBuffer); DFN(vkEndCommandBuffer); DFN(vkCmdBeginRenderPass); DFN(vkCmdEndRenderPass);
    DFN(vkCmdBindPipeline); DFN(vkCmdPushConstants); DFN(vkCmdDraw); DFN(vkCmdCopyImageToBuffer); DFN(vkQueueSubmit); DFN(vkQueueWaitIdle);
    DFN(vkDeviceWaitIdle); DFN(vkGetMemoryWin32HandleKHR);
#undef DFN
    VkQueue queue = VK_NULL_HANDLE;
    f.vkGetDeviceQueue(dev, qfam, 0, &queue);
    host_check(c.id, false); // pre-case state: must be S_OK, otherwise an earlier case leaked into this one

    VkPhysicalDeviceMemoryProperties mp{};
    f.vkGetPhysicalDeviceMemoryProperties(pdev, &mp);
    auto pick = [&](uint32_t bits, VkMemoryPropertyFlags want) {
        for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
            if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
                return i;
        return UINT32_MAX;
    };

    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory image_mem = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkRenderPass rp = VK_NULL_HANDLE;
    VkFramebuffer fbo = VK_NULL_HANDLE;
    VkShaderModule vs = VK_NULL_HANDLE, ps = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipe = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cbs[2] = {};
    VkBuffer rb_buf = VK_NULL_HANDLE;
    VkDeviceMemory rb_mem = VK_NULL_HANDLE;
    HANDLE shared_handle = nullptr;
    std::string readback = "not run";
    HRESULT removed_after = S_OK;

    do {
        // -- render target --
        VkExternalMemoryImageCreateInfo emi{ VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, nullptr, VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT };
        VkImageCreateInfo ii{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, c.exported ? &emi : nullptr, 0, VK_IMAGE_TYPE_2D, VK_FORMAT_B8G8R8A8_UNORM, { c.w, c.h, 1 },
            1, 1, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
            VK_SHARING_MODE_EXCLUSIVE, 0, nullptr, VK_IMAGE_LAYOUT_UNDEFINED };
        r = f.vkCreateImage(dev, &ii, nullptr, &image);
        if (r != VK_SUCCESS) { stop("vkCreateImage", r); break; }
        VkMemoryRequirements mr{};
        f.vkGetImageMemoryRequirements(dev, image, &mr);
        VkExportMemoryAllocateInfo emai{ VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, nullptr, VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT };
        VkMemoryDedicatedAllocateInfo dai{ VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, c.exported ? &emai : nullptr, image, VK_NULL_HANDLE };
        VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &dai, mr.size, pick(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
        r = f.vkAllocateMemory(dev, &mai, nullptr, &image_mem);
        if (r != VK_SUCCESS) { stop("vkAllocateMemory(image)", r); break; }
        r = f.vkBindImageMemory(dev, image, image_mem, 0);
        if (r != VK_SUCCESS) { stop("vkBindImageMemory", r); break; }
        VkImageViewCreateInfo vci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, nullptr, 0, image, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_B8G8R8A8_UNORM, {},
            { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
        r = f.vkCreateImageView(dev, &vci, nullptr, &view);
        if (r != VK_SUCCESS) { stop("vkCreateImageView", r); break; }
        if (c.exported) {
            VkMemoryGetWin32HandleInfoKHR gh{ VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR, nullptr, image_mem,
                VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT };
            r = f.vkGetMemoryWin32HandleKHR(dev, &gh, &shared_handle);
            if (r != VK_SUCCESS) { stop("vkGetMemoryWin32HandleKHR", r); break; }
        }
        blog("[%s] render target %ux%u B8G8R8A8 size=%llu memtype=%u%s: image/alloc/bind/view VK_SUCCESS", c.id, c.w, c.h,
            static_cast<unsigned long long>(mr.size), mai.memoryTypeIndex, c.exported ? " + exported D3D12_RESOURCE handle" : "");

        // -- render pass (clear -> TRANSFER_SRC), framebuffer --
        VkAttachmentDescription att{ 0, VK_FORMAT_B8G8R8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
            VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL };
        VkAttachmentReference ref{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        VkSubpassDescription sub{ 0, VK_PIPELINE_BIND_POINT_GRAPHICS, 0, nullptr, 1, &ref, nullptr, nullptr, 0, nullptr };
        VkSubpassDependency dep{ 0, VK_SUBPASS_EXTERNAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, 0 };
        VkRenderPassCreateInfo rpci{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, nullptr, 0, 1, &att, 1, &sub, 1, &dep };
        r = f.vkCreateRenderPass(dev, &rpci, nullptr, &rp);
        if (r != VK_SUCCESS) { stop("vkCreateRenderPass", r); break; }
        VkFramebufferCreateInfo fci{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, nullptr, 0, rp, 1, &view, c.w, c.h, 1 };
        r = f.vkCreateFramebuffer(dev, &fci, nullptr, &fbo);
        if (r != VK_SUCCESS) { stop("vkCreateFramebuffer", r); break; }

        // -- pipeline (cases B..F) --
        if (c.kind != Kind::ClearOnly) {
            VkShaderModuleCreateInfo smv{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr, 0, sizeof(kVitoxTriVs), kVitoxTriVs };
            VkShaderModuleCreateInfo smp{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr, 0,
                c.push_constants ? sizeof(kVitoxTriPs) : sizeof(kVitoxTriPsNoPc), c.push_constants ? kVitoxTriPs : kVitoxTriPsNoPc };
            r = f.vkCreateShaderModule(dev, &smv, nullptr, &vs);
            if (r != VK_SUCCESS) { stop("vkCreateShaderModule(vs)", r); break; }
            r = f.vkCreateShaderModule(dev, &smp, nullptr, &ps);
            if (r != VK_SUCCESS) { stop(c.push_constants ? "vkCreateShaderModule(ps_main)" : "vkCreateShaderModule(ps_nopc)", r); break; }
            VkPushConstantRange pcr{ VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(float) };
            VkPipelineLayoutCreateInfo plci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, nullptr, 0, 0, nullptr, c.push_constants ? 1u : 0u,
                c.push_constants ? &pcr : nullptr };
            r = f.vkCreatePipelineLayout(dev, &plci, nullptr, &layout);
            if (r != VK_SUCCESS) { stop("vkCreatePipelineLayout", r); break; }
            VkPipelineShaderStageCreateInfo stages[2] = {
                { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "vs_main", nullptr },
                { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, ps,
                    c.push_constants ? "ps_main" : "ps_nopc", nullptr },
            };
            VkPipelineVertexInputStateCreateInfo vi{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
            VkPipelineInputAssemblyStateCreateInfo ia{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, nullptr, 0,
                VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
            VkViewport vp{ 0.0f, 0.0f, static_cast<float>(c.w), static_cast<float>(c.h), 0.0f, 1.0f };
            VkRect2D sc_rect{ { 0, 0 }, { c.w, c.h } };
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
            const auto tp = clock::now();
            r = f.vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gp, nullptr, &pipe);
            blog("[%s] vkCreateGraphicsPipelines(%s) -> %s in %lld us", c.id, c.push_constants ? "ps_main + push-constant range" : "ps_nopc, empty layout",
                vkr(r), static_cast<long long>(std::chrono::duration_cast<std::chrono::microseconds>(clock::now() - tp).count()));
            if (r != VK_SUCCESS) { stop("vkCreateGraphicsPipelines", r); break; }
        }

        VkCommandPoolCreateInfo cpi{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr, 0, qfam };
        r = f.vkCreateCommandPool(dev, &cpi, nullptr, &pool);
        if (r != VK_SUCCESS) { stop("vkCreateCommandPool", r); break; }
        VkCommandBufferAllocateInfo cbai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 2 };
        r = f.vkAllocateCommandBuffers(dev, &cbai, cbs);
        if (r != VK_SUCCESS) { stop("vkAllocateCommandBuffers", r); break; }

        // -- the case's command buffer --
        VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
        r = f.vkBeginCommandBuffer(cbs[0], &bi);
        blog("[%s] vkBeginCommandBuffer -> %s", c.id, vkr(r));
        if (r != VK_SUCCESS) { stop("vkBeginCommandBuffer", r); break; }
        VkClearValue clear{};
        clear.color = { { 0.0f, 0.0f, 0.25f, 1.0f } }; // B8G8R8A8: expect bytes B=0x40 G=0 R=0 A=0xFF
        VkRenderPassBeginInfo rpbi{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, nullptr, rp, fbo, { { 0, 0 }, { c.w, c.h } }, 1, &clear };
        f.vkCmdBeginRenderPass(cbs[0], &rpbi, VK_SUBPASS_CONTENTS_INLINE);
        std::string recorded = "BeginRenderPass(clear)";
        if (c.kind != Kind::ClearOnly) {
            f.vkCmdBindPipeline(cbs[0], VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
            recorded += " BindPipeline";
        }
        if (c.kind == Kind::Draw) {
            if (c.push_constants) {
                const float pulse = 1.0f;
                f.vkCmdPushConstants(cbs[0], layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(float), &pulse);
                recorded += " PushConstants(1.0)";
            }
            f.vkCmdDraw(cbs[0], 3, 1, 0, 0);
            recorded += " Draw(3,1,0,0)";
        }
        f.vkCmdEndRenderPass(cbs[0]);
        recorded += " EndRenderPass";
        blog("[%s] recorded: %s (debug_utils messages so far this case: %u)", c.id, recorded.c_str(), g_msgs - msgs0);
        r = f.vkEndCommandBuffer(cbs[0]);
        blog("[%s] vkEndCommandBuffer -> %s", c.id, vkr(r));
        if (r != VK_SUCCESS) {
            stop("vkEndCommandBuffer", r);
            // never submit a command buffer that failed recording; the D3D12 state is still worth reading
            removed_after = host_check(c.id, true);
            break;
        }
        VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO, nullptr, 0, nullptr, nullptr, 1, &cbs[0], 0, nullptr };
        r = f.vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
        blog("[%s] vkQueueSubmit -> %s", c.id, vkr(r));
        if (r != VK_SUCCESS) {
            stop("vkQueueSubmit", r);
            removed_after = host_check(c.id, true);
            break;
        }
        r = f.vkQueueWaitIdle(queue);
        blog("[%s] vkQueueWaitIdle -> %s", c.id, vkr(r));
        if (r != VK_SUCCESS) {
            stop("vkQueueWaitIdle", r);
            removed_after = host_check(c.id, true);
            break;
        }
        removed_after = host_check(c.id, false);
        if (FAILED(removed_after)) {
            fail = "device removed after a successful submit/wait";
            break;
        }

        // -- readback: centre pixel (inside the triangle) + corner (8,8: clear colour) --
        VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, nullptr, 0, 1024, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE };
        r = f.vkCreateBuffer(dev, &bci, nullptr, &rb_buf);
        VkMemoryRequirements bmr{};
        if (r == VK_SUCCESS)
            f.vkGetBufferMemoryRequirements(dev, rb_buf, &bmr);
        VkMemoryAllocateInfo bmai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, bmr.size,
            pick(bmr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
        if (r == VK_SUCCESS)
            r = f.vkAllocateMemory(dev, &bmai, nullptr, &rb_mem);
        if (r == VK_SUCCESS)
            r = f.vkBindBufferMemory(dev, rb_buf, rb_mem, 0);
        VkResult rr = r == VK_SUCCESS ? f.vkBeginCommandBuffer(cbs[1], &bi) : r;
        if (rr == VK_SUCCESS) {
            VkBufferImageCopy regions[2] = {
                { 0, 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { static_cast<int32_t>(c.w / 2), static_cast<int32_t>(c.h / 2 + c.h / 8), 0 }, { 1, 1, 1 } },
                { 512, 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 8, 8, 0 }, { 1, 1, 1 } },
            };
            f.vkCmdCopyImageToBuffer(cbs[1], image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb_buf, 2, regions);
            rr = f.vkEndCommandBuffer(cbs[1]);
        }
        VkSubmitInfo si2{ VK_STRUCTURE_TYPE_SUBMIT_INFO, nullptr, 0, nullptr, nullptr, 1, &cbs[1], 0, nullptr };
        if (rr == VK_SUCCESS)
            rr = f.vkQueueSubmit(queue, 1, &si2, VK_NULL_HANDLE);
        if (rr == VK_SUCCESS)
            rr = f.vkQueueWaitIdle(queue);
        void *p = nullptr;
        if (rr == VK_SUCCESS)
            rr = f.vkMapMemory(dev, rb_mem, 0, VK_WHOLE_SIZE, 0, &p);
        if (rr != VK_SUCCESS) {
            readback = std::string("readback FAILED ") + vkr(rr);
            blog("[%s] %s", c.id, readback.c_str());
            removed_after = host_check(c.id, true);
            fail = readback;
            break;
        }
        const uint8_t *px = static_cast<const uint8_t *>(p);
        const uint8_t *cc = px + 512; // D3D12 copy footprints: 512-byte placement alignment
        const bool corner_clear = cc[0] >= 0x3E && cc[0] <= 0x42 && cc[1] == 0 && cc[2] == 0 && cc[3] == 0xFF;
        const bool centre_clear = px[0] >= 0x3E && px[0] <= 0x42 && px[1] == 0 && px[2] == 0;
        const bool centre_lit = px[3] == 0xFF && !centre_clear && (px[0] + px[1] + px[2]) > 0x80;
        char rbuf[300];
        snprintf(rbuf, sizeof(rbuf), "centre(BGRA)=%02X %02X %02X %02X corner(BGRA)=%02X %02X %02X %02X -> corner %s, centre %s", px[0], px[1], px[2],
            px[3], cc[0], cc[1], cc[2], cc[3], corner_clear ? "= clear colour" : "UNEXPECTED",
            c.kind == Kind::Draw ? (centre_lit ? "= triangle (shaded)" : "NOT DRAWN") : (centre_clear ? "= clear colour" : "UNEXPECTED"));
        f.vkUnmapMemory(dev, rb_mem);
        readback = rbuf;
        blog("[%s] readback: %s", c.id, rbuf);
        const bool pixels_ok = corner_clear && (c.kind == Kind::Draw ? centre_lit : centre_clear);
        if (!pixels_ok)
            fail = "pixels wrong: " + readback;
        res.ok = pixels_ok;
    } while (false);

    // -- teardown (always; frees dzn's device so the next case starts from a fresh D3D12 device) --
    if (dev)
        f.vkDeviceWaitIdle(dev);
    if (pool)
        f.vkDestroyCommandPool(dev, pool, nullptr);
    if (rb_buf)
        f.vkDestroyBuffer(dev, rb_buf, nullptr);
    if (rb_mem)
        f.vkFreeMemory(dev, rb_mem, nullptr);
    if (pipe)
        f.vkDestroyPipeline(dev, pipe, nullptr);
    if (layout)
        f.vkDestroyPipelineLayout(dev, layout, nullptr);
    if (vs)
        f.vkDestroyShaderModule(dev, vs, nullptr);
    if (ps)
        f.vkDestroyShaderModule(dev, ps, nullptr);
    if (fbo)
        f.vkDestroyFramebuffer(dev, fbo, nullptr);
    if (rp)
        f.vkDestroyRenderPass(dev, rp, nullptr);
    if (view)
        f.vkDestroyImageView(dev, view, nullptr);
    if (shared_handle)
        CloseHandle(shared_handle);
    if (image)
        f.vkDestroyImage(dev, image, nullptr);
    if (image_mem)
        f.vkFreeMemory(dev, image_mem, nullptr);
    f.vkDestroyDevice(dev, nullptr);
    if (messenger)
        f.vkDestroyDebugUtilsMessengerEXT(instance, messenger, nullptr);
    f.vkDestroyInstance(instance, nullptr);
    (void)dzn;

    const double mem1 = app_mem_mib();
    char s[700];
    snprintf(s, sizeof(s), "%s %s | %s | device_removed_after=%s | debug_utils msgs=%u | AppMemoryUsage %.1f -> %.1f MiB (delta %+.1f)", c.id,
        res.ok ? "PASS" : "FAIL", res.ok ? readback.c_str() : fail.c_str(), hr_hex(removed_after).c_str(), g_msgs - msgs0, mem0, mem1, mem1 - mem0);
    blog("[%s] RESULT %s", c.id, s);
    res.summary = s;
    return res;
}

} // namespace

std::string probe_m6b1(const std::wstring &local_state) {
    blog("begin: dzn draw-failure diagnostic (NON-VISUAL), bisect A..F; SPIR-V vs %zu B %s | ps_main %zu B %s | ps_nopc %zu B %s",
        sizeof(kVitoxTriVs), kVitoxTriVsSha256, sizeof(kVitoxTriPs), kVitoxTriPsSha256, sizeof(kVitoxTriPsNoPc), kVitoxTriPsNoPcSha256);
    mem_snapshot("m6b1_before");

    // 1. CRT stderr -> LocalState (dzn imports the same UCRT, so Mesa's stderr log sink lands here). Unbuffered.
    const std::wstring err_path = local_state + L"\\m6b1_stderr.txt";
    FILE *ef = nullptr;
    const errno_t fe = _wfreopen_s(&ef, err_path.c_str(), L"w", stderr);
    if (fe == 0 && ef)
        setvbuf(stderr, nullptr, _IONBF, 0);
    _putenv_s("MESA_LOG_LEVEL", "debug");
    blog("stderr -> %s: %s | MESA_LOG_LEVEL=debug | OutputDebugString copies: not capturable in-process (no debugger on the console)",
        narrow(err_path).c_str(), fe == 0 ? "ok" : "FAILED");
    fprintf(stderr, "[vitox] m6b1 stderr capture start\n");

    // 2. D3D12 debug layer + DRED: must precede the first D3D12 device in the process.
    using PFN_GetDebugInterface = HRESULT(WINAPI *)(REFIID, void **);
    auto get_dbg = reinterpret_cast<PFN_GetDebugInterface>(resolve(L"d3d12.dll", "D3D12GetDebugInterface"));
    HRESULT hdbg = E_NOINTERFACE, hdred = E_NOINTERFACE;
    if (get_dbg) {
        // 0.0.12.1 on Xbox: EnableDebugLayer() succeeded but then every ID3D12DeviceFactory::CreateDevice failed (no SDK layers
        // on the console), and that state is process-global. Only query the interface; never enable it.
        com_ptr<ID3D12Debug> dbg;
        hdbg = get_dbg(__uuidof(ID3D12Debug), dbg.put_void());
        com_ptr<ID3D12DeviceRemovedExtendedDataSettings> dred;
        hdred = get_dbg(__uuidof(ID3D12DeviceRemovedExtendedDataSettings), dred.put_void());
        if (dred) {
            dred->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            dred->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        }
    }
    blog("D3D12GetDebugInterface: %s | ID3D12Debug hr=%s (%s) | DRED settings hr=%s (%s)", get_dbg ? "resolved" : "MISSING", hr_hex(hdbg).c_str(),
        SUCCEEDED(hdbg) ? "interface present, NOT enabled (breaks device creation on Xbox)" : "debug layer unavailable", hr_hex(hdred).c_str(),
        SUCCEEDED(hdred) ? "auto-breadcrumbs + page-fault FORCED_ON" : "DRED unavailable");

    // 3. dzn ICD, loaded directly from the package
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
    wchar_t dzn_path[MAX_PATH] = {};
    GetModuleFileNameW(dzn, dzn_path, MAX_PATH);
    blog("vulkan_dzn.dll (Mesa 26.2.5 + VITOX_UWP patch incl. M6B.1 logging + view-instance-mask tier-0 fix) %s negotiate=%s v%u gipa=%s", narrow(dzn_path).c_str(), vkr(nr),
        icd_version, gipa ? "yes" : "no");
    if (!gipa)
        return "FAIL no vk_icdGetInstanceProcAddr";

    const Case cases[] = {
        { "A", "render pass + attachment clear only", Kind::ClearOnly, false, true, 1920, 1080 },
        { "B", "bind graphics pipeline (no push constants), no vkCmdDraw", Kind::BindNoDraw, false, true, 1920, 1080 },
        { "C", "draw, pipeline WITHOUT push constants", Kind::Draw, false, true, 1920, 1080 },
        { "D", "draw + push constants (the 0.0.12.0 path)", Kind::Draw, true, true, 1920, 1080 },
        { "E", "repeat C on a NON-EXPORTED ordinary VkImage", Kind::Draw, false, false, 1920, 1080 },
        { "F", "repeat C at 256x256", Kind::Draw, false, true, 256, 256 },
    };
    std::vector<CaseResult> results;
    bool raw_done = false;
    for (const Case &c : cases) {
        results.push_back(run_case(dzn, gipa, c));
        if (!raw_done && g_have_luid) {
            // after case A (the adapter LUID is known by then; dzn's device is already destroyed)
            g_case = "raw";
            raw_d3d12_report();
            raw_done = true;
        }
        mem_snapshot((std::string("m6b1_after_") + c.id).c_str());
    }
    g_case = "end";
    host_check("end", true);
    FreeLibrary(dzn);
    blog("dxil.dll still loaded after FreeLibrary(dzn): %s", GetModuleHandleW(L"dxil.dll") ? "yes" : "no");
    mem_snapshot("m6b1_after_teardown");

    std::string matrix;
    unsigned pass = 0;
    for (const auto &r : results) {
        blog("matrix: %s", r.summary.c_str());
        matrix += r.summary.substr(0, 1) + (r.ok ? "=PASS " : "=FAIL ");
        pass += r.ok ? 1 : 0;
    }
    fprintf(stderr, "[vitox] m6b1 stderr capture end\n");
    fflush(stderr);
    blog("end: %u/%zu cases pass: %s", pass, results.size(), matrix.c_str());
    // The milestone is the diagnosis itself; the status line carries the matrix.
    return "M6B.1 DIAG " + matrix;
}

} // namespace vitox
