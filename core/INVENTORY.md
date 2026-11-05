# Vitox M3 inventory: WindowsStore build-only of Vita3K core targets

- **Date:** 2026-10-17
- **Vita3K:** `7224a0fcc769fea781e4166e5f16a2456552f9f5`, read-only; `git status` clean.
- **Wrapper:** `C:\Vitox\Vitox-M3\core-build\CMakeLists.txt`. Vita3K's own per-module CMakeLists are added out-of-tree.
- **Build:** `C:\Vitox\Vitox-M3\out\core`, using VS 2026 (MSVC v145), x64, `CMAKE_SYSTEM_NAME=WindowsStore`, SDK 10.0.26100.0, Release.
- **Method:** each target is built on its own with `/p:BuildProjectReferences=false`, so each layer reports its own first blocker. Logs are in `out\build_<target>.log`.
- **Scope:** static libraries only. Nothing is linked into the Vitox app.

## Dependency provisioning
| Dependency | How it was provided |
|---|---|
| capstone, fmt, spdlog, miniz, vita-toolchain | in-tree `external/`, configured as in Vita3K's `external/CMakeLists.txt` |
| dynarmic + mcl | the approved M2 wrapper (`Vitox-M2\dynarmic-build`, W^X shim) |
| Boost | headers only (`external/boost`, `BOOST_ALL_NO_LIB`). Vita3K links Boost::filesystem, a compiled library, at app link time. |
| SDL3 | **headers only** (`external/sdl/include`); SDL3 is not built |
| OpenSSL (`ssl`, `crypto`), curl (`libcurl`) | **not provided**: empty placeholder targets. Vita3K downloads or fetches these. |
| winsock | `WS2_32 IPHLPAPI` |

## Configure-time blockers (fixed in the wrapper)
1. **capstone** `CPackConfig.txt:25` dereferences an unquoted `${CMAKE_SYSTEM_PROCESSOR}`. That variable is empty when `CMAKE_SYSTEM_NAME=WindowsStore`, because CMake treats the build as cross-compiling. **Fix:** set `CMAKE_SYSTEM_PROCESSOR=AMD64` before `project()`.
2. **Dynarmic's `install(EXPORT)` requires fmt in an export set.** Vita3K sets `FMT_INSTALL ON`, and the wrapper now matches that.

## Results per target
| Layer | Target | Result | First real blocker |
|---|---|---|---|
| 0 | capstone | **OK** | none |
| 0 | fmt | **OK** | none |
| 0 | miniz | **OK** | none |
| 0 | dynarmic (shimmed) | **OK** | none |
| 1 | nids | **OK** | none |
| 1 | rtc | **OK** | none (1 warning) |
| 1 | util | FAIL (5 TUs) | see B1–B5 |
| 1 | mem | FAIL (1 error) | B4 only |
| 2 | http | FAIL | B2 (OpenSSL) |
| 2 | cpu | **OK** | none. Dynarmic + capstone compile cleanly; 8 size_t→u8 conversion warnings. |
| 3 | kernel | FAIL | B1 (SDL3) |

## Blockers by root cause
**B1. SDL3 refuses UWP. This is a hard blocker for any SDL use.**
`external/sdl/include/SDL3/SDL_platform_defines.h:376`: `#error Windows RT/UWP is no longer supported in SDL`.
It fires on `WINAPI_FAMILY_APP`, which every WindowsStore build defines. Where SDL appears in the core layer:
- `util/src/fs_utils.cpp`: `SDL_IOFromFile`, `SDL_GetIOSize`, `SDL_ReadIO` and `SDL_CloseIO` in `read_data()`. That is plain file reading, which `std::ifstream` or `CreateFile2FromApp` could do.
- `kernel/src/kernel.cpp`: `SDL_CreateThread`, `SDL_DetachThread` and the `SDL_Semaphore` calls for guest-thread creation. The code's own comment says "TODO implement cross platform debug thread name setter and eliminate SDL thread".
- `util/src/android_driver.cpp` includes SDL too, but is Android-only.

Implication: SDL3 cannot serve as the Xbox platform layer for input, audio, window or threads. The core needs SDL-free paths. For these two call sites they are small.

**B2. OpenSSL is absent.** `util/src/hash.cpp` (`openssl/evp.h`) and `http/src/http.cpp` (`openssl/ssl.h`). Options:
- a WindowsStore build of OpenSSL
- BCrypt (CNG) for the hash functions
- dropping or stubbing `http` (online features) in the core

**B3. curl is absent.** `util/src/net_utils.cpp` (`curl/curl.h`). It is used for network and update helpers. Options: a WindowsStore libcurl build with Schannel, or excluding these helpers from the core.

**B4. `AddVectoredExceptionHandler` is not declared in the app partition.**
- Call sites: `util/src/logging.cpp:232` (crash-log handler) and `mem/src/mem.cpp:598` (`register_access_violation_handler`, the guest-memory protection mechanism).
- The function exists at runtime on Xbox: M2 VEH probe PASS, resolved via `GetProcAddress`. Only the SDK declaration is desktop-partition.
- Fix: a small compat header that declares or resolves it. Whether it links from `WindowsApp.lib` is still **unknown**; that's a link-stage item.
- This is the only blocker in `mem`.

