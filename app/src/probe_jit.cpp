// Raw JIT smoke test (probe 4). Implementation supplied by ChatGPT on
// nirally/Vitox-Bridge Issue #1 (comment 6221956933). Adapted only for the
// string-based Status fields and the printf-style log_line signature.
#include "common.h"

#include <windows.h>
#include <cstdint>
#include <cstring>

namespace vitox {

using JitFn = int(*)();

// Out-of-line so probe_jit() itself constructs no std::string temporaries
// (MSVC C2712: __try cannot be used in functions that require object unwinding).
static __declspec(noinline) void jit_status(std::string Status::*field, const char* value)
{
    set_status(field, value);
}

static void log_last_error(const char* step, BOOL ok)
{
    DWORD e = GetLastError();
    char buf[256];
    sprintf_s(buf, "%s: ok=%d GetLastError=%lu", step, ok ? 1 : 0, static_cast<unsigned long>(e));
    log_line(Log::Probe, "%s", buf);
}

void probe_jit()
{
    log_line(Log::Probe, "%s", "JIT: begin");

    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    const SIZE_T page = si.dwPageSize ? si.dwPageSize : 4096;

    // Direct executable allocation is expected to fail in AppContainer.
    SetLastError(ERROR_SUCCESS);
    void* rwx = VirtualAllocFromApp(nullptr, page, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    {
        char buf[256];
        sprintf_s(buf, "JIT: direct RWX allocation ptr=%p GetLastError=%lu",
                  rwx, static_cast<unsigned long>(GetLastError()));
        log_line(Log::Probe, "%s", buf);
    }
    if (rwx) {
        VirtualFree(rwx, 0, MEM_RELEASE);
        log_line(Log::Probe, "%s", "JIT: WARNING direct RWX unexpectedly succeeded");
    }
    jit_status(&Status::jit_rwx, rwx ? "NOT rejected (unexpected)" : "rejected (expected)");

    SetLastError(ERROR_SUCCESS);
    void* mem = VirtualAllocFromApp(nullptr, page, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!mem) {
        char buf[256];
        sprintf_s(buf, "JIT: RW allocation FAILED GetLastError=%lu",
                  static_cast<unsigned long>(GetLastError()));
        log_line(Log::Probe, "%s", buf);
        jit_status(&Status::jit, "FAIL");
        return;
    }

    // x64: mov eax, 0x12345678 ; ret
    const std::uint8_t code1[] = {
        0xB8, 0x78, 0x56, 0x34, 0x12,
        0xC3
    };
    std::memcpy(mem, code1, sizeof(code1));

    FlushInstructionCache(GetCurrentProcess(), mem, sizeof(code1));

    DWORD old_protect = 0;
    SetLastError(ERROR_SUCCESS);
    BOOL ok_rx = VirtualProtectFromApp(mem, page, PAGE_EXECUTE_READ, &old_protect);
    log_last_error("JIT: RW->RX", ok_rx);
    if (!ok_rx) {
        VirtualFree(mem, 0, MEM_RELEASE);
        jit_status(&Status::jit, "FAIL");
        return;
    }

    bool first_ok = false;
    int first_value = 0;

    __try {
        first_value = reinterpret_cast<JitFn>(mem)();
        first_ok = (first_value == 0x12345678);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        DWORD code = GetExceptionCode();
        char buf[256];
        sprintf_s(buf, "JIT: first execute SEH exception=0x%08lX",
                  static_cast<unsigned long>(code));
        log_line(Log::Probe, "%s", buf);
    }

    {
        char buf[256];
        sprintf_s(buf, "JIT: first execute value=0x%08X expected=0x12345678 ok=%d",
                  static_cast<unsigned>(first_value), first_ok ? 1 : 0);
        log_line(Log::Probe, "%s", buf);
    }

    // Dynarmic-relevant patch cycle: RX -> RW -> patch -> RX -> execute again.
    DWORD old2 = 0;
    SetLastError(ERROR_SUCCESS);
    BOOL ok_rw = VirtualProtectFromApp(mem, page, PAGE_READWRITE, &old2);
    log_last_error("JIT: RX->RW", ok_rw);

    bool second_ok = false;
    int second_value = 0;

    if (ok_rw) {
        const std::uint8_t code2[] = {
            0xB8, 0x21, 0x43, 0x65, 0x87,
            0xC3
        };
        std::memcpy(mem, code2, sizeof(code2));
        FlushInstructionCache(GetCurrentProcess(), mem, sizeof(code2));

        DWORD old3 = 0;
        SetLastError(ERROR_SUCCESS);
        BOOL ok_rx2 = VirtualProtectFromApp(mem, page, PAGE_EXECUTE_READ, &old3);
        log_last_error("JIT: RW->RX (patched)", ok_rx2);

        if (ok_rx2) {
            __try {
                second_value = reinterpret_cast<JitFn>(mem)();
                second_ok = (static_cast<std::uint32_t>(second_value) == 0x87654321u);
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {
                DWORD code = GetExceptionCode();
                char buf[256];
                sprintf_s(buf, "JIT: second execute SEH exception=0x%08lX",
                          static_cast<unsigned long>(code));
                log_line(Log::Probe, "%s", buf);
            }
        }
    }

    {
        char buf[256];
        sprintf_s(buf, "JIT: patched execute value=0x%08X expected=0x87654321 ok=%d",
                  static_cast<unsigned>(second_value), second_ok ? 1 : 0);
        log_line(Log::Probe, "%s", buf);
    }

    VirtualFree(mem, 0, MEM_RELEASE);

    const bool overall = first_ok && second_ok;
    jit_status(&Status::jit_rx, first_ok ? "PASS" : "FAIL");
    jit_status(&Status::jit_patch, second_ok ? "PASS" : "FAIL");
    jit_status(&Status::jit, overall ? "PASS" : "FAIL");
    log_line(Log::Probe, "%s", overall ? "JIT: PASS" : "JIT: FAIL");
}
} // namespace vitox
