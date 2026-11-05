// Vitox M6C: renderer side of external presentation (see vitox_external_present.h).
// Uses the C Vulkan API through vkGetDeviceProcAddr/vkGetInstanceProcAddr from Vita3K's default dispatcher, so the
// vulkan-hpp dispatcher layout is never touched by this TU (M6C.1 only swaps its vkQueueSubmit pointer, see below).
// M6C.1 (0.0.13.1): sync-stall instrumentation - per-submit records, a counting vkQueueSubmit hook on the default
// dispatcher (every submit NOT made here), one Vulkan+host snapshot at the first host wait > 100 ms, control variants.
#include "vitox_external_present_vk.h"

#include <renderer/vulkan/screen_renderer.h>
#include <renderer/vulkan/state.h>
#include <util/log.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <vulkan/vulkan_win32.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>

using renderer::vulkan::ScreenRenderer;

namespace {

vitox::ExternalPresenter *g_presenter = nullptr;

struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
};

struct SubmitRecord {
    uint64_t frame = 0;
    uint32_t image = 0;
    uint64_t value = 0;
    VkResult result = VK_SUCCESS;
    uint64_t counter_after = 0; // vkGetSemaphoreCounterValue right after vkQueueSubmit (may lag)
    uint64_t other_since_acquire = 0;
};

struct XpState {
    VitoxXpVariant variant = VitoxXpVariant::A_GpuWait;
    bool bound = false;
    bool timeline_enabled = false; // timeline feature chained at device creation
    std::vector<Image> images;
    VkSemaphore timeline = VK_NULL_HANDLE;
    uint64_t value = 0;
    uint32_t next = 0;
    VitoxExternalPresentStats stats{};
    uint32_t semaphores_created = 0;
    uint64_t max_completed_seen = 0;
    uint64_t other_since_acquire = 0;
    bool snapshot_taken = false;
    uint32_t snapshot_id = 0;
    std::array<SubmitRecord, 16> ring{};
    uint64_t ring_n = 0;
    PFN_vkQueueSubmit real_queue_submit = nullptr;
    // M7B Phase B counting hooks (Vita3K may create pipelines on its async compile threads: atomics)
    PFN_vkCmdDrawIndexed real_draw_indexed = nullptr;
    PFN_vkCmdDraw real_draw = nullptr;
    PFN_vkCreateGraphicsPipelines real_create_pipelines = nullptr;
    PFN_vkCreateShaderModule real_create_shader_module = nullptr;
    std::atomic<uint64_t> draw_indexed{ 0 }, draw{ 0 }, pipelines{ 0 }, pipeline_failures{ 0 }, shader_modules{ 0 }, shader_module_failures{ 0 };
    // M7B.6 logging-only hooks (GPU state the driver receives)
    PFN_vkCreateRenderPass real_create_render_pass = nullptr;
    PFN_vkCmdBeginRenderPass real_begin_render_pass = nullptr;
    PFN_vkCmdBindPipeline real_bind_pipeline = nullptr;
    PFN_vkCmdSetViewport real_set_viewport = nullptr;
    PFN_vkCmdSetScissor real_set_scissor = nullptr;
    PFN_vkCmdSetStencilCompareMask real_set_stencil_compare = nullptr;
    PFN_vkCmdSetStencilWriteMask real_set_stencil_write = nullptr;
    PFN_vkCmdSetStencilReference real_set_stencil_ref = nullptr;
    PFN_vkCmdSetDepthBias real_set_depth_bias = nullptr;
    PFN_vkCmdBindVertexBuffers real_bind_vertex_buffers = nullptr;
    PFN_vkCmdBindIndexBuffer real_bind_index_buffer = nullptr;
    std::atomic<uint64_t> m7b6_rp{ 0 }, m7b6_begin{ 0 }, m7b6_bind{ 0 }, m7b6_vp{ 0 }, m7b6_sc{ 0 }, m7b6_st{ 0 }, m7b6_db{ 0 }, m7b6_vb{ 0 }, m7b6_ib{ 0 };
    VkQueue our_queue = VK_NULL_HANDLE;
    VkDevice dev = VK_NULL_HANDLE;
};
XpState g;

// Counting hook on VULKAN_HPP_DEFAULT_DISPATCHER.vkQueueSubmit: everything Vita3K submits through vulkan-hpp. The
// external presenter's own submit calls the driver entry point directly and is never counted here.

VKAPI_ATTR void VKAPI_CALL counting_draw_indexed(VkCommandBuffer cb, uint32_t index_count, uint32_t instances, uint32_t first_index, int32_t vertex_offset,
    uint32_t first_instance);
VKAPI_ATTR void VKAPI_CALL counting_draw(VkCommandBuffer cb, uint32_t vertex_count, uint32_t instances, uint32_t first_vertex, uint32_t first_instance);
VKAPI_ATTR VkResult VKAPI_CALL counting_create_pipelines(VkDevice dev, VkPipelineCache cache, uint32_t count, const VkGraphicsPipelineCreateInfo *infos,
    const VkAllocationCallbacks *alloc, VkPipeline *pipelines);
VKAPI_ATTR VkResult VKAPI_CALL counting_create_shader_module(VkDevice dev, const VkShaderModuleCreateInfo *info, const VkAllocationCallbacks *alloc,
    VkShaderModule *module);

VKAPI_ATTR VkResult VKAPI_CALL counting_queue_submit(VkQueue queue, uint32_t count, const VkSubmitInfo *submits, VkFence fence) {
    g.stats.other_submits++;
    g.other_since_acquire++;
    const VkResult r = g.real_queue_submit(queue, count, submits, fence);
    if (g.stats.other_submits <= 8) {
        uint32_t waits = 0, signals = 0, cbs = 0;
        for (uint32_t i = 0; i < count; ++i) {
            waits += submits[i].waitSemaphoreCount;
            signals += submits[i].signalSemaphoreCount;
            cbs += submits[i].commandBufferCount;
        }
        LOG_INFO("Vitox external present [M6C.1]: OTHER vkQueueSubmit #{} queue={} ({}) submits={} cmdbufs={} waits={} signals={} fence={} -> {}",
            g.stats.other_submits, static_cast<void *>(queue), queue == g.our_queue ? "renderer general queue" : "a different queue", count, cbs, waits,
            signals, static_cast<void *>(fence), vk::to_string(static_cast<vk::Result>(r)));
    }
    return r;
}

template <typename T>
T dev_fn(VkDevice dev, const char *name) {
    return reinterpret_cast<T>(VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr(dev, name));
}
template <typename T>
T inst_fn(VkInstance inst, const char *name) {
    return reinterpret_cast<T>(VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr(inst, name));
}

std::string vkr(VkResult r) { return vk::to_string(static_cast<vk::Result>(r)); }

