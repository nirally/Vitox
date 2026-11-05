// Vitox M4.2 (0.0.8.0 / 0.0.8.1): REAL Vita3K IO + config on WindowsStore with the Vitox filesystem mapping.
//   LocalState                       -> config (config.yml, config/config_<app>.xml), log, cache
//   D:\DevelopmentFiles\Vitox\vita-fs -> Vita filesystem tree (ux0, uma0, vd0, ...)
// Uses the EmuEnvState-owned IOState and Config. No package/game loading, no renderer/audio/input/net.
// Root safety: an independent std::filesystem observer snapshots both roots (and the names directly
// under D:\DevelopmentFiles) before and after; probe files live in dedicated per-run directories.
#include "vitox_io_probe.h"

#include "vitox_bfs_compat_report.h"

#include <config/functions.h>
#include <config/settings.h>
#include <config/state.h>
#include <emuenv/state.h>
#include <io/functions.h>
#include <io/state.h>
#include <io/types.h>
#include <util/fs.h>
#include <util/log.h>

#include <vitox_lexical_relative.h>

#include <spdlog/sinks/callback_sink.h>
#include <spdlog/sinks/null_sink.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <atomic>
#include <mutex>
#include <set>
#include <string>
#include <vector>

extern "C" unsigned vitox_diag_final_path(const wchar_t *path, int via, unsigned flags, wchar_t *out, unsigned out_len, unsigned long *open_error,
    unsigned long *final_error); // compat/vitox_bfs_compat.cpp

namespace config {
ExitCode vitox_parse_config(Config &cfg, const fs::path &load_path, const fs::path &root_vita_fs_path); // shimmed config.cpp
}

namespace {

const VitoxCoreHost *g_host = nullptr;
namespace sfs = std::filesystem;

template <typename... Args>
void out(fmt::format_string<Args...> f, Args &&...args) {
    g_host->log(fmt::format(f, std::forward<Args>(args)...).c_str());
}
void snap(const char *tag) { g_host->mem_snapshot(tag); }
long long us_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
}
const char *pf(bool b) { return b ? "PASS" : "FAIL"; }
long long run_stamp() { return std::chrono::steady_clock::now().time_since_epoch().count(); }

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
std::string u8(const fs::path &p) { return fs_utils::path_to_utf8(p); }

// ---- independent observer (std::filesystem, never Boost; purely lexical paths) ----
// 0.0.8.0 used sfs::relative(), which goes through weakly_canonical(); canonicalisation fails on
// Xbox (see canonical_diag), which collapsed every entry and invalidated that run's root check.
std::set<std::string> tree(const sfs::path &root) {
    std::set<std::string> s;
    std::error_code ec;
    for (auto it = sfs::recursive_directory_iterator(root, sfs::directory_options::skip_permission_denied, ec);
         !ec && it != sfs::recursive_directory_iterator(); it.increment(ec))
        s.insert(u8(it->path().lexically_relative(root)) + (it->is_directory(ec) ? "/" : "")); // lexical: no canonical()
    return s;
}
std::set<std::string> names(const sfs::path &dir) { // names only, never descends (other projects' folders)
    std::set<std::string> s;
    std::error_code ec;
    for (auto it = sfs::directory_iterator(dir, ec); !ec && it != sfs::directory_iterator(); it.increment(ec))
        s.insert(u8(it->path().filename()));
    return s;
}
std::vector<std::string> minus(const std::set<std::string> &a, const std::set<std::string> &b) {
    std::vector<std::string> r;
    for (const auto &x : a)
        if (!b.count(x))
            r.push_back(x);
    return r;
}
bool sdir(const sfs::path &p) {
    std::error_code ec;
    return sfs::is_directory(p, ec);
}
bool sexists(const sfs::path &p) {
    std::error_code ec;
    return sfs::exists(p, ec);
}
std::string read_all(const sfs::path &p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
uint64_t fnv1a(const std::string &s) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s)
        h = (h ^ c) * 1099511628211ull;
    return h;
}

