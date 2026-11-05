// Vitox M2.5 - Dynarmic A32 fastmem on the W^X (NO_EXECUTE_SUPPORT) build.
// A 4 GiB guest window is reserved; guest data accesses go to fastmem_pointer + vaddr.
// A committed page must be served directly (no callback); an uncommitted page must fault into
// Dynarmic's Windows exception handler -> FastmemCallback (the path reordered by the W^X shim)
// -> fall back to the UserCallbacks. Commit is sampled around every step.
#include "common.h"

#include <dynarmic/interface/A32/a32.h>
#include <dynarmic/interface/A32/config.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

namespace vitox {

namespace {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

constexpr u64 kWindow = 1ull << 32;      // whole A32 address space
constexpr u32 kCodeLimit = 0x1000;       // guest code lives in the callback space below this
constexpr u32 kMapped = 0x10000000;      // committed RW page in the window
constexpr u32 kUnmapped = 0x20000000;    // reserved, never committed -> fastmem fault
constexpr u32 kFallbackValue = 0xFA11BAC0; // returned by the callbacks for kUnmapped reads
constexpr u32 kHostValue = 0x11223344;   // written by the host into the mapped page

// Guest code (ARM, little-endian), entered with r1 = mapped slot, r2 = store value, r4 = unmapped slot:
//   ldr r0, [r1]       ; fastmem hit  -> kHostValue + slot
//   str r2, [r1, #4]   ; fastmem hit  -> visible to the host at base + r1 + 4
//   ldr r3, [r4]       ; fastmem miss -> fault -> callback -> kFallbackValue + slot
//   b .
constexpr u32 kCode[] = { 0xE5910000, 0xE5812004, 0xE5943000, 0xEAFFFFFE };

class FastmemEnv final : public Dynarmic::A32::UserCallbacks {
public:
    u64 ticks_left = 0;
    std::array<u8, kCodeLimit> code{};
    std::atomic<u32> data_reads{ 0 };  // callback reads at or above kCodeLimit
    std::atomic<u32> data_writes{ 0 };
    u32 last_read_addr = 0;
    bool fallback = false;
    bool exception = false;

    u8 *window = nullptr; // callbacks mirror the committed page, like a real emulator's slow path

    explicit FastmemEnv(u8 *base)
        : window(base) {
        for (u32 i = 0; i < std::size(kCode); ++i)
            for (u32 b = 0; b < 4; ++b)
                code[i * 4 + b] = static_cast<u8>(kCode[i] >> (8 * b));
    }

    static bool in_mapped(u32 vaddr) { return vaddr >= kMapped && vaddr < kMapped + 0x10000; }

    u32 data(u32 vaddr) {
        ++data_reads;
        last_read_addr = vaddr;
        if (in_mapped(vaddr))
            return *reinterpret_cast<volatile u32 *>(window + vaddr);
        return kFallbackValue + (vaddr - kUnmapped) / 0x1000;
    }
    void store(u32 vaddr, u32 value) {
        ++data_writes;
        if (in_mapped(vaddr))
            *reinterpret_cast<volatile u32 *>(window + vaddr) = value;
    }

    u8 MemoryRead8(u32 vaddr) override {
        if (vaddr < kCodeLimit)
            return code[vaddr];
        return static_cast<u8>(data(vaddr));
    }
    u16 MemoryRead16(u32 vaddr) override {
        if (vaddr < kCodeLimit)
            return static_cast<u16>(MemoryRead8(vaddr) | (MemoryRead8(vaddr + 1) << 8));
        return static_cast<u16>(data(vaddr));
    }
    u32 MemoryRead32(u32 vaddr) override {
        if (vaddr < kCodeLimit)
            return static_cast<u32>(MemoryRead16(vaddr)) | (static_cast<u32>(MemoryRead16(vaddr + 2)) << 16);
        return data(vaddr);
    }
    u64 MemoryRead64(u32 vaddr) override {
        return static_cast<u64>(MemoryRead32(vaddr)) | (static_cast<u64>(MemoryRead32(vaddr + 4)) << 32);
    }
    void MemoryWrite8(u32, u8) override { ++data_writes; }
    void MemoryWrite16(u32, u16) override { ++data_writes; }
    void MemoryWrite32(u32 vaddr, u32 value) override { store(vaddr, value); }
    void MemoryWrite64(u32, u64) override { ++data_writes; }

