// Memory probe: Windows.System.MemoryManager + Win32 process counters.
#include "common.h"

#include <psapi.h>

#include <atomic>
#include <cstdio>
#include <thread>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.System.h>

using namespace winrt::Windows::System;

namespace vitox {

namespace {
std::atomic<bool> g_run{ false };
std::thread g_thread;

const char *level_name(AppMemoryUsageLevel l) {
    switch (l) {
    case AppMemoryUsageLevel::Low: return "Low";
    case AppMemoryUsageLevel::Medium: return "Medium";
    case AppMemoryUsageLevel::High: return "High";
    case AppMemoryUsageLevel::OverLimit: return "OverLimit";
    default: return "?";
    }
}

constexpr double MiB = 1024.0 * 1024.0;
} // namespace

void mem_snapshot(const char *tag) {
    uint64_t usage = 0, limit = 0;
    const char *level = "?";
    std::string mm_err;
    try {
        usage = MemoryManager::AppMemoryUsage();
        limit = MemoryManager::AppMemoryUsageLimit();
        level = level_name(MemoryManager::AppMemoryUsageLevel());
    } catch (winrt::hresult_error const &e) {
        char b[48];
        snprintf(b, sizeof(b), " mm_error=0x%08lX", static_cast<unsigned long>(e.code()));
        mm_err = b;
    }

    uint64_t r_priv = 0, r_peak = 0, r_total = 0, r_limit = 0;
    try {
        const AppMemoryReport r = MemoryManager::GetAppMemoryReport();
        r_priv = r.PrivateCommitUsage();
        r_peak = r.PeakPrivateCommitUsage();
        r_total = r.TotalCommitUsage();
        r_limit = r.TotalCommitLimit();
    } catch (...) {
    }

    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    const BOOL pmi_ok = GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&pmc), sizeof(pmc));
    const DWORD pmi_err = pmi_ok ? 0 : GetLastError();

    MEMORYSTATUSEX gms{};
    gms.dwLength = sizeof(gms);
    const BOOL gms_ok = GlobalMemoryStatusEx(&gms);

    log_line(Log::Mem,
        "%s usage=%llu limit=%llu usage_mib=%.1f limit_mib=%.1f level=%s%s | report.private=%llu report.peak_private=%llu "
        "report.total_commit=%llu report.total_commit_limit=%llu | pmi=%s ws=%zu peak_ws=%zu pagefile=%zu private=%zu | "
        "gms=%s load=%lu%% total_phys=%llu avail_phys=%llu total_virt=%llu avail_virt=%llu",
        tag, usage, limit, usage / MiB, limit / MiB, level, mm_err.c_str(), r_priv, r_peak, r_total, r_limit,
        pmi_ok ? "ok" : win32_err(pmi_err).c_str(), pmc.WorkingSetSize, pmc.PeakWorkingSetSize, pmc.PagefileUsage,
        pmc.PrivateUsage, gms_ok ? "ok" : "fail", gms.dwMemoryLoad, gms.ullTotalPhys, gms.ullAvailPhys,
        gms.ullTotalVirtual, gms.ullAvailVirtual);

    char s[96];
    snprintf(s, sizeof(s), "%.0f / %.0f MiB (%s)", usage / MiB, limit / MiB, level);
    set_status(&Status::mem, s);
}

void mem_thread_start() {
    g_run = true;
    g_thread = std::thread([] {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        while (g_run) {
            mem_snapshot("tick");
            for (int i = 0; i < 10 && g_run; ++i)
                Sleep(100);
        }
        winrt::uninit_apartment();
    });
}

void mem_thread_stop() {
    g_run = false;
    if (g_thread.joinable())
        g_thread.join();
}

} // namespace vitox
