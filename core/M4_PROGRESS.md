# M4: EmuEnvState construction closure (WindowsStore), final inventory, 2026-10-17

- **Vita3K:** `7224a0fc`, untouched.
- **Build:** `C:\Vitox\Vitox-M3\core-build` (targets `vitox_m4_<member>` and `vitox_m4_emuenv`, WindowsStore DLL probes that are linked, never packaged).
- **Logs:** `out\m4\*.log`. Unresolved-symbol lists: `out\m4\*.unresolved.txt`.

## SDL type-only shim (`sdl-shim\SDL3\`)
- `SDL_vitox_types.h`:
  - `Uint8/Sint32/Uint32/Uint64`
  - `SDL_TouchID`, `SDL_FingerID`, `SDL_WindowID`, `SDL_JoystickID`
  - `SDL_GUID` (16 B)
  - `SDL_EventType`: a 4-byte enum with **no event constants**
  - `SDL_TouchFingerEvent` (56 B) and `SDL_GamepadTouchpadEvent` (40 B)
  - Layouts are copied from SDL3 and pinned with `static_assert`. **Layout dependency:** `TouchState` embeds both event structs by value.
- `SDL_gamepad.h`: opaque `SDL_Gamepad` and the `SDL_GamepadButton` enum (values identical to SDL3, `COUNT==26`).
- `SDL_haptic.h`: opaque `SDL_Haptic`.
- `SDL_sensor.h`: opaque `SDL_Sensor`, plus **`SDL_CloseSensor` DECLARED ONLY, pending approval**. No definition exists anywhere.
- `SDL_events.h`: types only; no `SDL_Event` union.
- Unchanged: `SDL_thread.h` and `SDL_mutex.h` (the 10 core symbols from M3).
- **The real SDL3 include tree is never on any M4 target.** Any SDL function or constant outside these files is a compile error. For example, `camera.cpp` fails with `SDL3/SDL_camera.h` not found.

## Per-member results (each member constructed and destroyed exactly as EmuEnvState does, in its own DLL)
| Member | Header | Compile | Link closure (beyond the M3 core) |
|---|---|---|---|
| SfoAppInfo, SfoFile | packages/sfo.h | OK | none |
| Config | config/state.h | OK | none |
| MemState | mem/state.h | OK | none |
| CtrlState | ctrl/state.h | OK with shim types | none |
| TouchState | touch/state.h | OK with shim types | none |
| KernelState | kernel/state.h | OK | none |
| AppState | app/state.h | OK | none |
| AudioState | audio/state.h | OK | none |
| GxmState | gxm/state.h | OK | none |
| IOState | io/state.h | OK | none |
| **MotionState** | motion/state.h | OK with shim types | **`MotionInput::MotionInput()`** (motion/src/motion_input.cpp, light) + **`SDL_CloseSensor`** (dtor via `SDL_SensorDeleter`) |
| NetState, NetCtlState | net/state.h | OK | none |
| ngs::State | ngs/state.h | OK | none |
| NpState | np/state.h | OK | none |
| DisplayState | display/state.h | OK | none |
| DialogState | dialog/state.h | OK | none |
| Ime | ime/state.h | OK | none |
| License | packages/license.h | OK | none |
| RegMgrState | regmgr/state.h | OK | none |
| **GDBState** | gdbstub/state.h | **FAIL**: includes desktop-only `<winsock.h>` (SOCKET, WSADATA) | none once the guarded header substitution is applied (`<winsock2.h>`, app partition) |
| HTTPState | http/state.h | OK | none |
| **CameraState** | camera/state.h | OK | **`Camera::Camera()` / `~Camera()`** (camera/src/camera.cpp) |
| CompatState | compat/state.h | OK | none |
| renderer::State (empty unique_ptr) | renderer/state.h | OK | none |
| renderer::VulkanDeviceInfo (empty unique_ptr) | renderer/functions.h | OK | none |
| **overlay::display_manager** (empty unique_ptr) | overlay/display_manager.h | OK | **`~display_manager()`** (overlay/src/display_manager.cpp → input.cpp + overlay.cpp, all light) |