bool has_ext(const std::vector<vk::ExtensionProperties> &exts, const char *name) {
    return std::any_of(exts.begin(), exts.end(), [&](const vk::ExtensionProperties &e) { return std::strcmp(e.extensionName.data(), name) == 0; });
}

// M8E: the periodic per-frame / per-draw samples are high-volume diagnostics (VITOX_DIAG_VERBOSE, default OFF); the
// first frames/draws (startup proof) and every failure are always logged, the health counters are always kept.
#ifdef VITOX_DIAG_VERBOSE
constexpr bool kDiagVerbose = true;
#else
constexpr bool kDiagVerbose = false;
#endif
bool frame_logged(uint64_t n) { return n <= 16 || (kDiagVerbose && n % 300 == 0); }

uint64_t counter_value(VkDevice dev) {
    if (!g.timeline)
        return 0;
    uint64_t v = 0;
    auto get = dev_fn<PFN_vkGetSemaphoreCounterValue>(dev, "vkGetSemaphoreCounterValueKHR");
    if (!get)
        get = dev_fn<PFN_vkGetSemaphoreCounterValue>(dev, "vkGetSemaphoreCounterValue");
    if (get && get(dev, g.timeline, &v) == VK_SUCCESS)
        g.max_completed_seen = std::max(g.max_completed_seen, v);
    return v;
}

// The Vulkan half of the stall snapshot; the host half follows under the same id.
void vk_snapshot(ScreenRenderer &screen, uint32_t id, uint32_t waiting_image, const char *reason) {
    auto &st = screen.state;
    const VkDevice dev = static_cast<VkDevice>(st.device);
    const uint64_t counter = counter_value(dev);
    auto fence_status = dev_fn<PFN_vkGetFenceStatus>(dev, "vkGetFenceStatus");
    std::string fences;
    for (size_t i = 0; i < screen.fences.size(); ++i)
        fences += fmt::format("{}[{}]={} ", i, static_cast<void *>(static_cast<VkFence>(screen.fences[i])),
            vkr(fence_status(dev, static_cast<VkFence>(screen.fences[i]))));
    LOG_INFO("Vitox external present [M6C.1] SNAPSHOT {} vulkan ({}), variant {}: acquire #{} waiting for image {} | vkGetSemaphoreCounterValue(timeline {})={} "
             "| last submitted value={} | max completed value seen={} | per-image VkFence status: {}| VkQueue={} family={} | other submits since this "
             "frame's acquire={} (total {}) | semaphores created={}",
        id, reason, vitox_xp_variant_name(g.variant), g.stats.acquires, waiting_image, static_cast<void *>(g.timeline), counter, g.value, g.max_completed_seen,
        fences, static_cast<void *>(static_cast<VkQueue>(st.general_queue)), st.general_family_index, g.other_since_acquire, g.stats.other_submits,
        g.semaphores_created);
    const uint64_t first = g.ring_n > g.ring.size() ? g.ring_n - g.ring.size() : 0;
    for (uint64_t k = first; k < g.ring_n; ++k) {
        const SubmitRecord &r = g.ring[k % g.ring.size()];
        LOG_INFO("Vitox external present [M6C.1] SNAPSHOT {} submit record: frame {} image {} signal value={} vkQueueSubmit={} counter right after submit={} "
                 "other submits between acquire and submit={}",
            id, r.frame, r.image, r.value, vkr(r.result), r.counter_after, r.other_since_acquire);
    }
}

} // namespace

const char *vitox_xp_variant_name(VitoxXpVariant v) {
    switch (v) {
    case VitoxXpVariant::A_GpuWait: return "A (GPU-GPU, unchanged)";
    case VitoxXpVariant::B_CpuWait: return "B (LABELLED CPU wait on the Vulkan timeline, no host queue Wait)";
    case VitoxXpVariant::C_Paced: return "C (GPU-GPU, host paced to 1 frame ahead)";
    case VitoxXpVariant::D_AsyncOff: return "D (GPU-GPU, async pipeline compilation off)";
    case VitoxXpVariant::P_Production: return "P (production: GPU-GPU, latency waitable MaxFrameLatency=1, copy-fence image reuse, watchdog)";
    }
    return "?";
}

void vitox_set_external_presenter(vitox::ExternalPresenter *presenter) { g_presenter = presenter; }
vitox::ExternalPresenter *vitox_external_presenter() { return g_presenter; }

// 0.0.24.198 pre-warm loading screen
static char g_loading_title[64] = {};
static void (*g_loading_pump)() = nullptr;
void vitox_loading_configure(const char *title, void (*pump)()) {
    snprintf(g_loading_title, sizeof(g_loading_title), "%s", title ? title : "");
    g_loading_pump = pump;
}
void vitox_loading_progress(uint32_t done, uint32_t total) {
    if (g_loading_pump)
        g_loading_pump();
    if (g_presenter)
        g_presenter->present_loading(done, total, g_loading_title);
}
void vitox_xp_set_variant(VitoxXpVariant v) {
    // a fresh renderer per variant: per-variant stats; the process-lifetime semaphore count is kept
    g.variant = v;
    g.stats = {};
    g.max_completed_seen = 0;
    g.other_since_acquire = 0;
    g.snapshot_taken = false;
    g.ring_n = 0;
    g.bound = false;
    g.draw_indexed = 0, g.draw = 0, g.pipelines = 0, g.pipeline_failures = 0, g.shader_modules = 0, g.shader_module_failures = 0;
}