// ---- the real Vita3K IO path ----
bool io_proof(VitoxIoResult &r, EmuEnvState &emuenv, const fs::path &ls, const fs::path &vita_fs) {
    IOState &io = emuenv.io;
    // Vita3K's logging init creates the log root before init(IOState) runs (init uses create_directory
    // for log/shaderlog and log/texturelog); do the same here.
    fs::create_directories(emuenv.log_path);
    snap("io_before_init");
    auto t0 = std::chrono::steady_clock::now();
    r.io_init = init(io, emuenv.cache_path, emuenv.log_path, emuenv.vita_fs_path, false);
    out("io init(cache='{}', log='{}', vita_fs='{}', redirect_stdio=false) -> {} in {} us", u8(emuenv.cache_path), u8(emuenv.log_path),
        u8(emuenv.vita_fs_path), r.io_init, us_since(t0));
    snap("io_after_init");

    const char *expected[] = { "ux0", "uma0", "vd0", "ux0/data", "ux0/app", "ux0/music", "ux0/picture", "ux0/theme", "ux0/video", "ux0/user",
        "uma0/data", "vd0/registry", "vd0/network" };
    r.io_dirs = true;
    for (const char *d : expected)
        r.io_dirs &= sdir(sfs::path(vita_fs.native()) / d);
    for (const char *d : { "cache/shaders", "log/shaderlog", "log/texturelog" })
        r.io_dirs &= sdir(sfs::path(ls.native()) / d);
    std::string devs;
    for (const auto &n : names(sfs::path(vita_fs.native())))
        devs += n + " ";
    out("io dirs: vita-fs top level = [ {}]; expected Vita/cache/log dirs present -> {}", devs, pf(r.io_dirs));

    io.user_id = "00";
    io.savedata = "VITOX0001";
    io.app_path = "VITOX0001";
    const bool sd = init_savedata_app_path(io, emuenv.vita_fs_path);
    init_device_paths(io);
    const bool sd_dir = sdir(sfs::path(vita_fs.native()) / "ux0/user/00/savedata/VITOX0001");
    r.savedata = sd && sd_dir && io.device_paths.savedata0 == "user/00/savedata/VITOX0001" && io.device_paths.app0 == "app/VITOX0001";
    out("io init_savedata_app_path -> {} dir={} | init_device_paths: savedata0='{}' app0='{}' addcont0='{}' -> {}", sd, sd_dir,
        io.device_paths.savedata0, io.device_paths.app0, io.device_paths.addcont0, pf(r.savedata));

    // Vita-style path translation through the real resolver
    const std::string run_dir = fmt::format("vitox-m42-{}", run_stamp());
    const std::string vdir = "ux0:data/" + run_dir;
    const std::string va = vdir + "/a.bin", vb = vdir + "/b.bin";
    const fs::path ha = expand_path(io, va.c_str(), emuenv.vita_fs_path);
    const fs::path hs = expand_path(io, "savedata0:vitox_m42.bin", emuenv.vita_fs_path);
    const bool t_ux0 = sfs::path(ha.native()).lexically_normal() == (sfs::path(vita_fs.native()) / "ux0/data" / run_dir / "a.bin").lexically_normal();
    const bool t_sd = sfs::path(hs.native()).lexically_normal() == (sfs::path(vita_fs.native()) / "ux0/user/00/savedata/VITOX0001/vitox_m42.bin").lexically_normal();
    r.translation = t_ux0 && t_sd;
    out("io expand_path '{}' -> '{}' ({}) | 'savedata0:vitox_m42.bin' -> '{}' ({}) -> {}", va, u8(ha), t_ux0, u8(hs), t_sd, pf(r.translation));

    // Guest-style file operations through the real io.cpp API
    std::vector<uint8_t> data(4096), back(4096, 0);
    for (size_t i = 0; i < data.size(); ++i)
        data[i] = static_cast<uint8_t>(i * 31 + 7);
    const int mk = create_dir(io, vdir.c_str(), 0777, emuenv.vita_fs_path, "vitox");
    SceUID fd = open_file(io, va.c_str(), SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, emuenv.vita_fs_path, "vitox");
    const int wr = fd >= 0 ? write_file(fd, data.data(), static_cast<SceSize>(data.size()), io, "vitox") : -1;
    const int cl1 = fd >= 0 ? close_file(io, fd, "vitox") : -1;
    SceIoStat st{};
    const int sr = stat_file(io, va.c_str(), &st, emuenv.vita_fs_path, "vitox");
    fd = open_file(io, va.c_str(), SCE_O_RDONLY, emuenv.vita_fs_path, "vitox");
    const int rd = fd >= 0 ? read_file(back.data(), io, fd, static_cast<SceSize>(back.size()), "vitox") : -1;
    const int cl2 = fd >= 0 ? close_file(io, fd, "vitox") : -1;
    const bool same = back == data;
    const int rn = rename(io, va.c_str(), vb.c_str(), emuenv.vita_fs_path, "vitox");
    const bool moved = !sexists(sfs::path(ha.native())) && sexists(sfs::path(ha.native()).parent_path() / "b.bin");
    std::vector<std::string> listed;
    const SceUID dfd = open_dir(io, vdir.c_str(), emuenv.vita_fs_path, "vitox");
    if (dfd >= 0) {
        SceIoDirent dent{};
        while (read_dir(io, dfd, &dent, emuenv.vita_fs_path, "vitox") > 0)
            listed.emplace_back(dent.d_name);
        close_dir(io, dfd, "vitox");
    }
    const bool listed_b = listed.size() == 1 && listed[0] == "b.bin";
    // savedata0 device mapping: create + write + remove one file
    SceUID sfd = open_file(io, "savedata0:vitox_m42.bin", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, emuenv.vita_fs_path, "vitox");
    const int swr = sfd >= 0 ? write_file(sfd, data.data(), 16, io, "vitox") : -1;
    if (sfd >= 0)
        close_file(io, sfd, "vitox");
    const bool sd_file = sexists(sfs::path(hs.native()));
    const int srm = remove_file(io, "savedata0:vitox_m42.bin", emuenv.vita_fs_path, "vitox");
    const int rmf = remove_file(io, vb.c_str(), emuenv.vita_fs_path, "vitox");
    const int rmd = remove_dir(io, vdir.c_str(), emuenv.vita_fs_path, "vitox");
    const bool gone = !sexists(sfs::path(ha.native()).parent_path()) && !sexists(sfs::path(hs.native()));
    r.vita_ops = mk == 0 && wr == 4096 && cl1 == 0 && sr == 0 && st.st_size == 4096 && rd == 4096 && cl2 == 0 && same && rn == 0 && moved
        && listed_b && swr == 16 && sd_file && srm == 0 && rmf == 0 && rmd == 0 && gone;
    std::string ls_names;
    for (const auto &n : listed)
        ls_names += n + " ";
    out("io vita ops (real io.cpp) dir='{}': create_dir={} open(W|CREAT|TRUNC)+write={} close={} stat={} size={} open(R)+read={} close={} "
        "data_equal={} rename(a->b)={} moved={} open_dir/read_dir=[ {}] savedata0 write={} exists={} remove={} | remove_file={} remove_dir={} gone={} -> {}",
        vdir, mk, wr, cl1, sr, st.st_size, rd, cl2, same, rn, moved, ls_names, swr, sd_file, srm, rmf, rmd, gone, pf(r.vita_ops));
    snap("io_after_vita_ops");
    return r.io_init;
}


