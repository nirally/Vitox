// Vitox M8C.2 - staged real LLE PGF precheck (see vitox_pgf_lle_probe.h). Every load step is Vita3K's own code; the
// probe sets the staged root + MANUAL [libpgf] policy, enables the real decrypt path and the trap, and observes.
#include "vitox_pgf_lle_probe.h"

#include "vitox_import_dispatch.h"

#include <vitox_fself.h>

#include <config/state.h>
#include <emuenv/state.h>
#include <kernel/state.h>
#include <kernel/thread/thread_state.h>
#include <mem/functions.h>
#include <mem/state.h>
#include <module/load_module.h>
#include <modules/module_parent.h>
#include <nids/functions.h>
#include <packages/sce_types.h>
#include <util/fs.h>
#include <util/log.h>

#include <spdlog/sinks/callback_sink.h>
#include <spdlog/sinks/null_sink.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

std::string vitox_sha256_hex(const void *data, size_t size); // compat/vitox_sha256.cpp

namespace {
namespace sfs = std::filesystem;
const VitoxCoreHost *g_host = nullptr;
std::atomic<unsigned> g_errors{ 0 }, g_start_failed{ 0 }, g_net_lines{ 0 };

template <typename... Args>
void out(fmt::format_string<Args...> f, Args &&...args) {
    g_host->log(fmt::format(f, std::forward<Args>(args)...).c_str());
}
std::string u8(const sfs::path &p) {
    const auto s = p.u8string();
    return std::string(s.begin(), s.end());
}
std::map<std::string, uintmax_t> tree(const sfs::path &dir) {
    std::map<std::string, uintmax_t> t;
    std::error_code ec;
    for (auto it = sfs::recursive_directory_iterator(dir, sfs::directory_options::none, ec); !ec && it != sfs::recursive_directory_iterator(); it.increment(ec)) {
        std::error_code e2;
        t[u8(it->path().lexically_relative(dir))] = it->is_regular_file(e2) ? it->file_size(e2) : static_cast<uintmax_t>(-1);
    }
    return t;
}
bool is_network_name(const std::string &n) {
    return n.starts_with("sceHttp") || n.starts_with("sceNet") || n.starts_with("sceSsl") || n.starts_with("sceHttps");
}
} // namespace