// ---- M7B Phase B counting hooks ----
std::string vitox_sha256_hex(const void *data, size_t size); // compat/vitox_sha256.cpp (vitox_hle)
namespace {
// ---- M7B.6 logging-only GPU-state hooks: every call is forwarded unchanged; only the first few (and a few later
//      samples) are logged. Values are exactly what Vita3K passes to the Vulkan driver (dzn). ----
bool m7b6_sample(uint64_t n) { return n <= 6 || n == 60 || n == 600 || n == 2000; }
void m7b6_log_pipeline(const VkGraphicsPipelineCreateInfo &ci, VkPipeline p) {
    LOG_INFO("Vitox [M7B.6] pipeline {}: stages={} renderPass={} subpass={} layout={}", static_cast<void *>(p), ci.stageCount,
        static_cast<void *>(ci.renderPass), ci.subpass, static_cast<void *>(ci.layout));
    if (const auto *vi = ci.pVertexInputState) {
        for (uint32_t b = 0; b < vi->vertexBindingDescriptionCount; b++)
            LOG_INFO("Vitox [M7B.6]   vertex binding {}: stride {} rate {}", vi->pVertexBindingDescriptions[b].binding, vi->pVertexBindingDescriptions[b].stride,
                static_cast<int>(vi->pVertexBindingDescriptions[b].inputRate));
        for (uint32_t a = 0; a < vi->vertexAttributeDescriptionCount; a++)
            LOG_INFO("Vitox [M7B.6]   vertex attribute location {} binding {} format {} offset {}", vi->pVertexAttributeDescriptions[a].location,
                vi->pVertexAttributeDescriptions[a].binding, static_cast<int>(vi->pVertexAttributeDescriptions[a].format), vi->pVertexAttributeDescriptions[a].offset);
    }
    if (const auto *ia = ci.pInputAssemblyState)
        LOG_INFO("Vitox [M7B.6]   input assembly topology {} primitiveRestart {}", static_cast<int>(ia->topology), ia->primitiveRestartEnable);
    if (const auto *vp = ci.pViewportState) {
        LOG_INFO("Vitox [M7B.6]   viewport state: viewports {} (static ptr {}) scissors {} (static ptr {})", vp->viewportCount,
            static_cast<const void *>(vp->pViewports), vp->scissorCount, static_cast<const void *>(vp->pScissors));
        for (uint32_t v = 0; vp->pViewports && v < vp->viewportCount; v++)
            LOG_INFO("Vitox [M7B.6]   static viewport {}: x {} y {} w {} h {} minDepth {} maxDepth {}", v, vp->pViewports[v].x, vp->pViewports[v].y,
                vp->pViewports[v].width, vp->pViewports[v].height, vp->pViewports[v].minDepth, vp->pViewports[v].maxDepth);
        for (uint32_t s = 0; vp->pScissors && s < vp->scissorCount; s++)
            LOG_INFO("Vitox [M7B.6]   static scissor {}: x {} y {} w {} h {}", s, vp->pScissors[s].offset.x, vp->pScissors[s].offset.y, vp->pScissors[s].extent.width,
                vp->pScissors[s].extent.height);
    }
    if (const auto *rs = ci.pRasterizationState)
        LOG_INFO("Vitox [M7B.6]   raster: depthClamp {} discard {} polygonMode {} cullMode {} frontFace {} depthBias enable {} const {} clamp {} slope {} lineWidth {}",
            rs->depthClampEnable, rs->rasterizerDiscardEnable, static_cast<int>(rs->polygonMode), static_cast<int>(rs->cullMode), static_cast<int>(rs->frontFace),
            rs->depthBiasEnable, rs->depthBiasConstantFactor, rs->depthBiasClamp, rs->depthBiasSlopeFactor, rs->lineWidth);
    if (const auto *ms = ci.pMultisampleState)
        LOG_INFO("Vitox [M7B.6]   multisample: samples {} sampleShading {} alphaToCoverage {}", static_cast<int>(ms->rasterizationSamples), ms->sampleShadingEnable,
            ms->alphaToCoverageEnable);
    if (const auto *ds = ci.pDepthStencilState) {
        LOG_INFO("Vitox [M7B.6]   depth: test {} write {} compareOp {} boundsTest {} ({}..{}) | stencil test {}", ds->depthTestEnable, ds->depthWriteEnable,
            static_cast<int>(ds->depthCompareOp), ds->depthBoundsTestEnable, ds->minDepthBounds, ds->maxDepthBounds, ds->stencilTestEnable);
        const VkStencilOpState *f[2] = { &ds->front, &ds->back };
        for (int i = 0; i < 2; i++)
            LOG_INFO("Vitox [M7B.6]   stencil {}: fail {} pass {} depthFail {} compareOp {} compareMask 0x{:X} writeMask 0x{:X} reference {}", i ? "back" : "front",
                static_cast<int>(f[i]->failOp), static_cast<int>(f[i]->passOp), static_cast<int>(f[i]->depthFailOp), static_cast<int>(f[i]->compareOp), f[i]->compareMask,
                f[i]->writeMask, f[i]->reference);
    } else {
        LOG_INFO("Vitox [M7B.6]   depth-stencil state: none");
    }
    if (const auto *cb = ci.pColorBlendState) {
        LOG_INFO("Vitox [M7B.6]   colour blend: logicOp enable {} op {} attachments {}", cb->logicOpEnable, static_cast<int>(cb->logicOp), cb->attachmentCount);
        for (uint32_t a = 0; cb->pAttachments && a < cb->attachmentCount; a++) {
            const auto &at = cb->pAttachments[a];
            LOG_INFO("Vitox [M7B.6]   colour attachment {}: writeMask 0x{:X} blend {} src {} dst {} op {} srcA {} dstA {} opA {}", a, static_cast<uint32_t>(at.colorWriteMask),
                at.blendEnable, static_cast<int>(at.srcColorBlendFactor), static_cast<int>(at.dstColorBlendFactor), static_cast<int>(at.colorBlendOp),
                static_cast<int>(at.srcAlphaBlendFactor), static_cast<int>(at.dstAlphaBlendFactor), static_cast<int>(at.alphaBlendOp));
        }
    }
    if (const auto *dy = ci.pDynamicState) {
        std::string s;
        for (uint32_t d = 0; d < dy->dynamicStateCount; d++)
            s += fmt::format("{} ", static_cast<int>(dy->pDynamicStates[d]));
        LOG_INFO("Vitox [M7B.6]   dynamic states: [ {}] (1=VIEWPORT 2=SCISSOR 4=DEPTH_BIAS 6=STENCIL_COMPARE 7=STENCIL_WRITE 8=STENCIL_REF)", s);
    }
}
VKAPI_ATTR VkResult VKAPI_CALL m7b6_create_render_pass(VkDevice dev, const VkRenderPassCreateInfo *ci, const VkAllocationCallbacks *alloc, VkRenderPass *rp) {
    const VkResult r = g.real_create_render_pass(dev, ci, alloc, rp);
    const uint64_t n = ++g.m7b6_rp;
    if (n <= 16 && ci) {
        LOG_INFO("Vitox [M7B.6] vkCreateRenderPass #{} -> {} renderPass={} attachments {} subpasses {}", n, vkr(r), static_cast<void *>(rp ? *rp : VK_NULL_HANDLE),
            ci->attachmentCount, ci->subpassCount);
        for (uint32_t a = 0; a < ci->attachmentCount; a++) {
            const auto &d = ci->pAttachments[a];
            LOG_INFO("Vitox [M7B.6]   attachment {}: format {} samples {} loadOp {} storeOp {} stencilLoadOp {} stencilStoreOp {} initialLayout {} finalLayout {} "
                     "(loadOp 0=LOAD 1=CLEAR 2=DONT_CARE; storeOp 0=STORE 1=DONT_CARE)",
                a, static_cast<int>(d.format), static_cast<int>(d.samples), static_cast<int>(d.loadOp), static_cast<int>(d.storeOp), static_cast<int>(d.stencilLoadOp),
                static_cast<int>(d.stencilStoreOp), static_cast<int>(d.initialLayout), static_cast<int>(d.finalLayout));
        }
        for (uint32_t s = 0; s < ci->subpassCount; s++) {
            const auto &sp = ci->pSubpasses[s];
            LOG_INFO("Vitox [M7B.6]   subpass {}: colour refs {} (first attachment {}) depth ref {}", s, sp.colorAttachmentCount,
                sp.colorAttachmentCount ? static_cast<int64_t>(sp.pColorAttachments[0].attachment) : -1,
                sp.pDepthStencilAttachment ? static_cast<int64_t>(sp.pDepthStencilAttachment->attachment) : -1);
        }
    }
    return r;
}
VKAPI_ATTR void VKAPI_CALL m7b6_begin_render_pass(VkCommandBuffer cb, const VkRenderPassBeginInfo *bi, VkSubpassContents contents) {
    const uint64_t n = ++g.m7b6_begin;
    if (m7b6_sample(n) && bi) {
        std::string cv;
        for (uint32_t i = 0; i < bi->clearValueCount && bi->pClearValues; i++) {
            const VkClearValue &c = bi->pClearValues[i];
            cv += fmt::format(" [{}] colour({} {} {} {}) depth {} stencil {}", i, c.color.float32[0], c.color.float32[1], c.color.float32[2], c.color.float32[3],
                c.depthStencil.depth, c.depthStencil.stencil);
        }
        LOG_INFO("Vitox [M7B.6] vkCmdBeginRenderPass #{}: renderPass={} framebuffer={} renderArea {},{} {}x{} | clear values {}:{}", n,
            static_cast<void *>(bi->renderPass), static_cast<void *>(bi->framebuffer), bi->renderArea.offset.x, bi->renderArea.offset.y, bi->renderArea.extent.width,
            bi->renderArea.extent.height, bi->clearValueCount, cv);
    }
    g.real_begin_render_pass(cb, bi, contents);
}
VKAPI_ATTR void VKAPI_CALL m7b6_bind_pipeline(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipeline p) {
    const uint64_t n = ++g.m7b6_bind;
    if (m7b6_sample(n))
        LOG_INFO("Vitox [M7B.6] vkCmdBindPipeline #{}: bindPoint {} pipeline {}", n, static_cast<int>(bp), static_cast<void *>(p));
    g.real_bind_pipeline(cb, bp, p);
}
VKAPI_ATTR void VKAPI_CALL m7b6_set_viewport(VkCommandBuffer cb, uint32_t first, uint32_t count, const VkViewport *v) {
    const uint64_t n = ++g.m7b6_vp;
    if (m7b6_sample(n))
        for (uint32_t i = 0; i < count && v; i++)
            LOG_INFO("Vitox [M7B.6] vkCmdSetViewport #{} [{}]: x {} y {} w {} h {} minDepth {} maxDepth {}", n, first + i, v[i].x, v[i].y, v[i].width, v[i].height,
                v[i].minDepth, v[i].maxDepth);
    g.real_set_viewport(cb, first, count, v);
}
VKAPI_ATTR void VKAPI_CALL m7b6_set_scissor(VkCommandBuffer cb, uint32_t first, uint32_t count, const VkRect2D *s) {
    const uint64_t n = ++g.m7b6_sc;
    if (m7b6_sample(n))
        for (uint32_t i = 0; i < count && s; i++)
            LOG_INFO("Vitox [M7B.6] vkCmdSetScissor #{} [{}]: x {} y {} w {} h {}", n, first + i, s[i].offset.x, s[i].offset.y, s[i].extent.width, s[i].extent.height);
    g.real_set_scissor(cb, first, count, s);
}
VKAPI_ATTR void VKAPI_CALL m7b6_set_stencil_compare(VkCommandBuffer cb, VkStencilFaceFlags f, uint32_t m) {
    if (m7b6_sample(++g.m7b6_st))
        LOG_INFO("Vitox [M7B.6] vkCmdSetStencilCompareMask faces 0x{:X} mask 0x{:X}", f, m);
    g.real_set_stencil_compare(cb, f, m);
}
VKAPI_ATTR void VKAPI_CALL m7b6_set_stencil_write(VkCommandBuffer cb, VkStencilFaceFlags f, uint32_t m) {
    if (m7b6_sample(++g.m7b6_st))
        LOG_INFO("Vitox [M7B.6] vkCmdSetStencilWriteMask faces 0x{:X} mask 0x{:X}", f, m);
    g.real_set_stencil_write(cb, f, m);
}
VKAPI_ATTR void VKAPI_CALL m7b6_set_stencil_ref(VkCommandBuffer cb, VkStencilFaceFlags f, uint32_t r) {
    if (m7b6_sample(++g.m7b6_st))
        LOG_INFO("Vitox [M7B.6] vkCmdSetStencilReference faces 0x{:X} reference {}", f, r);
    g.real_set_stencil_ref(cb, f, r);
}
VKAPI_ATTR void VKAPI_CALL m7b6_set_depth_bias(VkCommandBuffer cb, float c, float clamp, float slope) {
    if (m7b6_sample(++g.m7b6_db))
        LOG_INFO("Vitox [M7B.6] vkCmdSetDepthBias const {} clamp {} slope {}", c, clamp, slope);
    g.real_set_depth_bias(cb, c, clamp, slope);
}
VKAPI_ATTR void VKAPI_CALL m7b6_bind_vertex_buffers(VkCommandBuffer cb, uint32_t first, uint32_t count, const VkBuffer *b, const VkDeviceSize *o) {
    const uint64_t n = ++g.m7b6_vb;
    if (m7b6_sample(n))
        for (uint32_t i = 0; i < count && b; i++)
            LOG_INFO("Vitox [M7B.6] vkCmdBindVertexBuffers #{} binding {}: buffer {} offset {}", n, first + i, static_cast<void *>(b[i]), o ? o[i] : 0);
    g.real_bind_vertex_buffers(cb, first, count, b, o);
}
VKAPI_ATTR void VKAPI_CALL m7b6_bind_index_buffer(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, VkIndexType t) {
    if (m7b6_sample(++g.m7b6_ib))
        LOG_INFO("Vitox [M7B.6] vkCmdBindIndexBuffer: buffer {} offset {} type {}", static_cast<void *>(b), o, static_cast<int>(t));
    g.real_bind_index_buffer(cb, b, o, t);
}
VKAPI_ATTR void VKAPI_CALL counting_draw_indexed(VkCommandBuffer cb, uint32_t index_count, uint32_t instances, uint32_t first_index, int32_t vertex_offset,
    uint32_t first_instance) {
    const uint64_t n = ++g.draw_indexed;
    if (n <= 3 || (kDiagVerbose && n % 600 == 0))
        LOG_INFO("Vitox [M7B] vkCmdDrawIndexed #{} indexCount={} instances={}", n, index_count, instances);
    g.real_draw_indexed(cb, index_count, instances, first_index, vertex_offset, first_instance);
}
VKAPI_ATTR void VKAPI_CALL counting_draw(VkCommandBuffer cb, uint32_t vertex_count, uint32_t instances, uint32_t first_vertex, uint32_t first_instance) {
    ++g.draw;
    g.real_draw(cb, vertex_count, instances, first_vertex, first_instance);
}
VKAPI_ATTR VkResult VKAPI_CALL counting_create_pipelines(VkDevice dev, VkPipelineCache cache, uint32_t count, const VkGraphicsPipelineCreateInfo *infos,
    const VkAllocationCallbacks *alloc, VkPipeline *pipelines) {
    const auto t0 = std::chrono::steady_clock::now();
    const VkResult r = g.real_create_pipelines(dev, cache, count, infos, alloc, pipelines);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (r == VK_SUCCESS)
        g.pipelines += count;
    else
        ++g.pipeline_failures;
    LOG_INFO("Vitox [M7B] vkCreateGraphicsPipelines(count={} stages={}) -> {} in {:.1f} ms | pipelines so far {} failures {}", count,
        count ? infos[0].stageCount : 0, vkr(r), ms, g.pipelines.load(), g.pipeline_failures.load());
#ifdef VITOX_DIAG_M7B
    for (uint32_t i = 0; i < count && infos; i++)
        m7b6_log_pipeline(infos[i], r == VK_SUCCESS && pipelines ? pipelines[i] : VK_NULL_HANDLE);
#endif
    return r;
}
VKAPI_ATTR VkResult VKAPI_CALL counting_create_shader_module(VkDevice dev, const VkShaderModuleCreateInfo *info, const VkAllocationCallbacks *alloc,
    VkShaderModule *module) {
    const VkResult r = g.real_create_shader_module(dev, info, alloc, module);
    if (r == VK_SUCCESS)
        ++g.shader_modules;
    else
        ++g.shader_module_failures;
    LOG_INFO("Vitox [M7B] vkCreateShaderModule {} B SHA-256 {} -> {}", info ? info->codeSize : 0,
        info && info->pCode ? vitox_sha256_hex(info->pCode, info->codeSize) : std::string("-"), vkr(r));
    return r;
}
} // namespace
VitoxExternalPresentStats vitox_external_present_stats() {
    VitoxExternalPresentStats s = g.stats;
    s.semaphores_created = g.semaphores_created;
    s.cmd_draw_indexed = g.draw_indexed;
    s.cmd_draw = g.draw;
    s.pipelines_created = g.pipelines;
    s.pipeline_failures = g.pipeline_failures;
    s.shader_modules = g.shader_modules;
    s.shader_module_failures = g.shader_module_failures;
    s.completed_value = g.timeline && g.dev ? counter_value(g.dev) : 0;
    return s;
}