// ---- 0.0.9.0 (M5A) A: real Vita3K logging::init into LocalState\log ----
std::atomic<int> g_log_cb{ 0 };
void logging_proof(VitoxIoResult &r, const fs::path &ls, long long stamp) {
    Root root;
    root.set_log_path(ls / "log");
    logging::set_log_callback([](std::string, int) { ++g_log_cb; }); // Vita3K's own callback sink
    snap("log_before_init");
    const ExitCode ir = logging::init(root, false); // file sink (+ msvc sink + callback sink), async logger
    logging::set_level(spdlog::level::trace);
    r.log_init = ir == Success;
    const std::string mark = fmt::format("VITOX-M5A-{}", stamp);
    LOG_TRACE("{} level=trace", mark);
    LOG_DEBUG("{} level=debug", mark);
    LOG_INFO("{} level=info", mark);
    LOG_WARN("{} level=warn", mark);
    LOG_ERROR("{} level=error (deliberate test message)", mark);
    LOG_CRITICAL("{} level=critical (deliberate test message)", mark);
    // The default logger is async: flush() queues a flush for the worker thread; shutdown() then
    // drains the queue and joins the worker. (0.0.9.0 called shutdown() alone: registry::shutdown()
    // does not flush and Vita3K keeps the file sink alive in logging::sinks, so vita3k.log stayed
    // 0 B although the callback sink saw all 6 messages.)
    spdlog::default_logger()->flush();
    spdlog::shutdown();
    logging::set_log_callback(nullptr);
    snap("log_after_shutdown");
    const sfs::path file = sfs::path(ls.native()) / "log" / "vita3k.log";
    const std::string txt = read_all(file);
    const char *levels[][2] = { { "trace", "|T|" }, { "debug", "|D|" }, { "info", "|I|" }, { "warn", "|W|" }, { "error", "|E|" }, { "critical", "|C|" } };
    int found = 0;
    std::string per;
    for (const auto &lv : levels) {
        const std::string needle = fmt::format("{} level={}", mark, lv[0]);
        const size_t at = txt.find(needle);
        bool ok = false;
        if (at != std::string::npos) {
            const size_t nl = txt.rfind('\n', at);
            const size_t bol = nl == std::string::npos ? 0 : nl + 1;
            ok = txt.substr(bol, at - bol).find(lv[1]) != std::string::npos; // pattern "|%L|" level tag on the same line
        }
        found += ok;
        per += fmt::format("{}={} ", lv[0], ok ? "found" : "MISSING");
    }
    r.log_verify = r.log_init && found == 6 && g_log_cb >= 6;
    out("logging: logging::init(Root{{log='{}'}}, use_stdout=false)={} set_level(trace); 6 messages '{} level=...' then spdlog::shutdown() | file '{}' {} B: [ {}] "
        "callback sink deliveries={} -> {}",
        u8(ls / "log"), static_cast<int>(ir), mark, u8(file), txt.size(), per, g_log_cb.load(), pf(r.log_verify));
    install_log_bridge(); // back to the probe's own synchronous bridge
}

