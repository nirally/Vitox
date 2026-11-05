// Vitox M3 core link probe: exercises the REAL Vita3K mem / cpu (DynarmicCPU) / kernel
// sources, compiled for WindowsStore. No frontend, renderer, audio, firmware or app loading.
#include "vitox_core_probe.h"

#include "vitox_camera_report.h"
#include "vitox_compat_report.h"
#include "vitox_import_dispatch.h"

#include <emuenv/state.h>
#include "vitox_core_sources.h" // generated: selected / excluded Vita3K sources
#include "vitox_sdl_shim_report.h"

#include <cpu/functions.h>
#include <cpu/state.h>
#include <kernel/load_self.h>
#include <kernel/state.h>
#include <kernel/thread/thread_state.h>
#include <nids/functions.h>
#include <mem/functions.h>
#include <mem/ptr.h>
#include <mem/state.h>
#include <modules/sysmem_state.h>
#include <util/log.h>

#include <spdlog/sinks/callback_sink.h>
#include <spdlog/sinks/null_sink.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <string_view>
#include <vector>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace {

const VitoxCoreHost *g_host = nullptr;

template <typename... Args>
void out(fmt::format_string<Args...> f, Args &&...args) {
    g_host->log(fmt::format(f, std::forward<Args>(args)...).c_str());
}

void snap(const char *tag) { g_host->mem_snapshot(tag); }

long long us_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
}

// Route Vita3K's own LOG_* (spdlog default logger) into the probe log.
void install_log_bridge() {
    auto sink = std::make_shared<spdlog::sinks::callback_sink_mt>([](const spdlog::details::log_msg &msg) {
        const auto level = spdlog::level::to_string_view(msg.level);
        const std::string line = fmt::format("[vita3k {}] {}", std::string_view(level.data(), level.size()),
            std::string_view(msg.payload.data(), msg.payload.size()));
        g_host->log(line.c_str());
    });
    auto logger = std::make_shared<spdlog::logger>("vita3k", sink);
    logger->set_level(spdlog::level::info);
    spdlog::set_default_logger(logger);
}

// ---------------------------------------------------------------- mem

bool mem_proof(MemState &mem) {
    const Address a = alloc(mem, 0x4000, "vitox-mem-test");
    bool alloc_ok = a >= user_main_memory_start && is_valid_addr(mem, a);
    uint32_t *p = Ptr<uint32_t>(a).get(mem);
    p[0] = 0x12345678;
    const bool rw_ok = *reinterpret_cast<uint32_t *>(mem.memory.get() + a) == 0x12345678;
    out("mem alloc addr=0x{:08X} valid={} rw={} available={} name='{}'", a, alloc_ok, rw_ok, mem_available(mem), mem_name(a, mem));

    // Protection + exception machinery: write to a read-only page from the host. The fault must be
    // resolved by Vita3K's VEH (registered through the compat wrapper) -> callback -> unprotect.
    const Address pa = a + 0x2000;
    std::atomic<int> cb_calls{ 0 };
    std::atomic<Address> cb_addr{ 0 };
    std::atomic<bool> cb_write{ false };
    const bool added = add_protect(mem, pa, 0x1000, MemPerm::ReadOnly, [&](Address addr, bool write) {
        ++cb_calls;
        cb_addr = addr;
        cb_write = write;
        return true;
    });
    const bool protecting = is_protecting(mem, pa);
    volatile uint32_t *pp = Ptr<uint32_t>(pa + 0x10).get(mem);
    *pp = 0xCAFEF00D;
    const bool prot_ok = added && protecting && cb_calls == 1 && cb_write && cb_addr == pa + 0x10 && *pp == 0xCAFEF00D && !is_protecting(mem, pa);
    out("mem protect page=0x{:08X} added={} protecting={} host-write -> callbacks={} addr=0x{:08X} write={} value_ok={} unprotected={} -> {}",
        pa, added, protecting, cb_calls.load(), cb_addr.load(), cb_write.load(), *pp == 0xCAFEF00D, !is_protecting(mem, pa), prot_ok ? "PASS" : "FAIL");

    free(mem, a);
    const bool freed = !is_valid_addr(mem, a);
    out("mem free valid_after={} -> {}", !freed, freed ? "PASS" : "FAIL");
    return alloc_ok && rw_ok && prot_ok && freed;
}

// ---------------------------------------------------------------- cpu