bool vitox_xp_add_device_extensions(vk::PhysicalDevice physical_device, std::vector<const char *> &device_extensions) {
    const auto available = physical_device.enumerateDeviceExtensionProperties();
    const char *required[] = { VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME, VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME, VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME,
        VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME, VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME, VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME,
        VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME };
    for (const char *e : required) {
        if (!has_ext(available, e)) {
            LOG_ERROR("Vitox external present: device extension {} is missing", e);
            return false;
        }
        if (std::none_of(device_extensions.begin(), device_extensions.end(), [&](const char *x) { return std::strcmp(x, e) == 0; }))
            device_extensions.push_back(e);
    }
    std::string list;
    for (const char *e : device_extensions)
        list += std::string(e) + " ";
    LOG_INFO("Vitox external present: enabled device extensions ({}): {}", device_extensions.size(), list);
    return true;
}

void vitox_xp_chain_device_features(vk::Instance instance, vk::PhysicalDevice physical_device, vk::DeviceCreateInfo &info) {
    static VkPhysicalDeviceTimelineSemaphoreFeatures timeline{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES };
    auto get_features2 = inst_fn<PFN_vkGetPhysicalDeviceFeatures2>(static_cast<VkInstance>(instance), "vkGetPhysicalDeviceFeatures2KHR");
    if (!get_features2)
        get_features2 = inst_fn<PFN_vkGetPhysicalDeviceFeatures2>(static_cast<VkInstance>(instance), "vkGetPhysicalDeviceFeatures2");
    VkPhysicalDeviceTimelineSemaphoreFeatures query{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES };
    VkPhysicalDeviceFeatures2 f2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &query };
    if (get_features2)
        get_features2(static_cast<VkPhysicalDevice>(physical_device), &f2);
    g.timeline_enabled = query.timelineSemaphore == VK_TRUE;
    if (g.timeline_enabled) {
        timeline.timelineSemaphore = VK_TRUE;
        timeline.pNext = const_cast<void *>(info.pNext);
        info.pNext = &timeline;
    }
    LOG_INFO("Vitox external present: timelineSemaphore feature supported={} -> {}", g.timeline_enabled,
        g.timeline_enabled ? "chained into VkDeviceCreateInfo" : "NOT chained (GPU sync unavailable; CPU-wait fallback)");
}