// ---- 0.0.9.0 (M5A) B1: the U3 lexical helper on its own ----
void lexical_unit(VitoxIoResult &r, const fs::path &vita_fs) {
    const fs::path base = vita_fs / "ux0" / "data" / "vitox-m5a-unit" / "src";
    struct Case {
        const char *name;
        fs::path child, base;
        bool expect_ok;
        const char *expect_rel;
    };
    const Case cases[] = {
        { "normal child", base / "top.bin", base, true, "top.bin" },
        { "nested child", base / "a" / "b" / "deep.dat", base, true, "a/b/deep.dat" },
        { "case-insensitive drive/dirs", fs::path(L"d:\\developmentfiles\\VITOX\\vita-fs\\ux0\\data\\vitox-m5a-unit\\SRC\\x.bin"), base, true, "x.bin" },
        { "sibling (name-prefix trap)", base.parent_path() / "src2" / "f.bin", base, false, "" },
        { "sibling directory", base.parent_path() / "other" / "f.bin", base, false, "" },
        { "'..' escape", base / "a" / ".." / ".." / "outside" / "f.bin", base, false, "" },
        { "'..' above the root", fs::path(L"D:\\..\\..\\Windows\\f.bin"), base, false, "" },
        { "different drive", fs::path(L"C:\\Vitox\\src\\top.bin"), base, false, "" },
        { "relative child", fs::path("src") / "top.bin", base, false, "" },
        { "base itself", base, base, false, "" },
    };
    int pass = 0;
    for (const Case &c : cases) {
        fs::path rel;
        std::string why;
        const bool ok = vitox::lexical_relative(c.child, c.base, rel, &why);
        const bool good = ok == c.expect_ok && (!ok || sfs::path(rel.native()) == sfs::path(c.expect_rel).make_preferred());
        pass += good;
        out("u3 unit [{}] child='{}' -> ok={} rel='{}' why='{}' (expect ok={}) -> {}", c.name, u8(c.child), ok, u8(rel), why, c.expect_ok, pf(good));
    }
    r.u3_unit = pass == static_cast<int>(std::size(cases));
    out("u3 unit: {}/{} -> {}", pass, std::size(cases), pf(r.u3_unit));
}

// ---- 0.0.9.0 (M5A) B2: the REAL Vita3K copy_path -> fs_utils::copy_directory_contents (U3 shim) ----
std::mutex g_trace_mutex;
std::vector<std::string> g_trace;
void lexrel_trace(const fs::path &child, const fs::path &base, const fs::path &rel, bool ok, const std::string &why) {
    std::lock_guard<std::mutex> lock(g_trace_mutex);
    g_trace.push_back(fmt::format("child='{}' base='{}' -> rel='{}' {}{}", u8(child), u8(base), u8(rel), ok ? "ok" : "REJECTED: ", why));
}
std::string pattern(size_t n, unsigned seed) {
    std::string d(n, '\0');
    for (size_t i = 0; i < n; ++i)
        d[i] = static_cast<char>((i * 131 + seed * 17 + (i >> 7)) & 0xFF);
    return d;
}
void copy_proof(VitoxIoResult &r, const fs::path &vita_fs, long long stamp) {
    // Everything lives in one per-run subtree under ux0\data; copy_path's own "vita_fs" argument is
    // a probe-local root inside it, so its target (<root>/ux0/app/<title>) stays inside the subtree.
    const sfs::path probe = sfs::path(vita_fs.native()) / "ux0" / "data" / fmt::format("vitox-m5a-{}", stamp);
    const sfs::path src = probe / "src", vfs = probe / "vfs";
    struct File {
        const char *rel;
        size_t size;
        unsigned seed;
    };
    const File files[] = { { "top.bin", 1000, 1 }, { "a/mid.txt", 333, 2 }, { "a/b/deep.dat", 4096, 3 }, { "a/b/c/deepest.bin", 70000, 4 } };
    std::error_code ec;
    sfs::create_directories(src / "a" / "b" / "c", ec);
    sfs::create_directories(src / "empty_dir", ec);
    for (const File &f : files) {
        std::ofstream o(src / sfs::path(f.rel).make_preferred(), std::ios::binary);
        const std::string d = pattern(f.size, f.seed);
        o.write(d.data(), static_cast<std::streamsize>(d.size()));
    }
    const bool built = sexists(src / "a" / "b" / "c" / "deepest.bin");
    {
        std::lock_guard<std::mutex> lock(g_trace_mutex);
        g_trace.clear();
    }
    vitox::set_lexical_relative_trace(&lexrel_trace);
    snap("u3_before_copy");
    const bool res = copy_path(fs::path(src.native()), fs::path(vfs.native()), "VITOXCP01", "gp"); // REAL io.cpp copy_path
    snap("u3_after_copy");
    vitox::set_lexical_relative_trace(nullptr);
    const sfs::path dst = vfs / "ux0" / "app" / "VITOXCP01";
    int equal = 0;
    std::string fl;
    for (const File &f : files) {
        const sfs::path p = dst / sfs::path(f.rel).make_preferred();
        const std::string got = read_all(p);
        const bool eq = got == pattern(f.size, f.seed);
        equal += eq;
        fl += fmt::format("{} ({} B, fnv1a64={:016x}) {}; ", f.rel, got.size(), fnv1a(got), eq ? "byte-equal" : "DIFFERENT");
    }
    // containment: after the copy (and copy_path's removal of its source) only vfs/... may exist
    const auto all = tree(probe);
    bool contained = !all.empty();
    for (const auto &e : all)
        contained &= e.rfind("vfs", 0) == 0;
    const bool src_removed = !sexists(src); // copy_path removes its source after copying
    const bool empty_dir = sdir(dst / "empty_dir");
    std::vector<std::string> trace;
    {
        std::lock_guard<std::mutex> lock(g_trace_mutex);
        trace = g_trace;
    }
    for (const auto &t : trace)
        out("u3 copy derivation: {}", t);
    bool all_ok = !trace.empty();
    for (const auto &t : trace)
        all_ok &= t.find("REJECTED") == std::string::npos;
    // src holds 4 files + 4 directories (a, a/b, a/b/c, empty_dir): one derivation per entry
    const auto removed = sfs::remove_all(probe, ec);
    const bool gone = !sexists(probe);
    r.u3_copy = built && res && equal == static_cast<int>(std::size(files)) && empty_dir && contained && src_removed && all_ok && trace.size() == 8 && gone;
    out("u3 copy_path(src='{}', vita_fs='{}', 'VITOXCP01', 'gp') = {} -> target '{}': [ {}] empty_dir copied={} | lexical derivations={} (want 8: 4 files + 4 dirs) "
        "all_ok={} | contained under probe\\vfs={} | source removed by copy_path={} | cleanup remove_all={} gone={} -> {}",
        u8(src), u8(vfs), res, u8(dst), fl, empty_dir, trace.size(), all_ok, contained, src_removed, removed, gone, pf(r.u3_copy));
}