extern "C" VitoxPgfLleResult vitox_pgf_lle_probe_run(const VitoxCoreHost *host, const wchar_t *dev_root, const wchar_t *local_state) {
    g_host = host;
    VitoxPgfLleResult r{};
    const sfs::path root = sfs::path(dev_root) / "firmware-staging", staged = root / "vita-fs";
    std::error_code ec;
    r.staged = sfs::exists(root / "stage-complete.txt", ec) && sfs::is_directory(staged / "vs0", ec);
    if (!r.staged) {
        out("m8c.2 PGF LLE precheck: no completed staging at '{}' - skipped", u8(root));
        return r;
    }
    g_errors = g_start_failed = g_net_lines = 0;
    auto sink = std::make_shared<spdlog::sinks::callback_sink_mt>([](const spdlog::details::log_msg &msg) {
        const std::string_view p(msg.payload.data(), msg.payload.size());
        if (msg.level >= spdlog::level::err)
            ++g_errors;
        if (p.find("did not return successfully") != std::string_view::npos)
            ++g_start_failed;
        if (p.find("libhttp") != std::string_view::npos || p.find("libssl") != std::string_view::npos || p.find("libnet") != std::string_view::npos)
            ++g_net_lines;
        const auto level = spdlog::level::to_string_view(msg.level);
        g_host->log(fmt::format("[vita3k {}] {}", std::string_view(level.data(), level.size()), p).c_str());
    });
    auto logger = std::make_shared<spdlog::logger>("vita3k-m8c2", sink);
    logger->set_level(spdlog::level::debug);
    spdlog::set_default_logger(logger);
    out("M8C.2 PGF LLE precheck begin: temporary vita_fs root '{}' (read-only use; io init not run)", u8(staged));
    const auto staging_before = tree(staged);

    // 1. the module file and its REAL decryption (the same decrypt_fself the loader calls)
    const sfs::path pgf = staged / "vs0" / "sys" / "external" / "libpgf.suprx";
    std::vector<uint8_t> enc;
    {
        std::ifstream f(pgf, std::ios::binary);
        enc.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    const std::vector<uint8_t> dec = enc.empty() ? std::vector<uint8_t>{} : decrypt_fself(enc, nullptr);
    // decrypt_fself returns the SELF with its segments decrypted (and decompressed): it must now be a valid UNENCRYPTED
    // SELF - exactly what the M5B checker accepts (SCE magic, every segment's encryption field = 2)
    const bool enc_refused = vitox_decrypt_fself_unencrypted_only(enc).empty(); // the encrypted input is refused by M5B's path
    r.decrypt = !dec.empty() && !vitox_decrypt_fself_unencrypted_only(dec).empty();
    out("m8c.2 libpgf.suprx (vs0:sys/external/libpgf.suprx): encrypted SELF {} B SHA-256 {} (unencrypted-only path refuses it: {}) -> REAL decrypt_fself -> "
        "{} B SHA-256 {} | valid unencrypted SELF {} -> {}",
        enc.size(), vitox_sha256_hex(enc.data(), enc.size()), enc_refused, dec.size(), dec.empty() ? std::string("-") : vitox_sha256_hex(dec.data(), dec.size()),
        r.decrypt, r.decrypt ? "PASS" : "FAIL");

    // 2. core environment for LLE: mem + kernel (no io init, no renderer), Vita3K's MANUAL modules policy
    auto *emuenv = new EmuEnvState();
    emuenv->config_path = fs::path(local_state);
    emuenv->log_path = fs::path(local_state) / "log";
    emuenv->cache_path = fs::path(local_state) / "cache";
    emuenv->vita_fs_path = fs::path(staged.native());
    emuenv->cfg.current_config.modules_mode = ModulesMode::MANUAL;
    emuenv->cfg.current_config.lle_modules = { "libpgf" };
    const bool pgf_lle = is_lle_module(SCE_SYSMODULE_PGF, *emuenv), http_lle = is_lle_module(SCE_SYSMODULE_HTTP, *emuenv),
               net_lle = is_lle_module(SCE_SYSMODULE_NET, *emuenv), pvf_lle = is_lle_module(std::string("libpvf"), *emuenv);
    r.policy = pgf_lle && !http_lle && !net_lle;
    out("m8c.2 modules policy: MANUAL lle-modules=[libpgf] -> is_lle_module PGF={} HTTP={} NET={} libpvf={} -> {}", pgf_lle, http_lle, net_lle, pvf_lle,
        r.policy ? "PASS" : "FAIL");
    bool ok = r.decrypt && r.policy && init(emuenv->mem, false);
    const auto call_import = [emuenv](CPUState &cpu, uint32_t nid, SceUID thread_id) { ::call_import(*emuenv, cpu, nid, thread_id); };
    ok = ok && emuenv->kernel.init(emuenv->mem, call_import, emuenv->cfg.current_config.cpu_opt);
    init_exported_vars(*emuenv);
    vitox_init_selected_libraries(*emuenv);
    out("m8c.2 core: mem + KernelState::init -> {}", ok);

    // 3. the REAL sysmodule LLE load, with the real decrypt path and the fail-stop trap
    std::set<std::pair<uint32_t, Address>> bound_before;
    for (const auto &[nid, b] : emuenv->kernel.func_binding_infos)
        bound_before.insert({ nid, b.entry_address });
    const size_t exports_before = emuenv->kernel.export_nids.size();
    vitox_import_stats_reset();
    vitox_import_trap_enable(true);
    vitox_set_fself_real_decrypt(true);
    if (ok) {
        r.loaded = load_sys_module(*emuenv, SCE_SYSMODULE_PGF);
        r.module_start_ok = r.loaded && g_start_failed == 0;
    }
    vitox_set_fself_real_decrypt(false);
    out("m8c.2 load_sys_module(SCE_SYSMODULE_PGF) -> {} | module_start {}", r.loaded, r.loaded ? (r.module_start_ok ? "returned success" : "FAILED") : "-");

    // 4. what got loaded and what it imports
    for (const auto &[uid, mod] : emuenv->kernel.loaded_modules)
        out("m8c.2 loaded module uid {}: '{}' path '{}' start_entry 0x{:08X} segments[0] 0x{:08X} {} B", uid, mod->info.module_name, mod->info.path,
            mod->info.start_entry.address(), mod->info.segments[0].vaddr.address(), mod->info.segments[0].memsz);
    for (const auto &[id, uids] : emuenv->kernel.loaded_sysmodules)
        out("m8c.2 loaded sysmodule id 0x{:X} -> {} module(s)", static_cast<int>(id), uids.size());
    std::map<uint32_t, unsigned> per_lib;
    for (const auto &[nid, b] : emuenv->kernel.func_binding_infos) {
        if (bound_before.contains({ nid, b.entry_address }))
            continue;
        ++r.imports;
        ++per_lib[b.library_nid];
        const VitoxSelectedImport *imp = vitox_resolve_selected_import(nid);
        r.unselected_imports += imp ? 0 : 1;
        const char *nm = import_name(nid);
        out("m8c.2 libpgf import: library_nid 0x{:08X} nid 0x{:08X} {} -> {}", b.library_nid, nid, nm ? nm : "?", imp ? "selected (REAL ImportFn)" : "NOT selected");
    }
    for (const auto &[lib, n] : per_lib)
        out("m8c.2 libpgf imports from library_nid 0x{:08X}: {}", lib, n);
    out("m8c.2 exports registered by the load: {} (export_nids {} -> {})", emuenv->kernel.export_nids.size() - exports_before, exports_before,
        emuenv->kernel.export_nids.size());

    // 5. runtime: traps (unselected HLE calls) and network guard
    std::vector<VitoxTrapRecord> traps(32);
    traps.resize(vitox_import_traps(traps.data(), static_cast<unsigned>(traps.size())));
    r.traps = static_cast<unsigned>(traps.size());
    r.no_traps = traps.empty();
    for (const auto &t : traps)
        out("m8c.2 TRAP during module_start: nid 0x{:08X} {} library_nid 0x{:08X} pc 0x{:08X} lr 0x{:08X}", t.nid, import_name(t.nid) ? import_name(t.nid) : "?",
            t.library_nid, t.pc, t.lr);
    vitox_import_trap_enable(false);
    std::vector<VitoxImportStat> stats(256);
    const unsigned n = vitox_import_stats(stats.data(), static_cast<unsigned>(stats.size()));
    bool net_call = false;
    for (unsigned i = 0; i < n; ++i) {
        const char *nm = stats[i].name ? stats[i].name : import_name(stats[i].nid);
        net_call |= nm && is_network_name(nm);
        out("m8c.2 invoked during load: nid 0x{:08X} {} calls {} -> {}", stats[i].nid, nm ? nm : "?", stats[i].calls, stats[i].resolved ? "REAL ImportFn" : "unselected (trapped)");
    }
    bool http_loaded = false;
    for (const auto &[uid, mod] : emuenv->kernel.loaded_modules)
        http_loaded |= std::string(mod->info.path).find("libhttp") != std::string::npos;
    r.no_network = !net_call && !http_loaded && g_net_lines == 0 && !emuenv->kernel.loaded_sysmodules.contains(SCE_SYSMODULE_HTTP);
    out("m8c.2 network guard: libhttp loaded={} SceHttp/SceNet calls={} network module log lines={} HTTP sysmodule={} -> {}", http_loaded, net_call, g_net_lines.load(),
        emuenv->kernel.loaded_sysmodules.contains(SCE_SYSMODULE_HTTP), r.no_network ? "PASS" : "FAIL");

    // 6. cleanup (Vita3K order) and staging check
    {
        std::lock_guard<std::mutex> lock(emuenv->kernel.mutex);
        for (auto &[_, t] : emuenv->kernel.threads)
            t->exit_delete(false);
    }
    bool threads_gone = false;
    {
        std::unique_lock<std::mutex> lock(emuenv->kernel.mutex);
        threads_gone = emuenv->kernel.thread_deleted_cond.wait_for(lock, std::chrono::seconds(5), [&] { return emuenv->kernel.threads.empty(); });
    }
    std::vector<SceUID> uids;
    for (const auto &[uid, _] : emuenv->kernel.loaded_modules)
        uids.push_back(uid);
    for (SceUID uid : uids)
        unload_module(*emuenv, uid);
    if (threads_gone) {
        emuenv->kernel.deinit(emuenv->mem);
        deinit_mem(emuenv->mem);
        delete emuenv;
    }
    auto *veh_anchor = new MemState; // Vita3K's process-wide VEH captured the freed MemState: re-anchor it
    if (init(*veh_anchor, false))
        deinit_mem(*veh_anchor);
    const auto staging_after = tree(staged);
    r.staging_unchanged = staging_after == staging_before;
    out("m8c.2 staging tree unchanged={} ({} entries) | threads deleted={}", r.staging_unchanged, staging_before.size(), threads_gone);
    const char *cls = !r.decrypt || !r.loaded ? "C (PGF LLE decrypt/load fails)"
        : !r.no_traps || r.unselected_imports ? "B (PGF LLE loads but needs HLE closure)"
        : r.module_start_ok ? "A (PGF LLE loads; font use not yet exercised)" : "E (module_start failed)";
    out("M8C.2 end: staged={} policy={} decrypt={} loaded={} module_start_ok={} imports={} unselected={} traps={} no_network={} staging_unchanged={} | "
        "Vita3K error lines {} -> class {}",
        r.staged, r.policy, r.decrypt, r.loaded, r.module_start_ok, r.imports, r.unselected_imports, r.traps, r.no_network, r.staging_unchanged, g_errors.load(), cls);
    spdlog::set_default_logger(std::make_shared<spdlog::logger>("vita3k-m8c2-off", std::make_shared<spdlog::sinks::null_sink_mt>()));
    return r;
}
