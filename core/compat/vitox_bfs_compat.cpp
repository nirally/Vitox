// Vitox: instrumented WindowsStore substitutes for Boost.Filesystem (see vitox_bfs_compat.h).
// The redirect macros are not applied here.
#define VITOX_BFS_COMPAT_IMPL
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <fileapifromapp.h>

#include "vitox_bfs_compat.h"
#include "vitox_bfs_compat_report.h"

#include <atomic>
#include <cstring>
#include <cwchar>
#include <mutex>

namespace {

std::atomic<unsigned> g_cf_calls{ 0 }, g_cf_ok{ 0 }, g_cf_dev{ 0 }, g_cf_local{ 0 }, g_ansi{ 0 }, g_windir{ 0 }, g_gmh{ 0 };
std::atomic<unsigned long> g_cf_last_error{ 0 };
std::mutex g_lookup_mutex;
VitoxBfsLookup g_lookup[16];
unsigned g_lookups = 0;

bool starts_with_icase(LPCWSTR s, LPCWSTR prefix) { return s && _wcsnicmp(s, prefix, wcslen(prefix)) == 0; }

} // namespace

extern "C" HANDLE WINAPI vitox_bfs_create_file(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disposition,
    DWORD flags, HANDLE templ) {
    ++g_cf_calls;
    // Boost passes long-path prefixed names ("\\?\D:\...") as well as plain ones
    LPCWSTR p = name && wcsncmp(name, L"\\\\?\\", 4) == 0 ? name + 4 : name;
    if (starts_with_icase(p, L"D:\\DevelopmentFiles"))
        ++g_cf_dev;
    if (p && wcsstr(p, L"\\LocalState"))
        ++g_cf_local;
    const HANDLE h = CreateFileFromAppW(name, access, share, sa, disposition, flags, templ);
    if (h != INVALID_HANDLE_VALUE)
        ++g_cf_ok;
    else
        g_cf_last_error = GetLastError();
    return h;
}

extern "C" BOOL WINAPI vitox_bfs_are_file_apis_ansi() {
    ++g_ansi;
    return TRUE;
}

extern "C" UINT WINAPI vitox_bfs_get_windows_directory(LPWSTR, UINT) {
    ++g_windir;
    SetLastError(ERROR_NOT_SUPPORTED);
    return 0;
}

extern "C" HMODULE WINAPI vitox_bfs_get_module_handle(LPCWSTR name) {
    ++g_gmh;
    return GetModuleHandleW(name);
}

extern "C" FARPROC WINAPI vitox_bfs_get_proc_address(HMODULE module, LPCSTR name) {
    const FARPROC p = GetProcAddress(module, name);
    std::lock_guard<std::mutex> lock(g_lookup_mutex);
    if (g_lookups < 16) {
        strncpy_s(g_lookup[g_lookups].name, name, _TRUNCATE);
        g_lookup[g_lookups].resolved = p != nullptr;
        ++g_lookups;
    }
    return p;
}

extern "C" VitoxBfsCompatReport vitox_bfs_compat_report() {
    VitoxBfsCompatReport r{};
    r.create_file_calls = g_cf_calls;
    r.create_file_ok = g_cf_ok;
    r.create_file_dev = g_cf_dev;
    r.create_file_local = g_cf_local;
    r.last_error_fail = g_cf_last_error;
    r.are_file_apis_ansi = g_ansi;
    r.get_windows_directory = g_windir;
    r.get_module_handle = g_gmh;
    std::lock_guard<std::mutex> lock(g_lookup_mutex);
    r.lookups = g_lookups;
    memcpy(r.lookup, g_lookup, sizeof(g_lookup));
    return r;
}

// 0.0.8.1 diagnostics: the two steps of Boost's Windows canonical(), done directly.
// via: 0 = CreateFileFromAppW (the substitute Boost uses), 1 = CreateFile2 (plain app API).
extern "C" unsigned vitox_diag_final_path(const wchar_t *path, int via, unsigned flags, wchar_t *out, unsigned out_len, unsigned long *open_error,
    unsigned long *final_error) {
    *open_error = *final_error = 0;
    out[0] = 0;
    HANDLE h;
    if (via == 0) {
        h = CreateFileFromAppW(path, FILE_READ_ATTRIBUTES | FILE_READ_EA, FILE_SHARE_DELETE | FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    } else {
        CREATEFILE2_EXTENDED_PARAMETERS ep{ sizeof(ep) };
        ep.dwFileFlags = FILE_FLAG_BACKUP_SEMANTICS;
        h = CreateFile2(path, FILE_READ_ATTRIBUTES | FILE_READ_EA, FILE_SHARE_DELETE | FILE_SHARE_READ | FILE_SHARE_WRITE, OPEN_EXISTING, &ep);
    }
    if (h == INVALID_HANDLE_VALUE) {
        *open_error = GetLastError();
        return 0;
    }
    const DWORD n = GetFinalPathNameByHandleW(h, out, out_len, flags);
    if (n == 0 || n >= out_len) {
        *final_error = n == 0 ? GetLastError() : ERROR_INSUFFICIENT_BUFFER;
        out[0] = 0;
    }
    CloseHandle(h);
    return n;
}