// ---- 0.0.8.1: canonical() / relative() on Xbox (0.0.8.0: boost canonical returned an empty path) ----
std::string wide_u8(const wchar_t *w) { return u8(sfs::path(w)); }
void canonical_diag(VitoxIoResult &r, const fs::path &d, const fs::path &ls) {
    struct Case {
        const char *label;
        fs::path p;
    };
    const Case cases[] = { { "D: clean", d / "x" / "one.txt" }, { "D: dotdot", d / "x" / "y" / ".." / "one.txt" }, { "LocalState dir", ls } };
    bool boost_ok = true;
    for (const Case &c : cases) {
        boost::system::error_code bec;
        const fs::path bc = fs::canonical(c.p, bec);
        std::error_code sec, wec;
        const sfs::path sc = sfs::canonical(sfs::path(c.p.native()), sec);
        const sfs::path wc = sfs::weakly_canonical(sfs::path(c.p.native()), wec);
        wchar_t buf[1024];
        unsigned long oe = 0, fe = 0;
        std::string raw;
        const struct {
            int via;
            unsigned flags;
            const char *name;
        } raws[] = { { 0, 0x0, "FromApp/DOS" }, { 0, 0x2, "FromApp/NT" }, { 1, 0x0, "CreateFile2/DOS" }, { 1, 0x2, "CreateFile2/NT" } };
        for (const auto &rw : raws) {
            const unsigned n = vitox_diag_final_path(c.p.c_str(), rw.via, rw.flags, buf, 1024, &oe, &fe);
            raw += fmt::format("{}: n={} open_err={} final_err={} '{}'; ", rw.name, n, oe, fe, n ? wide_u8(buf) : "");
        }
        boost_ok &= !bec;
        out("canonical diag [{}] '{}': boost canonical='{}' ec={} ({}) | std canonical='{}' ec={} ({}) | std weakly_canonical='{}' ec={} | raw GetFinalPathNameByHandleW: {}",
            c.label, u8(c.p), u8(bc), bec.value(), bec.message(), u8(sc), sec.value(), sec.message(), u8(wc), wec.value(), raw);
    }
    boost::system::error_code rec;
    const fs::path brel = fs::relative(d / "x" / "one.txt", d, rec);
    std::error_code srec;
    const sfs::path srel = sfs::relative(sfs::path((d / "x" / "one.txt").native()), sfs::path(d.native()), srec);
    const sfs::path lrel = sfs::path((d / "x" / "one.txt").native()).lexically_relative(sfs::path(d.native()));
    r.bfs_canonical = boost_ok;
    r.bfs_relative = !rec && sfs::path(brel.native()) == sfs::path("x") / "one.txt";
    out("relative diag: boost fs::relative='{}' ec={} ({}) | std relative='{}' ec={} | lexically_relative='{}' -> canonical {} / relative {} (reported, not an M4.2 requirement)",
        u8(brel), rec.value(), rec.message(), u8(srel), srec.value(), u8(lrel), pf(r.bfs_canonical), pf(r.bfs_relative));
}

