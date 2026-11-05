// Vitox 0.0.24.173 (stutter, user-chosen option 1): pipeline pre-warm at boot.
//
// On Xbox every first use of a pipeline in a session stalls a frame: the D3D12 driver compiles each PSO to GPU code when it is
// created, nothing is persisted across sessions, and dzn creates the PSO of a pipeline with dynamic stencil masks / depth bias
// (all Vita3K pipelines) only at its first draw (one "variant" per mask/bias combination).
//
// This file records, for every pipeline Vita3K creates, the exact Vulkan create-info inputs (a "recipe", keyed by Vita3K's own
// pipeline key) plus each dzn variant key the pipeline needed at draw time (dzn callback), and saves them next to the SPIR-V
// shader cache. At boot (read_pipeline_cache), every recipe whose SPIR-V modules are cached is rebuilt on worker threads with its
// variants, and the finished pipelines are placed in Vita3K's pipeline map before the game draws, so the game finds them ready.
//
// Included once, by the patched renderer/src/vulkan/pipeline_cache.cpp (VITOX_UWP only).
#pragma once

#include <vitox_vk_entry.h>
#include "vitox_external_present.h" // 0.0.24.198 loading screen

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace vitox_prewarm {

constexpr uint32_t file_magic = 0x57505856; // "VXPW"
constexpr uint32_t file_version = 1;
constexpr uint32_t max_bindings = 16, max_attributes = 64, max_variants = 64, max_dynamic = 8;
constexpr int64_t time_budget_ms = 120000; // never hold the boot longer than this

struct RenderPassParams {
    uint32_t format = 0, force_load = 0, force_store = 0, is_color_transient = 0, no_color = 0;
    uint32_t valid = 0;
};

// fixed part of a recipe (plain integers only, written as-is)
struct Head {
    uint64_t key;
    uint8_t vs_hash[32];
    uint8_t fs_hash[32];
    uint32_t stage_count;
    uint32_t fs_spec; // 0: no specialization info, 1: gamma correction false, 2: true
    uint32_t topology;
    uint32_t polygon_mode, cull_mode, front_face, depth_bias_enable, line_width_bits;
    uint32_t depth_test, depth_write, depth_compare, stencil_test;
    VkStencilOpState front, back;
    uint32_t blend_flags, blend_count;
    VkPipelineColorBlendAttachmentState blend;
    uint32_t vert_texture_count, frag_texture_count;
    RenderPassParams rp;
    uint32_t dynamic_count;
    uint32_t dynamic_states[max_dynamic];
    uint32_t n_bindings, n_attributes, n_variants;
};
static_assert(std::is_trivially_copyable_v<Head>);

struct Recipe {
    Head h{};
    std::vector<VkVertexInputBindingDescription> bindings;
    std::vector<VkVertexInputAttributeDescription> attributes;
    std::vector<std::vector<uint8_t>> variants;
};

struct Store {
    std::mutex m;
    std::unordered_map<uint64_t, Recipe> recipes;
    std::unordered_map<uint64_t, uint64_t> pipeline_to_key; // VkPipeline handle -> recipe key
    uint32_t key_size = 0; // dzn variant key size, 0 when the DLL has no pre-warm exports
    bool loaded = false;
    bool dirty = false;
    uint64_t *save_at = nullptr; // PipelineCache::next_pipeline_cache_save
    unsigned variants_recorded = 0;
};

inline Store &store() {
    static Store s;
    return s;
}

// pipeline key + render pass of the pipeline being compiled on this thread (set by retrieve_pipeline / compiler_thread)
struct Context {
    uint64_t key = 0;
    RenderPassParams rp{};
};
inline thread_local Context tl_context;

inline uint64_t now_s() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

// dzn: a pipeline needed a new variant at draw time (render thread)
inline void on_variant(uint64_t pipeline, const void *key, uint32_t size) {
    Store &s = store();
    std::lock_guard<std::mutex> lock(s.m);
    if (size == 0 || size != s.key_size)
        return;
    const auto pk = s.pipeline_to_key.find(pipeline);
    if (pk == s.pipeline_to_key.end())
        return; // not a recorded pipeline (e.g. the default variant dzn builds while the pipeline is created)
    const auto it = s.recipes.find(pk->second);
    if (it == s.recipes.end() || it->second.variants.size() >= max_variants)
        return;
    const uint8_t *k = static_cast<const uint8_t *>(key);
    for (const auto &v : it->second.variants)
        if (v.size() == size && std::memcmp(v.data(), k, size) == 0)
            return;
    it->second.variants.emplace_back(k, k + size);
    s.dirty = true;
    s.variants_recorded++;
    // the pipeline cache timer then also saves the recipes
    if (s.save_at && *s.save_at == std::numeric_limits<uint64_t>::max())
        *s.save_at = now_s() + 15;
}

inline void ensure_callback() {
    static std::once_flag once;
    std::call_once(once, [] {
        const VitoxDznPrewarmApi &api = vitox_dzn_prewarm_api();
        if (api.set_variant_callback && api.variant_key_size && api.prebuild_variant) {
            store().key_size = api.variant_key_size();
            api.set_variant_callback(&on_variant);
        }
        LOG_INFO("Vitox pre-warm: dzn variant hooks {} (key size {})", store().key_size ? "on" : "absent", store().key_size);
    });
}

// a pipeline was just created by compile_pipeline (any thread)
inline void record(uint64_t *save_at, VkPipeline pipeline, const VkGraphicsPipelineCreateInfo &pi, const uint8_t *vs_hash, const uint8_t *fs_hash,
    uint32_t vert_texture_count, uint32_t frag_texture_count) {
    const Context ctx = tl_context;
    tl_context = {};
    if (!pipeline || !ctx.key || !ctx.rp.valid)
        return;
    const VkPipelineVertexInputStateCreateInfo &vi = *pi.pVertexInputState;
    const VkPipelineColorBlendStateCreateInfo &cb = *pi.pColorBlendState;
    if (vi.vertexBindingDescriptionCount > max_bindings || vi.vertexAttributeDescriptionCount > max_attributes || cb.attachmentCount > 1
        || pi.pDynamicState->dynamicStateCount > max_dynamic || pi.stageCount < 1 || pi.stageCount > 2)
        return;
    ensure_callback();

    Recipe r;
    Head &h = r.h;
    h.key = ctx.key;
    std::memcpy(h.vs_hash, vs_hash, 32);
    std::memcpy(h.fs_hash, fs_hash, 32);
    h.stage_count = pi.stageCount;
    h.fs_spec = 0;
    if (pi.stageCount == 2 && pi.pStages[1].pSpecializationInfo)
        h.fs_spec = *static_cast<const uint32_t *>(pi.pStages[1].pSpecializationInfo->pData) ? 2 : 1;
    h.topology = pi.pInputAssemblyState->topology;
    const VkPipelineRasterizationStateCreateInfo &rs = *pi.pRasterizationState;
    h.polygon_mode = rs.polygonMode;
    h.cull_mode = rs.cullMode;
    h.front_face = rs.frontFace;
    h.depth_bias_enable = rs.depthBiasEnable;
    std::memcpy(&h.line_width_bits, &rs.lineWidth, 4);
    const VkPipelineDepthStencilStateCreateInfo &ds = *pi.pDepthStencilState;
    h.depth_test = ds.depthTestEnable;
    h.depth_write = ds.depthWriteEnable;
    h.depth_compare = ds.depthCompareOp;
    h.stencil_test = ds.stencilTestEnable;
    h.front = ds.front;
    h.back = ds.back;
    h.blend_flags = cb.flags;
    h.blend_count = cb.attachmentCount;
    if (cb.attachmentCount)
        h.blend = cb.pAttachments[0];
    h.vert_texture_count = vert_texture_count;
    h.frag_texture_count = frag_texture_count;
    h.rp = ctx.rp;
    h.dynamic_count = pi.pDynamicState->dynamicStateCount;
    for (uint32_t i = 0; i < h.dynamic_count; i++)
        h.dynamic_states[i] = pi.pDynamicState->pDynamicStates[i];
    r.bindings.assign(vi.pVertexBindingDescriptions, vi.pVertexBindingDescriptions + vi.vertexBindingDescriptionCount);
    r.attributes.assign(vi.pVertexAttributeDescriptions, vi.pVertexAttributeDescriptions + vi.vertexAttributeDescriptionCount);

    Store &s = store();
    std::lock_guard<std::mutex> lock(s.m);
    s.save_at = save_at;
    auto it = s.recipes.find(ctx.key);
    if (it != s.recipes.end())
        r.variants = std::move(it->second.variants); // same key: keep what was learnt
    s.recipes[ctx.key] = std::move(r);
    s.pipeline_to_key[reinterpret_cast<uint64_t>(pipeline)] = ctx.key;
    s.dirty = true;
}

template <typename T>
inline bool read_pod(std::ifstream &f, T &v) {
    return static_cast<bool>(f.read(reinterpret_cast<char *>(&v), sizeof(T)));
}
template <typename T>
inline void write_pod(std::ofstream &f, const T &v) {
    f.write(reinterpret_cast<const char *>(&v), sizeof(T));
}

inline std::wstring file_path(const std::wstring &dir) {
    return dir + L"\\vitox-pipelines-v1.bin";
}

// caller holds s.m
inline void load_locked(Store &s, const std::wstring &dir) {
    s.loaded = true;
    std::ifstream f(file_path(dir), std::ios::binary);
    if (!f.is_open())
        return;
    uint32_t magic = 0, version = 0, key_size = 0, count = 0;
    if (!read_pod(f, magic) || !read_pod(f, version) || !read_pod(f, key_size) || !read_pod(f, count) || magic != file_magic || version != file_version
        || key_size > 256 || count > 100000) {
        LOG_WARN("Vitox pre-warm: recipe file ignored (bad header)");
        return;
    }
    for (uint32_t i = 0; i < count; i++) {
        Recipe r;
        if (!read_pod(f, r.h) || r.h.n_bindings > max_bindings || r.h.n_attributes > max_attributes || r.h.n_variants > max_variants
            || r.h.dynamic_count > max_dynamic || r.h.stage_count < 1 || r.h.stage_count > 2 || r.h.blend_count > 1
            || r.h.vert_texture_count > 16 || r.h.frag_texture_count > 16) {
            LOG_WARN("Vitox pre-warm: recipe file truncated or corrupt at entry {}; keeping {} entries", i, s.recipes.size());
            return;
        }
        r.bindings.resize(r.h.n_bindings);
        r.attributes.resize(r.h.n_attributes);
        bool ok = true;
        for (auto &b : r.bindings)
            ok = ok && read_pod(f, b);
        for (auto &a : r.attributes)
            ok = ok && read_pod(f, a);
        for (uint32_t v = 0; v < r.h.n_variants && ok; v++) {
            std::vector<uint8_t> k(key_size);
            ok = static_cast<bool>(f.read(reinterpret_cast<char *>(k.data()), key_size));
            if (key_size == s.key_size) // a DLL with another key layout: drop the variants, keep the pipeline
                r.variants.push_back(std::move(k));
        }
        if (!ok) {
            LOG_WARN("Vitox pre-warm: recipe file truncated at entry {}; keeping {} entries", i, s.recipes.size());
            return;
        }
        s.recipes[r.h.key] = std::move(r);
    }
}

// from PipelineCache::save_pipeline_cache (pipeline cache timer / exit)
inline void save(const std::wstring &dir) {
    Store &s = store();
    std::lock_guard<std::mutex> lock(s.m);
    if (!s.dirty || dir.empty())
        return;
    const std::wstring path = file_path(dir), tmp = path + L".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f.is_open())
            return;
        write_pod(f, file_magic);
        write_pod(f, file_version);
        write_pod(f, s.key_size);
        write_pod(f, static_cast<uint32_t>(s.recipes.size()));
        for (auto &[key, r] : s.recipes) {
            Head h = r.h;
            h.n_bindings = static_cast<uint32_t>(r.bindings.size());
            h.n_attributes = static_cast<uint32_t>(r.attributes.size());
            h.n_variants = s.key_size ? static_cast<uint32_t>(r.variants.size()) : 0;
            write_pod(f, h);
            for (const auto &b : r.bindings)
                write_pod(f, b);
            for (const auto &a : r.attributes)
                write_pod(f, a);
            for (uint32_t v = 0; v < h.n_variants; v++)
                f.write(reinterpret_cast<const char *>(r.variants[v].data()), s.key_size);
        }
        f.flush();
        if (!f.good())
            return;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        LOG_WARN("Vitox pre-warm: saving recipes failed ({})", ec.message());
        return;
    }
    s.dirty = false;
    LOG_INFO("Vitox pre-warm: saved {} pipeline recipes ({} variants learnt this session)", s.recipes.size(), s.variants_recorded);
}

