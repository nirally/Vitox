// Vitox M8C.1 - staged firmware installer (see vitox_firmware_stage.h). All installer logic is Vita3K's own
// install_pup(); this probe only verifies inputs, points it at the staging root, and validates the output.
#include "vitox_firmware_stage.h"

#include <openssl/evp.h> // compat/openssl-shim (vitox_evp_selftest / vitox_evp_failures)
#include <packages/functions.h>
#include <util/fs.h>
#include <util/log.h>

#include <spdlog/sinks/callback_sink.h>
#include <spdlog/sinks/null_sink.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

std::string vitox_sha256_hex(const void *data, size_t size); // compat/vitox_sha256.cpp

namespace {
namespace sfs = std::filesystem;
const VitoxCoreHost *g_host = nullptr;
std::atomic<unsigned> g_errors{ 0 };

template <typename... Args>
void out(fmt::format_string<Args...> f, Args &&...args) {
    g_host->log(fmt::format(f, std::forward<Args>(args)...).c_str());
}
std::string u8(const sfs::path &p) {
    const auto s = p.u8string();
    return std::string(s.begin(), s.end());
}
std::string sha_file(const sfs::path &p, uintmax_t &size) {
    std::ifstream f(p, std::ios::binary);
    const std::string data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    size = data.size();
    return vitox_sha256_hex(data.data(), data.size());
}
// relative path -> size of every regular file (no symlinks followed)
std::map<std::string, uintmax_t> tree(const sfs::path &dir, unsigned *links = nullptr) {
    std::map<std::string, uintmax_t> t;
    std::error_code ec;
    for (auto it = sfs::recursive_directory_iterator(dir, sfs::directory_options::none, ec); !ec && it != sfs::recursive_directory_iterator(); it.increment(ec)) {
        std::error_code e2;
        if (it->is_symlink(e2) && links)
            ++*links;
        if (it->is_regular_file(e2))
            t[u8(it->path().lexically_relative(dir))] = it->file_size(e2);
    }
    return t;
}
std::string upper(std::string s) {
    for (auto &c : s)
        c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
    return s;
}
struct Input {
    const char *file, *sha;
    const char *alt_sha = nullptr; // 0.0.33.1: a second accepted version
};
const Input kInputs[] = {
    { "PSVUPDAT.PUP", "6EF6DC8DA6DB026F28647713E473486D770087A605C52A8D751BFCA7478386CF" }, // system 3.74: os0 + vs0
    { "PREINST.PUP", "339D1439EB329CFBD1A936F0A1563458E0555EF52975486B2844E24B6049E33E" },  // pre-install 3.74: pd0
    { "FONTPKG.PUP", "F372D306A7F2711125BEA29AA75CBCB1EB19724E9648478FBA07B8D655B37344",  // font package 3.73: sa0
        "C3C03FC7363DD573D90E5157629BF11551F434B283CC898D9FFC71DD716B791C" },       // 0.0.33.1: Sony's 2022 font package
};
} // namespace