bool cpu_proof(MemState &mem, const char *label) {
    const Address code = alloc(mem, 0x1000, "vitox-cpu-code");
    const Address data = alloc(mem, 0x1000, "vitox-cpu-data");
    uint32_t *c = Ptr<uint32_t>(code).get(mem);
    c[0] = 0xE3A0002A; // mov r0, #42
    c[1] = 0xE5921000; // ldr r1, [r2]
    c[2] = 0xE5820004; // str r0, [r2, #4]
    c[3] = 0xEF000055; // svc #0x55            -> run() returns, svc_called
    c[4] = 0xEAFFFFFE; // b .
    c[5] = 0xE5820008; // str r0, [r2, #8]     -> into a write-protected page (second run)
    c[6] = 0xEF000056; // svc #0x56
    c[7] = 0xEAFFFFFE; // b .
    uint32_t *d = Ptr<uint32_t>(data).get(mem);
    d[0] = 0xBEEF1234;
    d[1] = d[2] = 0;

    const std::string tag = std::string("core_") + label;
    snap((tag + "_before_cpu_init").c_str());
    const auto t0 = std::chrono::steady_clock::now();
    CPUStatePtr cpu = init_cpu(true, 1, 0, mem);
    const long long create_us = us_since(t0);
    if (!cpu) {
        out("cpu[{}] init_cpu FAILED", label);
        return false;
    }
    snap((tag + "_after_cpu_init").c_str());
    out("cpu[{}] init_cpu ok in {} us: Vita3K DynarmicCPU, cpu_opt=1, mem.use_page_table={} -> {}", label, create_us,
        mem.use_page_table, mem.use_page_table ? "config.page_table (absolute offset)" : "config.fastmem_pointer = MemState.memory");

    write_pc(*cpu, code);
    write_reg(*cpu, 2, data);
    const auto t1 = std::chrono::steady_clock::now();
    const int res = run(*cpu);
    const long long run_us = us_since(t1);
    const uint32_t r0 = read_reg(*cpu, 0), r1 = read_reg(*cpu, 1);
    const bool run1 = res == 0 && cpu->svc_called && cpu->svc == 0x55 && r0 == 42 && r1 == 0xBEEF1234 && d[1] == 42;
    out("cpu[{}] run1 res={} svc_called={} svc=0x{:X} r0={} r1=0x{:08X} stored={} first_run_us={} -> {}", label, res, cpu->svc_called,
        cpu->svc, r0, r1, d[1], run_us, run1 ? "PASS" : "FAIL");

    // Guest store into a write-protected guest page: Vita3K's VEH (first handler) must service the
    // fault raised inside JIT code, run the callback, unprotect, and let the store retry.
    std::atomic<int> cb_calls{ 0 };
    add_protect(mem, data, 0x1000, MemPerm::ReadOnly, [&](Address, bool) {
        ++cb_calls;
        return true;
    });
    write_pc(*cpu, code + 0x14);
    const int res2 = run(*cpu);
    const bool run2 = res2 == 0 && cpu->svc_called && cpu->svc == 0x56 && d[2] == 42 && cb_calls == 1 && !is_protecting(mem, data);
    out("cpu[{}] run2 (JIT store into protected page) res={} svc=0x{:X} stored={} protect_callbacks={} unprotected={} -> {}", label, res2,
        cpu->svc, d[2], cb_calls.load(), !is_protecting(mem, data), run2 ? "PASS" : "FAIL");

    cpu.reset();
    snap((tag + "_after_cpu_destroy").c_str());
    free(mem, code);
    free(mem, data);
    out("cpu[{}] destroyed -> {}", label, run1 && run2 ? "PASS" : "FAIL");
    return run1 && run2;
}

// ---------------------------------------------------------------- kernel

