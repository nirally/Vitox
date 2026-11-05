// Vitox M7B.6 DIAGNOSTIC (VITOX_UWP, logging only) - see vitox_m7b6_diag.h.
#include <vitox_m7b6_diag.h>

#include <util/log.h>

#include <spirv_glsl.hpp>

#include <sstream>

static uint64_t vitox_scene = 0;
static uint64_t vitox_first_draw_taken = 0;
static unsigned vitox_pipelines_logged = 0;
static unsigned vitox_shaders_logged = 0;

uint64_t vitox_m7b6_begin_scene() {
    return ++vitox_scene;
}

uint64_t vitox_m7b6_scene() {
    return vitox_scene;
}

bool vitox_m7b6_diag_enabled() {
#ifdef VITOX_DIAG_M7B
    return true;
#else
    return false; // M8A: the per-scene GPU-state logging is off by default
#endif
}

bool vitox_m7b6_sampled() {
    return vitox_m7b6_diag_enabled() && (vitox_scene <= 3 || vitox_scene == 60 || vitox_scene == 600 || vitox_scene == 2000);
}

bool vitox_m7b6_take_first_draw() {
    if (!vitox_m7b6_sampled() || vitox_first_draw_taken == vitox_scene)
        return false;
    vitox_first_draw_taken = vitox_scene;
    return true;
}

const char *vitox_m7b6_load_op(vk::AttachmentLoadOp op) {
    switch (op) {
    case vk::AttachmentLoadOp::eLoad: return "LOAD";
    case vk::AttachmentLoadOp::eClear: return "CLEAR";
    case vk::AttachmentLoadOp::eDontCare: return "DONT_CARE";
    default: return "other";
    }
}

const char *vitox_m7b6_store_op(vk::AttachmentStoreOp op) {
    switch (op) {
    case vk::AttachmentStoreOp::eStore: return "STORE";
    case vk::AttachmentStoreOp::eDontCare: return "DONT_CARE";
    default: return "other";
    }
}

void vitox_m7b6_log_render_pass(vk::RenderPass rp, vk::Format format, bool force_load, bool force_store, bool is_color_transient, bool no_color,
    vk::Format ds_format, const vk::AttachmentDescription &color, const vk::AttachmentDescription &ds) {
    if (!vitox_m7b6_diag_enabled())
        return;
    LOG_INFO("Vitox [M7B.6] render pass created {}: format {} force_load {} force_store {} color_transient {} no_color {}",
        static_cast<void *>(static_cast<VkRenderPass>(rp)), static_cast<int>(format), force_load, force_store, is_color_transient, no_color);
    if (!no_color)
        LOG_INFO("Vitox [M7B.6]   colour attachment: format {} samples {} load {} store {} layout {} -> {} (subpass ref layout General, also input attachment 0)",
            static_cast<int>(color.format), static_cast<int>(color.samples), vitox_m7b6_load_op(color.loadOp), vitox_m7b6_store_op(color.storeOp),
            static_cast<int>(color.initialLayout), static_cast<int>(color.finalLayout));
    LOG_INFO("Vitox [M7B.6]   depth-stencil attachment: format {} (state.deep_stencil_use {}) load {} store {} stencil load {} store {} layout {} -> {}",
        static_cast<int>(ds.format), static_cast<int>(ds_format), vitox_m7b6_load_op(ds.loadOp), vitox_m7b6_store_op(ds.storeOp),
        vitox_m7b6_load_op(ds.stencilLoadOp), vitox_m7b6_store_op(ds.stencilStoreOp), static_cast<int>(ds.initialLayout), static_cast<int>(ds.finalLayout));
}

static void log_stencil_op(const char *side, const vk::StencilOpState &s) {
    LOG_INFO("Vitox [M7B.6]   stencil {}: compare {} fail {} pass {} depthFail {} (static compareMask {} writeMask {} ref {}; all three are dynamic)", side,
        static_cast<int>(s.compareOp), static_cast<int>(s.failOp), static_cast<int>(s.passOp), static_cast<int>(s.depthFailOp), s.compareMask, s.writeMask,
        s.reference);
}