vk::SurfaceCapabilitiesKHR vitox_xp_capabilities() {
    vk::SurfaceCapabilitiesKHR caps{};
    const uint32_t n = std::max<uint32_t>(g_presenter->image_count(), 2);
    caps.minImageCount = n - 1; // ScreenRenderer::create_swapchain uses minImageCount + 1
    caps.maxImageCount = n;
    caps.currentExtent = vk::Extent2D{ g_presenter->width(), g_presenter->height() };
    caps.minImageExtent = caps.currentExtent;
    caps.maxImageExtent = caps.currentExtent;
    caps.maxImageArrayLayers = 1;
    caps.supportedTransforms = vk::SurfaceTransformFlagBitsKHR::eIdentity;
    caps.currentTransform = vk::SurfaceTransformFlagBitsKHR::eIdentity;
    caps.supportedCompositeAlpha = vk::CompositeAlphaFlagBitsKHR::eOpaque;
    // no eStorage: the renderer then leaves FSR off (renderer.cpp support_fsr)
    caps.supportedUsageFlags = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst;
    LOG_INFO("Vitox external present: synthesized surface capabilities {}x{} images={} usage=ColorAttachment|TransferSrc|TransferDst (no Storage -> FSR off)",
        caps.currentExtent.width, caps.currentExtent.height, n);
    return caps;
}

