// Filesystem probe: LocalState and D:\DevelopmentFiles\Vitox.
// Tries three routes per target and logs exact Win32/HRESULT errors:
//   1. Win32 APP-partition APIs (CreateDirectoryW, CreateFile2, DeleteFileW)
//   2. *FromApp variants (fileapifromapp.h)
//   3. std::filesystem + fstream (the route Vita3K's io code actually uses)
#include "common.h"

#include <fileapifromapp.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <fstream>
#include <system_error>

namespace vitox {

namespace {
const char k_payload[] = "Vitox M1 filesystem probe payload 0123456789\n";
constexpr DWORD k_payload_len = sizeof(k_payload) - 1;

using CreateDirFn = BOOL (*)(LPCWSTR);
using OpenFn = HANDLE (*)(LPCWSTR, DWORD, DWORD, DWORD);
using DeleteFn = BOOL (*)(LPCWSTR);

struct Route {
    const char *name;
    CreateDirFn create_dir;
    OpenFn open;
    DeleteFn del;
};

BOOL dir_win32(LPCWSTR p) { return CreateDirectoryW(p, nullptr); }
BOOL dir_fromapp(LPCWSTR p) { return CreateDirectoryFromAppW(p, nullptr); }
HANDLE open_win32(LPCWSTR p, DWORD access, DWORD share, DWORD disp) { return CreateFile2(p, access, share, disp, nullptr); }
HANDLE open_fromapp(LPCWSTR p, DWORD access, DWORD share, DWORD disp) { return CreateFile2FromAppW(p, access, share, disp, nullptr); }
BOOL del_win32(LPCWSTR p) { return DeleteFileW(p); }
BOOL del_fromapp(LPCWSTR p) { return DeleteFileFromAppW(p); }

const Route k_routes[] = {
    { "win32", dir_win32, open_win32, del_win32 },
    { "fromapp", dir_fromapp, open_fromapp, del_fromapp },
};

void log_exists(const char *label, const std::wstring &path) {
    WIN32_FILE_ATTRIBUTE_DATA a{};
    SetLastError(ERROR_SUCCESS);
    BOOL ok = GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a);
    DWORD e = ok ? 0 : GetLastError();
    WIN32_FILE_ATTRIBUTE_DATA b{};
    SetLastError(ERROR_SUCCESS);
    BOOL ok2 = GetFileAttributesExFromAppW(path.c_str(), GetFileExInfoStandard, &b);
    DWORD e2 = ok2 ? 0 : GetLastError();
    log_line(Log::Fs, "[%s] exists? path=%s win32:%s attrs=0x%08lX %s | fromapp:%s attrs=0x%08lX %s", label,
        narrow(path).c_str(), ok ? "yes" : "no", a.dwFileAttributes, ok ? "" : win32_err(e).c_str(),
        ok2 ? "yes" : "no", b.dwFileAttributes, ok2 ? "" : win32_err(e2).c_str());
}

// Returns true when write, read-back verify and delete all succeeded.
bool rw_route(const char *label, const Route &r, const std::wstring &dir) {
    const std::wstring file = dir + L"\\vitox_m1_fs_test_" + widen(r.name) + L".txt";

    SetLastError(ERROR_SUCCESS);
    BOOL dir_ok = r.create_dir(dir.c_str());
    DWORD dir_err = dir_ok ? 0 : GetLastError();
    log_line(Log::Fs, "[%s/%s] create_dir %s -> %s %s", label, r.name, narrow(dir).c_str(),
        dir_ok ? "created" : (dir_err == ERROR_ALREADY_EXISTS ? "already exists" : "FAILED"),
        dir_ok ? "" : win32_err(dir_err).c_str());

    SetLastError(ERROR_SUCCESS);
    HANDLE h = r.open(file.c_str(), GENERIC_WRITE, 0, CREATE_ALWAYS);
    if (h == INVALID_HANDLE_VALUE) {
        log_line(Log::Fs, "[%s/%s] open-for-write %s FAILED %s", label, r.name, narrow(file).c_str(), win32_err(GetLastError()).c_str());
        return false;
    }
    DWORD written = 0;
    BOOL w_ok = WriteFile(h, k_payload, k_payload_len, &written, nullptr);
    DWORD w_err = w_ok ? 0 : GetLastError();
    BOOL f_ok = FlushFileBuffers(h);
    CloseHandle(h);
    log_line(Log::Fs, "[%s/%s] write %s -> %s bytes=%lu flush=%s %s", label, r.name, narrow(file).c_str(),
        w_ok ? "ok" : "FAILED", written, f_ok ? "ok" : "FAILED", w_ok ? "" : win32_err(w_err).c_str());
    if (!w_ok || written != k_payload_len)
        return false;

    SetLastError(ERROR_SUCCESS);
    h = r.open(file.c_str(), GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING);
    if (h == INVALID_HANDLE_VALUE) {
        log_line(Log::Fs, "[%s/%s] open-for-read FAILED %s", label, r.name, win32_err(GetLastError()).c_str());
        return false;
    }
    char buf[128] = {};
    DWORD read = 0;
    BOOL r_ok = ReadFile(h, buf, sizeof(buf), &read, nullptr);
    DWORD r_err = r_ok ? 0 : GetLastError();
    CloseHandle(h);
    const bool match = r_ok && read == k_payload_len && memcmp(buf, k_payload, k_payload_len) == 0;
    log_line(Log::Fs, "[%s/%s] read -> %s bytes=%lu verify=%s %s", label, r.name, r_ok ? "ok" : "FAILED", read,
        match ? "MATCH" : "MISMATCH", r_ok ? "" : win32_err(r_err).c_str());

    SetLastError(ERROR_SUCCESS);
    BOOL d_ok = r.del(file.c_str());
    DWORD d_err = d_ok ? 0 : GetLastError();
    log_line(Log::Fs, "[%s/%s] delete -> %s %s", label, r.name, d_ok ? "ok" : "FAILED", d_ok ? "" : win32_err(d_err).c_str());

    return match && d_ok;
}

bool rw_std(const char *label, const std::wstring &dir) {
    namespace fs = std::filesystem;
    const fs::path d(dir);
    const fs::path file = d / L"vitox_m1_fs_test_std.txt";
    std::error_code ec;

    fs::create_directories(d, ec);
    log_line(Log::Fs, "[%s/std] create_directories -> %s (%d: %s)", label, ec ? "FAILED" : "ok", ec.value(), ec.message().c_str());
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        if (out)
            out.write(k_payload, k_payload_len);
        out.flush();
        log_line(Log::Fs, "[%s/std] ofstream write -> %s", label, out.good() ? "ok" : "FAILED");
        if (!out.good())
            return false;
    }
    std::string back;
    {
        std::ifstream in(file, std::ios::binary);
        back.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    const bool match = back == std::string(k_payload, k_payload_len);
    log_line(Log::Fs, "[%s/std] ifstream read -> bytes=%zu verify=%s", label, back.size(), match ? "MATCH" : "MISMATCH");

    ec.clear();
    const bool removed = fs::remove(file, ec);
    log_line(Log::Fs, "[%s/std] remove -> %s (%d: %s)", label, removed && !ec ? "ok" : "FAILED", ec.value(), ec.message().c_str());
    return match && removed && !ec;
}

// Runs all routes; returns a short status string.
std::string probe_target(const char *label, const std::wstring &dir) {
    log_line(Log::Fs, "[%s] ---- target %s ----", label, narrow(dir).c_str());
    std::string passed;
    for (const Route &r : k_routes) {
        if (rw_route(label, r, dir))
            passed += passed.empty() ? r.name : std::string("+") + r.name;
    }
    if (rw_std(label, dir))
        passed += passed.empty() ? "std" : "+std";
    const std::string result = passed.empty() ? "FAIL (all routes)" : "PASS via " + passed;
    log_line(Log::Fs, "[%s] RESULT %s", label, result.c_str());
    return result;
}
} // namespace

void probe_fs(const std::wstring &local_state) {
    log_line(Log::Fs, "FS probe begin");

    set_status(&Status::fs_local, probe_target("LocalState", local_state + L"\\fs_probe"));

    log_exists("DevFiles", L"D:\\");
    log_exists("DevFiles", L"D:\\DevelopmentFiles");
    log_exists("DevFiles", L"D:\\DevelopmentFiles\\Vitox");
    set_status(&Status::fs_dev, probe_target("DevFiles", L"D:\\DevelopmentFiles\\Vitox"));

    log_line(Log::Fs, "FS probe end");
}

} // namespace vitox