**24 of 28 members need nothing.** Their constructors and destructors are header-inline, even for Audio, Gxm, Net, Ngs, Display and Renderer.

## emuenv.cpp build-only, done in steps
1. **emuenv.cpp + shim types + gdbstub header patch:** **compiles**. Unresolved: Camera ctor/dtor, `MotionInput()`, `~display_manager()`, `SDL_CloseSensor` (5).
2. **+ motion_input.cpp + overlay/display_manager.cpp:** the overlay closure grows by 3 (`overlay_input_handler` ctor, `user_interface::run_input_loop`, `stop_input_processing`).
3. **+ overlay/input.cpp + overlay/overlay.cpp:** **final closure = 3 symbols**: `Camera::Camera()`, `Camera::~Camera()`, `SDL_CloseSensor`.
4. **Trying camera.cpp:** it fails to compile against the type-only shim (`SDL3/SDL_camera.h`). It uses about 40 SDL camera, surface and timer symbols (`SDL_OpenCamera`, `SDL_GetCameras`, `SDL_AcquireCameraFrame`, `SDL_CreateSurface`, `SDL_ConvertSurfaceAndColorspace`, `SDL_GetTicksNS`, …). `Camera::Camera()` itself is only `pImpl(std::make_unique<CameraImpl>())`, but `CameraImpl` is defined in that SDL TU.

**No renderer, audio, network or package implementation is needed** to construct and destroy `EmuEnvState`.

## Closure classification
- **A. Must-have for EmuEnvState construction:**
  - emuenv.cpp
  - the type-only SDL shim (ctrl/touch/motion state headers)
  - the guarded gdbstub/state.h winsock2 header substitution
  - motion/src/motion_input.cpp
  - overlay/src/{display_manager,input,overlay}.cpp
  - **`Camera::Camera()`/`~Camera()`**
  - **`SDL_CloseSensor`**
- **B. Needed only when subsystem init/runtime functions are called:** every other implementation TU of audio, gxm, renderer, net/netctl, np, ngs, display, dialog, ime, http, regmgr, packages, compat, io and config. None is referenced by the constructor or destructor.
- **C. Frontend/renderer/audio/network/package paths:**
  - app_init.cpp, interface.cpp, app/session_controller.cpp
  - Vulkan renderer backends, the SDL audio backend
  - http (OpenSSL), util/net_utils.cpp (curl), util/hash.cpp (OpenSSL)
  - config.cpp's CLI11 command line
  - camera.cpp's SDL capture backend
- **D. Replaceable by inert Vitox state during bring-up:**
  - **CameraImpl:** inert, no device
  - **SDL_CloseSensor:** sensors are never opened in an inert MotionState, so the deleter never runs with a non-null pointer
  - GDBState: data only once winsock2 is used
  - overlay::display_manager: empty by default
- **E. Likely upstream decoupling:**
  - ctrl/motion/touch **state** headers embed SDL types (should be opaque handles)
  - camera.cpp mixes the `Camera` ctor/dtor with the SDL backend (split into `camera.cpp` + `camera_sdl.cpp`)
  - gdbstub/state.h uses the legacy `<winsock.h>`
  - config.cpp mixes CLI parsing with config logic
  - module_parent's monolithic `resolve_import`

## Honest RTC invocation and packaging
**Not yet possible, so no 0.0.6.0 package.** The only remaining items are SDL-backed:
1. **`SDL_CloseSensor`:** needs approval for a narrow shim **definition**. It would be fail-loud: log and abort if ever called with a non-null sensor, which an inert MotionState never does.
2. **Camera ctor/dtor:** needs approval for **either**:
   - an out-of-tree guarded substitution of `camera.cpp` that keeps `Camera::Camera()`/`~Camera()` and an inert `CameraImpl` with no SDL (camera functions fail or return "no device"), **or**
   - an upstream-style split of camera.cpp.

With both approved, a legitimately constructed `EmuEnvState` should link with no renderer, audio or network implementations. The real `import_sceRtcIsLeapYear` and `import_sceRtcGetDaysInMonth` could then be invoked through the M3.1 loader path (package 0.0.6.0).
