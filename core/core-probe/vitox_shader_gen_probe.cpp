// Vitox M7B Phase A (0.0.15.x): NON-VISUAL shader generation + validation.
//
// Route 2a (approved): open-shacccg (MIT, frangarcj/open-shacccg @ 26da8ebc, dependency-free core compiled for WindowsStore
// into this signed package) compiles OUR OWN deterministic shader source - the two SPIR-V modules built word by word
// below (GLSL equivalent alongside) - into tri_v.gxp / tri_f.gxp. No third-party shader binary is read, linked or
// packaged. Then, all real Vita3K code:
//   - the GXPs are copied into real guest memory (MemState alloc) and checked by the real exports
//     sceGxmProgramCheck / sceGxmProgramFindParameterByName / sceGxmProgramParameterGetResourceIndex,
//   - shader::convert_gxp (Vita3K's USSE recompiler) translates each to SPIR-V for the Vulkan target and to GLSL,
//   - vkCreateShaderModule on the real VKState device (renderer::init -> dzn) accepts the recompiled SPIR-V.
// Outputs (.spv, .gxp, recompiled .glsl/.spv, source listing) go to LocalState\m7b with SHA-256s.
#include "vitox_shader_gen_probe.h"

#include <openshacccg/compiler.h>

#include <vitox_external_present.h>

#include <config/state.h>
#include <emuenv/state.h>
#include <gxm/functions.h>
#include <gxm/types.h>
#include <mem/functions.h>
#include <mem/state.h>
#include <renderer/frame_host.h>
#include <renderer/functions.h>
#include <renderer/state.h>
#include <renderer/vulkan/state.h>
#include <shader/spirv_recompiler.h>
#include <util/log.h>

#include <spdlog/sinks/callback_sink.h>
#include <spdlog/sinks/null_sink.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

std::string vitox_sha256_hex(const void *data, size_t size); // compat/vitox_sha256.cpp

// The REAL Vita3K exports (modules/SceGxm/SceGxm.cpp, EXPORT -> DECL_EXPORT), called exactly as the bridge would.
int export_sceGxmProgramCheck(EmuEnvState &emuenv, SceUID thread_id, const char *export_name, const SceGxmProgram *program);
Ptr<SceGxmProgramParameter> export_sceGxmProgramFindParameterByName(EmuEnvState &emuenv, SceUID thread_id, const char *export_name,
    Ptr<const SceGxmProgram> program_ptr, const char *name);
uint32_t export_sceGxmProgramParameterGetResourceIndex(EmuEnvState &emuenv, SceUID thread_id, const char *export_name, const SceGxmProgramParameter *parameter);

