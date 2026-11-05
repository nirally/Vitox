// Line-flushed text logs in LocalState plus a minimal crash log.
#include "common.h"

#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <exception>
#include <intrin.h>
#include <io.h>
#include <share.h>
#include <stdlib.h>

namespace vitox {

namespace {
std::mutex g_mutex;
FILE *g_files[static_cast<int>(Log::Count)] = {};
const wchar_t *const k_names[] = { L"lifecycle_log.txt", L"mem_log.txt", L"probe_log.txt", L"fs_log.txt",
                                        L"dynarmic_log.txt", L"veh_log.txt", L"unwind_log.txt", L"fastmem_log.txt", L"core_log.txt" };
LARGE_INTEGER g_qpc0{}, g_qpf{};
HANDLE g_crash = INVALID_HANDLE_VALUE;
HANDLE g_milestones = INVALID_HANDLE_VALUE;

void flush_to_disk(FILE *f) {
    if (!f)
        return;
    fflush(f);
    const intptr_t h = _get_osfhandle(_fileno(f));
    if (h != -1 && h != -2)
        FlushFileBuffers(reinterpret_cast<HANDLE>(h));
}

void crash_frames(CONTEXT ctx);

LONG WINAPI crash_filter(EXCEPTION_POINTERS *ep) {
    if (g_crash != INVALID_HANDLE_VALUE && ep && ep->ExceptionRecord) {
        const EXCEPTION_RECORD *r = ep->ExceptionRecord;
        SYSTEMTIME st;
        GetLocalTime(&st);
        char buf[512];
        int n = snprintf(buf, sizeof(buf),
            "%04u-%02u-%02u %02u:%02u:%02u.%03u UNHANDLED EXCEPTION code=0x%08lX addr=%p exe_base=%p rip=%p nparams=%lu p0=0x%llx p1=0x%llx tid=%lu\r\n",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
            r->ExceptionCode, r->ExceptionAddress, static_cast<void *>(GetModuleHandleW(nullptr)),
            reinterpret_cast<void *>(ep->ContextRecord ? ep->ContextRecord->Rip : 0), r->NumberParameters,
            static_cast<unsigned long long>(r->NumberParameters > 0 ? r->ExceptionInformation[0] : 0),
            static_cast<unsigned long long>(r->NumberParameters > 1 ? r->ExceptionInformation[1] : 0),
            GetCurrentThreadId());
        if (n > 0) {
            DWORD written = 0;
            WriteFile(g_crash, buf, static_cast<DWORD>(n), &written, nullptr);
        }
        // 0.0.16.1 (M7B): module + RVA of the faulting address and of up to 16 unwound frames (RtlVirtualUnwind from the
        // exception context), so a fault inside a DLL (dzn, D3D12) can be symbolized offline. No allocation.
        if (ep->ContextRecord)
            crash_frames(*ep->ContextRecord);
        FlushFileBuffers(g_crash);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void crash_frames(CONTEXT ctx) {
    char buf[512];
    int n = 0;
    {
        {
            for (int frame = 0; frame < 64 && ctx.Rip; ++frame) {
                HMODULE mod = nullptr;
                wchar_t path[MAX_PATH] = L"?";
                if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                        reinterpret_cast<LPCWSTR>(ctx.Rip), &mod))
                    GetModuleFileNameW(mod, path, MAX_PATH);
                const wchar_t *name = wcsrchr(path, L'\\') ? wcsrchr(path, L'\\') + 1 : path;
                n = snprintf(buf, sizeof(buf), "    frame %2d rip=%p module=%ls base=%p rva=0x%llX\r\n", frame, reinterpret_cast<void *>(ctx.Rip), name,
                    static_cast<void *>(mod), static_cast<unsigned long long>(mod ? ctx.Rip - reinterpret_cast<DWORD64>(mod) : 0));
                if (n > 0) {
                    DWORD written = 0;
                    WriteFile(g_crash, buf, static_cast<DWORD>(n), &written, nullptr);
                }
                DWORD64 image_base = 0;
                PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &image_base, nullptr);
                if (!fn) { // leaf function: return address on top of the stack
                    ctx.Rip = *reinterpret_cast<DWORD64 *>(ctx.Rsp);
                    ctx.Rsp += 8;
                    continue;
                }
                void *handler_data = nullptr;
                DWORD64 establisher = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, ctx.Rip, fn, &ctx, &handler_data, &establisher, nullptr);
            }
        }
    }
}

// 0.0.24.50 (M9A.6, diagnostics only): paths that end the process WITHOUT an exception reaching crash_filter
// (std::terminate -> abort -> __fastfail, CRT invalid parameter, pure call). Log one line + the caller's frames, then
// let the default behaviour continue.
void crash_event(const char *what) {
    if (g_crash == INVALID_HANDLE_VALUE)
        return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    char buf[512];
    const int n = snprintf(buf, sizeof(buf), "%04u-%02u-%02u %02u:%02u:%02u.%03u PROCESS END PATH: %s tid=%lu\r\n", st.wYear, st.wMonth, st.wDay,
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, what, GetCurrentThreadId());
    if (n > 0) {
        DWORD written = 0;
        WriteFile(g_crash, buf, static_cast<DWORD>(n), &written, nullptr);
    }
    CONTEXT ctx{};
    RtlCaptureContext(&ctx);
    crash_frames(ctx);
    FlushFileBuffers(g_crash);
}

