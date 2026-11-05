# M4.1 parallel inventory: IO init + programmatic Config (WindowsStore), 2026-10-18

- **Vita3K:** `7224a0fc`, untouched.
- **Method:** link probes built as WindowsStore DLLs in `core-build` (targets `vitox_m41_{io,config}_{nobfs,bfs}`). They are never packaged and never run.
- **Layers:** each probe is linked twice, so the closure shows up layer by layer:
  - `nobfs`: real TUs + `vitox_core`
  - `bfs`: adds the out-of-tree Boost.Filesystem, the shimmed `fs_utils.cpp` and `physical_key.cpp`
- **Logs:** `out\m41\*.log`, `*.unresolved.txt`, `*.imports.txt`.

## Probes (`inv-io-config\`)
- **`inv_io.cpp`:** `IOState io; init(io, LocalState/cache, LocalState/log, DevFiles/vita-fs, false)`, then `init_savedata_app_path`, `init_device_paths` and `io_deinit`.
- **`inv_config.cpp`:**
  1. `Config cfg`, then set `vita_fs_path`, `config_path`, `cpu_opt`, `pstv_mode` and `log_level` programmatically.
  2. `config::serialize_config(cfg, LocalState/config.yml)`.
  3. `config::vitox_parse_config(back, ...)`, i.e. the real YAML load path.
  4. `config::set_current_config(back, LocalState, app)` from settings.cpp (pugixml per-app config).

## Results
| Probe | Unresolved (first layer) | Unresolved (final) | DLL imports |
|---|---|---|---|
| IO | 23 (20 Boost.Filesystem + 3 fs_utils) | **0** | only api-ms-win-core-* + `_APP` CRT |
| Config | 21 (17 Boost.Filesystem + 2 fs_utils + 2 input key names) | **0** | only api-ms-win-core-* + `_APP` CRT |

Neither final DLL has any SDL, Shell32, CLI11, curl, OpenSSL, renderer or audio dependency. The only file-open import is **`CreateFileFromAppW`**.

## Exact TUs
- **IO:**
  - `io/src/{device,filesystem,io,state_functions}.cpp`: real, unmodified.
  - `util/src/fs_utils.cpp`: **shimmed**.
  - **Boost.Filesystem**: compiled out-of-tree.
- **Config:**
  - `config/src/config.cpp`: **shimmed**.
  - `config/src/settings.cpp`: real.
  - `input/src/physical_key.cpp`: real, pure.
  - `util/src/fs_utils.cpp`: **shimmed**.
  - Boost.Filesystem.
  - yaml-cpp and pugixml: in-tree, configured as in Vita3K's `external/CMakeLists.txt`.
- **Already in `vitox_core`:** `util/src/string_utils.cpp` and `rtc`.

## Shims (all guarded `vitox_substitute()`; configure fails on upstream drift)
1. **`config.cpp` (CLI11 removed):**
   - `#include <CLI11.hpp>` is dropped.
   - `init_config()` is compiled out with `#if 0`. It is the only CLI11 user; CLI11's `compute_win32_argv` needs `CommandLineToArgvW` (shellapi.h, desktop).
   - The file-static `parse()` is exposed as `config::vitox_parse_config()`.
   - The YAML load/save logic is otherwise unchanged, including `serialize_config`, `merge` and legacy key reset.
   - **Upstream-style fix:** split `init_config`'s CLI front end from config load/save into separate TUs.
2. **`fs_utils.cpp` (SDL removed):**
   - `read_data()` is the only SDL user (`SDL_IOFromFile/GetIOSize/ReadIO/CloseIO`, whole-file reads). It is replaced by `std::ifstream` with the same results: open failure, an empty file and a short read all return false, and the data is cleared on a short read.
   - This is the same SDL_IOStream item as M3 B1. **Upstream-style fix:** use std streams there.

## Boost.Filesystem on WindowsStore (the main finding)
Vita3K's `fs` is **Boost.Filesystem** (`util/fs.h`: `namespace fs = boost::filesystem`), not `std::filesystem`. It is a compiled library that Vita3K builds with b2 inside `external/boost`; building it there would write into the read-only tree. Here it is compiled from `external/boost/libs/filesystem/src/*.cpp` with the definitions of its own CMakeLists.

**The unmodified sources do not compile for WindowsStore.** A per-API declaration test against the app partition (Windows SDK 10.0.26100, `WINAPI_FAMILY_APP`) showed:

