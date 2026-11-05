// Vitox M5B - first headless homebrew execution (VITOX0001) through the real Vita3K core loader.
// Calls into vitox_hle.lib (real module_parent load_module/call_import + SceLibKernel/SceIofilemgr/
// SceThreadmgr/SceModulemgr TUs) through its plain C ABI.
#include "common.h"

#include <vitox_homebrew_probe.h>

namespace vitox {

namespace {

void hb_log(const char *line) { log_line(Log::Core, "%s", line); }

} // namespace

void probe_homebrew(const std::wstring &local_state, const std::wstring &install_dir) {
    log_line(Log::Core, "Homebrew M5B begin");
    const VitoxCoreHost host{ &hb_log, &mem_snapshot };
    const std::wstring src = install_dir + L"\\homebrew\\VITOX0001";
    const VitoxHomebrewResult r = vitox_homebrew_probe_run(&host, src.c_str(), local_state.c_str(), L"D:\\DevelopmentFiles\\Vitox");
    const bool ok = r.installed && r.sfo && r.loaded && r.imports_bound && r.thread && r.exited && r.exit_ok && r.imports_real && r.sentinel && r.cleanup
        && r.root_safety;
    char s[220];
    snprintf(s, sizeof(s), "M5B %s (install=%d sfo=%d load=%d bound=%d thread=%d exit=%d code0=%d real=%d sentinel=%d clean=%d roots=%d)", ok ? "PASS" : "FAIL",
        r.installed, r.sfo, r.loaded, r.imports_bound, r.thread, r.exited, r.exit_ok, r.imports_real, r.sentinel, r.cleanup, r.root_safety);
    set_status(&Status::hb, s);
    log_line(Log::Core, "Homebrew %s", s);
}

} // namespace vitox
