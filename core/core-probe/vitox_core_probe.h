// Vitox M3 core link probe - plain C ABI so the UWP app never includes Vita3K headers
// (all Vita3K/spdlog/fmt/Boost flags stay inside the CMake-built vitox_core library).
#pragma once

struct VitoxCoreHost {
    void (*log)(const char *line);       // one line, no trailing newline
    void (*mem_snapshot)(const char *tag); // AppMemoryUsage sample in the app's mem log
    // 0.0.24.3 (M9A.4, observability only): optional durable checkpoint - one line to the write-through milestone file,
    // then every log is flushed to disk (survives a stall / forced stop). nullptr: not provided by this host.
    void (*checkpoint)(const char *line) = nullptr;
};

struct VitoxCoreResult {
    bool mem;            // real Vita3K MemState: init, alloc, r/w, protect + VEH, free
    bool cpu_fastmem;    // real Vita3K DynarmicCPU, fastmem_pointer = MemState.memory
    bool cpu_page_table; // real Vita3K DynarmicCPU, page_table (absolute offset)
    bool kernel;         // real Vita3K KernelState + ThreadState guest thread, import call, deinit
    // M3.1 (real load_self / relocation / nids / SceRtcUser module TU)
    bool loader;         // hand-built ET_SCE_RELEXEC loaded + relocated at a non-preferred address
    bool nid_table;      // nids.inc names + kernel func_binding_infos for the imports
    bool import_stubs;   // load_func_imports wrote svc/mov pc,lr/NID stubs; guest svc -> NID -> call_import
    bool export_resolved;   // NID -> the REAL Vita3K ImportFn object (import_sceRtc*), linked from the module TU
    bool export_invoked;    // the REAL ImportFn objects were called (exact per-NID counts)
    bool guest_result;   // deterministic guest-visible result + clean guest return
    bool loader_cleanup; // unload_self + KernelState::deinit
    // 0.0.7.0 (M4.1): stateful SceSysmem memblock exports, all REAL Vita3K code
    bool sysmem_init;      // real LIBRARY_INIT(SceSysmem) created SysmemState in kernel.obj_store
    bool sysmem_alloc;     // sceKernelAllocMemBlock returned a uid
    bool sysmem_base;      // sceKernelGetMemBlockBase wrote a valid base through a guest pointer; FindMemBlockByAddr agrees
    bool sysmem_rw;        // deterministic guest store visible in emuenv.mem at that base
    bool sysmem_free;      // sceKernelFreeMemBlock == 0 and SysmemState has no blocks
    bool sysmem_post_free; // after free: address and uid lookups fail, is_valid_addr false
    bool rtc_regression;   // sceRtcIsLeapYear(2024) == 1 through the same path
};

extern "C" VitoxCoreResult vitox_core_probe_run(const VitoxCoreHost *host);
