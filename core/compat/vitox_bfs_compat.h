// Vitox: forced include (/FI) for the out-of-tree Boost.Filesystem build on WindowsStore.
// Boost.Filesystem's Windows backend uses five APIs the app partition does not declare (measured,
// see M41_IO_CONFIG.md); this header routes four of them to app-partition equivalents. The fifth
// (CryptAcquireContextW) is avoided by building with BOOST_FILESYSTEM_HAS_BCRYPT.
// 0.0.8.0: each substitute is an instrumented function in vitox_bfs_compat.cpp, so the hardware run
// reports which compatibility branches were actually exercised (vitox_bfs_compat_report.h).
#pragma once

#if defined(VITOX_UWP) && !defined(VITOX_BFS_COMPAT_IMPL)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

extern "C" {
// CreateFileW (desktop partition) -> CreateFileFromAppW (fileapifromapp.h, app partition; same
// signature; honours the app's broader file-system access, e.g. D:\DevelopmentFiles on Xbox).
HANDLE WINAPI vitox_bfs_create_file(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disposition,
    DWORD flags, HANDLE templ);
// AreFileApisANSI (desktop partition): packaged-app file APIs always use CP_ACP -> TRUE.
BOOL WINAPI vitox_bfs_are_file_apis_ansi();
// GetWindowsDirectoryW (desktop partition): only temp_directory_path()'s last-resort fallback
// (no TMP/TEMP/LOCALAPPDATA/USERPROFILE) uses it; reports failure on WindowsStore.
UINT WINAPI vitox_bfs_get_windows_directory(LPWSTR buffer, UINT size);
// boost::winapi hides these outside BOOST_WINAPI_PARTITION_DESKTOP although the Windows 10 SDK
// declares both for the app partition (startup lookup of optional kernel32/ntdll entry points).
HMODULE WINAPI vitox_bfs_get_module_handle(LPCWSTR name);
FARPROC WINAPI vitox_bfs_get_proc_address(HMODULE module, LPCSTR name);
}

#define CreateFileW vitox_bfs_create_file
#define AreFileApisANSI() vitox_bfs_are_file_apis_ansi()
#define GetWindowsDirectoryW vitox_bfs_get_windows_directory

#include <boost/winapi/config.hpp>
#if !BOOST_WINAPI_PARTITION_DESKTOP
namespace boost::winapi {
inline HMODULE GetModuleHandleW(LPCWSTR name) { return vitox_bfs_get_module_handle(name); }
template <typename Ptr>
inline Ptr get_proc_address(HMODULE module, const char *name) {
    return reinterpret_cast<Ptr>(vitox_bfs_get_proc_address(module, name));
}
} // namespace boost::winapi
#endif
#endif
