// Vitox M5B - first headless homebrew execution through the real Vita3K core loader. Plain C ABI.
#pragma once

#include "vitox_core_probe.h" // VitoxCoreHost

struct VitoxHomebrewResult {
    bool installed;     // packaged VITOX0001 copied into vita-fs\ux0\app\VITOX0001 (real fs_utils::copy_directory_contents)
    bool sfo;           // real vfs::read_app_file + sfo::load of sce_sys/param.sfo, TITLE_ID == VITOX0001
    bool loaded;        // real module_parent load_module("app0:eboot.bin") -> load_self / relocation / imports
    bool imports_bound; // the real loader bound exactly the 10 expected NIDs, all in the selected set
    bool thread;        // real guest main thread created + started (run_app style)
    bool exited;        // guest called the real sceKernelExitProcess -> process_exit_callback
    bool exit_ok;       // exit code 0
    bool imports_real;  // every invoked NID dispatched to a REAL ImportFn; 0 unknown; missing_nids empty
    bool sentinel;      // ux0:data/VitoxHomebrew/result.txt written by the guest, verified by the host
    bool cleanup;       // threads deleted, unload_module, KernelState::deinit, deinit_mem, ~EmuEnvState
    bool root_safety;   // only VitoxHomebrew (removed again) appeared under ux0\data; app install kept
};

extern "C" VitoxHomebrewResult vitox_homebrew_probe_run(const VitoxCoreHost *host, const wchar_t *install_src, const wchar_t *local_state,
    const wchar_t *dev_root);
