// Vitox compat layer implementation (compiled with VITOX_COMPAT_IMPL, so the redirects in
// vitox_compat.h are not applied here).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>

#include "vitox_compat.h"
#include "vitox_compat_report.h"

namespace {

using AddVeh = PVOID(WINAPI *)(ULONG, PVECTORED_EXCEPTION_HANDLER);
using RemoveVeh = ULONG(WINAPI *)(PVOID);

std::atomic<unsigned> g_add_calls{ 0 }, g_add_ok{ 0 }, g_remove_calls{ 0 };

FARPROC kernel32_export(const char *name) {
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    return k32 ? GetProcAddress(k32, name) : nullptr;
}

} // namespace

extern "C" void *__stdcall vitox_compat_add_veh(unsigned long first, void *handler) {
    ++g_add_calls;
    static const auto add = reinterpret_cast<AddVeh>(kernel32_export("AddVectoredExceptionHandler"));
    if (!add) {
        SetLastError(ERROR_PROC_NOT_FOUND);
        return nullptr;
    }
    void *cookie = add(first, reinterpret_cast<PVECTORED_EXCEPTION_HANDLER>(handler));
    if (cookie)
        ++g_add_ok;
    return cookie;
}

extern "C" unsigned long __stdcall vitox_compat_remove_veh(void *cookie) {
    ++g_remove_calls;
    static const auto remove = reinterpret_cast<RemoveVeh>(kernel32_export("RemoveVectoredExceptionHandler"));
    return remove ? remove(cookie) : 0;
}

extern "C" VitoxCompatReport vitox_compat_report() {
    return { g_add_calls.load(), g_add_ok.load(), g_remove_calls.load(),
        kernel32_export("AddVectoredExceptionHandler") != nullptr,
        kernel32_export("RemoveVectoredExceptionHandler") != nullptr };
}