std::vector<vk::Image> vitox_xp_create_images(ScreenRenderer &screen) {
    auto &st = screen.state;
    const VkDevice dev = static_cast<VkDevice>(st.device);
    const VkInstance inst = static_cast<VkInstance>(st.instance);
    const VkPhysicalDevice pd = static_cast<VkPhysicalDevice>(st.physical_device);
    std::vector<vk::Image> out;

    if (!g.bound) {
        auto get_props2 = inst_fn<PFN_vkGetPhysicalDeviceProperties2>(inst, "vkGetPhysicalDeviceProperties2KHR");
        if (!get_props2)
            get_props2 = inst_fn<PFN_vkGetPhysicalDeviceProperties2>(inst, "vkGetPhysicalDeviceProperties2");
        VkPhysicalDeviceIDProperties idp{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
        VkPhysicalDeviceProperties2 p2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &idp };
        if (get_props2)
            get_props2(pd, &p2);
        if (!idp.deviceLUIDValid) {
            LOG_ERROR("Vitox external present: no valid adapter LUID (vkGetPhysicalDeviceProperties2 {})", get_props2 ? "returned no LUID" : "missing");
            return out;
        }
        g.bound = g_presenter->bind(idp.deviceLUID);
        LOG_INFO("Vitox external present: adapter LUID {:02X}{:02X}{:02X}{:02X}-{:02X}{:02X}{:02X}{:02X} -> host bind {}", idp.deviceLUID[7], idp.deviceLUID[6],
            idp.deviceLUID[5], idp.deviceLUID[4], idp.deviceLUID[3], idp.deviceLUID[2], idp.deviceLUID[1], idp.deviceLUID[0], g.bound ? "OK" : "FAILED");
        if (!g.bound)
            return out;
    }

    // M6C.1: count every other vkQueueSubmit (Vita3K re-initialises the dispatcher per device, so install per image set)
    if (VULKAN_HPP_DEFAULT_DISPATCHER.vkQueueSubmit != &counting_queue_submit) {
        g.real_queue_submit = VULKAN_HPP_DEFAULT_DISPATCHER.vkQueueSubmit;
        VULKAN_HPP_DEFAULT_DISPATCHER.vkQueueSubmit = &counting_queue_submit;
    }
    // M7B Phase B: draw / pipeline / shader-module counting (observation only; every call reaches the driver)
    if (VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdDrawIndexed != &counting_draw_indexed) {
        g.real_draw_indexed = VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdDrawIndexed;
        VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdDrawIndexed = &counting_draw_indexed;
    }
    if (VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdDraw != &counting_draw) {
        g.real_draw = VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdDraw;
        VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdDraw = &counting_draw;
    }
    if (VULKAN_HPP_DEFAULT_DISPATCHER.vkCreateGraphicsPipelines != &counting_create_pipelines) {
        g.real_create_pipelines = VULKAN_HPP_DEFAULT_DISPATCHER.vkCreateGraphicsPipelines;
        VULKAN_HPP_DEFAULT_DISPATCHER.vkCreateGraphicsPipelines = &counting_create_pipelines;
    }
    // M7B.6: logging-only GPU-state hooks (installed like the counting hooks; every call is forwarded unchanged).
    // M8A: only in VITOX_DIAG_M7B builds (CMake option, default OFF).
#ifdef VITOX_DIAG_M7B
#define VITOX_M7B6_HOOK(member, fn, real)                     \
    if (VULKAN_HPP_DEFAULT_DISPATCHER.member && VULKAN_HPP_DEFAULT_DISPATCHER.member != &fn) { \
        g.real = VULKAN_HPP_DEFAULT_DISPATCHER.member;        \
        VULKAN_HPP_DEFAULT_DISPATCHER.member = &fn;           \
    }
    VITOX_M7B6_HOOK(vkCreateRenderPass, m7b6_create_render_pass, real_create_render_pass)
    VITOX_M7B6_HOOK(vkCmdBeginRenderPass, m7b6_begin_render_pass, real_begin_render_pass)
    VITOX_M7B6_HOOK(vkCmdBindPipeline, m7b6_bind_pipeline, real_bind_pipeline)
    VITOX_M7B6_HOOK(vkCmdSetViewport, m7b6_set_viewport, real_set_viewport)
    VITOX_M7B6_HOOK(vkCmdSetScissor, m7b6_set_scissor, real_set_scissor)
    VITOX_M7B6_HOOK(vkCmdSetStencilCompareMask, m7b6_set_stencil_compare, real_set_stencil_compare)
    VITOX_M7B6_HOOK(vkCmdSetStencilWriteMask, m7b6_set_stencil_write, real_set_stencil_write)
    VITOX_M7B6_HOOK(vkCmdSetStencilReference, m7b6_set_stencil_ref, real_set_stencil_ref)
    VITOX_M7B6_HOOK(vkCmdSetDepthBias, m7b6_set_depth_bias, real_set_depth_bias)
    VITOX_M7B6_HOOK(vkCmdBindVertexBuffers, m7b6_bind_vertex_buffers, real_bind_vertex_buffers)
    VITOX_M7B6_HOOK(vkCmdBindIndexBuffer, m7b6_bind_index_buffer, real_bind_index_buffer)
#undef VITOX_M7B6_HOOK
#endif
    if (VULKAN_HPP_DEFAULT_DISPATCHER.vkCreateShaderModule != &counting_create_shader_module) {
        g.real_create_shader_module = VULKAN_HPP_DEFAULT_DISPATCHER.vkCreateShaderModule;
        VULKAN_HPP_DEFAULT_DISPATCHER.vkCreateShaderModule = &counting_create_shader_module;
    }
    g.our_queue = static_cast<VkQueue>(st.general_queue);
    g.dev = dev;
    LOG_INFO("Vitox external present [M6C.1]: variant {} | counting vkQueueSubmit hook installed | renderer general queue={} family={} | transfer queue={} family={}",
        vitox_xp_variant_name(g.variant), static_cast<void *>(g.our_queue), st.general_family_index,
        static_cast<void *>(static_cast<VkQueue>(st.transfer_queue)), st.transfer_family_index);

    // ---- GPU sync: exportable timeline semaphore = shared D3D12 fence ----
    if (!g.timeline && g.timeline_enabled) {
        auto create_semaphore = dev_fn<PFN_vkCreateSemaphore>(dev, "vkCreateSemaphore");
        auto get_sem_handle = dev_fn<PFN_vkGetSemaphoreWin32HandleKHR>(dev, "vkGetSemaphoreWin32HandleKHR");
        VkExportSemaphoreCreateInfo esci{ VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO, nullptr, VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT };
        VkSemaphoreTypeCreateInfo stci{ VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, &esci, VK_SEMAPHORE_TYPE_TIMELINE, 0 };
        VkSemaphoreCreateInfo sci{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &stci };
        const VkResult rs = create_semaphore(dev, &sci, nullptr, &g.timeline);
        g.semaphores_created += rs == VK_SUCCESS;
        HANDLE h = nullptr;
        VkResult rh = VK_ERROR_INITIALIZATION_FAILED;
        if (rs == VK_SUCCESS && get_sem_handle) {
            VkSemaphoreGetWin32HandleInfoKHR ghi{ VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR, nullptr, g.timeline,
                VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT };
            rh = get_sem_handle(dev, &ghi, &h);
        }
        g.stats.gpu_sync = rh == VK_SUCCESS && h && g_presenter->import_fence(h);
        if (h)
            CloseHandle(h);
        LOG_INFO("Vitox external present: sync primitive = timeline VkSemaphore {} (#{} this process) exported as D3D12_FENCE: create={} vkGetSemaphoreWin32HandleKHR={} handle={} host import={} -> {}",
            static_cast<void *>(g.timeline), g.semaphores_created, vkr(rs), vkr(rh), static_cast<void *>(h), g.stats.gpu_sync,
            g.stats.gpu_sync ? "GPU-GPU sync" : "CPU-wait FALLBACK");
    } else if (g.timeline) {
        LOG_INFO("Vitox external present [M6C.1]: image set recreated - timeline semaphore {} KEPT (not re-exported)", static_cast<void *>(g.timeline));
    } else if (!g.timeline_enabled) {
        LOG_WARN("Vitox external present: timeline semaphores not enabled -> CPU-wait FALLBACK (labelled, timed)");
    }

    // ---- N exportable dedicated images ----
    auto create_image = dev_fn<PFN_vkCreateImage>(dev, "vkCreateImage");
    auto get_reqs = dev_fn<PFN_vkGetImageMemoryRequirements>(dev, "vkGetImageMemoryRequirements");
    auto alloc_mem = dev_fn<PFN_vkAllocateMemory>(dev, "vkAllocateMemory");
    auto bind_mem = dev_fn<PFN_vkBindImageMemory>(dev, "vkBindImageMemory");
    auto get_mem_handle = dev_fn<PFN_vkGetMemoryWin32HandleKHR>(dev, "vkGetMemoryWin32HandleKHR");
    const auto mem_props = st.physical_device.getMemoryProperties();
    const VkFormat format = static_cast<VkFormat>(screen.surface_format.format);
    for (uint32_t i = 0; i < screen.swapchain_size; ++i) {
        Image img;
        VkExternalMemoryImageCreateInfo emi{ VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, nullptr, VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT };
        VkImageCreateInfo ici{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &emi, 0, VK_IMAGE_TYPE_2D, format, { screen.extent.width, screen.extent.height, 1 }, 1, 1,
            VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, nullptr,
            VK_IMAGE_LAYOUT_UNDEFINED };
        const VkResult ri = create_image(dev, &ici, nullptr, &img.image);
        VkMemoryRequirements mr{};
        if (ri == VK_SUCCESS)
            get_reqs(dev, img.image, &mr);
        uint32_t type = UINT32_MAX;
        for (uint32_t t = 0; t < mem_props.memoryTypeCount && type == UINT32_MAX; ++t)
            if ((mr.memoryTypeBits & (1u << t)) && (mem_props.memoryTypes[t].propertyFlags & vk::MemoryPropertyFlagBits::eDeviceLocal))
                type = t;
        VkExportMemoryAllocateInfo emai{ VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, nullptr, VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT };
        VkMemoryDedicatedAllocateInfo dai{ VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, &emai, img.image, VK_NULL_HANDLE };
        VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &dai, mr.size, type };
        const VkResult ra = (ri == VK_SUCCESS && type != UINT32_MAX) ? alloc_mem(dev, &mai, nullptr, &img.memory) : VK_ERROR_OUT_OF_DEVICE_MEMORY;
        const VkResult rb = ra == VK_SUCCESS ? bind_mem(dev, img.image, img.memory, 0) : ra;
        HANDLE h = nullptr;
        VkResult rh = rb;
        if (rb == VK_SUCCESS) {
            VkMemoryGetWin32HandleInfoKHR ghi{ VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR, nullptr, img.memory, VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT };
            rh = get_mem_handle(dev, &ghi, &h);
        }
        const bool imported = rh == VK_SUCCESS && h && g_presenter->import_image(i, h);
        if (h)
            CloseHandle(h);
        LOG_INFO("Vitox external present: image {} {}x{} {} create={} alloc={} (type {}, {} B) bind={} vkGetMemoryWin32HandleKHR={} host OpenSharedHandle={}", i,
            screen.extent.width, screen.extent.height, vk::to_string(screen.surface_format.format), vkr(ri), vkr(ra), type, mr.size, vkr(rb), vkr(rh), imported);
        g.images.push_back(img);
        g.stats.images++;
        g.stats.image_handles_ok += imported;
        if (!imported) {
            LOG_ERROR("Vitox external present: image {} could not be shared with the host", i);
            vitox_xp_destroy_images(screen);
            return {};
        }
        out.push_back(vk::Image(img.image));
    }
    g.next = 0;
    return out;
}