void vitox_m7b6_log_pipeline(vk::Pipeline pipeline, vk::Result result, const vk::GraphicsPipelineCreateInfo &info, const std::string &gxm_notes) {
    if (!vitox_m7b6_diag_enabled() || vitox_pipelines_logged++ >= 16)
        return;
    LOG_INFO("Vitox [M7B.6] pipeline #{} {} result {} (scene {}): stages {} layout {} renderPass {} subpass {} | {}", vitox_pipelines_logged,
        static_cast<void *>(static_cast<VkPipeline>(pipeline)), static_cast<int>(result), vitox_scene, info.stageCount,
        static_cast<void *>(static_cast<VkPipelineLayout>(info.layout)), static_cast<void *>(static_cast<VkRenderPass>(info.renderPass)), info.subpass, gxm_notes);
    for (uint32_t i = 0; i < info.stageCount; i++)
        LOG_INFO("Vitox [M7B.6]   stage {}: {} module {} entry {} specialization {}", i, static_cast<uint32_t>(info.pStages[i].stage),
            static_cast<void *>(static_cast<VkShaderModule>(info.pStages[i].module)), info.pStages[i].pName ? info.pStages[i].pName : "(null)",
            info.pStages[i].pSpecializationInfo ? "yes" : "no");
    if (const auto *vi = info.pVertexInputState) {
        for (uint32_t i = 0; i < vi->vertexBindingDescriptionCount; i++)
            LOG_INFO("Vitox [M7B.6]   vertex binding {}: stride {} rate {}", vi->pVertexBindingDescriptions[i].binding, vi->pVertexBindingDescriptions[i].stride,
                static_cast<int>(vi->pVertexBindingDescriptions[i].inputRate));
        for (uint32_t i = 0; i < vi->vertexAttributeDescriptionCount; i++)
            LOG_INFO("Vitox [M7B.6]   vertex attribute: location {} binding {} format {} offset {}", vi->pVertexAttributeDescriptions[i].location,
                vi->pVertexAttributeDescriptions[i].binding, static_cast<int>(vi->pVertexAttributeDescriptions[i].format), vi->pVertexAttributeDescriptions[i].offset);
    }
    if (const auto *ia = info.pInputAssemblyState)
        LOG_INFO("Vitox [M7B.6]   topology {} primitiveRestart {}", static_cast<int>(ia->topology), ia->primitiveRestartEnable);
    if (const auto *rs = info.pRasterizationState)
        LOG_INFO("Vitox [M7B.6]   raster: depthClamp {} discard {} polygonMode {} cullMode {} frontFace {} depthBiasEnable {} (constant/clamp/slope dynamic) lineWidth {}",
            rs->depthClampEnable, rs->rasterizerDiscardEnable, static_cast<int>(rs->polygonMode), static_cast<uint32_t>(rs->cullMode),
            static_cast<int>(rs->frontFace), rs->depthBiasEnable, rs->lineWidth);
    if (const auto *ms = info.pMultisampleState)
        LOG_INFO("Vitox [M7B.6]   samples {} sampleShading {} alphaToCoverage {}", static_cast<uint32_t>(ms->rasterizationSamples), ms->sampleShadingEnable,
            ms->alphaToCoverageEnable);
    if (const auto *ds = info.pDepthStencilState) {
        LOG_INFO("Vitox [M7B.6]   depth: test {} write {} compare {} boundsTest {} | stencil test {}", ds->depthTestEnable, ds->depthWriteEnable,
            static_cast<int>(ds->depthCompareOp), ds->depthBoundsTestEnable, ds->stencilTestEnable);
        log_stencil_op("front", ds->front);
        log_stencil_op("back", ds->back);
    }
    if (const auto *cb = info.pColorBlendState) {
        LOG_INFO("Vitox [M7B.6]   colour blend: flags {} logicOp enable {} op {} attachments {}", static_cast<uint32_t>(cb->flags), cb->logicOpEnable,
            static_cast<int>(cb->logicOp), cb->attachmentCount);
        for (uint32_t i = 0; i < cb->attachmentCount; i++) {
            const auto &a = cb->pAttachments[i];
            LOG_INFO("Vitox [M7B.6]   attachment {}: writeMask 0x{:X} blend {} colour {}*src {} {}*dst | alpha {}*src {} {}*dst", i, static_cast<uint32_t>(a.colorWriteMask),
                a.blendEnable, static_cast<int>(a.srcColorBlendFactor), static_cast<int>(a.colorBlendOp), static_cast<int>(a.dstColorBlendFactor),
                static_cast<int>(a.srcAlphaBlendFactor), static_cast<int>(a.alphaBlendOp), static_cast<int>(a.dstAlphaBlendFactor));
        }
    }
    if (const auto *dyn = info.pDynamicState) {
        std::string s;
        for (uint32_t i = 0; i < dyn->dynamicStateCount; i++)
            s += fmt::format("{}{}", i ? "," : "", static_cast<int>(dyn->pDynamicStates[i]));
        LOG_INFO("Vitox [M7B.6]   dynamic states: {}", s);
    }
}