namespace {

const VitoxCoreHost *g_host = nullptr;
namespace sfs = std::filesystem;

template <typename... Args>
void out(fmt::format_string<Args...> f, Args &&...args) {
    g_host->log(fmt::format(f, std::forward<Args>(args)...).c_str());
}
void snap(const char *tag) { g_host->mem_snapshot(tag); }
const char *pf(bool b) { return b ? "PASS" : "FAIL"; }

void install_log_bridge() {
    auto sink = std::make_shared<spdlog::sinks::callback_sink_mt>([](const spdlog::details::log_msg &msg) {
        const auto level = spdlog::level::to_string_view(msg.level);
        g_host->log(fmt::format("[vita3k {}] {}", std::string_view(level.data(), level.size()), std::string_view(msg.payload.data(), msg.payload.size()))
                .c_str());
    });
    auto logger = std::make_shared<spdlog::logger>("vita3k", sink);
    logger->set_level(spdlog::level::info);
    spdlog::set_default_logger(logger);
}
void silence_log() { spdlog::set_default_logger(std::make_shared<spdlog::logger>("vita3k-m7b-off", std::make_shared<spdlog::sinks::null_sink_mt>())); }

class VitoxFrameHost : public renderer::FrameHost {
public:
    explicit VitoxFrameHost(const vitox::ExternalPresenter &p)
        : p(p) {}
    renderer::DisplayHandle handle() const override { return {}; }
    int drawable_width() const override { return static_cast<int>(p.width()); }
    int drawable_height() const override { return static_cast<int>(p.height()); }
    std::vector<std::string> font_dirs() const override { return {}; }

private:
    const vitox::ExternalPresenter &p;
};

// ============================================================================================================
// OUR SHADER SOURCE (deterministic, under our control). SPIR-V 1.0, Logical / GLSL450, written to the SPIR-V spec.
//
// tri_v (vertex)                                   tri_f (fragment)
//   layout(location = 0) in vec3 aPosition;          layout(location = 0) in vec4 vColor;
//   layout(location = 1) in vec4 aColor;             layout(location = 0) out vec4 outColor;
//   uniform mat4 wvp;                                void main() { outColor = vColor; }
//   layout(location = 0) out vec4 vColor;
//   void main() {
//       gl_Position = wvp * vec4(aPosition, 1.0);
//       vColor = aColor;
//   }
// (the shape open-shacccg's dependency-free path validates: matrix position + one colour varying; varying-colour
// fragment)
// ============================================================================================================
enum : uint16_t {
    OpName = 5, OpMemoryModel = 14, OpEntryPoint = 15, OpExecutionMode = 16, OpCapability = 17, OpTypeVoid = 19, OpTypeFloat = 22,
    OpTypeVector = 23, OpTypeMatrix = 24, OpTypeFunction = 33, OpTypePointer = 32, OpConstant = 43, OpFunction = 54, OpFunctionEnd = 56,
    OpVariable = 59, OpLoad = 61, OpStore = 62, OpDecorate = 71, OpCompositeConstruct = 80, OpMatrixTimesVector = 145, OpLabel = 248,
    OpReturn = 253,
};
enum : uint32_t { SC_Input = 1, SC_Uniform = 2, SC_Output = 3, Dec_BuiltIn = 11, Dec_Location = 30, BuiltIn_Position = 0, Exec_Vertex = 0,
    Exec_Fragment = 4, Mode_OriginUpperLeft = 7, One_f32 = 0x3F800000u };

struct Spv {
    std::vector<uint32_t> w;
    explicit Spv(uint32_t bound) : w{ 0x07230203u, 0x00010000u, 0u, bound, 0u } {}
    void op(uint16_t code, std::initializer_list<uint32_t> args) {
        w.push_back((static_cast<uint32_t>(args.size() + 1) << 16) | code);
        w.insert(w.end(), args.begin(), args.end());
    }
    // instruction with a literal string between `pre` and `post` operands
    void op_str(uint16_t code, std::initializer_list<uint32_t> pre, const char *s, std::initializer_list<uint32_t> post = {}) {
        const size_t n = std::strlen(s) + 1, nw = (n + 3) / 4;
        std::vector<uint32_t> sw(nw, 0);
        std::memcpy(sw.data(), s, n);
        w.push_back((static_cast<uint32_t>(1 + pre.size() + nw + post.size()) << 16) | code);
        w.insert(w.end(), pre.begin(), pre.end());
        w.insert(w.end(), sw.begin(), sw.end());
        w.insert(w.end(), post.begin(), post.end());
    }
};

std::vector<uint32_t> tri_vertex_spirv() {
    // ids
    enum : uint32_t { tVoid = 1, tFn, tF32, tV3, tV4, tM4, pInV3, pInV4, pOutV4, pUniM4, cOne, vPos, vCol, vOutPos, vOutCol, vWvp, fMain, lEntry,
        xM, xP, xP4, xPos, xC, kBound };
    Spv s(kBound);
    s.op(OpCapability, { 1 }); // Shader
    s.op(OpMemoryModel, { 0, 1 }); // Logical, GLSL450
    s.op_str(OpEntryPoint, { Exec_Vertex, fMain }, "main", { vPos, vCol, vOutPos, vOutCol });
    s.op_str(OpName, { vPos }, "aPosition");
    s.op_str(OpName, { vCol }, "aColor");
    s.op_str(OpName, { vWvp }, "wvp");
    s.op_str(OpName, { vOutCol }, "vColor");
    s.op_str(OpName, { vOutPos }, "gl_Position");
    s.op(OpDecorate, { vPos, Dec_Location, 0 });
    s.op(OpDecorate, { vCol, Dec_Location, 1 });
    s.op(OpDecorate, { vOutPos, Dec_BuiltIn, BuiltIn_Position });
    s.op(OpDecorate, { vOutCol, Dec_Location, 0 });
    s.op(OpTypeVoid, { tVoid });
    s.op(OpTypeFunction, { tFn, tVoid });
    s.op(OpTypeFloat, { tF32, 32 });
    s.op(OpTypeVector, { tV3, tF32, 3 });
    s.op(OpTypeVector, { tV4, tF32, 4 });
    s.op(OpTypeMatrix, { tM4, tV4, 4 });
    s.op(OpTypePointer, { pInV3, SC_Input, tV3 });
    s.op(OpTypePointer, { pInV4, SC_Input, tV4 });
    s.op(OpTypePointer, { pOutV4, SC_Output, tV4 });
    s.op(OpTypePointer, { pUniM4, SC_Uniform, tM4 });
    s.op(OpConstant, { tF32, cOne, One_f32 });
    s.op(OpVariable, { pInV3, vPos, SC_Input });
    s.op(OpVariable, { pInV4, vCol, SC_Input });
    s.op(OpVariable, { pOutV4, vOutPos, SC_Output });
    s.op(OpVariable, { pOutV4, vOutCol, SC_Output });
    s.op(OpVariable, { pUniM4, vWvp, SC_Uniform });
    s.op(OpFunction, { tVoid, fMain, 0, tFn });
    s.op(OpLabel, { lEntry });
    s.op(OpLoad, { tM4, xM, vWvp });                   // m  = wvp
    s.op(OpLoad, { tV3, xP, vPos });                   // p  = aPosition
    s.op(OpCompositeConstruct, { tV4, xP4, xP, cOne }); // p4 = vec4(p, 1.0)
    s.op(OpMatrixTimesVector, { tV4, xPos, xM, xP4 }); // pos = m * p4
    s.op(OpStore, { vOutPos, xPos });                  // gl_Position = pos
    s.op(OpLoad, { tV4, xC, vCol });                   // c  = aColor
    s.op(OpStore, { vOutCol, xC });                    // vColor = c
    s.op(OpReturn, {});
    s.op(OpFunctionEnd, {});
    return s.w;
}

std::vector<uint32_t> tri_fragment_spirv() {
    enum : uint32_t { tVoid = 1, tFn, tF32, tV4, pInV4, pOutV4, vIn, vOut, fMain, lEntry, xC, kBound };
    Spv s(kBound);
    s.op(OpCapability, { 1 });
    s.op(OpMemoryModel, { 0, 1 });
    s.op_str(OpEntryPoint, { Exec_Fragment, fMain }, "main", { vIn, vOut });
    s.op(OpExecutionMode, { fMain, Mode_OriginUpperLeft });
    s.op_str(OpName, { vIn }, "vColor");
    s.op_str(OpName, { vOut }, "outColor");
    s.op(OpDecorate, { vIn, Dec_Location, 0 });
    s.op(OpDecorate, { vOut, Dec_Location, 0 });
    s.op(OpTypeVoid, { tVoid });
    s.op(OpTypeFunction, { tFn, tVoid });
    s.op(OpTypeFloat, { tF32, 32 });
    s.op(OpTypeVector, { tV4, tF32, 4 });
    s.op(OpTypePointer, { pInV4, SC_Input, tV4 });
    s.op(OpTypePointer, { pOutV4, SC_Output, tV4 });
    s.op(OpVariable, { pInV4, vIn, SC_Input });
    s.op(OpVariable, { pOutV4, vOut, SC_Output });
    s.op(OpFunction, { tVoid, fMain, 0, tFn });
    s.op(OpLabel, { lEntry });
    s.op(OpLoad, { tV4, xC, vIn });   // c = vColor
    s.op(OpStore, { vOut, xC });      // outColor = c
    s.op(OpReturn, {});
    s.op(OpFunctionEnd, {});
    return s.w;
}

void *osc_alloc(size_t size, void *) { return ::operator new(size); }
void osc_free(void *p, void *) { ::operator delete(p); }

struct Compiled {
    bool ok = false;
    std::vector<uint8_t> gxp;
    std::string diag;
};
Compiled compile(const std::vector<uint32_t> &spv, VscStage stage) {
    VscSpirvRequest req{};
    req.words = spv.data();
    req.word_count = spv.size();
    req.entrypoint = "main";
    req.stage = stage;
    req.allocator = { &osc_alloc, &osc_free, nullptr };
    VscCompileResult res{};
    Compiled c;
    const int rc = vsc_compile_spirv(&req, &res);
    for (size_t i = 0; i < res.diagnostic_count; ++i)
        c.diag += fmt::format("[{} 0x{:04X}] {}; ", static_cast<int>(res.diagnostics[i].severity), res.diagnostics[i].code,
            res.diagnostics[i].message ? res.diagnostics[i].message : "");
    if (res.gxp_data && res.gxp_size)
        c.gxp.assign(res.gxp_data, res.gxp_data + res.gxp_size);
    c.ok = rc == 0 && !c.gxp.empty();
    vsc_destroy_result(&req.allocator, &res);
    return c;
}

bool write_file(const sfs::path &p, const void *data, size_t size) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(static_cast<const char *>(data), static_cast<std::streamsize>(size));
    return static_cast<bool>(f);
}