// ---- Boost.Filesystem runtime, directly, on D:\DevelopmentFiles ----
void bfs_proof(VitoxIoResult &r, const fs::path &vita_fs, const fs::path &ls) {
    const fs::path u1 = fs::unique_path("vitox-m42-bfs-%%%%-%%%%-%%%%");
    const fs::path u2 = fs::unique_path("vitox-m42-bfs-%%%%-%%%%-%%%%");
    const fs::path d = vita_fs / "ux0" / "data" / u1;
    boost::system::error_code ec;
    const bool mk = fs::create_directories(d / "x" / "y", ec) && !ec;
    {
        fs::ofstream f(d / "x" / "one.txt");
        f << "vitox m4.2 boost.filesystem";
    }
    const auto sz = fs::file_size(d / "x" / "one.txt", ec);
    const bool cp = fs::copy_file(d / "x" / "one.txt", d / "x" / "y" / "two.txt", ec) && !ec;
    fs::rename(d / "x" / "y" / "two.txt", d / "x" / "y" / "three.txt", ec);
    const bool rn = !ec && fs::exists(d / "x" / "y" / "three.txt") && !fs::exists(d / "x" / "y" / "two.txt");
    int top = 0, rec = 0;
    for (fs::directory_iterator it(d / "x", ec); !ec && it != fs::directory_iterator(); it.increment(ec))
        ++top;
    for (fs::recursive_directory_iterator it(d, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
        ++rec;
    (void)ls; // 0.0.9.0: canonical_diag() is no longer run (U3 is recorded; see 0.0.8.1)
    const bool status_ok = fs::is_regular_file(d / "x" / "one.txt") && fs::is_directory(d / "x" / "y");
    const bool rm1 = fs::remove(d / "x" / "one.txt", ec) && !ec;
    const auto removed = fs::remove_all(d, ec);
    const bool gone = !ec && !fs::exists(d);
    boost::system::error_code tec;
    const fs::path tmp = fs::temp_directory_path(tec);
    r.bfs_ops = u1 != u2 && mk && sz == 27 && cp && rn && top == 2 && rec == 4 && status_ok && rm1 && removed == 4 && gone;
    out("bfs (Boost.Filesystem runtime) on '{}': unique_path(BCrypt) '{}' != '{}' -> {} | create_directories={} ofstream+file_size={} copy_file={} "
        "rename={} directory_iterator={} (want 2) recursive_directory_iterator={} (want 4) status={} remove={} remove_all={} (want 4) gone={} "
        "| temp_directory_path='{}' ec={} ({}) -> {}",
        u8(d), u8(u1), u8(u2), u1 != u2, mk, sz, cp, rn, top, rec, status_ok, rm1, removed, gone, u8(tmp), tec.value(),
        tec.message(), pf(r.bfs_ops));
}

// ---- config: CLI-free, programmatic, LocalState ----
void config_proof(VitoxIoResult &r, EmuEnvState &emuenv, const fs::path &ls, const fs::path &vita_fs) {
    Config &cfg = emuenv.cfg;
    cfg.config_path = emuenv.config_path;
    cfg.set_vita_fs_path(vita_fs);
    cfg.log_level = 2;
    cfg.cpu_opt = false;
    cfg.pstv_mode = true;
    cfg.sys_lang = 5;
    cfg.sys_button = 0;
    cfg.modules_mode = 1;
    cfg.user_id = "00";
    cfg.user_lang = "en";
    cfg.resolution_multiplier = 1.5f;
    cfg.show_welcome = false;
    cfg.lle_modules = { "libfios2", "libc" };
    out("config written: log-level=2 cpu-opt=false pstv-mode=true sys-lang=5 sys-button=0 modules-mode=1 user-id='00' user-lang='en' "
        "resolution-multiplier=1.5 show-welcome=false lle-modules=[libfios2, libc] pref-path='{}'", cfg.vita_fs_path);

    snap("config_before_save");
    // current_config <- global (no app path), then the real save path: per-app XML + global YAML
    config::set_current_config(cfg, emuenv.config_path, "");
    config::save_current_config(cfg, emuenv.config_path, "VITOX0001", true);
    const sfs::path yml = sfs::path(ls.native()) / "config.yml", xml = sfs::path(ls.native()) / "config" / "config_VITOX0001.xml";
    const std::string ytxt = read_all(yml), xtxt = read_all(xml);
    r.config_save = !ytxt.empty() && !xtxt.empty();
    out("config save_current_config(LocalState, 'VITOX0001', create_custom=true): '{}' {} B fnv1a64={:016x} | '{}' {} B fnv1a64={:016x} -> {}", u8(yml),
        ytxt.size(), fnv1a(ytxt), u8(xml), xtxt.size(), fnv1a(xtxt), pf(r.config_save));

    // a fresh object, parsed through the real YAML/config load path
    auto fresh = std::make_unique<Config>();
    const ExitCode pr = config::vitox_parse_config(*fresh, emuenv.config_path / "config.yml", vita_fs);
    r.config_parse = pr == Success;
    const Config &b = *fresh;
    const bool fields = b.log_level == 2 && !b.cpu_opt && b.pstv_mode && b.sys_lang == 5 && b.sys_button == 0 && b.modules_mode == 1 && b.user_id == "00"
        && b.user_lang == "en" && b.resolution_multiplier == 1.5f && !b.show_welcome && b.lle_modules == std::vector<std::string>{ "libfios2", "libc" }
        && b.vita_fs_path == cfg.vita_fs_path && b.get_vita_fs_path() == vita_fs;
    // re-serialize the parsed object into a run-scoped directory and compare bytes
    const fs::path rt_dir = emuenv.config_path / fmt::format("vitox-m42-roundtrip-{}", run_stamp());
    const ExitCode sr = config::serialize_config(*fresh, rt_dir / "config.yml");
    const std::string rtxt = read_all(sfs::path(rt_dir.native()) / "config.yml");
    const bool bytes_equal = sr == Success && rtxt == ytxt;
    boost::system::error_code ec;
    fs::remove_all(rt_dir, ec);
    r.config_roundtrip = r.config_parse && fields && bytes_equal && !sexists(sfs::path(rt_dir.native()));
    out("config parse(real YAML path, CLI-free)={} -> read: log-level={} cpu-opt={} pstv-mode={} sys-lang={} sys-button={} modules-mode={} user-id='{}' "
        "user-lang='{}' resolution-multiplier={} show-welcome={} lle-modules={} pref-path='{}' | fields_equal={} reserialized {} B fnv1a64={:016x} bytes_equal={} -> {}",
        static_cast<int>(pr), b.log_level, b.cpu_opt, b.pstv_mode, b.sys_lang, b.sys_button, b.modules_mode, b.user_id, b.user_lang, b.resolution_multiplier,
        b.show_welcome, b.lle_modules.size(), b.vita_fs_path, fields, rtxt.size(), fnv1a(rtxt), bytes_equal, pf(r.config_roundtrip));

    // settings.cpp: per-app custom config. Flip the global first so only the XML can restore it.
    fresh->cpu_opt = true;
    fresh->modules_mode = 0;
    config::set_current_config(*fresh, emuenv.config_path, "VITOX0001");
    const auto &cc = fresh->current_config;
    r.settings = !cc.cpu_opt && cc.modules_mode == 1 && cc.lle_modules == std::vector<std::string>{ "libfios2", "libc" } && cc.resolution_multiplier == 1.5f
        && config::has_custom_config(emuenv.config_path, "VITOX0001");
    out("settings set_current_config(LocalState, 'VITOX0001') [global flipped to cpu-opt=true modules-mode=0]: current.cpu-opt={} modules-mode={} lle={} "
        "resolution={} has_custom_config={} -> {}",
        cc.cpu_opt, cc.modules_mode, cc.lle_modules.size(), cc.resolution_multiplier, config::has_custom_config(emuenv.config_path, "VITOX0001"),
        pf(r.settings));
    snap("config_after_load");
}

} // namespace

extern "C" VitoxIoResult vitox_io_probe_run(const VitoxCoreHost *host, const wchar_t *local_state, const wchar_t *dev_root) {
    g_host = host;
    VitoxIoResult r{};
    install_log_bridge();
    const fs::path ls(local_state), dev(dev_root), vita_fs = dev / "vita-fs";
    const sfs::path s_ls(local_state), s_dev(dev_root), s_devfiles = s_dev.parent_path();
    out("IO/config probe begin: LocalState='{}' DevelopmentFiles root='{}' vita_fs='{}'", u8(ls), u8(dev), u8(vita_fs));
    snap("io_base");

    const auto pre_ls = tree(s_ls), pre_dev = tree(s_dev), pre_top = names(s_devfiles);
    out("root safety: before: LocalState entries={} Vitox entries={} names directly under '{}'={}", pre_ls.size(), pre_dev.size(), u8(s_devfiles),
        pre_top.size());

    const long long stamp = run_stamp();
    try {
        logging_proof(r, ls, stamp);
        lexical_unit(r, vita_fs);
        copy_proof(r, vita_fs, stamp);
    } catch (const std::exception &e) {
        out("M5A logging/U3 EXCEPTION: {}", e.what());
        install_log_bridge();
    }

    try {
        snap("io_emuenv_before_ctor");
        auto *emuenv = new EmuEnvState(); // real Vita3K constructor
        snap("io_emuenv_after_ctor");
        r.emuenv = true;
        // The Vitox mapping in EmuEnvState's own root slots (shared/static/patch stay empty: not needed yet)
        emuenv->config_path = ls;
        emuenv->log_path = ls / "log";
        emuenv->cache_path = ls / "cache";
        emuenv->vita_fs_path = vita_fs;
        out("emuenv roots: config_path='{}' log_path='{}' cache_path='{}' vita_fs_path='{}' shared/static_assets/patch=(empty)", u8(emuenv->config_path),
            u8(emuenv->log_path), u8(emuenv->cache_path), u8(emuenv->vita_fs_path));

        if (io_proof(r, *emuenv, ls, vita_fs)) {
            bfs_proof(r, vita_fs, ls);
            io_deinit(emuenv->io);
            r.io_deinit = emuenv->io.std_files.empty() && emuenv->io.dir_entries.empty() && emuenv->io.device_paths.savedata0.empty();
            out("io io_deinit -> std_files={} dir_entries={} device_paths cleared={} -> {}", emuenv->io.std_files.size(), emuenv->io.dir_entries.size(),
                emuenv->io.device_paths.savedata0.empty(), pf(r.io_deinit));
        }
        config_proof(r, *emuenv, ls, vita_fs);
        delete emuenv;
        snap("io_emuenv_after_dtor");
        r.cleanup = true;
    } catch (const std::exception &e) {
        out("IO/config probe EXCEPTION: {}", e.what());
    }

    const VitoxBfsCompatReport br = vitox_bfs_compat_report();
    std::string lk;
    for (unsigned i = 0; i < br.lookups && i < 16; ++i)
        lk += fmt::format("{}={} ", br.lookup[i].name, br.lookup[i].resolved ? "resolved" : "null");
    r.bfs_compat = br.create_file_dev > 0 && br.create_file_ok > 0; // LocalState count is logged, not required
    out("bfs compat exercised: CreateFileW->CreateFileFromAppW calls={} ok={} (DevelopmentFiles={} LocalState={}) last_fail_error={} | AreFileApisANSI->TRUE calls={} "
        "| GetWindowsDirectoryW->0 calls={} | boost::winapi GetModuleHandleW calls={} get_proc_address: [ {}] | unique_path: BOOST_FILESYSTEM_HAS_BCRYPT branch "
        "(the only one compiled) -> {}",
        br.create_file_calls, br.create_file_ok, br.create_file_dev, br.create_file_local, br.last_error_fail, br.are_file_apis_ansi, br.get_windows_directory,
        br.get_module_handle, lk, pf(r.bfs_compat));

    const auto post_ls = tree(s_ls), post_dev = tree(s_dev), post_top = names(s_devfiles);
    const auto removed_ls = minus(pre_ls, post_ls), removed_dev = minus(pre_dev, post_dev), new_top = minus(post_top, pre_top),
               gone_top = minus(pre_top, post_top);
    const auto created_ls = minus(post_ls, pre_ls), created_dev = minus(post_dev, pre_dev);
    std::string c1, c2;
    for (const auto &x : created_ls)
        c1 += x + " ";
    for (const auto &x : created_dev)
        c2 += x + " ";
    bool leftovers = false;
    for (const auto &x : post_dev)
        leftovers |= x.find("vitox-m42-") != std::string::npos;
    for (const auto &x : post_ls)
        leftovers |= x.find("vitox-m42-") != std::string::npos;
    r.root_safety = removed_ls.empty() && removed_dev.empty() && new_top.empty() && gone_top.empty() && !leftovers;
    out("root safety: after: created under LocalState ({}): [ {}]", created_ls.size(), c1);
    out("root safety: after: created under '{}' ({}): [ {}]", u8(s_dev), created_dev.size(), c2);
    out("root safety: pre-existing removed: LocalState={} Vitox={} | names under DevelopmentFiles added={} removed={} | probe leftovers={} -> {}", removed_ls.size(),
        removed_dev.size(), new_top.size(), gone_top.size(), leftovers, pf(r.root_safety));
    snap("io_after_cleanup");
    out("M5A: log_init={} log_verify={} u3_unit={} u3_copy={}", r.log_init, r.log_verify, r.u3_unit, r.u3_copy);
    out("IO/config probe end: emuenv={} io_init={} io_dirs={} savedata={} translation={} vita_ops={} bfs_ops={} bfs_compat={} io_deinit={} config_save={} "
        "config_parse={} config_roundtrip={} settings={} root_safety={} cleanup={}",
        r.emuenv, r.io_init, r.io_dirs, r.savedata, r.translation, r.vita_ops, r.bfs_ops, r.bfs_compat, r.io_deinit, r.config_save, r.config_parse,
        r.config_roundtrip, r.settings, r.root_safety, r.cleanup);
    spdlog::set_default_logger(spdlog::null_logger_mt("vita3k-io-off"));
    return r;
}