| API used by Boost.Filesystem | App partition | Used for | Vitox handling |
|---|---|---|---|
| `CreateFileW` | **not declared** | every handle-based op (`windows_tools.hpp`, `operations.cpp`) | `#define CreateFileW CreateFileFromAppW` (same signature; app partition; honours DevelopmentFiles access) |
| `CryptAcquireContextW` / `CryptGenRandom` | **not declared** | `unique_path` | build with `BOOST_FILESYSTEM_HAS_BCRYPT` (what Boost's own CMake check sets; `BCryptGenRandom` is app-partition) |
| `AreFileApisANSI` | **not declared** | path codecvt code page | `TRUE`, because packaged-app file APIs use CP_ACP (`SetFileApisToOEM` is desktop-only too) |
| `GetWindowsDirectoryW` | **not declared** | last-resort `temp_directory_path` fallback only | returns 0 / `ERROR_NOT_SUPPORTED` |
| `GetModuleHandleW`, `GetProcAddress` | **declared**, but `boost::winapi` hides them outside `BOOST_WINAPI_PARTITION_DESKTOP` | startup lookup of optional kernel32/ntdll entry points (`GetFileInformationByHandleEx`, `SetFileInformationByHandle`, `CreateHardLinkW`, `CreateSymbolicLinkW`, `NtCreateFile`, `NtQueryDirectoryFile`) | `using ::GetModuleHandleW` + `get_proc_address` in `boost::winapi` |

Also measured: `CreateSymbolicLinkW` and `CommandLineToArgvW` are not declared. Boost resolves `CreateSymbolicLinkW` at runtime, so it may be null. The following ARE declared for the app partition:
- `CreateFile2`, `CreateFileFromAppW`
- `DeviceIoControl`, `GetFinalPathNameByHandleW`, `FindFirstFileExW`
- `MoveFileExW`, `CopyFileW`/`CopyFile2`, `CreateHardLinkW`
- `GetFileInformationByHandleEx`, `SetFileInformationByHandle`
- `GetTempPathW`, `Get/SetCurrentDirectoryW`, `GetCommandLineW`
- `BCryptGenRandom`

All four substitutes live in one forced include, `compat\vitox_bfs_compat.h`, applied to the Boost.Filesystem TUs only. With it, all 10 Boost.Filesystem TUs compile and both probes link.

**Not yet verified on hardware:**
- Boost.Filesystem's runtime behaviour on Xbox: the `GetModuleHandleW(L"ntdll.dll")` → `NtCreateFile`/`NtQueryDirectoryFile` directory-iterator path, and `CreateFileFromAppW` on `D:\DevelopmentFiles`.
- M1 already proved Win32 app APIs, `CreateFile2FromAppW` and `std::filesystem`/fstream there.

## Filesystem APIs on the IO path
- **`io.cpp` init:** `fs::create_directories` for every writable `VitaIoDevice` under `vita_fs_path` (ux0, uma0, vd0, …), `ux0/{data,app,music,picture,theme,video,user}`, `uma0/data`, `vd0/{registry,network}`, `cache/shaders` and `log/{shaderlog,texturelog}`.
- **Guest file I/O (not in the init path):**
  - `_wfopen` (io/filesystem.h), `_wstati64`
  - dirent `_wopendir` (external/dirent, FindFirstFileExW)
  - `fs::ofstream` / `fs::recursive_directory_iterator`, `fs::file_size`, `fs::rename`, `fs::remove_all`
  - These are UCRT plus Boost.Filesystem. The UCRT/fstream route is what M1 proved on DevelopmentFiles.
- **`IOState` defaults:** `case_isens_find_enabled` is false on `_WIN32` (NTFS is case-insensitive), so it matches desktop Windows.

## Config route (no CLI11, no Shell32)
- **Construction:** `Config{}` is header-inline (M4: no closure). Run-time fields are plain public members set directly (CONFIG_LIST in `config/config.h`).
- **Paths:** use `set_vita_fs_path()`, which goes through `fs_utils::path_to_utf8`.
- **Save:** `config::serialize_config(cfg, path)` is public and unchanged.
- **Load:** upstream reaches `parse()` only through `init_config()`, the CLI entry. The shim's `vitox_parse_config()` is the minimal out-of-tree route.
- **Per-app config:** `settings.cpp` (`set_current_config` / `save_current_config`, pugixml XML under `config_path/config/`) works unchanged.
- **`Root` (util/fs.h):** a plain setter/getter class that `app_init` normally fills. Vitox can set every root explicitly.

## LocalState + DevelopmentFiles mapping: feasible without permanent Vita3K edits
Every IO/config entry point takes explicit paths, so no Vita3K edit is needed for the mapping:
- `init(IOState&, cache, log, vita_fs, redirect_stdio)`
- `serialize_config`/`vitox_parse_config`
- `Root` setters

| Vita3K root | Proposed Xbox location |
|---|---|
| config (`config.yml`, per-app `config/*.xml`) | LocalState |
| log (`log/shaderlog`, `log/texturelog`) | LocalState |
| cache (`cache/shaders`) | LocalState (LocalCache later if size matters) |
| vita_fs (ux0/uma0/vd0/…: apps, savedata, registry) | `D:\DevelopmentFiles\Vitox\vita-fs` (large user content) |
| shared / static assets / patch | not needed yet (package/firmware/game stages) |

**The remaining items are out-of-tree and guarded:**
1. Compiling Boost.Filesystem with `vitox_bfs_compat.h`.
2. The `config.cpp` CLI split.
3. The `fs_utils.cpp` read_data stream shim.

Items 2 and 3 are upstream-style refactor candidates. Item 1 could be an upstream Boost.Winapi/Filesystem UWP fix, or Vita3K could move to `std::filesystem`, which works on WindowsStore.

## Stop-list check
No package or game loading, renderer, audio, input runtime or network code was touched. Nothing here is packaged or run.
