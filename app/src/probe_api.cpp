// API-availability probe: runtime GetModuleHandle/LoadLibrary + GetProcAddress only.
// Nothing here is statically imported, so an absent API can never block startup.
#include "common.h"

#include <cstdio>

namespace vitox {

namespace {
struct Query {
    const char *fn;
    bool required; // one of the five APIs M1 must report
    const wchar_t *modules[4];
};

const Query k_queries[] = {
    { "AddVectoredExceptionHandler", true, { L"kernel32.dll", L"kernelbase.dll" } },
    { "RtlAddVectoredExceptionHandler", false, { L"ntdll.dll" } },
    { "RemoveVectoredExceptionHandler", false, { L"kernel32.dll", L"kernelbase.dll" } },
    { "SetUnhandledExceptionFilter", false, { L"kernel32.dll", L"kernelbase.dll" } },
    { "CreateFileW", true, { L"kernel32.dll", L"kernelbase.dll" } },
    { "CreateFile2", false, { L"kernel32.dll", L"kernelbase.dll" } },
    { "CreateFile2FromAppW", false, { L"kernelbase.dll", L"api-ms-win-core-file-fromapp-l1-1-0.dll" } },
    { "VirtualAlloc", false, { L"kernel32.dll", L"kernelbase.dll" } },
    { "VirtualProtect", false, { L"kernel32.dll", L"kernelbase.dll" } },
    { "VirtualAllocFromApp", false, { L"kernelbase.dll", L"api-ms-win-core-memory-l1-1-3.dll" } },
    { "VirtualProtectFromApp", false, { L"kernelbase.dll", L"api-ms-win-core-memory-l1-1-3.dll" } },
    { "VirtualAlloc2FromApp", true, { L"kernelbase.dll", L"api-ms-win-core-memory-l1-1-6.dll" } },
    { "VirtualAlloc2", false, { L"kernelbase.dll", L"api-ms-win-core-memory-l1-1-6.dll" } },
    { "MapViewOfFile3FromApp", false, { L"kernelbase.dll", L"api-ms-win-core-memory-l1-1-6.dll" } },
    { "RtlAddFunctionTable", true, { L"ntdll.dll", L"kernel32.dll" } },
    { "RtlDeleteFunctionTable", false, { L"ntdll.dll", L"kernel32.dll" } },
    { "RtlInstallFunctionTableCallback", false, { L"ntdll.dll", L"kernel32.dll" } },
    { "FlushInstructionCache", false, { L"kernel32.dll", L"kernelbase.dll" } },
    { "GetAdaptersInfo", true, { L"iphlpapi.dll" } },
    { "GetAdaptersAddresses", false, { L"iphlpapi.dll" } },
};

HMODULE get_module(const wchar_t *name, const char **how, DWORD *err) {
    HMODULE h = GetModuleHandleW(name);
    if (h) {
        *how = "GetModuleHandleW";
        *err = 0;
        return h;
    }
    h = LoadLibraryW(name);
    *how = "LoadLibraryW";
    *err = h ? 0 : GetLastError();
    return h;
}
} // namespace

void probe_api() {
    log_line(Log::Probe, "API probe begin (runtime lookup only; no static imports of probed APIs)");
    int required_total = 0, required_present = 0;
    std::string missing;
    for (const Query &q : k_queries) {
        bool found_any = false;
        for (const wchar_t *mod : q.modules) {
            if (!mod)
                break;
            const char *how = "";
            DWORD mod_err = 0;
            HMODULE h = get_module(mod, &how, &mod_err);
            FARPROC p = nullptr;
            DWORD proc_err = 0;
            if (h) {
                p = GetProcAddress(h, q.fn);
                proc_err = p ? 0 : GetLastError();
            }
            found_any = found_any || p != nullptr;
            log_line(Log::Probe, "API %-32s module=%-42s via=%-16s hmodule=%p module_err=%s -> %s addr=%p%s%s",
                q.fn, narrow(mod).c_str(), how, static_cast<void *>(h), h ? "none" : win32_err(mod_err).c_str(),
                p ? "PRESENT" : "ABSENT", reinterpret_cast<void *>(p), p ? "" : " proc_err=",
                p ? "" : win32_err(proc_err).c_str());
        }
        log_line(Log::Probe, "API %-32s RESULT=%s%s", q.fn, found_any ? "PRESENT" : "ABSENT", q.required ? " (required)" : "");
        if (q.required) {
            ++required_total;
            if (found_any) {
                ++required_present;
            } else {
                if (!missing.empty())
                    missing += ", ";
                missing += q.fn;
            }
        }
    }
    char s[256];
    snprintf(s, sizeof(s), "%d/%d required present%s%s", required_present, required_total,
        missing.empty() ? "" : "; absent: ", missing.c_str());
    log_line(Log::Probe, "API probe end: %s", s);
    set_status(&Status::api, s);
}

} // namespace vitox