void vitox_m7b6_log_shader(bool is_vertex, const std::string &hash, const std::vector<uint32_t> &spirv) {
    if (!vitox_m7b6_diag_enabled() || vitox_shaders_logged++ >= 4)
        return;
    LOG_INFO("Vitox [M7B.6] {} shader {}: Vita3K SPIR-V {} words", is_vertex ? "vertex" : "fragment", hash, spirv.size());
    std::string glsl;
    try {
        spirv_cross::CompilerGLSL compiler(spirv);
        spirv_cross::CompilerGLSL::Options options;
        options.version = 450;
        options.es = false;
        options.vulkan_semantics = true;
        compiler.set_common_options(options);
        glsl = compiler.compile();
    } catch (const std::exception &e) {
        LOG_INFO("Vitox [M7B.6]   SPIRV-Cross failed: {}", e.what());
        return;
    }
    std::istringstream lines(glsl);
    std::string line;
    unsigned n = 0;
    while (std::getline(lines, line) && n < 400)
        LOG_INFO("Vitox [M7B.6]   {}{:03} | {}", is_vertex ? "VS" : "FS", ++n, line);
}

// ---- M9A.10 DARKNESS (0.0.24.125, read-only): see vitox_m7b6_diag.h ----
#include <gxm/functions.h>
#include <gxm/types.h>
#include <shader/gxp_parser.h>

#define NOMINMAX
#include <windows.h>

#include <cstdio>
#include <mutex>

void vitox_shader_dump(const SceGxmProgram &program, bool is_vertex, const std::string &hash, const std::vector<uint32_t> &spirv) {
    static std::mutex m;
    static int n = 0;
    std::lock_guard<std::mutex> g(m);
    static std::vector<std::string> seen;
    if (n >= 400) // 0.0.24.129: every program (vertex and fragment), appended to one file
        return;
    for (const std::string &h : seen)
        if (h == hash)
            return;
    seen.push_back(hash);
    wchar_t dir[400];
    const DWORD len = GetEnvironmentVariableW(L"VITOX_LOCALSTATE", dir, 400);
    if (len == 0 || len >= 400)
        return;
    wchar_t path[512];
    swprintf(path, 512, L"%s\\shd_all.txt", dir);
    n++;
    FILE *f = _wfopen(path, L"a");
    if (!f)
        return;
    fprintf(f, "==== PROGRAM\n");
    fprintf(f, "hash %s gxp_size %u vertex %d program_flags 0x%x buffer_flags 0x%x parameters %u\n", hash.c_str(), program.size,
        is_vertex, program.program_flags, program.buffer_flags, program.parameter_count);
    const SceGxmProgramParameter *params = program.program_parameters();
    for (uint32_t i = 0; i < program.parameter_count; i++) {
        const SceGxmProgramParameter &p = params[i];
        fprintf(f, "param %3u cat %u type %u comps %u container %u semantic %u array %u resource %u name %s\n", i, (unsigned)p.category,
            (unsigned)p.type, (unsigned)p.component_count, (unsigned)p.container_index, (unsigned)p.semantic, p.array_size, p.resource_index,
            gxp::parameter_name(p).c_str());
    }
    const shader::usse::ProgramInput in = shader::get_program_input(program);
    for (const auto &ub : in.uniform_buffers)
        fprintf(f, "ub index %u size_f32 %u reg_start %u reg_block %u\n", ub.index, ub.size, ub.reg_start_offset, ub.reg_block_size);
    try {
        spirv_cross::CompilerGLSL compiler(spirv);
        spirv_cross::CompilerGLSL::Options options;
        options.version = 450;
        options.es = false;
        options.vulkan_semantics = true;
        compiler.set_common_options(options);
        const std::string glsl = compiler.compile();
        fputs("---- GLSL ----\n", f);
        fwrite(glsl.data(), 1, glsl.size(), f);
    } catch (const std::exception &e) {
        fprintf(f, "SPIRV-Cross failed: %s\n", e.what());
    }
    fclose(f);
}