void vitox_xp_destroy_images(ScreenRenderer &screen) {
    if (g.images.empty() && !g.timeline)
        return;
    const VkDevice dev = static_cast<VkDevice>(screen.state.device);
    screen.state.device.waitIdle();
    g_presenter->release();
    auto destroy_image = dev_fn<PFN_vkDestroyImage>(dev, "vkDestroyImage");
    auto free_mem = dev_fn<PFN_vkFreeMemory>(dev, "vkFreeMemory");
    for (auto &img : g.images) {
        destroy_image(dev, img.image, nullptr);
        free_mem(dev, img.memory, nullptr);
    }
    LOG_INFO("Vitox external present: released {} images + timeline semaphore (host released first)", g.images.size());
    g.images.clear();
    if (g.timeline) {
        dev_fn<PFN_vkDestroySemaphore>(dev, "vkDestroySemaphore")(dev, g.timeline, nullptr);
        g.timeline = VK_NULL_HANDLE;
    }
    g.value = 0;
    // the device dies with this renderer: unhook (the next device re-initialises the dispatcher anyway)
    if (VULKAN_HPP_DEFAULT_DISPATCHER.vkQueueSubmit == &counting_queue_submit)
        VULKAN_HPP_DEFAULT_DISPATCHER.vkQueueSubmit = g.real_queue_submit;
    if (VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdDrawIndexed == &counting_draw_indexed)
        VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdDrawIndexed = g.real_draw_indexed;
    if (VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdDraw == &counting_draw)
        VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdDraw = g.real_draw;
    if (VULKAN_HPP_DEFAULT_DISPATCHER.vkCreateGraphicsPipelines == &counting_create_pipelines)
        VULKAN_HPP_DEFAULT_DISPATCHER.vkCreateGraphicsPipelines = g.real_create_pipelines;
    if (VULKAN_HPP_DEFAULT_DISPATCHER.vkCreateShaderModule == &counting_create_shader_module)
        VULKAN_HPP_DEFAULT_DISPATCHER.vkCreateShaderModule = g.real_create_shader_module;
}

