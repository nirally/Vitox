// Vitox M5B (0.0.10.0): FIRST HEADLESS HOMEBREW EXECUTION through the real Vita3K core loader.
//
// VITOX0001 is a self-made VitaSDK homebrew (C:\Vitox\Vitox-Homebrew\VITOX0001, packaged with the app).
// Path, all real Vita3K code unless noted:
//   install:  fs_utils::copy_directory_contents(package\homebrew\VITOX0001 -> vita-fs\ux0\app\VITOX0001)
//   setup:    EmuEnvState, IOState init, device/savedata paths, param.sfo (vfs::read_app_file + sfo::load),
//             mem init, KernelState::init(call_import = module_parent call_import), init_exported_vars,
//             selected LIBRARY_INITs (SceSysmem)              [load_app_impl() equivalent, headless]
//   load:     module_parent load_module("app0:eboot.bin") -> vfs read -> (unencrypted-FSELF check) ->
//             load_self -> relocation -> import stubs           [module_parent.cpp, shim: NID tables only]
//   run:      create_thread(entry = module start_entry) + start(..., true)   [run_app() equivalent]
//   imports:  guest svc -> ThreadState -> module_parent call_import -> REAL ImportFns
//   exit:     guest sceKernelExitProcess -> KernelState::request_process_exit -> process_exit_callback
// This is "headless homebrew execution via core loader", not an AppSessionController boot.
#include "vitox_homebrew_probe.h"

#include "vitox_import_dispatch.h"

#include <emuenv/state.h>
#include <io/functions.h>
#include <io/state.h>
#include <io/vfs.h>
#include <kernel/state.h>
#include <kernel/thread/thread_state.h>
#include <mem/functions.h>
#include <mem/state.h>
#include <modules/module_parent.h>
#include <nids/functions.h>
#include <packages/sfo.h>
#include <util/fs.h>
#include <util/log.h>

#include <spdlog/sinks/callback_sink.h>
#include <spdlog/sinks/null_sink.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <thread>

std::string vitox_sha256_hex(const void *data, size_t size); // compat/vitox_sha256.cpp

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
        const std::string line = fmt::format("[vita3k {}] {}", std::string_view(level.data(), level.size()),
            std::string_view(msg.payload.data(), msg.payload.size()));
        g_host->log(line.c_str());
    });
    auto logger = std::make_shared<spdlog::logger>("vita3k", sink);
    logger->set_level(spdlog::level::info);
    spdlog::set_default_logger(logger);
}

std::string u8(const sfs::path &p) {
    const auto s = p.u8string();
    return std::string(s.begin(), s.end());
}
std::string read_all(const sfs::path &p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
std::set<std::string> names(const sfs::path &dir) {
    std::set<std::string> s;
    std::error_code ec;
    for (auto it = sfs::directory_iterator(dir, ec); !ec && it != sfs::directory_iterator(); it.increment(ec))
        s.insert(u8(it->path().filename()));
    return s;
}

// The 10 imports of VITOX0001 (vita-elf-create -v, host side), as {NID, library NID}.
struct Expected {
    uint32_t nid, lib;
};
constexpr Expected kExpected[] = {
    { 0x49EB4556, 0x6704D3C6 }, { 0xF17FD8B5, 0x6704D3C6 },                             // SceRtcUser
    { 0xB9D5EBDE, 0x37FE725A }, { 0xA91E15EE, 0x37FE725A }, { 0xB8EF5818, 0x37FE725A }, // SceSysmem
    { 0x9670D39F, 0xCAE9ACE6 }, { 0x6C60AC61, 0xCAE9ACE6 }, { 0x7595D9AA, 0xCAE9ACE6 }, // SceLibKernel
    { 0xC70B8886, 0xF2FF276E }, { 0x34EFD876, 0xF2FF276E },                             // SceIofilemgr
};

} // namespace