bool kernel_proof(MemState &mem) {
    auto *kernel = new KernelState; // deleted only after a successful deinit (threads hold a reference)
    std::atomic<int> import_calls{ 0 };
    std::atomic<uint32_t> import_nid{ 0 };
    std::atomic<SceUID> import_tid{ 0 };
    const CallImportFunc call_import = [&](CPUState &cpu, uint32_t nid, SceUID thread_id) {
        ++import_calls;
        import_nid = nid;
        import_tid = thread_id;
        write_reg(cpu, 0, read_reg(cpu, 0) * 6);
    };

    snap("core_before_kernel_init");
    if (!kernel->init(mem, call_import, true)) {
        out("kernel init FAILED");
        return false;
    }
    out("kernel init ok: halt_instruction_pc=0x{:08X}", kernel->halt_instruction_pc);

    // Guest entry (ARM): r0 = 7; import call (Vita3K stub layout: svc, bx lr, NID) -> r0 * 6; return to
    // the kernel halt stub (NOP + WFI) -> guest_returned, returned_value = r0.
    const Address code = alloc(mem, 0x1000, "vitox-kernel-entry");
    uint32_t *c = Ptr<uint32_t>(code).get(mem);
    c[0] = 0xE3A00007; // mov r0, #7
    c[1] = 0xEF000000; // svc #0
    c[2] = 0xE12FFF1E; // bx lr
    c[3] = 0xC0DEC0DE; // NID read by ThreadState::run_loop at pc + 4

    ThreadStatePtr thread = kernel->create_thread(mem, "vitox-core-thread", Ptr<const void>(code));
    if (!thread) {
        out("kernel create_thread FAILED");
        return false;
    }
    snap("core_after_kernel_thread_create");
    const SceUID tid = thread->id;
    const int start_res = thread->start(0, Ptr<void>(0), false);

    bool returned = false;
    uint32_t value = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        {
            std::lock_guard<std::mutex> lock(thread->mutex);
            if (thread->status == ThreadStatus::dormant && thread->returned_value != static_cast<uint32_t>(SCE_KERNEL_ERROR_NOT_DORMANT)) {
                returned = true;
                value = thread->returned_value;
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const bool run_ok = start_res == 0 && returned && value == 42 && import_calls == 1 && import_nid == 0xC0DEC0DE && import_tid == tid;
    out("kernel thread id={} start={} returned={} value={} import_calls={} nid=0x{:08X} import_tid={} -> {}", tid, start_res, returned,
        value, import_calls.load(), import_nid.load(), import_tid.load(), run_ok ? "PASS" : "FAIL");
    snap("core_after_kernel_thread_run");
    thread.reset();

    // Cleanup through Vita3K's own path: KernelState::deinit -> process_exit waits for all threads
    // to be deleted. Guard it with our own bounded wait first so a stuck thread cannot hang the probe.
    {
        std::lock_guard<std::mutex> lock(kernel->mutex);
        for (auto &[_, t] : kernel->threads)
            t->exit_delete(false);
    }
    bool threads_gone = false;
    {
        std::unique_lock<std::mutex> lock(kernel->mutex);
        threads_gone = kernel->thread_deleted_cond.wait_for(lock, std::chrono::seconds(5), [&] { return kernel->threads.empty(); });
    }
    if (threads_gone) {
        kernel->deinit(mem);
        delete kernel;
    }
    free(mem, code);
    snap("core_after_kernel_deinit");
    out("kernel cleanup threads_deleted={} deinit={} -> {}", threads_gone, threads_gone, threads_gone ? "PASS" : "FAIL");
    return run_ok && threads_gone;
}

// ---------------------------------------------------------------- loader / imports (M3.1 -> 0.0.7.0)

// Hand-built ET_SCE_RELEXEC: one PT_LOAD segment (module info, one short import entry per library,
// NID/entry tables, stubs, data, ARM code) + one PT_SCE_RELA segment with Format0/Abs32 relocations
// for every absolute pointer of the import entries. Layout offsets are relative to the segment start.
namespace guest {
constexpr uint32_t kPreferredVaddr = 0x81000000;
constexpr uint32_t kSegSize = 0x1000;
constexpr uint32_t kImports = 0x100;   // sce_module_imports_short_raw, 0x24 each
constexpr uint32_t kTables = 0x160;    // per library: NID table, then entry table
constexpr uint32_t kLibNames = 0x1C0;  // 0x20 per library name
constexpr uint32_t kStubs = 0x200;     // 16 bytes per imported function (written by the loader)
constexpr uint32_t kBlockName = 0x2C0; // memblock name passed to sceKernelAllocMemBlock
constexpr uint32_t kCode = 0x300;
constexpr uint32_t kModuleNid = 0x56495458; // "VITX"
constexpr char kBlockNameStr[] = "vitox-m41-block";

void put16(std::vector<uint8_t> &b, size_t o, uint16_t v) { memcpy(&b[o], &v, 2); }
void put32(std::vector<uint8_t> &b, size_t o, uint32_t v) { memcpy(&b[o], &v, 4); }
uint32_t rel24(uint32_t from, uint32_t to) { return ((static_cast<int32_t>(to) - static_cast<int32_t>(from + 8)) >> 2) & 0xFFFFFF; }

struct Library {
    const char *name;
    std::vector<uint32_t> nids;
};
struct Reloc {
    uint32_t patch;  // offset in segment 0 to patch
    uint32_t target; // offset in segment 0 the pointer must point at
};
struct Image {
    std::vector<uint8_t> file;
    std::vector<Reloc> relocs;
    std::vector<std::pair<uint32_t, uint32_t>> stubs; // (NID, stub offset)
    uint32_t stub_of(uint32_t nid) const {
        for (const auto &[n, o] : stubs)
            if (n == nid)
                return o;
        return 0;
    }
};

// Minimal ARM assembler for the guest program. Every failure exit loads a stage code into r0 and
// branches to `done`, so the guest return value names the first failing step.
enum Cond : uint32_t { EQ = 0x0, NE = 0x1, GE = 0xA, LT = 0xB, AL = 0xE };
struct Asm {
    std::vector<uint32_t> w;
    std::vector<size_t> to_done;
    uint32_t here() const { return kCode + static_cast<uint32_t>(w.size()) * 4; }
    void op(uint32_t i) { w.push_back(i); }
    void movw(int d, uint32_t imm) { op(0xE3000000 | ((imm >> 12) & 0xF) << 16 | d << 12 | (imm & 0xFFF)); }
    void movt(int d, uint32_t imm) { op(0xE3400000 | ((imm >> 12) & 0xF) << 16 | d << 12 | (imm & 0xFFF)); }
    void mov32(int d, uint32_t v) {
        movw(d, v & 0xFFFF);
        if (v >> 16)
            movt(d, v >> 16);
    }
    void mov(int d, int m) { op(0xE1A00000 | d << 12 | m); }
    void cmp0(int n) { op(0xE3500000 | n << 16); }
    void cmp(int n, int m) { op(0xE1500000 | n << 16 | m); }
    void ldr(int d, int n) { op(0xE5900000 | n << 16 | d << 12); }
    void str(int d, int n) { op(0xE5800000 | n << 16 | d << 12); }
    void bl(uint32_t to) { op(0xEB000000 | rel24(here(), to)); }
    bool adr(int d, uint32_t to) { // add/sub rd, pc, #imm8
        const uint32_t pc = here() + 8;
        const uint32_t off = to < pc ? pc - to : to - pc;
        op((to < pc ? 0xE24F0000 : 0xE28F0000) | d << 12 | (off & 0xFF));
        return off <= 0xFF;
    }
    void fail_if(Cond c, uint8_t code) {
        op(c << 28 | 0x03A00000 | code); // mov<c> r0, #code
        to_done.push_back(w.size());
        op(c << 28 | 0x0A000000); // b<c> done
    }
    void bind_done() {
        for (size_t i : to_done)
            w[i] |= rel24(kCode + static_cast<uint32_t>(i) * 4, here());
    }
};

constexpr uint32_t kSize = 0x10000;     // memblock size
constexpr uint32_t kValue = 0x5A17BEEF; // deterministic guest store/load value
constexpr uint32_t kSuccess = 0x0041600D;

// Guest program (0.0.7.0): stateful SceSysmem memblock lifecycle + one RTC regression call.
//   uid = sceKernelAllocMemBlock("vitox-m41-block", USER_RW, 0x10000, NULL)   uid < 0     -> 0xE1
//   r = sceKernelGetMemBlockBase(uid, &base)  (guest pointer on the stack) r != 0      -> 0xE2
//   base != 0 && sceKernelFindMemBlockByAddr(base + 0x8000, 4) == uid                  else 0xE3
//   *base = 0x5A17BEEF; reload; compare                                                 else 0xE4
//   sceKernelFreeMemBlock(uid) == 0                                                     else 0xE5
//   sceKernelFindMemBlockByAddr(base, 4) < 0 && sceKernelGetMemBlockBase(uid, &b) != 0  else 0xE6
//   sceRtcIsLeapYear(2024) == 1                                                         else 0xE7
//   return 0x0041600D
Image build(const std::vector<Library> &libs, uint32_t nid_alloc, uint32_t nid_base, uint32_t nid_free, uint32_t nid_find,
    uint32_t nid_leap, bool &adr_ok) {
    Image img;
    constexpr uint32_t seg_off = 0x100;
    std::vector<uint8_t> seg(kSegSize, 0);

    // module info (0x5C)
    put16(seg, 0x00, 0);
    put16(seg, 0x02, 0x0101);
    memcpy(&seg[0x04], "VitoxM41", 8);
    seg[0x1F] = 0; // executable
    put32(seg, 0x24, kImports); // export_top
    put32(seg, 0x28, kImports); // export_end (no exports)
    put32(seg, 0x2C, kImports); // import_top
    put32(seg, 0x30, kImports + static_cast<uint32_t>(libs.size()) * 0x24);
    put32(seg, 0x34, kModuleNid);

    uint32_t table = kTables, stub = kStubs;
    for (size_t i = 0; i < libs.size(); ++i) {
        const Library &lib = libs[i];
        const uint32_t imp = kImports + static_cast<uint32_t>(i) * 0x24;
        const uint32_t name = kLibNames + static_cast<uint32_t>(i) * 0x20;
        const uint32_t nid_table = table, entry_table = table + static_cast<uint32_t>(lib.nids.size()) * 4;
        table = entry_table + static_cast<uint32_t>(lib.nids.size()) * 4;
        put16(seg, imp + 0x00, 0x24);
        put16(seg, imp + 0x02, 1);
        put16(seg, imp + 0x06, static_cast<uint16_t>(lib.nids.size()));
        put32(seg, imp + 0x0C, 0); // library_nid (resolution falls back to the function NID)
        // pointers hold preferred-vaddr values: a missing relocation would point at the blocked address
        put32(seg, imp + 0x10, kPreferredVaddr + name);
        put32(seg, imp + 0x14, kPreferredVaddr + nid_table);
        put32(seg, imp + 0x18, kPreferredVaddr + entry_table);
        img.relocs.push_back({ imp + 0x10, name });
        img.relocs.push_back({ imp + 0x14, nid_table });
        img.relocs.push_back({ imp + 0x18, entry_table });
        memcpy(&seg[name], lib.name, strlen(lib.name));
        for (size_t j = 0; j < lib.nids.size(); ++j) {
            put32(seg, nid_table + j * 4, lib.nids[j]);
            put32(seg, entry_table + j * 4, kPreferredVaddr + stub);
            img.relocs.push_back({ entry_table + static_cast<uint32_t>(j) * 4, stub });
            img.stubs.emplace_back(lib.nids[j], stub);
            stub += 0x10;
        }
    }
    memcpy(&seg[kBlockName], kBlockNameStr, sizeof(kBlockNameStr));

    Asm a;
    a.op(0xE92D41F0); // push {r4-r8, lr}
    a.op(0xE24DD008); // sub sp, sp, #8        [sp] = guest out-pointer slot for the base address
    a.movw(7, 0);
    a.str(7, 13);     // [sp] = 0
    // 1. allocate
    adr_ok = a.adr(0, kBlockName);
    a.mov32(1, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW);
    a.mov32(2, kSize);
    a.movw(3, 0);
    a.bl(img.stub_of(nid_alloc));
    a.mov(4, 0); // r4 = uid
    a.cmp0(4);
    a.fail_if(LT, 0xE1);
    // 2. base address through a guest pointer argument
    a.mov(0, 4);
    a.mov(1, 13); // r1 = sp (guest address of the out slot)
    a.bl(img.stub_of(nid_base));
    a.cmp0(0);
    a.fail_if(NE, 0xE2);
    a.ldr(6, 13); // r6 = base
    // 3. validity: non-null, and the real lookup maps an inner address back to the same uid
    a.cmp0(6);
    a.fail_if(EQ, 0xE3);
    a.op(0xE2860902); // add r0, r6, #0x8000
    a.movw(1, 4);
    a.bl(img.stub_of(nid_find));
    a.cmp(0, 4);
    a.fail_if(NE, 0xE3);
    // 4. deterministic 32-bit store + load through the block
    a.mov32(7, kValue);
    a.str(7, 6);
    a.ldr(8, 6);
    a.cmp(8, 7);
    a.fail_if(NE, 0xE4);
    // 5. free
    a.mov(0, 4);
    a.bl(img.stub_of(nid_free));
    a.cmp0(0);
    a.fail_if(NE, 0xE5);
    // 6. the allocation is gone: address lookup and uid lookup must both fail now
    a.mov(0, 6);
    a.movw(1, 4);
    a.bl(img.stub_of(nid_find));
    a.cmp0(0);
    a.fail_if(GE, 0xE6);
    a.mov(0, 4);
    a.mov(1, 13);
    a.bl(img.stub_of(nid_base));
    a.cmp0(0);
    a.fail_if(EQ, 0xE6);
    // 7. RTC regression through the same path
    a.movw(0, 2024);
    a.bl(img.stub_of(nid_leap));
    a.op(0xE3500001); // cmp r0, #1  (sceRtcIsLeapYear(2024) == 1)
    a.fail_if(NE, 0xE7);
    a.mov32(0, kSuccess);
    a.bind_done();
    a.op(0xE28DD008); // done: add sp, sp, #8
    a.op(0xE8BD81F0); // pop {r4-r8, pc}  -> kernel halt stub (lr)
    for (size_t i = 0; i < a.w.size(); ++i)
        put32(seg, kCode + i * 4, a.w[i]);

    // ELF file: header, 2 phdrs, segment 0, PT_SCE_RELA
    const uint32_t rela_off = seg_off + kSegSize;
    const uint32_t rela_size = static_cast<uint32_t>(img.relocs.size()) * 12;
    std::vector<uint8_t> &f = img.file;
    f.assign(rela_off + rela_size, 0);
    const uint8_t ident[16] = { 0x7F, 'E', 'L', 'F', 1 /*32*/, 1 /*LSB*/, 1 /*EV_CURRENT*/ };
    memcpy(&f[0], ident, 16);
    put16(f, 0x10, 0xFE04); // e_type ET_SCE_RELEXEC
    put16(f, 0x12, 40);     // e_machine EM_ARM
    put32(f, 0x14, 1);      // e_version
    put32(f, 0x18, 0);      // e_entry: module info at segment 0, offset 0
    put32(f, 0x1C, 0x34);   // e_phoff
    put16(f, 0x28, 0x34);   // e_ehsize
    put16(f, 0x2A, 0x20);   // e_phentsize
    put16(f, 0x2C, 2);      // e_phnum
    put32(f, 0x34 + 0x00, 1); // PT_LOAD
    put32(f, 0x34 + 0x04, seg_off);
    put32(f, 0x34 + 0x08, kPreferredVaddr);
    put32(f, 0x34 + 0x0C, kPreferredVaddr);
    put32(f, 0x34 + 0x10, kSegSize);
    put32(f, 0x34 + 0x14, kSegSize);
    put32(f, 0x34 + 0x18, 5); // R|X
    put32(f, 0x34 + 0x1C, 0x10);
    put32(f, 0x54 + 0x00, 0x60000000); // PT_SCE_RELA
    put32(f, 0x54 + 0x04, rela_off);
    put32(f, 0x54 + 0x10, rela_size);
    memcpy(&f[seg_off], seg.data(), kSegSize);
    // Format0, symbol segment 0, code Abs32 (2), patch segment 0
    for (size_t i = 0; i < img.relocs.size(); ++i) {
        put32(f, rela_off + i * 12 + 0, 0x00000200);
        put32(f, rela_off + i * 12 + 4, img.relocs[i].target); // addend
        put32(f, rela_off + i * 12 + 8, img.relocs[i].patch);  // offset
    }
    return img;
}
} // namespace guest

// 0.0.7.0 (M4.1): a legitimately constructed EmuEnvState; the guest drives the REAL SceSysmem
// memblock exports (state in kernel.obj_store SysmemState, memory in emuenv.mem) and one REAL RTC
// export through load_self -> relocation -> import stubs -> svc -> run_loop -> call_import -> ImportFn.
void loader_proof(VitoxCoreResult &r) {
    constexpr uint32_t kNidAlloc = 0xB9D5EBDE, kNidBase = 0xB8EF5818, kNidFree = 0xA91E15EE, kNidFind = 0xA33B99D1;
    constexpr uint32_t kNidLeap = 0xF17FD8B5;
    const uint32_t selected[] = { kNidAlloc, kNidBase, kNidFree, kNidFind, kNidLeap };
    r.export_resolved = true;
    for (const uint32_t nid : selected) {
        const VitoxSelectedImport *imp = vitox_resolve_selected_import(nid);
        const bool ok = imp && imp->fn && *imp->fn && std::string_view(import_name(nid)) == imp->name;
        r.export_resolved &= ok;
        out("m41 resolve NID 0x{:08X} -> {} ImportFn={} nids.inc='{}' -> {}", nid, imp ? imp->name : "?",
            imp && imp->fn ? reinterpret_cast<const void *>(*imp->fn) : nullptr, import_name(nid), ok ? "PASS" : "FAIL");
    }
    if (!r.export_resolved)
        return;

    snap("core_emuenv_before_ctor");
    auto t0 = std::chrono::steady_clock::now();
    auto *emuenv = new EmuEnvState(); // real Vita3K constructor (emuenv/src/emuenv.cpp)
    const long long ctor_us = us_since(t0);
    snap("core_emuenv_after_ctor");
    out("emuenv: EmuEnvState constructed in {} us (real emuenv.cpp; inert camera; SDL type-only shim); no subsystem init called "
        "(renderer/audio/net/np/http/ngs/gxm/package implementation TUs are not even linked)", ctor_us);
    MemState &mem = emuenv->mem;
    KernelState &kernel = emuenv->kernel;
    if (!init(mem, false)) {
        out("emuenv.mem init FAILED");
        return;
    }
    snap("core_emuenv_after_mem_init");

    struct Calls {
        std::atomic<int> total{ 0 }, real{ 0 }, alloc{ 0 }, base{ 0 }, free{ 0 }, find{ 0 }, leap{ 0 };
    } calls;
    std::atomic<int32_t> alloc_uid{ 0 }, base_ret{ -1 }, free_ret{ -1 }, post_find{ 0 }, post_base{ 0 }, find_ret{ 0 }, leap_ret{ -1 };
    std::atomic<uint32_t> basep_addr{ 0 }, block_base{ 0 }, value_before_free{ 0 };
    std::atomic<bool> valid_after_alloc{ false };
    const CallImportFunc call_import = [&](CPUState &cpu, uint32_t nid, SceUID thread_id) {
        ++calls.total;
        const VitoxSelectedImport *imp = vitox_resolve_selected_import(nid);
        if (!imp) {
            out("m41 call_import NID 0x{:08X}: NOT in the selected set -> r0=0", nid);
            write_reg(cpu, 0, 0);
            return;
        }
        const uint32_t a0 = read_reg(cpu, 0), a1 = read_reg(cpu, 1), a2 = read_reg(cpu, 2), a3 = read_reg(cpu, 3);
        std::string args;
        if (nid == kNidAlloc)
            args = fmt::format("pName=0x{:08X}('{}') type=0x{:08X} size=0x{:X} optp=0x{:08X}", a0, Ptr<const char>(a0).get(mem), a1, a2, a3);
        else if (nid == kNidBase)
            args = fmt::format("uid={} basep=0x{:08X} (guest stack slot)", static_cast<int32_t>(a0), a1);
        else if (nid == kNidFind)
            args = fmt::format("addr=0x{:08X} size={}", a0, a1);
        else if (nid == kNidFree) {
            // host view of the guest's store, read through emuenv.mem while the block is still live
            if (block_base)
                value_before_free = *Ptr<uint32_t>(block_base.load()).get(mem);
            args = fmt::format("uid={} (host reads [base]=0x{:08X} before free)", static_cast<int32_t>(a0), value_before_free.load());
        } else
            args = fmt::format("year={}", a0);

        (*imp->fn)(*emuenv, cpu, thread_id); // REAL Vita3K ImportFn -> real export
        ++calls.real;
        const int32_t ret = static_cast<int32_t>(read_reg(cpu, 0));
        std::string extra;
        if (nid == kNidAlloc) {
            ++calls.alloc;
            alloc_uid = ret;
        } else if (nid == kNidBase) {
            if (++calls.base == 1) {
                base_ret = ret;
                basep_addr = a1;
                block_base = *Ptr<uint32_t>(a1).get(mem);
                valid_after_alloc = is_valid_addr(mem, block_base);
                extra = fmt::format(" -> *basep=0x{:08X} is_valid_addr={}", block_base.load(), valid_after_alloc.load());
                snap("core_m41_after_alloc");
            } else
                post_base = ret;
        } else if (nid == kNidFind) {
            (++calls.find == 1 ? find_ret : post_find) = ret;
        } else if (nid == kNidFree) {
            ++calls.free;
            free_ret = ret;
            snap("core_m41_after_free");
        } else {
            ++calls.leap;
            leap_ret = ret;
        }
        const auto *st = emuenv->kernel.obj_store.get<SysmemState>();
        out("m41 real export {}(NID 0x{:08X}, ImportFn={}) {} -> returned r0=0x{:08X} ({}){} | SysmemState blocks={} next_uid={} allocated_user=0x{:X}",
            imp->name, nid, reinterpret_cast<const void *>(*imp->fn), args, static_cast<uint32_t>(ret), ret, extra, st->blocks.size(), st->next_uid,
            st->allocated_user);
    };
    kernel.init(mem, call_import, true);
    vitox_init_selected_libraries(*emuenv); // REAL LIBRARY_INIT(SceSysmem): obj_store.create<SysmemState>()
    {
        const auto *st = kernel.obj_store.get<SysmemState>();
        r.sysmem_init = st && st->blocks.empty() && st->next_uid == 1 && st->allocated_user == 0;
        out("m41 LIBRARY_INIT(SceSysmem) (import_library_init_SceSysmem, real) -> SysmemState={} blocks={} next_uid={} -> {}",
            static_cast<const void *>(st), st ? st->blocks.size() : 0, st ? st->next_uid : 0, r.sysmem_init ? "PASS" : "FAIL");
    }

    // Occupy the preferred vaddr so the loader must relocate the module elsewhere.
    const Address blocker = alloc_at(mem, guest::kPreferredVaddr, guest::kSegSize, "vitox-m41-blocker");
    bool adr_ok = false;
    const guest::Image img = guest::build({ { "SceSysmem", { kNidAlloc, kNidBase, kNidFree, kNidFind } }, { "SceRtcUser", { kNidLeap } } },
        kNidAlloc, kNidBase, kNidFree, kNidFind, kNidLeap, adr_ok);
    snap("core_m41_before_load");
    t0 = std::chrono::steady_clock::now();
    const SceUID uid = load_self(kernel, mem, img.file.data(), "app0:vitox_m41.elf", fs::path());
    const long long load_us = us_since(t0);
    snap("core_m41_after_load");
    if (uid < 0 || !adr_ok) {
        out("m41 load_self FAILED uid={} adr_ok={}", uid, adr_ok);
        return;
    }
    const auto mod = kernel.loaded_modules[uid];
    const Address base = mod->info.segments[0].vaddr.address();
    const uint32_t *seg = Ptr<uint32_t>(base).get(mem);
    bool relocs_ok = base != guest::kPreferredVaddr && blocker == guest::kPreferredVaddr;
    for (const auto &rl : img.relocs)
        relocs_ok &= seg[rl.patch / 4] == base + rl.target;
    r.loader = relocs_ok && std::string_view(mod->info.module_name) == "VitoxM41";
    out("m41 load_self uid={} in {} us: module='{}' seg0=0x{:08X} (preferred 0x{:08X} blocked) size=0x{:X}; {} Abs32 relocations applied={} -> {}",
        uid, load_us, mod->info.module_name, base, guest::kPreferredVaddr, mod->info.segments[0].memsz, img.relocs.size(), relocs_ok,
        r.loader ? "PASS" : "FAIL");

    bool stubs_ok = true, bound = true;
    for (const auto &[nid, off] : img.stubs) {
        const uint32_t *s = Ptr<uint32_t>(base + off).get(mem);
        stubs_ok &= s[0] == 0xEF000000 && s[1] == 0xE1A0F00E && s[2] == nid;
        bound &= kernel.func_binding_infos.count(nid) == 1;
        out("m41 stub {} @0x{:08X} = [{:08X} {:08X} {:08X}]", import_name(nid), base + off, s[0], s[1], s[2]);
    }
    r.nid_table = bound;
    out("m41 stubs ok={} func_binding_infos bound={} -> nid_table {}", stubs_ok, bound, r.nid_table ? "PASS" : "FAIL");

    ThreadStatePtr thread = kernel.create_thread(mem, "vitox-m41-main", Ptr<const void>(base + guest::kCode));
    const int start_res = thread ? thread->start(0, Ptr<void>(0), false) : -1;
    bool returned = false;
    uint32_t value = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (thread && std::chrono::steady_clock::now() < deadline) {
        {
            std::lock_guard<std::mutex> lock(thread->mutex);
            if (thread->status == ThreadStatus::dormant && thread->returned_value != static_cast<uint32_t>(SCE_KERNEL_ERROR_NOT_DORMANT)) {
                returned = true;
                value = thread->returned_value;
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    snap("core_m41_after_run");

    const auto *st = kernel.obj_store.get<SysmemState>();
    const Address bb = block_base;
    const bool valid_after_free = bb && is_valid_addr(mem, bb);
    r.import_stubs = stubs_ok && calls.total == 7;
    r.export_invoked = calls.real == 7 && calls.alloc == 1 && calls.base == 2 && calls.find == 2 && calls.free == 1 && calls.leap == 1;
    r.sysmem_alloc = alloc_uid > 0;
    r.sysmem_base = base_ret == 0 && bb >= 0x80000000u && (bb & (guest::kSize - 1)) == 0 && valid_after_alloc && find_ret == alloc_uid;
    r.sysmem_rw = value_before_free == guest::kValue;
    r.sysmem_free = free_ret == 0 && st->blocks.empty();
    r.sysmem_post_free = post_find == static_cast<int32_t>(SCE_KERNEL_ERROR_BLOCK_ERROR) && post_base == static_cast<int32_t>(SCE_KERNEL_ERROR_INVALID_UID)
        && !valid_after_free;
    r.rtc_regression = leap_ret == 1;
    r.guest_result = start_res == 0 && returned && value == guest::kSuccess;
    out("m41 alloc uid={} | GetMemBlockBase ret={} basep(guest)=0x{:08X} base=0x{:08X} valid_after_alloc={} FindMemBlockByAddr(base+0x8000)={} | "
        "stored/read value: guest-compared, host-read before free=0x{:08X} (want 0x{:08X}) | free ret={} | post-free FindMemBlockByAddr=0x{:08X} "
        "GetMemBlockBase=0x{:08X} is_valid_addr={} SysmemState blocks={} allocated_user=0x{:X}",
        alloc_uid.load(), base_ret.load(), basep_addr.load(), bb, valid_after_alloc.load(), find_ret.load(), value_before_free.load(),
        guest::kValue, free_ret.load(), static_cast<uint32_t>(post_find.load()), static_cast<uint32_t>(post_base.load()), valid_after_free,
        st->blocks.size(), st->allocated_user);
    out("m41 counts: import_calls={} REAL export calls={} (alloc={} base={} find={} free={} leap={}); probe-side handlers for these NIDs: NONE",
        calls.total.load(), calls.real.load(), calls.alloc.load(), calls.base.load(), calls.find.load(), calls.free.load(), calls.leap.load());
    out("m41 guest thread start={} returned={} value=0x{:08X} (want 0x{:08X}; 0xE1..0xE7 = failing step) -> init {} alloc {} base {} rw {} free {} post_free {} rtc {} / result {}",
        start_res, returned, value, guest::kSuccess, r.sysmem_init ? "PASS" : "FAIL", r.sysmem_alloc ? "PASS" : "FAIL",
        r.sysmem_base ? "PASS" : "FAIL", r.sysmem_rw ? "PASS" : "FAIL", r.sysmem_free ? "PASS" : "FAIL", r.sysmem_post_free ? "PASS" : "FAIL",
        r.rtc_regression ? "PASS" : "FAIL", r.guest_result ? "PASS" : "FAIL");
    thread.reset();

    // cleanup through Vita3K: unload_self, threads, KernelState::deinit, deinit_mem, ~EmuEnvState
    const int unload_res = unload_self(kernel, mem, *mod);
    {
        std::lock_guard<std::mutex> lock(kernel.mutex);
        for (auto &[_, t] : kernel.threads)
            t->exit_delete(false);
    }
    bool threads_gone = false;
    {
        std::unique_lock<std::mutex> lock(kernel.mutex);
        threads_gone = kernel.thread_deleted_cond.wait_for(lock, std::chrono::seconds(5), [&] { return kernel.threads.empty(); });
    }
    const bool freed = !is_valid_addr(mem, base);
    bool dtor_ok = false;
    if (threads_gone) {
        kernel.deinit(mem);
        free(mem, blocker);
        deinit_mem(mem);
        snap("core_emuenv_before_dtor");
        t0 = std::chrono::steady_clock::now();
        delete emuenv; // real ~EmuEnvState
        out("emuenv: ~EmuEnvState completed in {} us", us_since(t0));
        dtor_ok = true;
    }
    snap("core_emuenv_after_dtor");
    r.loader_cleanup = unload_res == 0 && freed && threads_gone && dtor_ok;
    out("m41 cleanup unload_self={} segment_freed={} threads_deleted={} emuenv_dtor={} -> {}", unload_res, freed, threads_gone, dtor_ok,
        r.loader_cleanup ? "PASS" : "FAIL");
}

} // namespace

extern "C" VitoxCoreResult vitox_core_probe_run(const VitoxCoreHost *host) {
    g_host = host;
    VitoxCoreResult r{};
    install_log_bridge();
    out("Vita3K core link probe begin");
    out("sources: {}", VITOX_CORE_SOURCES);
    out("excluded: {}", VITOX_CORE_EXCLUDED);
    out("sdl shim symbols: {}", VITOX_SDL_SHIM_SYMBOLS);
    out("compat wrappers: AddVectoredExceptionHandler RemoveVectoredExceptionHandler -> GetProcAddress(kernel32)");
    snap("core_base");

    // Vita3K keeps one MemState per process and never removes its VEH (the handler captures &state),
    // so the states live for the whole process; only deinit_mem() is called on them.
    static MemState *mem_fast = new MemState;
    static MemState *mem_pt = new MemState;

    auto t0 = std::chrono::steady_clock::now();
    const bool init_fast = init(*mem_fast, false);
    out("mem init(use_page_table=false) -> {} in {} us, memory={}, host_page_size={}", init_fast, us_since(t0),
        static_cast<void *>(mem_fast->memory.get()), mem_fast->host_page_size);
    snap("core_after_mem_init");
    if (init_fast) {
        r.mem = mem_proof(*mem_fast);
        r.cpu_fastmem = cpu_proof(*mem_fast, "fastmem");
        r.kernel = kernel_proof(*mem_fast);
        deinit_mem(*mem_fast);
        snap("core_after_mem_deinit");
        out("mem deinit -> memory={}", static_cast<void *>(mem_fast->memory.get()));
    }

    t0 = std::chrono::steady_clock::now();
    const bool init_pt = init(*mem_pt, true);
    out("mem init(use_page_table=true) -> {} in {} us", init_pt, us_since(t0));
    snap("core_after_mem_init_page_table");
    if (init_pt) {
        r.cpu_page_table = cpu_proof(*mem_pt, "page_table");
        deinit_mem(*mem_pt);
        snap("core_after_m3_regression");
    }

    // 0.0.7.0: real EmuEnvState + real SceSysmem/RTC exports through the loader/import path (last: see below).
    loader_proof(r);

    // ~EmuEnvState freed the MemState that Vita3K's process-wide VEH captured; re-point the handler
    // at a process-lifetime MemState (init registers it, deinit leaves it inert) so it never dangles.
    static MemState *mem_veh_anchor = new MemState;
    if (init(*mem_veh_anchor, false))
        deinit_mem(*mem_veh_anchor);
    snap("core_after_cleanup");

    const VitoxCompatReport cr = vitox_compat_report();
    out("compat: add_veh calls={} ok={} remove_veh calls={} resolved add={} remove={}", cr.add_veh_calls, cr.add_veh_ok,
        cr.remove_veh_calls, cr.add_veh_resolved, cr.remove_veh_resolved);
    const VitoxSdlShimReport sr = vitox_sdl_shim_report();
    out("sdl shim calls: CreateThread={} DetachThread={} named={} CreateSemaphore={} SignalSemaphore={} WaitSemaphore={} DestroySemaphore={}",
        sr.create_thread, sr.detach_thread, sr.threads_named, sr.create_sem, sr.signal_sem, sr.wait_sem, sr.destroy_sem);
    const VitoxSdlSensorReport ss = vitox_sdl_sensor_report();
    const VitoxCameraReport cam = vitox_camera_report();
    out("sdl type shim: {}", VITOX_SDL_TYPE_SHIM_SYMBOLS);
    out("SDL_CloseSensor: total={} non_null={} (required non_null=0) | inert camera: ctor={} dtor={} unexpected_use={} (required 0)",
        ss.close_total, ss.close_non_null, cam.ctor, cam.dtor, cam.unexpected_use);
    r.export_invoked = r.export_invoked && ss.close_non_null == 0 && cam.unexpected_use == 0;
    out("Vita3K core link probe end (M3 regression): mem={} cpu_fastmem={} cpu_page_table={} kernel={}", r.mem, r.cpu_fastmem, r.cpu_page_table, r.kernel);
    out("0.0.7.0: loader={} nid_table={} import_stubs={} export_resolved={} REAL_export_invoked={} sysmem init={} alloc={} base={} rw={} free={} post_free={} "
        "rtc_regression={} guest_result={} cleanup(incl. ~EmuEnvState)={}",
        r.loader, r.nid_table, r.import_stubs, r.export_resolved, r.export_invoked, r.sysmem_init, r.sysmem_alloc, r.sysmem_base, r.sysmem_rw,
        r.sysmem_free, r.sysmem_post_free, r.rtc_regression, r.guest_result, r.loader_cleanup);
    spdlog::set_default_logger(spdlog::null_logger_mt("vita3k-off"));
    return r;
}