extern "C" VitoxFirmwareStageResult vitox_firmware_stage_run(const VitoxCoreHost *host, const wchar_t *dev_root) {
    g_host = host;
    VitoxFirmwareStageResult r{};
    const sfs::path root = sfs::path(dev_root) / "firmware-staging", input = root / "input", staged = root / "vita-fs";
    const sfs::path active = sfs::path(dev_root) / "vita-fs", homebrew = sfs::path(dev_root) / "homebrew";
    std::error_code ec;
    if (!sfs::is_directory(input, ec)) {
        out("m8c.1 firmware staging: no input dir '{}' - not requested", u8(input));
        return r;
    }
    const sfs::path marker = root / "stage-complete.txt";
    if (sfs::exists(marker, ec)) {
        out("m8c.1 firmware staging: '{}' exists - a completed stage is not redone (skipped)", u8(marker));
        return r;
    }
    if (sfs::exists(staged, ec)) {
        // an earlier attempt did not complete: remove ITS output (inside the staging root only) and stage again
        const auto n = sfs::remove_all(staged, ec);
        out("m8c.1 firmware staging: incomplete earlier output '{}' removed ({} entries, {})", u8(staged), n, ec ? ec.message() : "ok");
    }
    // Vita3K's install_pup expects its vita-fs root to exist (as Vita3K's own vita-fs always does)
    sfs::create_directories(staged, ec);
    r.requested = true;
    g_errors = 0;
    auto sink = std::make_shared<spdlog::sinks::callback_sink_mt>([](const spdlog::details::log_msg &msg) {
        if (msg.level >= spdlog::level::err)
            ++g_errors;
        const auto level = spdlog::level::to_string_view(msg.level);
        g_host->log(fmt::format("[vita3k {}] {}", std::string_view(level.data(), level.size()), std::string_view(msg.payload.data(), msg.payload.size())).c_str());
    });
    auto logger = std::make_shared<spdlog::logger>("vita3k-m8c", sink);
    logger->set_level(spdlog::level::info);
    spdlog::set_default_logger(logger);
    out("M8C.1 firmware staging begin: inputs '{}' -> staging '{}' (active vita-fs '{}' is NOT written)", u8(input), u8(staged), u8(active));

    // 1. crypto substitution known-answer tests
    char rep[512] = {};
    r.selftest = vitox_evp_selftest(rep, sizeof(rep));
    out("m8c.1 EVP (BCrypt) self-test: {}-> {}", rep, r.selftest ? "PASS" : "FAIL");

    // 2. manifests that must not change
    const auto active_before = tree(active), homebrew_before = tree(homebrew);

    // 3. hashes immediately before use
    r.hashes = r.selftest;
    for (const Input &in : kInputs) {
        uintmax_t size = 0;
        const std::string h = upper(sha_file(input / in.file, size));
        const bool ok = h == in.sha || (in.alt_sha && h == in.alt_sha);
        r.hashes &= ok;
        out("m8c.1 input {}: {} B SHA-256 {} -> {}", in.file, size, h, ok ? "matches the approved hash" : "MISMATCH");
    }

    // 4. Vita3K's install_pup into the staging root, system firmware first
    if (r.hashes) {
        r.installed = true;
        for (const Input &in : kInputs) {
            const auto t0 = std::chrono::steady_clock::now();
            const unsigned err0 = g_errors, evp0 = vitox_evp_failures();
            std::string ver;
            bool threw = false;
            try {
                ver = install_pup(fs::path(staged.native()), fs::path((input / in.file).native()), [&](uint32_t p) {
                    if (p == 30 || p == 70 || p == 100)
                        out("m8c.1   {} progress {}%", in.file, p);
                });
            } catch (const std::exception &e) {
                threw = true;
                out("m8c.1   {} install_pup threw: {}", in.file, e.what());
            }
            const bool ok = !threw && g_errors == err0 && vitox_evp_failures() == evp0;
            r.installed &= ok;
            out("m8c.1 install_pup({}) -> version.txt '{}' in {:.1f} s | Vita3K error lines {} | EVP failures {} -> {}", in.file, ver,
                std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(), g_errors - err0, vitox_evp_failures() - evp0, ok ? "PASS" : "FAIL");
            if (!ok)
                break; // STOP on a decrypt/decompress error
        }
    }

    // 5. validation of the staged tree
    unsigned links = 0;
    const auto all = tree(staged, &links);
    r.files = static_cast<unsigned>(all.size());
    std::map<std::string, std::pair<unsigned, uintmax_t>> per;
    for (const auto &[rel, size] : all) {
        const std::string top = rel.substr(0, rel.find_first_of("/\\"));
        per[top].first++, per[top].second += size;
        r.bytes += size;
    }
    r.partitions = true;
    for (const char *p : { "os0", "vs0", "sa0", "pd0" }) {
        const auto it = per.find(p);
        const bool ok = it != per.end() && it->second.first > 0;
        r.partitions &= ok;
        out("m8c.1 staged {}: {} files, {} B -> {}", p, ok ? it->second.first : 0, ok ? it->second.second : 0, ok ? "present" : "MISSING");
    }
    for (const auto &[top, v] : per)
        if (top != "os0" && top != "vs0" && top != "sa0" && top != "pd0")
            out("m8c.1 staged other top-level entry '{}': {} files, {} B", top, v.first, v.second);
    for (const char *mod : { "vs0/sys/external/libpgf.suprx", "vs0/sys/external/libpvf.suprx" }) {
        uintmax_t size = 0;
        const sfs::path p = staged / mod;
        const bool present = sfs::is_regular_file(p, ec);
        if (std::string(mod).find("libpgf") != std::string::npos)
            r.libpgf = present;
        out("m8c.1 module {}: {}", mod, present ? fmt::format("{} B SHA-256 {}", sfs::file_size(p, ec), upper(sha_file(p, size))) : std::string("ABSENT"));
    }
    // font resources: every file under the font directories of sa0 and vs0 (names + sizes; a few hashed)
    unsigned font_files = 0, hashed = 0;
    for (const auto &[rel, size] : all) {
        std::string lower = rel;
        for (auto &c : lower)
            c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        if (lower.find("font") == std::string::npos)
            continue;
        ++font_files;
        if (font_files <= 60) {
            uintmax_t s2 = 0;
            const bool hash_it = hashed < 8 && (lower.ends_with(".pgf") || lower.ends_with(".pvf"));
            hashed += hash_it;
            out("m8c.1 font resource: {} ({} B){}", rel, size, hash_it ? " SHA-256 " + upper(sha_file(staged / rel, s2)) : std::string());
        }
    }
    r.fonts = font_files > 0;
    out("m8c.1 font resources: {} files whose path contains 'font' (first 60 listed) -> {}", font_files, r.fonts ? "present" : "MISSING");

    // 6. containment: nothing outside the staging root changed, nothing linked out of it, temp dir removed
    const bool active_same = tree(active) == active_before, homebrew_same = tree(homebrew) == homebrew_before;
    bool escaped = false;
    for (const auto &[rel, _] : all)
        escaped |= rel.starts_with("..") || sfs::path(rel).is_absolute();
    const bool tmp_gone = !sfs::exists(staged / "PUP_DEC", ec);
    r.contained = active_same && homebrew_same && !escaped && links == 0 && tmp_gone;
    out("m8c.1 containment: active vita-fs unchanged={} ({} files) | homebrew root unchanged={} | escaped paths={} | symlinks={} | PUP_DEC removed={} -> {}",
        active_same, active_before.size(), homebrew_same, escaped, links, tmp_gone, r.contained ? "PASS" : "FAIL");
    if (r.selftest && r.hashes && r.installed && r.partitions && r.libpgf && r.fonts && r.contained) {
        std::ofstream(marker) << fmt::format("M8C.1 staged: files={} bytes={} inputs=PSVUPDAT,PREINST,FONTPKG\n", r.files, r.bytes);
        out("m8c.1 completion marker written: '{}'", u8(marker));
    }
    out("M8C.1 end: requested={} selftest={} hashes={} installed={} partitions={} libpgf={} fonts={} contained={} | staged {} files {} B | EVP failures {}", r.requested,
        r.selftest, r.hashes, r.installed, r.partitions, r.libpgf, r.fonts, r.contained, r.files, r.bytes, vitox_evp_failures());
    spdlog::set_default_logger(std::make_shared<spdlog::logger>("vita3k-m8c-off", std::make_shared<spdlog::sinks::null_sink_mt>()));
    return r;
}