    void InterpreterFallback(u32, std::size_t) override {
        fallback = true;
        ticks_left = 0;
    }
    void CallSVC(u32) override { ticks_left = 0; }
    void ExceptionRaised(u32, Dynarmic::A32::Exception) override {
        exception = true;
        ticks_left = 0;
    }
    void AddTicks(u64 ticks) override { ticks_left = ticks >= ticks_left ? 0 : ticks_left - ticks; }
    u64 GetTicksRemaining() override { return ticks_left; }
};

struct Window {
    u8 *base = nullptr;
    bool ok = false;
};

Window reserve_window() {
    Window w;
    w.base = static_cast<u8 *>(VirtualAlloc(nullptr, kWindow, MEM_RESERVE, PAGE_NOACCESS));
    if (!w.base) {
        log_line(Log::Fastmem, "reserve 4 GiB FAILED error=%s", win32_err(GetLastError()).c_str());
        return w;
    }
    if (!VirtualAlloc(w.base + kMapped, 0x10000, MEM_COMMIT, PAGE_READWRITE)) {
        log_line(Log::Fastmem, "commit 64 KiB at +0x%08X FAILED error=%s", kMapped, win32_err(GetLastError()).c_str());
        VirtualFree(w.base, 0, MEM_RELEASE);
        w.base = nullptr;
        return w;
    }
    w.ok = true;
    return w;
}

Dynarmic::A32::UserConfig make_config(FastmemEnv &env, u8 *base) {
    Dynarmic::A32::UserConfig cfg{};
    cfg.callbacks = &env;
    cfg.arch_version = Dynarmic::A32::ArchVersion::v7;
    cfg.code_cache_size = 16ull * 1024 * 1024;
    cfg.fastmem_pointer = reinterpret_cast<uintptr_t>(base);
    cfg.recompile_on_fastmem_failure = true;
    return cfg;
}

struct RunResult {
    u32 r0 = 0, r3 = 0, stored = 0, reads = 0, writes = 0;
    bool ok = false;
};

// One pass of the guest block for JIT slot `slot` (distinct addresses per slot).
RunResult run_once(Dynarmic::A32::Jit &cpu, FastmemEnv &env, u8 *base, u32 slot) {
    const u32 mapped = kMapped + slot * 0x100;
    const u32 unmapped = kUnmapped + slot * 0x1000;
    const u32 store_value = 0x5EED0000 + slot;
    *reinterpret_cast<volatile u32 *>(base + mapped) = kHostValue + slot;
    *reinterpret_cast<volatile u32 *>(base + mapped + 4) = 0;
    env.data_reads = 0;
    env.data_writes = 0;
    env.fallback = env.exception = false;

    cpu.Reset();
    cpu.Regs()[1] = mapped;
    cpu.Regs()[2] = store_value;
    cpu.Regs()[4] = unmapped;
    cpu.Regs()[15] = 0;
    cpu.SetCpsr(0x00000010); // ARM, user mode
    env.ticks_left = 1;
    cpu.Run();

    RunResult r;
    r.r0 = cpu.Regs()[0];
    r.r3 = cpu.Regs()[3];
    r.stored = *reinterpret_cast<volatile u32 *>(base + mapped + 4);
    r.reads = env.data_reads;
    r.writes = env.data_writes;
    r.ok = r.r0 == kHostValue + slot && r.stored == store_value && r.r3 == kFallbackValue + slot &&
        env.last_read_addr == unmapped && !env.fallback && !env.exception;
    return r;
}

// Single JIT: first run must hit fastmem for the mapped accesses (no callbacks for them) and
// fault into the callback exactly for the unmapped load; second run uses the recompiled block.
bool single_jit(u8 *base) {
    FastmemEnv env{ base };
    auto cfg = make_config(env, base);
    const auto t0 = std::chrono::steady_clock::now();
    Dynarmic::A32::Jit cpu{ cfg };
    const auto create_us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
    mem_snapshot("fm_after_jit_create");

    const RunResult a = run_once(cpu, env, base, 0);
    // First pass: the only data callback is the faulting unmapped load (1 read, 0 writes).
    const bool first_ok = a.ok && a.reads == 1 && a.writes == 0;
    log_line(Log::Fastmem, "single run1 %s r0=0x%08X stored=0x%08X r3=0x%08X cb_reads=%u cb_writes=%u create_us=%lld (want hit/hit/fault: cb_reads=1 cb_writes=0)",
        first_ok ? "PASS" : "FAIL", a.r0, a.stored, a.r3, a.reads, a.writes, static_cast<long long>(create_us));

    const RunResult b = run_once(cpu, env, base, 0);
    // After recompile_on_fastmem_failure the faulting access goes straight to the callback;
    // values must be identical. Callback counts are reported, not required.
    log_line(Log::Fastmem, "single run2 (recompiled) %s r0=0x%08X stored=0x%08X r3=0x%08X cb_reads=%u cb_writes=%u",
        b.ok ? "PASS" : "FAIL", b.r0, b.stored, b.r3, b.reads, b.writes);
    mem_snapshot("fm_after_single_runs");
    return first_ok && b.ok;
}

// Several JITs over the same fastmem window, faulting concurrently on different unmapped pages.
bool multi_jit(u8 *base, int count) {
    std::vector<std::unique_ptr<FastmemEnv>> envs;
    std::vector<std::unique_ptr<Dynarmic::A32::Jit>> jits;
    for (int i = 0; i < count; ++i) {
        envs.emplace_back(std::make_unique<FastmemEnv>(base));
        auto cfg = make_config(*envs.back(), base);
        jits.emplace_back(std::make_unique<Dynarmic::A32::Jit>(cfg));
    }
    mem_snapshot("fm_multi_after_create");

    std::vector<RunResult> results(static_cast<std::size_t>(count));
    std::vector<std::thread> threads;
    for (int i = 0; i < count; ++i)
        threads.emplace_back([&, i] { results[i] = run_once(*jits[i], *envs[i], base, static_cast<u32>(i + 1)); });
    for (auto &t : threads)
        t.join();

    int passed = 0;
    for (int i = 0; i < count; ++i) {
        const RunResult &r = results[i];
        const bool ok = r.ok && r.reads == 1 && r.writes == 0;
        passed += ok ? 1 : 0;
        if (!ok)
            log_line(Log::Fastmem, "multi%d jit%d FAIL r0=0x%08X stored=0x%08X r3=0x%08X cb_reads=%u cb_writes=%u", count, i, r.r0, r.stored, r.r3, r.reads, r.writes);
    }
    log_line(Log::Fastmem, "multi count=%d concurrent_pass=%d/%d", count, passed, count);
    mem_snapshot("fm_multi_after_run");
    jits.clear();
    envs.clear();
    mem_snapshot("fm_multi_after_destroy");
    return passed == count;
}

} // namespace

void probe_fastmem() {
    log_line(Log::Fastmem, "Fastmem M2.5 begin");
    mem_snapshot("fm_begin");

    Window w = reserve_window();
    if (!w.ok) {
        set_status(&Status::fastmem, "FAIL (window)");
        log_line(Log::Fastmem, "Fastmem M2.5 FAIL");
        return;
    }
    log_line(Log::Fastmem, "window base=%p size=4 GiB reserved, 64 KiB committed at +0x%08X; unmapped probe at +0x%08X",
        w.base, kMapped, kUnmapped);
    mem_snapshot("fm_after_reserve_commit");

    bool overall = single_jit(w.base);
    mem_snapshot("fm_after_single_destroy");
    for (int n : { 2, 4 })
        overall &= multi_jit(w.base, n);

    const BOOL freed = VirtualFree(w.base, 0, MEM_RELEASE);
    log_line(Log::Fastmem, "window release=%d", freed ? 1 : 0);
    overall &= freed != FALSE;
    Sleep(100);
    mem_snapshot("fm_end");

    set_status(&Status::fastmem, overall ? "PASS" : "FAIL");
    log_line(Log::Fastmem, "Fastmem M2.5 %s", overall ? "PASS" : "FAIL");
}

} // namespace vitox
