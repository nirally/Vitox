// Vitox M4.2 / M5A - real Vita3K IO + CLI-free config + logging with the Vitox filesystem mapping.
// Calls into vitox_hostio.lib (real io/config/logging sources + out-of-tree Boost.Filesystem)
// through its plain C ABI.
#include "common.h"

#include <vitox_io_probe.h>

namespace vitox {

namespace {

void io_log(const char *line) { log_line(Log::Core, "%s", line); }

} // namespace

void probe_io(const std::wstring &local_state) {
    log_line(Log::Core, "IO/config M5A begin");
    const VitoxCoreHost host{ &io_log, &mem_snapshot };
    const VitoxIoResult r = vitox_io_probe_run(&host, local_state.c_str(), L"D:\\DevelopmentFiles\\Vitox");
    const bool io = r.emuenv && r.io_init && r.io_dirs && r.savedata && r.translation && r.vita_ops && r.io_deinit;
    const bool bfs = r.bfs_ops && r.bfs_compat;
    const bool cfg = r.config_save && r.config_parse && r.config_roundtrip && r.settings;
    const bool log = r.log_init && r.log_verify;
    const bool u3 = r.u3_unit && r.u3_copy;
    const bool ok = io && bfs && cfg && log && u3 && r.root_safety && r.cleanup;
    char s[220];
    snprintf(s, sizeof(s), "M5A %s (log=%d u3_unit=%d u3_copy=%d io=%d bfs=%d config=%d settings=%d roots=%d clean=%d)", ok ? "PASS" : "FAIL", log,
        r.u3_unit, r.u3_copy, io, bfs, r.config_save && r.config_parse && r.config_roundtrip, r.settings, r.root_safety, r.cleanup);
    set_status(&Status::io, s);
    log_line(Log::Core, "IO/config %s", s);
}

} // namespace vitox