void on_terminate() {
    char what[400] = "std::terminate";
    if (const std::exception_ptr e = std::current_exception()) {
        try {
            std::rethrow_exception(e);
        } catch (const std::exception &x) {
            snprintf(what, sizeof(what), "std::terminate: uncaught std::exception: %s", x.what());
        } catch (...) {
            snprintf(what, sizeof(what), "std::terminate: uncaught non-std exception");
        }
    }
    crash_event(what);
    abort();
}
void on_abort(int) { crash_event("SIGABRT (abort)"); }
void on_invalid_parameter(const wchar_t *, const wchar_t *, const wchar_t *, unsigned, uintptr_t) {
    crash_event("CRT invalid parameter");
    // a handler that returns makes the CRT call fail with EINVAL; keep the default (fast fail) instead
    __fastfail(FAST_FAIL_INVALID_ARG);
}
void on_purecall() { crash_event("pure virtual call"); }
} // namespace

bool log_open(const std::wstring &dir) {
    QueryPerformanceFrequency(&g_qpf);
    QueryPerformanceCounter(&g_qpc0);
    bool ok = true;
    for (int i = 0; i < static_cast<int>(Log::Count); ++i) {
        const std::wstring path = dir + L"\\" + k_names[i];
        g_files[i] = _wfsopen(path.c_str(), L"ab", _SH_DENYNO);
        if (!g_files[i])
            ok = false;
    }
    // 0.0.24.3 (M9A.4): write-through milestone file; see log_checkpoint
    CREATEFILE2_EXTENDED_PARAMETERS xp{ sizeof(xp) };
    xp.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
    xp.dwFileFlags = FILE_FLAG_WRITE_THROUGH;
    const std::wstring mpath = dir + L"\\m9a4_milestones.txt";
    g_milestones = CreateFile2(mpath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, OPEN_ALWAYS, &xp);
    if (g_milestones == INVALID_HANDLE_VALUE)
        ok = false;
    else {
        char hdr[96];
        const int n = snprintf(hdr, sizeof(hdr), "===== Vitox milestones session start pid=%lu =====\n", GetCurrentProcessId());
        DWORD written = 0;
        WriteFile(g_milestones, hdr, static_cast<DWORD>(n), &written, nullptr);
        FlushFileBuffers(g_milestones);
    }
    return ok;
}

void log_line(Log which, const char *fmt, ...) {
    char body[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);

    SYSTEMTIME st;
    GetLocalTime(&st);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const double ms = g_qpf.QuadPart ? (now.QuadPart - g_qpc0.QuadPart) * 1000.0 / g_qpf.QuadPart : 0.0;

    char line[2400];
    int n = snprintf(line, sizeof(line), "%04u-%02u-%02u %02u:%02u:%02u.%03u +%10.1fms [%5lu] %s\n",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, ms,
        GetCurrentThreadId(), body);
    if (n < 0)
        return;
    if (n >= static_cast<int>(sizeof(line)))
        n = static_cast<int>(sizeof(line)) - 1;

    std::lock_guard<std::mutex> lock(g_mutex);
    if (FILE *f = g_files[static_cast<int>(which)]) {
        fwrite(line, 1, static_cast<size_t>(n), f);
        fflush(f);
    }
    OutputDebugStringA(line);
}

// 0.0.24.3 (M9A.4, observability only): fflush leaves a line in the OS cache, which an Xbox stall + forced stop lost
// (20261025-182708: every log cut short). A checkpoint writes one line through to the milestone file and pushes every
// log (and the redirected stderr) to disk, so everything logged up to the checkpoint survives.
void log_checkpoint(const char *text) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const double ms = g_qpf.QuadPart ? (now.QuadPart - g_qpc0.QuadPart) * 1000.0 / g_qpf.QuadPart : 0.0;
    char line[2400];
    int n = snprintf(line, sizeof(line), "%04u-%02u-%02u %02u:%02u:%02u.%03u +%10.1fms [%5lu] %s\n", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
        st.wSecond, st.wMilliseconds, ms, GetCurrentThreadId(), text);
    if (n < 0)
        return;
    if (n >= static_cast<int>(sizeof(line)))
        n = static_cast<int>(sizeof(line)) - 1;

    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_milestones != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(g_milestones, line, static_cast<DWORD>(n), &written, nullptr);
        FlushFileBuffers(g_milestones);
    }
    for (FILE *f : g_files)
        flush_to_disk(f);
    flush_to_disk(stderr);
}

void crash_install(const std::wstring &dir) {
    const std::wstring path = dir + L"\\crash_log.txt";
    g_crash = CreateFile2(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, OPEN_ALWAYS, nullptr);
    SetUnhandledExceptionFilter(crash_filter);
    // 0.0.24.50: non-exception process-end paths (diagnostics only)
    std::set_terminate(on_terminate);
    signal(SIGABRT, on_abort);
    _set_invalid_parameter_handler(on_invalid_parameter);
    _set_purecall_handler(on_purecall);
}

std::string narrow(const std::wstring &w) {
    if (w.empty())
        return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring widen(const std::string &s) {
    if (s.empty())
        return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string win32_err(DWORD e) {
    char buf[64];
    snprintf(buf, sizeof(buf), "win32=%lu hr=0x%08lX", e, static_cast<unsigned long>(HRESULT_FROM_WIN32(e)));
    return buf;
}

FARPROC resolve(const wchar_t *module, const char *fn) {
    HMODULE h = GetModuleHandleW(module);
    if (!h)
        h = LoadLibraryW(module);
    return h ? GetProcAddress(h, fn) : nullptr;
}

} // namespace vitox