**B5. Console APIs and the UNICODE default.**
- `util/src/logging.cpp:110`: `SetConsoleTitle("…")` resolves to `SetConsoleTitleW`, because the WindowsStore generator defines `UNICODE`, and the narrow literal doesn't convert.
- Console APIs are not meaningful in the app partition anyway. Fix: guard with `WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)`.

## Layer 2 inventory (M3.1): emuenv / config / io / module dispatch / loader
Method:
- **Compile pass:** target `vitox_inv_l2` (compile-only, never linked) with every `vita3k/*/include` root and the in-tree third-party headers. The SDL core shim is searched first and the **real** SDL headers after it. Each TU is compiled separately (`SelectedFiles`). Results are in `out\inventory_l2.txt`.
- **Static pass:** `tools\include_fanout.py` gives the transitive include fan-out.

| TU | WindowsStore compile | Vita3K modules reached (transitive includes) | Notes |
|---|---|---|---|
| emuenv/src/emuenv.cpp | **FAIL**: real SDL3 `#error` (UWP) | **33**: app audio camera compat config cpu ctrl dialog display features gdbstub gxm http ime input io kernel mem motion net ngs nids np overlay packages regmgr renderer rtc shader threads touch util | + Vulkan, SDL3, winsock. **This is the wall:** `EmuEnvState()` builds every subsystem state |
| config/src/config.cpp | **FAIL**: CLI11 `CommandLineToArgvW` (shell32, desktop partition) | input util | + CLI11, yaml-cpp |
| config/src/settings.cpp | OK | input util | pugixml |
| io/src/{device,filesystem,io,state_functions}.cpp | **OK (all 4)** | mem rtc util | dirent |
| modules/module_parent.cpp | OK (compile) | 13: config cpu emuenv input io kernel mem module nids packages rtc util | **cannot link small:** `resolve_import` is a switch over all of `nids.inc`, so it references every module export |
| modules/SceDriverUser/SceRtcUser.cpp, modules/SceRtc/SceRtc.cpp | OK, **and linked** (M3.1) | config cpu emuenv input kernel mem module rtc util | references `EmuEnvState`, never constructs it |
| kernel/src/load_self.cpp, relocation.cpp, nids/src/nids.cpp | OK, **and linked** (M3.1) | cpu emuenv mem nids rtc util | load_self needs compiled Boost.Filesystem **only** in its `dump_elfs` block (shimmed, see below) |
| app/src/session_controller.cpp (AppSessionController) | not compiled (static pass) | **19**: audio config cpu ctrl display emuenv features gxm input io kernel mem motion renderer rtc shader threads util | + SDL3, Vulkan; `session_controller.h` itself includes `emuenv/state.h` + `renderer/state.h` |
| app/src/app_init.cpp | not compiled (static pass) | **35** | + SDL3 (11 SDL calls), Vulkan, Discord, stb, winsock |
| interface.cpp | not compiled (static pass) | 28 | + SDL3, Vulkan, Discord, stb, pugixml |

**Where the subsystems get dragged in:** `EmuEnvState`, one level *below* AppSessionController.
- `emuenv/state.h` only forward-declares the subsystem states, so referencing `EmuEnvState&` is cheap; that's what every module export does.
- **Constructing** it (`emuenv.cpp`) needs complete types and ctors/dtors for all 25 owned states.
- Three of those state headers embed SDL3 input types directly: `ctrl/state.h` (SDL_gamepad, SDL_haptic), `motion/state.h` (SDL_sensor) and `touch/state.h` (SDL_events).
- `audio/impl/sdl_audio.h` (SDL_audio) is the audio backend.
- SDL is therefore coupled at the **state-struct** level, not only in the frontend.

**Every module export takes `EmuEnvState&`** (`ImportFn = void(*)(EmuEnvState&, CPUState&, SceUID)`). The bridge itself only reads `emuenv.mem` (plus `emuenv.cfg` under Tracy). The selected RTC exports never touch `emuenv`. A real export call still needs a *constructed* `EmuEnvState`.

### M3.1 link closure: exact symbols the loader path needed
- `encode_arm_inst` → `util/src/arm.cpp` (added).
- `write_return_value` → `module/src/write_return_value.cpp` (added).
- `mz_uncompress` → miniz (added to the app link).
- **Boost.Filesystem (6), all inside load_self's `kernel.debugger.dump_elfs` block:** `path_traits::convert` ×2, `path_algorithms::filename_v3`, `stem_v3`, `append_v3`, `detail::create_directories`. **`fs_utils::dump_data`** is in the same block.
- Resolution: a guarded source substitution (`vitox_substitute()` in core-build) replaces exactly that block with `std::filesystem` + `std::ofstream`. The file name and bytes are unchanged, and configure fails if the upstream text drifts.
- Result: **no compiled Boost.Filesystem and no fs_utils/SDL_IOStream** are needed for the loader.

## Notes
- Only the first fatal error per translation unit is visible, so B1–B3 may hide further errors in the same files. The other TUs in util, mem, http and kernel compiled.
- Built in Release: `TRACY_ENABLE` is defined only for Debug/RelWithDebInfo in `util`, which would need a Tracy build.
- Link-stage items not yet assessed: Boost::filesystem for WindowsStore, `AddVectoredExceptionHandler` availability in `WindowsApp.lib`, and every not-yet-compiled dependency (SDL3, OpenSSL, curl).
