# M5A: init_libraries() / module_parent link-closure inventory (WindowsStore), 2026-10-18

- **Vita3K:** `7224a0fc`, untouched.
- **Build:** `core-build` targets `vitox_m5a_modules_compile`, `vitox_m5a_initlib` and `vitox_m5a_module_parent`. They are never packaged.
- **Flags:** the same as the shipped vitox libs, plus `/sdl-` and `/bigobj` (Vita3K's desktop flags) and dlmalloc's PUBLIC definitions (`ONLY_MSPACES=1 USE_LOCK=0`).
- **SDL:** only the Vitox type-only shim. The real SDL3 headers are never on the path.
- **Logs:** `out\m5a\modules_compile.log`, `initlib.log`, `module_parent.log` and `module_parent.unresolved.txt` (classified). `fanout.txt` is the include fan-out of every module TU.

## 1. Stock init_libraries()
`modules/library_init_list.inc` registers **5 libraries**. Every `LIBRARY_INIT` body is a single `emuenv.kernel.obj_store.create<XState>()`:

| Library | TU | Init creates | Compile (WS) | Extra link closure of the TU | Category |
|---|---|---|---|---|---|
| SceSysmem | SceSysmem/SceSysmem.cpp | SysmemState | OK (in vitox_core) | none (proven on Xbox, M4.1) | **A** |
| SceFiber | SceFiber/SceFiber.cpp | FiberState | OK | **none** | **A** |
| taihen | taiHEN/taiHEN.cpp | TaihenState | OK | `load_module`, `start_module`, `has_hle_implementation` (module_parent / module), `transform_dis_main` (external/substitute, C) | **B** (with module_parent + substitute) / plugin loading is **D**-adjacent |
| SceSharedFb | SceAppMgr/SceSharedFb.cpp | SharedFbState | OK | `export__sceDisplaySetFrameBuf` (SceDisplay TU → display/renderer) | **C** (display/renderer) |
| SceAudiodec | SceAudiodec/SceAudiodecUser.cpp | AudiodecState | OK | `Aac/Mp3DecoderState` (codec → FFmpeg), `Atrac9DecoderState` (codec → LibAtrac9) | **C** (audio codecs) |

A stock-semantics `init_libraries()` (the same `LIBRARY()` expansion, generated outside `module_parent.cpp`) over these TUs links with exactly **8 unresolved symbols**, and only through exports that share those TUs. The inits themselves need nothing.

## 2. module_parent.cpp + all 215 module TUs (Vita3K's Windows SOURCE_LIST)
**Compile for WindowsStore: 207 / 215 OK.** The 8 failures (first blocker in each):

| TU | First blocker | Dependency | Category |
|---|---|---|---|
| SceAudioIn/SceAudioIn.cpp | `SDL3/SDL_audio.h` | SDL audio capture | C (audio) |
| SceCtrl/SceCtrl.cpp | `SDL_HAPTIC_INFINITY` (beyond the type shim) | SDL gamepad/haptics runtime | C (input) |
| SceCommonDialog/SceCommonDialog.cpp | `SDL3/SDL_timer.h` | SDL timer | C (dialog UI); timer alone is easily replaceable |
| ScePower/ScePower.cpp | `SDL3/SDL_power.h` | SDL battery query | B-candidate (tiny host shim) |
| SceHttp/SceHttp.cpp | `openssl/err.h` | OpenSSL | C (network) |
| SceSsl/SceSsl.cpp | `openssl/ssl.h` | OpenSSL | C (network) |
| SceAppUtil/SceAppUtil.cpp | `gethostname` (`#include <winsock.h>`, desktop-only) | WinSock (one fallback, for the user name) | E (narrow; `<winsock2.h>` as for gdbstub) |
| SceNet/SceNet.cpp | `GetAdaptersInfo` (desktop-only iphlpapi) | network adapters | E / C (network) |

**Link of module_parent + the 207 compiled TUs:** **593 unresolved externals.**
- **427 are the export tables of the 8 non-compiling TUs.** They are referenced by `module_parent` `resolve_import` (all of them), SceAppMgrUser, SceNetCtl and SceNetInternal.
- **~166 are Vita3K subsystem implementations not linked yet:**

| Missing subsystem (symbols) | Referenced by module TUs |
|---|---|
| renderer (≈45: create/destroy/finish, contexts, draw, set_*, transfers, sync_surface) + vkutil | SceGxm |
| codec (25: H264/MJPEG/AAC/MP3 decoders, PlayerState, YUV/RGB) → FFmpeg; Atrac9 → LibAtrac9 | SceAudiodecUser, SceVideodecUser, SceAvPlayer, SceJpegUser, SceJpegEncUser |
| ngs (≈21: system/rack/voice/scheduler) | SceNgs |
| gxm/gxp helpers (14: format/stride/bits_per_pixel, gxp params) | SceGxm |
| motion (14: sensor sampling, orientation) | SceMotion |
| np / trophy (12, incl. `init`/`deinit(NpState/NpTrophyState)`) | SceNpManager, SceNpTrophy |
| regmgr (7: get/set registry values) | SceRegMgr, SceRegMgrForGame, SceRegMgrForSDK |
| audio (4: AudioState ports/output/volume) | SceAudio |
| display (3: vblank wait, frame prediction) | SceDisplay, SceGxm |
| touch (2) | SceTouch |
| module (`sysmodule_paths`, `sysmodule_internal_paths`, `is_lle_module`, `is_module_loaded`: module/src/load_module.cpp) | module_parent, SceSysmodule |
| packages (`decrypt_fself`: OpenSSL; LLE module loading) | module_parent |
| util (`net_utils::*`: curl/winsock; `float_to_half`) | SceNetCtl, SceGxm |
| dlmalloc (mspace_*: just the external lib) | SceLibKernel and others |
| substitute (`transform_dis_main`) | taiHEN |

## 3. Dependency summary
- **SDL:** SceAudioIn, SceCtrl, SceCommonDialog and ScePower directly. Underneath, the ctrl, touch and motion runtimes and the SDL audio backend.
- **Vulkan:** renderer, vkutil and display, all via SceGxm.
- **OpenSSL:** SceHttp, SceSsl, and packages `decrypt_fself` (reached from module_parent for LLE/encrypted modules).
- **curl:** `util/net_utils` (SceNetCtl).
- **FFmpeg:** codec (5 module TUs).
- **WinSock:** SceAppUtil (`<winsock.h>`), SceNet (`GetAdaptersInfo`), `net_utils`.
- **Filesystem/package:** packages (`decrypt_fself`), `module/load_module` (sysmodule paths) and `copy_path` (U3 solved).

## 4. Answer: selected init vs stock init_libraries()
- **Stock `init_libraries()` itself is cheap and nearly free.** It is five `obj_store.create<>()` calls. The 4 TUs not yet linked compile for WindowsStore and add only 8 symbols (display, codec, module_parent and substitute). For a first homebrew, calling the stock function is practical once those TUs are linked with display/codec stubs, or with their real subsystems.
- **The real gate before `load_app` is `module_parent`, not `init_libraries`.**
  - `module_parent`'s `resolve_import` is a switch over **every** export of every module. Vita3K's real `call_import` uses it, and homebrew can import any NID.
  - The `CallImportFunc` used by `load_app` → `ThreadState` calls into this dispatcher, which links all 215 TUs.
  - Their closure is the 8 non-compiling TUs plus the subsystem implementations listed above, with renderer and codec the largest.
- **A selected-subset dispatcher remains legitimate for first-homebrew bring-up**, as the M3.1–M4.2 probes show. Unselected NIDs behave like Vita3K's own "unimplemented import" path, and every selected NID calls the REAL export. It is not a substitute for `resolve_import` once real titles are the goal.
- **Recommended route:**
  1. Link `module_parent` + the 207 compiling TUs.
  2. Supply the 8 failing TUs through narrow shims: winsock2 for SceAppUtil; host replacements for SDL power/timer; leave SceAudioIn/SceCtrl/SceHttp/SceSsl/SceNet as explicit "not available" stubs until their tracks.
  3. Link the light subsystems (regmgr, module/load_module, touch/motion data, display timing).
  4. Stub renderer/codec/ngs/np behind their own tracks.
