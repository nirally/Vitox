// M2 dynamic function table / unwind probe. Implementation supplied by ChatGPT on
// nirally/Vitox-Bridge Issue #1 (comment 6224354062); used unchanged.
#include "common.h"

#include <windows.h>
#include <cstdint>
#include <cstring>

namespace vitox {

namespace {

using AddFunctionTableFn =
    BOOLEAN (WINAPI*)(PRUNTIME_FUNCTION, DWORD, DWORD64);
using DeleteFunctionTableFn =
    BOOLEAN (WINAPI*)(PRUNTIME_FUNCTION);
using LookupFunctionEntryFn =
    PRUNTIME_FUNCTION (WINAPI*)(DWORD64, PDWORD64, PUNWIND_HISTORY_TABLE);
using VirtualUnwindFn =
    PEXCEPTION_ROUTINE (WINAPI*)(DWORD,
                                 DWORD64,
                                 DWORD64,
                                 PRUNTIME_FUNCTION,
                                 PCONTEXT,
                                 PVOID*,
                                 PDWORD64,
                                 PKNONVOLATILE_CONTEXT_POINTERS);

bool virtual_unwind_safe(VirtualUnwindFn fn,
                         DWORD64 image_base,
                         DWORD64 control_pc,
                         PRUNTIME_FUNCTION rf,
                         PCONTEXT ctx,
                         PVOID* handler_data,
                         PDWORD64 establisher) {
    __try {
        fn(UNW_FLAG_NHANDLER,
           image_base,
           control_pc,
           rf,
           ctx,
           handler_data,
           establisher,
           nullptr);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

} // namespace

void probe_unwind() {
    log_line(Log::Unwind, "Unwind probe begin");

    auto add = reinterpret_cast<AddFunctionTableFn>(
        resolve(L"ntdll.dll", "RtlAddFunctionTable"));
    auto del = reinterpret_cast<DeleteFunctionTableFn>(
        resolve(L"ntdll.dll", "RtlDeleteFunctionTable"));
    auto lookup = reinterpret_cast<LookupFunctionEntryFn>(
        resolve(L"ntdll.dll", "RtlLookupFunctionEntry"));
    auto vunwind = reinterpret_cast<VirtualUnwindFn>(
        resolve(L"ntdll.dll", "RtlVirtualUnwind"));

    if (!add || !del || !lookup || !vunwind) {
        log_line(Log::Unwind,
                 "Unwind FAIL exports add=%p del=%p lookup=%p virtual=%p",
                 add, del, lookup, vunwind);
        set_status(&Status::unwind, "FAIL");
        return;
    }

    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    const SIZE_T page_size = si.dwPageSize ? si.dwPageSize : 4096;

    auto* mem = static_cast<std::uint8_t*>(
        VirtualAllocFromApp(nullptr,
                            page_size,
                            MEM_RESERVE | MEM_COMMIT,
                            PAGE_READWRITE));

    if (!mem) {
        log_line(Log::Unwind, "Unwind FAIL allocation error=%lu",
                 static_cast<unsigned long>(GetLastError()));
        set_status(&Status::unwind, "FAIL");
        return;
    }

    // Generated function body: ret
    mem[0] = 0xC3;

    // Minimal x64 UNWIND_INFO at offset 0x100:
    // Version=1, Flags=0, SizeOfProlog=0, CountOfCodes=0,
    // FrameRegister=0, FrameOffset=0.
    constexpr DWORD kUnwindOffset = 0x100;
    mem[kUnwindOffset + 0] = 0x01;
    mem[kUnwindOffset + 1] = 0x00;
    mem[kUnwindOffset + 2] = 0x00;
    mem[kUnwindOffset + 3] = 0x00;

    RUNTIME_FUNCTION rf{};
    rf.BeginAddress = 0;
    rf.EndAddress = 1;
    rf.UnwindData = kUnwindOffset;

    DWORD old_protect = 0;
    if (!VirtualProtectFromApp(mem, page_size, PAGE_EXECUTE_READ, &old_protect)) {
        log_line(Log::Unwind, "Unwind FAIL RW->RX error=%lu",
                 static_cast<unsigned long>(GetLastError()));
        VirtualFree(mem, 0, MEM_RELEASE);
        set_status(&Status::unwind, "FAIL");
        return;
    }

    FlushInstructionCache(GetCurrentProcess(), mem, 1);

    const DWORD64 base = reinterpret_cast<DWORD64>(mem);

    const BOOLEAN add_ok = add(&rf, 1, base);
    log_line(Log::Unwind, "RtlAddFunctionTable=%d base=%p",
             add_ok ? 1 : 0, mem);

    bool lookup_ok = false;
    bool unwind_ok = false;
    bool rip_ok = false;
    bool rsp_ok = false;

    if (add_ok) {
        DWORD64 image_base = 0;
        PRUNTIME_FUNCTION found =
            lookup(base, &image_base, nullptr);

        lookup_ok = found != nullptr &&
                    found->BeginAddress == 0 &&
                    found->EndAddress == 1 &&
                    image_base == base;

        log_line(Log::Unwind,
                 "RtlLookupFunctionEntry found=%p image_base=%p ok=%d",
                 found,
                 reinterpret_cast<void*>(image_base),
                 lookup_ok ? 1 : 0);

        if (found) {
            alignas(16) DWORD64 fake_stack[4]{};
            constexpr DWORD64 kReturnAddress = 0x1234567812345678ull;
            fake_stack[0] = kReturnAddress;

            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_CONTROL;
            ctx.Rip = base;
            ctx.Rsp = reinterpret_cast<DWORD64>(&fake_stack[0]);

            const DWORD64 rsp_before = ctx.Rsp;
            PVOID handler_data = nullptr;
            DWORD64 establisher = 0;

            unwind_ok = virtual_unwind_safe(vunwind,
                                            base,
                                            base,
                                            found,
                                            &ctx,
                                            &handler_data,
                                            &establisher);

            rip_ok = unwind_ok && ctx.Rip == kReturnAddress;
            rsp_ok = unwind_ok && ctx.Rsp == rsp_before + sizeof(DWORD64);

            log_line(Log::Unwind,
                     "RtlVirtualUnwind call_ok=%d rip=%p rip_ok=%d rsp_delta=%lld rsp_ok=%d establisher=%p",
                     unwind_ok ? 1 : 0,
                     reinterpret_cast<void*>(ctx.Rip),
                     rip_ok ? 1 : 0,
                     static_cast<long long>(ctx.Rsp - rsp_before),
                     rsp_ok ? 1 : 0,
                     reinterpret_cast<void*>(establisher));
        }
    }

    const BOOLEAN del_ok = add_ok ? del(&rf) : FALSE;
    log_line(Log::Unwind, "RtlDeleteFunctionTable=%d", del_ok ? 1 : 0);

    VirtualFree(mem, 0, MEM_RELEASE);

    const bool overall =
        add_ok && lookup_ok && unwind_ok && rip_ok && rsp_ok && del_ok;

    set_status(&Status::unwind, overall ? "PASS" : "FAIL");
    log_line(Log::Unwind, "Unwind probe %s", overall ? "PASS" : "FAIL");
}

} // namespace vitox