bool vitox_xp_acquire(ScreenRenderer &screen) {
    if (g.images.empty())
        return false;
    const uint32_t idx = g.next;
    g.next = (g.next + 1) % static_cast<uint32_t>(g.images.size());
    screen.swapchain_image_idx = idx;
    g.stats.acquires++;
    g.other_since_acquire = 0;
    const VkDevice dev = static_cast<VkDevice>(screen.state.device);
    const auto t0 = std::chrono::steady_clock::now();
    bool free = g_presenter->wait_image_free(idx, 100);
    if (!free) {
        // M6C.1: first wait > 100 ms -> ONE detailed snapshot (Vulkan half here, host half from the presenter)
        g.stats.stalls++;
        if (!g.stats.first_stall_frame)
            g.stats.first_stall_frame = g.stats.acquires;
        const bool snap = !g.snapshot_taken;
        g.snapshot_taken = true;
        const uint32_t id = ++g.snapshot_id;
        if (snap) {
            vk_snapshot(screen, id, idx, "first host wait > 100 ms");
            g_presenter->snapshot(id, "first host wait > 100 ms");
        }
        free = g_presenter->wait_image_free(idx, 2900);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (!free) {
            g.stats.stall_unresolved++;
            if (snap) { // the same snapshot 3 s later: did anything advance?
                vk_snapshot(screen, id, idx, "still waiting after 3 s");
                g_presenter->snapshot(id, "still waiting after 3 s");
            }
        }
        LOG_INFO("Vitox external present [M6C.1]: acquire #{} image {} host wait {:.1f} ms -> {} (timeline counter now {})", g.stats.acquires, idx, ms,
            free ? "free (late)" : "GAVE UP", counter_value(dev));
    }
    if (frame_logged(g.stats.acquires))
        LOG_INFO("Vitox external present: acquire #{} -> image {} (host done with it: {}) | timeline counter={} last signalled={}", g.stats.acquires, idx, free,
            counter_value(dev), g.value);
    return free;
}

void vitox_xp_submit_present(ScreenRenderer &screen) {
    auto &st = screen.state;
    const VkDevice dev = static_cast<VkDevice>(st.device);
    const uint32_t idx = screen.swapchain_image_idx;
    VkCommandBuffer cb = static_cast<VkCommandBuffer>(screen.current_cmd_buffer);
    VkFence fence = static_cast<VkFence>(screen.fences[idx]);
    VkQueue queue = static_cast<VkQueue>(st.general_queue);
    auto queue_submit = dev_fn<PFN_vkQueueSubmit>(dev, "vkQueueSubmit");
    uint64_t value = 0, counter_after = 0;
    VkResult r;
    if (g.stats.gpu_sync) {
        value = ++g.value;
        if (value <= g.stats.last_value) // strictly increasing, never reused (M6C.1 ordering check)
            g.stats.value_order_errors++;
        VkTimelineSemaphoreSubmitInfo tsi{ VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO, nullptr, 0, nullptr, 1, &value };
        VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO, &tsi, 0, nullptr, nullptr, 1, &cb, 1, &g.timeline };
        r = queue_submit(queue, 1, &si, fence);
        counter_after = counter_value(dev);
        if (r == VK_SUCCESS && g.variant == VitoxXpVariant::B_CpuWait) {
            // LABELLED CPU WAIT (M6C.1 variant B, diagnostics only): wait for this frame's value on the CPU; the host
            // then presents with no queue Wait on the shared fence
            auto wait_sems = dev_fn<PFN_vkWaitSemaphores>(dev, "vkWaitSemaphoresKHR");
            if (!wait_sems)
                wait_sems = dev_fn<PFN_vkWaitSemaphores>(dev, "vkWaitSemaphores");
            VkSemaphoreWaitInfo wi{ VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, nullptr, 0, 1, &g.timeline, &value };
            const auto t0 = std::chrono::steady_clock::now();
            const VkResult rw = wait_sems(dev, &wi, 3000000000ull);
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            g.stats.cpu_wait_fallback++;
            g.stats.cpu_wait_fallback_ms += ms;
            g.stats.cpu_wait_fallback_max_ms = std::max(g.stats.cpu_wait_fallback_max_ms, ms);
            if (rw != VK_SUCCESS || frame_logged(g.stats.cpu_wait_fallback))
                LOG_INFO("Vitox external present [M6C.1] B: LABELLED CPU wait #{} vkWaitSemaphores(value {}) -> {} in {:.2f} ms (counter {})",
                    g.stats.cpu_wait_fallback, value, vkr(rw), ms, counter_value(dev));
            if (rw != VK_SUCCESS)
                r = rw;
        }
    } else {
        // LABELLED CPU-WAIT FALLBACK (first renderer proof only): wait for this frame on the CPU before presenting
        VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO, nullptr, 0, nullptr, nullptr, 1, &cb, 0, nullptr };
        r = queue_submit(queue, 1, &si, fence);
        if (r == VK_SUCCESS) {
            const auto t0 = std::chrono::steady_clock::now();
            r = dev_fn<PFN_vkWaitForFences>(dev, "vkWaitForFences")(dev, 1, &fence, VK_TRUE, UINT64_MAX);
            g.stats.cpu_wait_fallback++;
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            g.stats.cpu_wait_fallback_ms += ms;
            g.stats.cpu_wait_fallback_max_ms = std::max(g.stats.cpu_wait_fallback_max_ms, ms);
        }
    }
    const uint64_t n = g.stats.submits_ok + g.stats.submits_failed + 1;
    g.ring[g.ring_n++ % g.ring.size()] = SubmitRecord{ n, idx, value, r, counter_after, g.other_since_acquire };
    if (r != VK_SUCCESS) {
        g.stats.submits_failed++;
        LOG_ERROR("Vitox external present: frame {} image {} vkQueueSubmit -> {}", n, idx, vkr(r));
        return;
    }
    g.stats.submits_ok++;
    g.stats.last_value = value;
    {
        // M7B visual, diagnostic only: locate frames whose submit saw no new guest draw (the 2099/2100 note)
        const uint64_t d = g.draw_indexed;
        if (d == g.stats.draws_at_last_present) {
            if (!g.stats.first_no_draw_frame)
                g.stats.first_no_draw_frame = n;
            g.stats.last_no_draw_frame = n;
            if (++g.stats.presents_without_new_draw <= 8)
                LOG_INFO("Vitox [M7B] present frame {}: no new vkCmdDrawIndexed since the previous present (draw total {})", n, d);
        }
        g.stats.draws_at_last_present = d;
        g.stats.last_present_frame = n;
    }
    // variant B: the CPU already waited for `value`, so the host gets 0 (no queue Wait)
    const bool presented = g_presenter->present(idx, g.variant == VitoxXpVariant::B_CpuWait ? 0 : value);
    presented ? g.stats.presents_ok++ : g.stats.presents_failed++;
    if (frame_logged(n) || !presented)
        LOG_INFO("Vitox external present: frame {} image {} vkQueueSubmit={} timeline value={} counter right after submit={} other submits since acquire={} "
                 "host present={}",
            n, idx, vkr(r), value, counter_after, g.other_since_acquire, presented);
}
