# Dynarmic W^X ordering shim (Vitox local patch)

Status: **local integration patch, candidate for upstream**. Review it before carrying it past M3.
Applied by: `dynarmic-build/CMakeLists.txt` (section "Vitox shim").
Dynarmic: `C:\Vitox\Vita3K\external\dynarmic` @ `86458a0bd369d63ba4c2ef812cacbb6c9080c065`. It is read-only and never edited.

## Problem
With `DYNARMIC_ENABLE_NO_EXECUTE_SUPPORT=ON` on Windows, every `Dynarmic::A32::Jit`
constructor takes a write access violation. The fault is inside
`ExceptionHandler::SetFastmemCallback` (`backend/x64/exception_handler_windows.cpp:261`).

The cause is the order of calls in `A32EmitX64::A32EmitX64` (`backend/x64/a32_emit_x64.cpp:82-92`):

```cpp
code.PreludeComplete();        // -> BlockOfCode::DisableWriting(): whole cache RW -> RX
ClearFastDispatchTable();
exception_handler.SetFastmemCallback(...);
    // Windows: Impl::SetCallback writes except_info->ExceptionHandler, and
    // except_info (UNW_EXCEPTION_INFO) was allocated *inside* the code cache
    // (exception_handler_windows.cpp:220) -> write to RX memory -> AV
```

Vita3K's normal Windows build doesn't enable `NO_EXECUTE_SUPPORT`, so the bug is latent there.
It shows up as soon as the cache is W^X, which the Xbox AppContainer path needs.

## Fix
Call `SetFastmemCallback` **before** `PreludeComplete()`, while the cache is still writable:

```cpp
exception_handler.SetFastmemCallback([this](u64 rip_) { return FastmemCallback(rip_); });
code.PreludeComplete();
ClearFastDispatchTable();
```

How the shim applies it:
- CMake reads the original file and normalizes CRLF to LF.
- It replaces the exact constructor tail and writes the result to `out/dynarmic/vitox-patched/a32_emit_x64.cpp`.
- It swaps that copy into the `dynarmic` target's `SOURCES` in place of `backend/x64/a32_emit_x64.cpp`.
- **Configure fails** if either the expected text or the source entry is not found.

Runtime record: `vitox_dynarmic_build.h` (generated) is logged by the probe as
`build: dynarmic commit=… tree=clean no_execute_support=ON shims=a32_emit_x64-fastmem-cb-before-prelude-complete`.

## Other W^X write paths reviewed (A32, x64 backend). No change needed
- `A32EmitX64::Emit` and `EmitX64::InvalidateBasicBlocks` wrap their writes in `EnableWriting()`/`DisableWriting()`.
- `A32EmitX64::Unpatch` does `DisableWriting()` → call → `EnableWriting()` deliberately. It *executes* the generated fast-dispatch lookup while inside an enabled-writing scope.
- `ClearCache` (`BlockOfCode::ClearCache`, `A32EmitX64::ClearCache`) only resets pointers and host-side tables.
- `EnsureMemoryCommitted` commits new pages as `PAGE_READWRITE`. The next `Emit` scope re-protects the committed range.

## A64: same ordering observed, not patched
`backend/x64/a64_emit_x64.cpp:52-63` (`PreludeComplete` at :57, `SetFastmemCallback` at :60) (`A64EmitX64::A64EmitX64`) has the identical
`code.PreludeComplete(); ClearFastDispatchTable(); exception_handler.SetFastmemCallback(...)`
sequence, so it would fault the same way under W^X on Windows. Vitox builds only the A32
frontend (`DYNARMIC_FRONTENDS=A32`), so A64 is neither patched nor built.

## Verified
- PC (Windows Desktop, AppContainer): the A32 suite and the 1/2/4/8 concurrent JIT tests PASS. See `releases\0.0.2.0`.
- Physical Xbox (Dev Mode, OS 10.0.26100.9445, run `XBOX-RUNS\20261017-120230_m2-xbox`): all M2 items PASS.
- Not yet exercised: the fastmem fault path itself, which goes through the callback this shim reorders. That is the M2.5 target.

## Upstreaming
The upstream fix would be the same two-line reorder in both emitters. A defensive alternative is an
`EnableWriting()`/`DisableWriting()` pair around `except_info` writes in `ExceptionHandler::Impl::SetCallback`,
but that needs a `BlockOfCode&` stored in `Impl`. Check upstream Dynarmic for an existing fix before proposing either.
