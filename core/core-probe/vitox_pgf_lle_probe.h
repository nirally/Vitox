// Vitox M8C.2 - STAGED REAL LLE PGF PRECHECK (non-visual, user-approved). Vita3K's real sysmodule LLE path for
// SCE_SYSMODULE_PGF against the STAGED firmware tree (firmware-staging\vita-fs, used read-only as a temporary vita_fs
// root; io init is NOT run so nothing is created in it): MANUAL modules mode with lle-modules = [libpgf] only,
// load_sys_module -> load_module(vs0:sys/external/libpgf.suprx) -> REAL decrypt_fself (BCrypt EVP) -> load_self /
// relocation / import binding -> module_start, with the M8B.1 fail-stop trap for every unselected HLE call.
// No VitaGraphs, no promotion, AUTOMATIC mode never used.
#pragma once

#include "vitox_core_probe.h"

struct VitoxPgfLleResult {
    bool staged;          // stage-complete.txt present
    bool policy;          // MANUAL + [libpgf]: PGF is LLE, HTTP/NET are not
    bool decrypt;         // REAL decrypt_fself of libpgf.suprx produced an ELF
    bool loaded;          // load_sys_module(PGF) returned true (module loaded, relocated, module_start ran)
    bool module_start_ok; // module_start returned SCE_KERNEL_START_SUCCESS
    bool no_traps;        // no unselected HLE import was called
    bool no_network;      // no libhttp / SceHttp / SceNet activity
    bool staging_unchanged;
    unsigned imports, unselected_imports, traps;
};

extern "C" VitoxPgfLleResult vitox_pgf_lle_probe_run(const VitoxCoreHost *host, const wchar_t *dev_root, const wchar_t *local_state);