// From PipelineCache::read_pipeline_cache (boot, before the game's first draw; no other renderer thread uses the cache yet).
// get_shader(hash) -> vk::ShaderModule from the SPIR-V disk cache or null; get_render_pass(params) -> vk::RenderPass;
// get_layout(vert, frag) -> vk::PipelineLayout; publish(key, VkPipeline) stores a finished pipeline.
template <typename GetShader, typename GetRenderPass, typename GetLayout, typename Publish>
inline void run(uint64_t *save_at, const vk::Device &device, const vk::PipelineCache &cache, const std::wstring &dir, uint32_t gamma_spec_id,
    GetShader get_shader, GetRenderPass get_render_pass, GetLayout get_layout, Publish publish) {
    ensure_callback();
    const auto t0 = std::chrono::steady_clock::now();
    Store &s = store();

    struct Job {
        const Recipe *r;
        VkShaderModule vs, fs;
        VkRenderPass rp;
        VkPipelineLayout layout;
        VkPipeline result = VK_NULL_HANDLE;
        uint32_t variants_ok = 0;
    };
    std::vector<Job> jobs;
    unsigned no_shader = 0;
    {
        std::lock_guard<std::mutex> lock(s.m);
        s.save_at = save_at;
        if (!s.loaded)
            load_locked(s, dir);
        if (!vitox_prewarm_enabled()) { // Vitox 0.0.28.3: Settings > Shader Compilation; the store stays loaded so recording keeps every recipe
            LOG_INFO("Vitox pre-warm: disabled in Settings ({} recipes kept, none built at boot)", s.recipes.size());
            return;
        }
        jobs.reserve(s.recipes.size());
        for (const auto &[key, r] : s.recipes) {
            Job j{ &r };
            j.vs = get_shader(r.h.vs_hash);
            j.fs = r.h.stage_count == 2 ? get_shader(r.h.fs_hash) : VK_NULL_HANDLE;
            if (!j.vs || (r.h.stage_count == 2 && !j.fs)) {
                no_shader++;
                continue;
            }
            j.rp = get_render_pass(r.h.rp);
            j.layout = get_layout(r.h.vert_texture_count, r.h.frag_texture_count);
            if (j.rp && j.layout)
                jobs.push_back(j);
        }
    }
    if (jobs.empty()) {
        LOG_INFO("Vitox pre-warm: nothing to build ({} recipes, {} without cached shaders)", s.recipes.size(), no_shader);
        return;
    }

    const VitoxDznPrewarmApi &api = vitox_dzn_prewarm_api();
    const uint32_t key_size = s.key_size;
    static const uint32_t spec_true = VK_TRUE, spec_false = VK_FALSE;
    const VkSpecializationMapEntry spec_entry{ gamma_spec_id, 0, sizeof(uint32_t) };
    const VkSpecializationInfo spec_info_true{ 1, &spec_entry, sizeof(uint32_t), &spec_true };
    const VkSpecializationInfo spec_info_false{ 1, &spec_entry, sizeof(uint32_t), &spec_false };

    std::atomic<size_t> next{ 0 };
    std::atomic<size_t> finished{ 0 }; // 0.0.24.198: jobs completed (built or not), for the loading screen
    std::atomic<unsigned> running{ 0 };
    std::atomic<bool> out_of_time{ false };
    struct FinishOne {
        std::atomic<size_t> &f;
        ~FinishOne() { f.fetch_add(1); }
    };
    const auto worker = [&]() {
        struct Exit {
            std::atomic<unsigned> &r;
            ~Exit() { r.fetch_sub(1); }
        } exit_guard{ running };
        for (size_t i; (i = next.fetch_add(1)) < jobs.size();) {
            FinishOne finish_guard{ finished };
            if (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count() > time_budget_ms) {
                out_of_time = true;
                break;
            }
            Job &j = jobs[i];
            const Head &h = j.r->h;
            VkPipelineShaderStageCreateInfo stages[2] = {
                { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, j.vs, "main_vs", nullptr },
                { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, j.fs, "main_fs",
                    h.fs_spec == 2 ? &spec_info_true : (h.fs_spec == 1 ? &spec_info_false : nullptr) },
            };
            VkPipelineVertexInputStateCreateInfo vi{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
            vi.vertexBindingDescriptionCount = static_cast<uint32_t>(j.r->bindings.size());
            vi.pVertexBindingDescriptions = j.r->bindings.data();
            vi.vertexAttributeDescriptionCount = static_cast<uint32_t>(j.r->attributes.size());
            vi.pVertexAttributeDescriptions = j.r->attributes.data();
            VkPipelineInputAssemblyStateCreateInfo ia{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
            ia.topology = static_cast<VkPrimitiveTopology>(h.topology);
            VkPipelineViewportStateCreateInfo vp{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
            vp.viewportCount = 1;
            vp.scissorCount = 1;
            VkPipelineRasterizationStateCreateInfo rs{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
            rs.polygonMode = static_cast<VkPolygonMode>(h.polygon_mode);
            rs.cullMode = h.cull_mode;
            rs.frontFace = static_cast<VkFrontFace>(h.front_face);
            rs.depthBiasEnable = h.depth_bias_enable;
            std::memcpy(&rs.lineWidth, &h.line_width_bits, 4);
            VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
            ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
            VkPipelineDepthStencilStateCreateInfo ds{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
            ds.depthTestEnable = h.depth_test;
            ds.depthWriteEnable = h.depth_write;
            ds.depthCompareOp = static_cast<VkCompareOp>(h.depth_compare);
            ds.stencilTestEnable = h.stencil_test;
            ds.front = h.front;
            ds.back = h.back;
            VkPipelineColorBlendStateCreateInfo cb{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
            cb.flags = h.blend_flags;
            cb.attachmentCount = h.blend_count;
            cb.pAttachments = &h.blend;
            VkDynamicState dyn[max_dynamic];
            for (uint32_t d = 0; d < h.dynamic_count; d++)
                dyn[d] = static_cast<VkDynamicState>(h.dynamic_states[d]);
            VkPipelineDynamicStateCreateInfo dy{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
            dy.dynamicStateCount = h.dynamic_count;
            dy.pDynamicStates = dyn;
            VkGraphicsPipelineCreateInfo info{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
            info.stageCount = h.stage_count;
            info.pStages = stages;
            info.pVertexInputState = &vi;
            info.pInputAssemblyState = &ia;
            info.pViewportState = &vp;
            info.pRasterizationState = &rs;
            info.pMultisampleState = &ms;
            info.pDepthStencilState = &ds;
            info.pColorBlendState = &cb;
            info.pDynamicState = &dy;
            info.layout = j.layout;
            info.renderPass = j.rp;
            info.subpass = 0;

            const auto res = device.createGraphicsPipeline(cache, *reinterpret_cast<const vk::GraphicsPipelineCreateInfo *>(&info));
            if (res.result != vk::Result::eSuccess)
                continue;
            j.result = static_cast<VkPipeline>(res.value);
            if (api.prebuild_variant && key_size)
                for (const auto &v : j.r->variants)
                    j.variants_ok += api.prebuild_variant(reinterpret_cast<uint64_t>(j.result), v.data(), key_size) ? 1 : 0;
        }
    };
    const unsigned hc = std::max(2u, std::thread::hardware_concurrency());
    const unsigned n_threads = std::clamp(hc - 1, 1u, 6u);
    std::vector<std::thread> threads;
    running = n_threads;
    for (unsigned t = 0; t < n_threads; t++)
        threads.emplace_back(worker);
    // 0.0.24.198: this is the launcher thread; show the loading screen (and keep the window pumped) until the workers end
    unsigned loading_frames = 0;
    while (running.load() != 0) {
        vitox_loading_progress(static_cast<uint32_t>(std::min(finished.load(), jobs.size())), static_cast<uint32_t>(jobs.size()));
        loading_frames++;
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    vitox_loading_progress(static_cast<uint32_t>(jobs.size()), static_cast<uint32_t>(jobs.size())); // the full bar stays until the game's first frame
    for (auto &t : threads)
        t.join();
    LOG_INFO("Vitox pre-warm: loading screen {} frames", loading_frames + 1);

    unsigned built = 0, failed = 0, variants = 0, variants_ok = 0;
    {
        std::lock_guard<std::mutex> lock(s.m);
        for (const Job &j : jobs) {
            variants += static_cast<unsigned>(j.r->variants.size());
            if (!j.result) {
                failed++;
                continue;
            }
            built++;
            variants_ok += j.variants_ok;
            s.pipeline_to_key[reinterpret_cast<uint64_t>(j.result)] = j.r->h.key;
            publish(j.r->h.key, j.result);
        }
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    LOG_INFO("Vitox pre-warm: built {}/{} pipelines (+{}/{} D3D12 variants) in {} ms on {} threads | {} without cached shaders, {} failed/skipped{}",
        built, jobs.size(), variants_ok, variants, ms, n_threads, no_shader, failed, out_of_time ? " (time budget reached)" : "");
}

} // namespace vitox_prewarm
