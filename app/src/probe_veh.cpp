// M2 VEH dispatch probe. Implementation supplied by ChatGPT on nirally/Vitox-Bridge
// Issue #1 (comment 6224354062); used unchanged.
#include "common.h"

#include <windows.h>
#include <cstdint>

namespace vitox {

namespace {
void* g_expected_page = nullptr;
SIZE_T g_expected_size = 0;
volatile LONG g_veh_seen = 0;
volatile LONG g_veh_protect_ok = 0;

LONG CALLBACK probe_veh_handler(PEXCEPTION_POINTERS ep) {
    if (!ep || !ep->ExceptionRecord)
        return EXCEPTION_CONTINUE_SEARCH;

    auto* er = ep->ExceptionRecord;
    if (er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION ||
        er->NumberParameters < 2 ||
        !g_expected_page)
        return EXCEPTION_CONTINUE_SEARCH;

    const auto fault = static_cast<std::uintptr_t>(er->ExceptionInformation[1]);
    const auto begin = reinterpret_cast<std::uintptr_t>(g_expected_page);
    const auto end = begin + g_expected_size;

    if (fault < begin || fault >= end)
        return EXCEPTION_CONTINUE_SEARCH;

    InterlockedExchange(&g_veh_seen, 1);

    DWORD old_protect = 0;
    if (VirtualProtectFromApp(g_expected_page,
                              g_expected_size,
                              PAGE_READWRITE,
                              &old_protect)) {
        InterlockedExchange(&g_veh_protect_ok, 1);
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    return EXCEPTION_CONTINUE_SEARCH;
}

bool trigger_expected_fault(volatile std::uint8_t* p) {
    __try {
        *p = 0x5A;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
} // namespace

void probe_veh() {
    log_line(Log::Veh, "VEH probe begin");

    using AddFn = PVOID (WINAPI*)(ULONG, PVECTORED_EXCEPTION_HANDLER);
    using RemoveFn = ULONG (WINAPI*)(PVOID);

    auto add = reinterpret_cast<AddFn>(
        resolve(L"kernel32.dll", "AddVectoredExceptionHandler"));
    auto remove = reinterpret_cast<RemoveFn>(
        resolve(L"kernel32.dll", "RemoveVectoredExceptionHandler"));

    if (!add || !remove) {
        log_line(Log::Veh, "VEH FAIL: add/remove export missing");
        set_status(&Status::veh, "FAIL");
        return;
    }

    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    const SIZE_T page_size = si.dwPageSize ? si.dwPageSize : 4096;

    void* page = VirtualAllocFromApp(nullptr,
                                     page_size,
                                     MEM_RESERVE | MEM_COMMIT,
                                     PAGE_READWRITE);
    if (!page) {
        log_line(Log::Veh, "VEH FAIL: allocation error=%lu",
                 static_cast<unsigned long>(GetLastError()));
        set_status(&Status::veh, "FAIL");
        return;
    }

    DWORD old_protect = 0;
    if (!VirtualProtectFromApp(page, page_size, PAGE_NOACCESS, &old_protect)) {
        log_line(Log::Veh, "VEH FAIL: PAGE_NOACCESS error=%lu",
                 static_cast<unsigned long>(GetLastError()));
        VirtualFree(page, 0, MEM_RELEASE);
        set_status(&Status::veh, "FAIL");
        return;
    }

    g_expected_page = page;
    g_expected_size = page_size;
    InterlockedExchange(&g_veh_seen, 0);
    InterlockedExchange(&g_veh_protect_ok, 0);

    PVOID cookie = add(1, probe_veh_handler);
    if (!cookie) {
        log_line(Log::Veh, "VEH FAIL: AddVectoredExceptionHandler returned null");
        g_expected_page = nullptr;
        VirtualFree(page, 0, MEM_RELEASE);
        set_status(&Status::veh, "FAIL");
        return;
    }

    const bool resumed = trigger_expected_fault(
        reinterpret_cast<volatile std::uint8_t*>(page));

    const ULONG removed = remove(cookie);

    const bool seen = InterlockedCompareExchange(&g_veh_seen, 0, 0) != 0;
    const bool protect_ok =
        InterlockedCompareExchange(&g_veh_protect_ok, 0, 0) != 0;
    const bool value_ok =
        resumed && *reinterpret_cast<volatile std::uint8_t*>(page) == 0x5A;

    log_line(Log::Veh,
             "VEH seen=%d protect_ok=%d resumed=%d value_ok=%d remove=%lu",
             seen ? 1 : 0,
             protect_ok ? 1 : 0,
             resumed ? 1 : 0,
             value_ok ? 1 : 0,
             static_cast<unsigned long>(removed));

    g_expected_page = nullptr;
    g_expected_size = 0;
    VirtualFree(page, 0, MEM_RELEASE);

    const bool overall = seen && protect_ok && resumed && value_ok && removed != 0;
    set_status(&Status::veh, overall ? "PASS" : "FAIL");
    log_line(Log::Veh, "VEH probe %s", overall ? "PASS" : "FAIL");
}

} // namespace vitox
