// Vitox M7B Phase A - NON-VISUAL shader generation + validation: our own deterministic shader source -> open-shacccg
// (MIT, pinned, compiled into the signed package) -> tri_v.gxp / tri_f.gxp -> Vita3K's real GXP checks + recompiler +
// Vulkan shader-module creation on the dzn device.
#pragma once

#include "vitox_core_probe.h" // VitoxCoreHost

#include <vitox_external_present.h>

#include <cstdint>

struct VitoxShaderGenResult {
    bool generated;     // open-shacccg vsc_compile_spirv produced both programs, 0 error diagnostics
    bool deterministic; // a second compile of the same source produced byte-identical GXP
    bool program_check; // Vita3K export sceGxmProgramCheck == 0 for both (programs in guest memory)
    bool parameters;    // Vita3K export sceGxmProgramFindParameterByName finds aPosition / aColor / wvp (vertex), resource indices logged
    bool types;         // Vita3K SceGxmProgram::is_vertex / is_fragment match
    bool recompiled;    // Vita3K shader::convert_gxp -> SPIR-V (Vulkan target) + GLSL, non-empty, no exception
    bool vk_modules;    // vkCreateShaderModule on the real VKState device (dzn) succeeded for both
    bool written;       // .spv / .gxp / generated GLSL + source listing written to LocalState\m7b with SHA-256s
    bool cleanup;
};

extern "C" VitoxShaderGenResult vitox_shader_gen_probe_run(const VitoxCoreHost *host, vitox::ExternalPresenter *presenter, const wchar_t *local_state,
    const wchar_t *dev_root, const wchar_t *static_assets);