std::string hex_prefix(const std::vector<uint8_t> &b, size_t n) {
    std::string s;
    for (size_t i = 0; i < b.size() && i < n; ++i)
        s += fmt::format("{:02X}", b[i]);
    return s;
}

} // namespace

extern "C" VitoxShaderGenResult vitox_shader_gen_probe_run(const VitoxCoreHost *host, vitox::ExternalPresenter *presenter, const wchar_t *local_state,
    const wchar_t *dev_root, const wchar_t *static_assets) {
    g_host = host;
    VitoxShaderGenResult r{};
    install_log_bridge();
    const sfs::path ls(local_state), outdir = ls; // files are prefixed m7b_ (the harness collects LocalState root files)
    std::error_code ec;
    sfs::create_directories(outdir, ec);
    out("M7B Phase A begin: open-shacccg {} (pinned 26da8ebc, MIT) inside the signed package; our own SPIR-V source -> GXP -> Vita3K checks",
        vsc_version_string() ? vsc_version_string() : "?");
    snap("m7b_base");

    // ---- 1. generate (twice, for determinism) ----
    const auto vs = tri_vertex_spirv(), fs = tri_fragment_spirv();
    const Compiled v1 = compile(vs, VSC_STAGE_VERTEX), f1 = compile(fs, VSC_STAGE_FRAGMENT);
    const Compiled v2 = compile(vs, VSC_STAGE_VERTEX), f2 = compile(fs, VSC_STAGE_FRAGMENT);
    r.generated = v1.ok && f1.ok;
    r.deterministic = r.generated && v1.gxp == v2.gxp && f1.gxp == f2.gxp;
    const std::string vs_sha = vitox_sha256_hex(vs.data(), vs.size() * 4), fs_sha = vitox_sha256_hex(fs.data(), fs.size() * 4);
    const std::string vg_sha = r.generated ? vitox_sha256_hex(v1.gxp.data(), v1.gxp.size()) : "-",
                      fg_sha = r.generated ? vitox_sha256_hex(f1.gxp.data(), f1.gxp.size()) : "-";
    out("m7b source: tri_v.spv {} words SHA-256 {} | tri_f.spv {} words SHA-256 {}", vs.size(), vs_sha, fs.size(), fs_sha);
    out("m7b generate: tri_v.gxp ok={} {} B SHA-256 {} head {} diag [{}]", v1.ok, v1.gxp.size(), vg_sha, hex_prefix(v1.gxp, 16), v1.diag);
    out("m7b generate: tri_f.gxp ok={} {} B SHA-256 {} head {} diag [{}]", f1.ok, f1.gxp.size(), fg_sha, hex_prefix(f1.gxp, 16), f1.diag);
    out("m7b deterministic: second compile identical: vertex={} fragment={} -> {} | generated -> {}", v1.gxp == v2.gxp, f1.gxp == f2.gxp,
        pf(r.deterministic), pf(r.generated));

    // ---- 2. Vita3K: renderer (for the real VKState device + feature state), guest memory ----
    auto *emuenv = new EmuEnvState();
    emuenv->config_path = ls;
    emuenv->log_path = ls / "log";
    emuenv->cache_path = ls / "cache";
    emuenv->vita_fs_path = fs::path(dev_root) / "vita-fs";
    emuenv->static_assets_path = fs::path(static_assets);
    vitox_set_external_presenter(presenter);
    vitox_xp_set_variant(VitoxXpVariant::P_Production);
    VitoxFrameHost frame_host(*presenter);
    const bool rinit = renderer::init(frame_host, emuenv->renderer, renderer::Backend::Vulkan, emuenv->cfg, emuenv->get_root_paths());
    bool mem_ok = false;
    if (rinit) {
        emuenv->renderer->late_init(emuenv->cfg, "VITOX0003", emuenv->mem);
        mem_ok = init(emuenv->mem, false);
    }
    out("m7b vita3k: renderer::init(Vulkan) -> {} | GPU '{}' | late_init + init(mem) -> {}", rinit, rinit ? emuenv->renderer->get_gpu_name() : "-", mem_ok);
    snap("m7b_after_renderer");

    std::vector<uint8_t> v_spirv_bytes, f_spirv_bytes;
    std::string v_glsl, f_glsl;
    if (r.generated && mem_ok) {
        struct Prog {
            const char *name;
            const std::vector<uint8_t> *gxp;
            bool vertex;
            Address guest = 0;
        } progs[2] = { { "tri_v", &v1.gxp, true }, { "tri_f", &f1.gxp, false } };
        bool check_ok = true, type_ok = true, param_ok = true, recomp_ok = true, vk_ok = true;
        auto *vk = dynamic_cast<renderer::vulkan::VKState *>(emuenv->renderer.get());
        for (auto &p : progs) {
            // into real guest memory, as a homebrew would hold it
            p.guest = alloc(emuenv->mem, static_cast<uint32_t>(p.gxp->size()), p.name);
            std::memcpy(Ptr<uint8_t>(p.guest).get(emuenv->mem), p.gxp->data(), p.gxp->size());
            const auto *prog = Ptr<const SceGxmProgram>(p.guest).get(emuenv->mem);
            const int chk = export_sceGxmProgramCheck(*emuenv, 0, "sceGxmProgramCheck", prog);
            check_ok &= chk == 0;
            const bool type_match = p.vertex ? prog->is_vertex() : prog->is_fragment();
            type_ok &= type_match;
            out("m7b {}: guest 0x{:08X} | sceGxmProgramCheck -> 0x{:08X} | program size {} type {} (expected {}) | parameters {}", p.name, p.guest,
                static_cast<uint32_t>(chk), prog->size, prog->is_vertex() ? "vertex" : prog->is_fragment() ? "fragment" : "?",
                p.vertex ? "vertex" : "fragment", prog->parameter_count);
            for (uint32_t i = 0; i < prog->parameter_count; ++i) {
                const auto &pp = prog->program_parameters()[i];
                out("m7b {}   parameter {}: '{}' category={} type={} components={} array_size={} resource_index={} container={}", p.name, i,
                    gxp::parameter_name(pp), static_cast<int>(pp.category), static_cast<int>(pp.type), static_cast<unsigned>(pp.component_count), pp.array_size,
                    pp.resource_index, static_cast<unsigned>(pp.container_index));
            }
            std::vector<SceGxmVertexAttribute> attributes;
            if (p.vertex) {
                for (const char *name : { "aPosition", "aColor", "wvp" }) {
                    const auto param = export_sceGxmProgramFindParameterByName(*emuenv, 0, "sceGxmProgramFindParameterByName", Ptr<const SceGxmProgram>(p.guest), name);
                    const uint32_t ri = param ? export_sceGxmProgramParameterGetResourceIndex(*emuenv, 0, "sceGxmProgramParameterGetResourceIndex", param.get(emuenv->mem)) : 0;
                    param_ok &= static_cast<bool>(param);
                    out("m7b tri_v: sceGxmProgramFindParameterByName('{}') -> 0x{:08X} | sceGxmProgramParameterGetResourceIndex -> {}", name, param.address(), ri);
                    if (param && std::strcmp(name, "wvp") != 0) {
                        SceGxmVertexAttribute a{};
                        a.streamIndex = 0;
                        a.offset = std::strcmp(name, "aPosition") == 0 ? 0 : 12;
                        a.format = SCE_GXM_ATTRIBUTE_FORMAT_F32;
                        a.componentCount = std::strcmp(name, "aPosition") == 0 ? 3 : 4;
                        a.regIndex = static_cast<uint16_t>(ri);
                        attributes.push_back(a);
                    }
                }
            }
            // Vita3K's USSE recompiler, as renderer/src/shaders.cpp calls it
            shader::Hints hints{};
            hints.attributes = &attributes;
            hints.color_format = SCE_GXM_COLOR_FORMAT_A8B8G8R8;
            const std::string hash = vitox_sha256_hex(p.gxp->data(), p.gxp->size()).substr(0, 32);
            try {
                const auto spv = shader::convert_gxp(*prog, hash, emuenv->renderer->features, shader::Target::SpirVVulkan, hints);
                const auto glsl = shader::convert_gxp(*prog, hash, emuenv->renderer->features, shader::Target::GLSLOpenGL, hints);
                const bool good = !spv.spirv.empty() && !glsl.glsl.empty();
                recomp_ok &= good;
                auto &bytes = p.vertex ? v_spirv_bytes : f_spirv_bytes;
                bytes.assign(reinterpret_cast<const uint8_t *>(spv.spirv.data()), reinterpret_cast<const uint8_t *>(spv.spirv.data()) + spv.spirv.size() * 4);
                (p.vertex ? v_glsl : f_glsl) = glsl.glsl;
                out("m7b {}: shader::convert_gxp -> SPIR-V (Vulkan) {} words, GLSL {} chars -> {}", p.name, spv.spirv.size(), glsl.glsl.size(), pf(good));
                if (vk && !spv.spirv.empty()) {
                    vk::ShaderModuleCreateInfo ci{};
                    ci.codeSize = spv.spirv.size() * 4;
                    ci.pCode = spv.spirv.data();
                    vk::ShaderModule mod{};
                    const vk::Result res = vk->device.createShaderModule(&ci, nullptr, &mod);
                    vk_ok &= res == vk::Result::eSuccess;
                    out("m7b {}: vkCreateShaderModule on the real VKState device (dzn) -> {}", p.name, vk::to_string(res));
                    if (res == vk::Result::eSuccess)
                        vk->device.destroyShaderModule(mod);
                } else {
                    vk_ok = false;
                }
            } catch (const std::exception &e) {
                recomp_ok = false;
                vk_ok = false;
                out("m7b {}: shader::convert_gxp threw: {}", p.name, e.what());
            }
        }
        r.program_check = check_ok;
        r.types = type_ok;
        r.parameters = param_ok;
        r.recompiled = recomp_ok;
        r.vk_modules = vk_ok;
        for (auto &p : progs)
            if (p.guest)
                free(emuenv->mem, p.guest);
    }
    out("m7b vita3k checks: sceGxmProgramCheck {} | program types {} | parameters {} | convert_gxp {} | vkCreateShaderModule {}", pf(r.program_check),
        pf(r.types), pf(r.parameters), pf(r.recompiled), pf(r.vk_modules));

    // ---- 3. archive the outputs ----
    if (r.generated) {
        std::string listing = "M7B tri_v / tri_f source: Vitox-M3/core-probe/vitox_shader_gen_probe.cpp (tri_vertex_spirv / tri_fragment_spirv)\n"
                              "generator: open-shacccg 26da8ebc44717147d002c60d4a80cd6604aace7f (MIT), vsc_compile_spirv, dependency-free core\n";
        listing += fmt::format("tri_v.spv {}\ntri_f.spv {}\ntri_v.gxp {}\ntri_f.gxp {}\n", vs_sha, fs_sha, vg_sha, fg_sha);
        r.written = write_file(outdir / "m7b_tri_v.spv", vs.data(), vs.size() * 4) && write_file(outdir / "m7b_tri_f.spv", fs.data(), fs.size() * 4)
            && write_file(outdir / "m7b_tri_v.gxp", v1.gxp.data(), v1.gxp.size()) && write_file(outdir / "m7b_tri_f.gxp", f1.gxp.data(), f1.gxp.size())
            && write_file(outdir / "m7b_manifest.txt", listing.data(), listing.size());
        if (!v_glsl.empty())
            write_file(outdir / "m7b_tri_v.vita3k.glsl", v_glsl.data(), v_glsl.size());
        if (!f_glsl.empty())
            write_file(outdir / "m7b_tri_f.vita3k.glsl", f_glsl.data(), f_glsl.size());
        if (!v_spirv_bytes.empty())
            write_file(outdir / "m7b_tri_v.vita3k.spv", v_spirv_bytes.data(), v_spirv_bytes.size());
        if (!f_spirv_bytes.empty())
            write_file(outdir / "m7b_tri_f.vita3k.spv", f_spirv_bytes.data(), f_spirv_bytes.size());
        out("m7b outputs -> LocalState root, m7b_* (tri_v/tri_f .spv .gxp .vita3k.glsl .vita3k.spv, manifest.txt) -> {}", pf(r.written));
    }

    // ---- 4. teardown ----
    if (emuenv->renderer) {
        emuenv->renderer->cleanup();
        emuenv->renderer.reset();
    }
    vitox_set_external_presenter(nullptr);
    if (mem_ok)
        deinit_mem(emuenv->mem);
    delete emuenv;
    auto *veh_anchor = new MemState; // Vita3K's process-wide VEH captured the freed MemState: re-anchor it
    if (init(*veh_anchor, false))
        deinit_mem(*veh_anchor);
    r.cleanup = true;
    snap("m7b_end");
    out("M7B Phase A end: generated={} deterministic={} program_check={} types={} parameters={} recompiled={} vk_modules={} written={} cleanup={}",
        r.generated, r.deterministic, r.program_check, r.types, r.parameters, r.recompiled, r.vk_modules, r.written, r.cleanup);
    silence_log();
    return r;
}
