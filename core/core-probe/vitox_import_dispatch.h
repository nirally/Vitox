// Vitox M3.1 - selected-NID import resolution (see vitox_import_dispatch.cpp).
#pragma once

#include <cstdint>

struct EmuEnvState;
struct CPUState;
using ImportFn = void (*)(EmuEnvState &emuenv, CPUState &cpu, int thread_id);

struct VitoxSelectedImport {
    uint32_t nid;
    const char *name;
    const ImportFn *fn; // the real Vita3K ImportFn object (bridge to the export)
};

const VitoxSelectedImport *vitox_resolve_selected_import(uint32_t nid);

// The REAL LIBRARY_INIT functions of the selected modules (subset of Vita3K init_libraries()).
void vitox_init_selected_libraries(EmuEnvState &emuenv);

// 0.0.10.0 (M5B): observation hook called by the shimmed module_parent.cpp call_import() for every
// guest import call, before dispatch. Unknown (unselected) NIDs are recorded with resolved=false.
void vitox_note_import_call(uint32_t nid, int thread_id, bool resolved);
// M8D: the most recent import NID called by a guest thread (0 = none); shutdown diagnostics only
uint32_t vitox_last_import_nid(int thread_id);
// M7B.6 DIAGNOSTIC: read-only logging of selected guest call arguments (sceGxmDraw / SetVertexStream / SetUniformDataF)
// and the GXM context state they use; called by the same shim right after vitox_note_import_call.
void vitox_observe_import_args(EmuEnvState &emuenv, CPUState &cpu, uint32_t nid);

struct VitoxImportStat {
    uint32_t nid;
    const char *name; // nullptr: not in the selected set
    ImportFn fn;      // the real ImportFn called (nullptr if unknown)
    unsigned calls;
    bool resolved;
};
unsigned vitox_import_stats(VitoxImportStat *out, unsigned max);
void vitox_import_stats_reset();

// M8B.1 DIAGNOSTIC TRAP (runtime-exact import discovery only, never for acceptance): when enabled, a guest call to an
// unselected NID stops every guest thread immediately and returns nothing to the guest (the shimmed call_import()
// calls vitox_import_trap() before upstream's return-0 path; true = trapped).
struct VitoxTrapRecord {
    uint32_t nid, library_nid, pc, lr;
    int thread_id;
    char thread_name[32];
};
void vitox_import_trap_enable(bool on); // also clears the records
bool vitox_import_trap(EmuEnvState &emuenv, CPUState &cpu, uint32_t nid, int thread_id);
unsigned vitox_import_traps(VitoxTrapRecord *out, unsigned max);
// 0.0.24.3 (M9A.4, observability only): optional host checkpoint (durable milestone line + flush of every log), called
// once for the FIRST trap of a run, before the guest threads are stopped. nullptr disables it.
void vitox_import_checkpoint_hook(void (*checkpoint)(const char *line));
