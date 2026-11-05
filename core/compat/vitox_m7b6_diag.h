// Vitox M7B.6 DIAGNOSTIC (VITOX_UWP, logging only): GPU state of the guest scene draw. Called only from the guarded
// substitutions in renderer/src/vulkan/{context,scene,pipeline_cache}.cpp. Nothing here changes Vulkan state, the
// cache, the surfaces or the guest data; every function only reads its arguments and logs.
#pragma once

#include <vkutil/objects.h>

#include <cstdint>
#include <string>
#include <vector>

// Scene counter: advanced once per set_context (one per guest sceGxmBeginScene). Sampled scenes: 1..3, 60, 600, 2000.
// M8A: false unless built with VITOX_DIAG_M7B (CMake option, default OFF); the scene counter itself always runs.
bool vitox_m7b6_diag_enabled();
uint64_t vitox_m7b6_begin_scene();
uint64_t vitox_m7b6_scene();
bool vitox_m7b6_sampled();
// true once per sampled scene, at its first guest draw
bool vitox_m7b6_take_first_draw();

const char *vitox_m7b6_load_op(vk::AttachmentLoadOp op);
const char *vitox_m7b6_store_op(vk::AttachmentStoreOp op);

void vitox_m7b6_log_render_pass(vk::RenderPass rp, vk::Format format, bool force_load, bool force_store, bool is_color_transient, bool no_color,
    vk::Format ds_format, const vk::AttachmentDescription &color, const vk::AttachmentDescription &ds);
// full create-info of a graphics pipeline (after creation, with its result)
void vitox_m7b6_log_pipeline(vk::Pipeline pipeline, vk::Result result, const vk::GraphicsPipelineCreateInfo &info, const std::string &gxm_notes);
// the SPIR-V Vita3K generates for a GXP (recomputed with the same converter, never cached or written), as GLSL
void vitox_m7b6_log_shader(bool is_vertex, const std::string &hash, const std::vector<uint32_t> &spirv);
// M9A.10 DARKNESS (0.0.24.125, read-only): the parameter table, Vita3K's uniform buffer sizes and the full GLSL of up to
// 10 large vertex programs, written to %VITOX_LOCALSTATE%\shd_NN.txt
struct SceGxmProgram;
void vitox_shader_dump(const SceGxmProgram &program, bool is_vertex, const std::string &hash, const std::vector<uint32_t> &spirv);