extern "C" VitoxHomebrewResult vitox_homebrew_probe_run(const VitoxCoreHost *host, const wchar_t *install_src, const wchar_t *local_state,
    const wchar_t *dev_root) {
    g_host = host;
    VitoxHomebrewResult r{};
    install_log_bridge();
    const fs::path ls(local_state), vita_fs = fs::path(dev_root) / "vita-fs";
    const sfs::path s_vita_fs(vita_fs.native()), s_app = s_vita_fs / "ux0" / "app" / "VITOX0001";
    const sfs::path s_result_dir = s_vita_fs / "ux0" / "data" / "VitoxHomebrew", s_result = s_result_dir / "result.txt";
    out("M5B headless homebrew begin: package source '{}' -> '{}'", u8(sfs::path(install_src)), u8(s_app));
    snap("hb_base");

    // ---- install (host step; real Vita3K copy) ----
    std::error_code ec;
    const bool result_dir_preexisting = sfs::exists(s_result_dir, ec);
    if (sfs::exists(s_result, ec)) // stale sentinel from an earlier run of this probe
        sfs::remove(s_result, ec);
    const auto data_before = names(s_vita_fs / "ux0" / "data");
    const bool copied = fs_utils::copy_directory_contents(fs::path(install_src), fs::path(s_app.native()));
    const std::string eboot = read_all(s_app / "eboot.bin"), sfo_bytes = read_all(s_app / "sce_sys" / "param.sfo");
    const std::string pkg_eboot = read_all(sfs::path(install_src) / "eboot.bin");
    const std::string eboot_sha = vitox_sha256_hex(eboot.data(), eboot.size());
    r.installed = copied && !eboot.empty() && eboot == pkg_eboot && !sfo_bytes.empty();
    out("hb install: fs_utils::copy_directory_contents={} | eboot.bin {} B SHA-256 {} (== package copy: {}) | sce_sys/param.sfo {} B SHA-256 {} -> {}", copied,
        eboot.size(), eboot_sha, eboot == pkg_eboot, sfo_bytes.size(), vitox_sha256_hex(sfo_bytes.data(), sfo_bytes.size()), pf(r.installed));
    if (!r.installed)
        return r;

    // ---- load_app_impl() equivalent, headless ----
    snap("hb_emuenv_before_ctor");
    auto *emuenv = new EmuEnvState();
    snap("hb_emuenv_after_ctor");
    emuenv->config_path = ls;
    emuenv->log_path = ls / "log";
    emuenv->cache_path = ls / "cache";
    emuenv->vita_fs_path = vita_fs;
    std::atomic<bool> exited{ false };
    std::atomic<int> exit_code{ -1 };
    std::mutex exit_mutex;
    std::condition_variable exit_cv;
    emuenv->kernel.process_exit_callback = [&](int res, auto) {
        exit_code = res;
        exited = true;
        exit_cv.notify_all();
    };
    const auto call_import = [emuenv](CPUState &cpu, uint32_t nid, SceUID thread_id) { ::call_import(*emuenv, cpu, nid, thread_id); };
    vitox_import_stats_reset();
    bool ok = init(emuenv->io, emuenv->cache_path, emuenv->log_path, emuenv->vita_fs_path, false) && init(emuenv->mem, false);
    snap("hb_after_mem_init");
    ok = ok && emuenv->kernel.init(emuenv->mem, call_import, emuenv->cfg.current_config.cpu_opt);
    emuenv->io.title_id = "VITOX0001";
    emuenv->io.app_path = "VITOX0001";
    emuenv->io.savedata = "VITOX0001";
    emuenv->io.user_id = "00";
    init_device_paths(emuenv->io);
    init_savedata_app_path(emuenv->io, emuenv->vita_fs_path);
    vfs::FileBuffer param_sfo;
    std::string title, title_id;
    if (vfs::read_app_file(param_sfo, emuenv->vita_fs_path, emuenv->io.app_path, "sce_sys/param.sfo") && sfo::load(emuenv->sfo_handle, param_sfo)) {
        sfo::get_data_by_key(title, emuenv->sfo_handle, "TITLE");
        sfo::get_data_by_key(title_id, emuenv->sfo_handle, "TITLE_ID");
    }
    r.sfo = title_id == "VITOX0001";
    out("hb setup: io init + mem init + KernelState::init(call_import=module_parent::call_import, cpu_opt={}) -> {} | param.sfo TITLE='{}' TITLE_ID='{}' -> {}",
        emuenv->cfg.current_config.cpu_opt, ok, title, title_id, pf(r.sfo));
    init_exported_vars(*emuenv);           // real (shim: no variable exports selected)
    vitox_init_selected_libraries(*emuenv); // real LIBRARY_INIT(SceSysmem)
    out("hb setup: init_exported_vars (real; 0 variable exports selected) + LIBRARY_INIT(SceSysmem) (real) | SceFiber/taihen/SceSharedFb/SceAudiodec inits not needed by this import set");

    // ---- load: the real module_parent load_module from the Vita FS ----
    snap("hb_before_load");
    const SceUID main_module_id = ok ? load_module(*emuenv, "app0:eboot.bin") : -1;
    snap("hb_after_load");
    if (main_module_id >= 0) {
        const auto mod = emuenv->kernel.loaded_modules[main_module_id];
        const auto &info = mod->info;
        std::string segs;
        for (const auto &s : info.segments)
            if (s.memsz)
                segs += fmt::format("[vaddr=0x{:08X} memsz=0x{:X}] ", s.vaddr.address(), s.memsz);
        out("hb load_module('app0:eboot.bin') uid={} module='{}' path='{}' start_entry=0x{:08X} segments {}", main_module_id, info.module_name, info.path,
            info.start_entry.address(), segs);
        // The import set as bound by the real loader
        std::map<uint32_t, uint32_t> bound;
        for (const auto &[nid, b] : emuenv->kernel.func_binding_infos)
            bound[nid] = b.library_nid;
        int expected_found = 0;
        bool all_selected = true;
        for (const auto &[nid, lib] : bound) {
            const VitoxSelectedImport *imp = vitox_resolve_selected_import(nid);
            all_selected &= imp != nullptr;
            bool expected = false;
            for (const auto &e : kExpected)
                expected |= e.nid == nid && e.lib == lib;
            expected_found += expected;
            out("hb import bound: library_nid=0x{:08X} nid=0x{:08X} {} -> {}", lib, nid, import_name(nid),
                imp ? fmt::format("selected, REAL ImportFn={}", reinterpret_cast<const void *>(*imp->fn)) : std::string("NOT SELECTED (would be unknown)"));
        }
        r.loaded = info.start_entry.address() != 0;
        r.imports_bound = bound.size() == std::size(kExpected) && expected_found == static_cast<int>(std::size(kExpected)) && all_selected;
        out("hb imports: bound={} expected={} matched={} all_selected={} -> {}", bound.size(), std::size(kExpected), expected_found, all_selected,
            pf(r.imports_bound));

        // ---- run_app() equivalent ----
        const ThreadStatePtr main_thread = emuenv->kernel.create_thread(emuenv->mem, emuenv->io.title_id.c_str(), info.start_entry,
            SCE_KERNEL_DEFAULT_PRIORITY_USER, SCE_KERNEL_CPU_MASK_USER_ALL, SCE_KERNEL_STACK_SIZE_USER_MAIN, nullptr);
        if (main_thread) {
            emuenv->main_thread_id = main_thread->id;
            const auto t0 = std::chrono::steady_clock::now();
            const int sr = main_thread->start(0, Ptr<void>(0), true);
            r.thread = sr >= 0;
            {
                std::unique_lock<std::mutex> lock(exit_mutex);
                exit_cv.wait_for(lock, std::chrono::seconds(10), [&] { return exited.load(); });
            }
            const long long run_us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
            snap("hb_after_run");
            r.exited = exited;
            r.exit_ok = exited && exit_code == 0;
            out("hb run: main thread id={} start={} -> guest exit via sceKernelExitProcess: exited={} exit_code=0x{:X} after {} us -> {}", main_thread->id, sr,
                exited.load(), static_cast<uint32_t>(exit_code.load()), run_us, pf(r.exit_ok));
        }
    } else {
        out("hb load_module('app0:eboot.bin') FAILED: {}", main_module_id);
    }

    // ---- imports actually invoked ----
    VitoxImportStat stats[64];
    const unsigned n = vitox_import_stats(stats, 64);
    unsigned unknown = 0, total = 0;
    for (unsigned i = 0; i < n; ++i) {
        total += stats[i].calls;
        unknown += stats[i].resolved ? 0 : 1;
        out("hb invoked: nid=0x{:08X} {} calls={} -> {}", stats[i].nid, stats[i].name ? stats[i].name : import_name(stats[i].nid), stats[i].calls,
            stats[i].resolved ? fmt::format("REAL ImportFn={}", reinterpret_cast<const void *>(stats[i].fn)) : std::string("UNKNOWN (not selected): FAIL"));
    }
    r.imports_real = n == std::size(kExpected) && unknown == 0 && emuenv->missing_nids.empty();
    out("hb invoked: distinct NIDs={} total calls={} unknown={} emuenv.missing_nids={} | probe-side implementations for these NIDs: NONE -> {}", n, total,
        unknown, emuenv->missing_nids.size(), pf(r.imports_real));

    // ---- sentinel written by the guest through the real Vita IO layer ----
    const std::string sentinel = read_all(s_result);
    std::map<std::string, std::string> kv;
    for (size_t pos = 0; pos < sentinel.size();) {
        const size_t nl = sentinel.find('\n', pos);
        const std::string line = sentinel.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        const size_t eq = line.find('=');
        if (eq != std::string::npos)
            kv[line.substr(0, eq)] = line.substr(eq + 1);
        pos = nl == std::string::npos ? sentinel.size() : nl + 1;
    }
    r.sentinel = kv["VITOX_HOMEBREW"] == "VITOX0001" && kv["VITOX_HOMEBREW_PASS"] == "1" && kv["RTC_LEAP_2024"] == "1" && kv["RTC_LEAP_1900"] == "0"
        && kv["RTC_DAYS_2024_02"] == "29" && kv["MEMBLOCK_PATTERN_OK"] == "1" && kv["MEMBLOCK_FREE"] == "0x00000000" && kv["EXIT_CODE"] == "0x00000000";
    std::string shown = sentinel;
    for (auto &c : shown)
        if (c == '\n')
            c = ';';
    out("hb sentinel '{}' {} B SHA-256 {}: [{}] -> {}", u8(s_result), sentinel.size(), vitox_sha256_hex(sentinel.data(), sentinel.size()), shown,
        pf(r.sentinel));

    // ---- cleanup through Vita3K ----
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
    const int unload_res = main_module_id >= 0 ? unload_module(*emuenv, main_module_id) : -1;
    if (threads_gone) {
        emuenv->kernel.deinit(emuenv->mem);
        deinit_mem(emuenv->mem);
        io_deinit(emuenv->io);
        delete emuenv;
    }
    snap("hb_after_cleanup");
    // Vita3K's process-wide VEH captured the freed MemState: re-anchor it (as after M4.1)
    static MemState *veh_anchor = new MemState;
    if (init(*veh_anchor, false))
        deinit_mem(*veh_anchor);
    r.cleanup = threads_gone && unload_res == 0;
    out("hb cleanup: threads_deleted={} unload_module={} KernelState::deinit+deinit_mem+io_deinit+~EmuEnvState={} -> {}", threads_gone, unload_res, threads_gone,
        pf(r.cleanup));

    // ---- root safety: remove the sentinel (created by this run), keep the app install ----
    sfs::remove(s_result, ec);
    if (!result_dir_preexisting)
        sfs::remove(s_result_dir, ec); // only removes it if empty
    const auto data_after = names(s_vita_fs / "ux0" / "data");
    r.root_safety = data_after == data_before && sfs::exists(s_app / "eboot.bin");
    out("hb root safety: ux0\\data entries before={} after={} (sentinel + VitoxHomebrew removed: {}) | app install kept at '{}' -> {}", data_before.size(),
        data_after.size(), !sfs::exists(s_result), u8(s_app), pf(r.root_safety));
    snap("hb_end");
    out("M5B end: installed={} sfo={} loaded={} imports_bound={} thread={} exited={} exit_ok={} imports_real={} sentinel={} cleanup={} root_safety={}",
        r.installed, r.sfo, r.loaded, r.imports_bound, r.thread, r.exited, r.exit_ok, r.imports_real, r.sentinel, r.cleanup, r.root_safety);
    spdlog::set_default_logger(spdlog::null_logger_mt("vita3k-hb-off"));
    return r;
}
