// Vitox M3/M3.1 - Vita3K core link. Calls into vitox_core.lib (real Vita3K mem / cpu / kernel /
// loader / nids / SceRtcUser / SceSysmem sources built out-of-tree for WindowsStore) through its plain C ABI.
#include "common.h"

#include <vitox_core_probe.h>

namespace vitox {

namespace {

void core_log(const char *line) { log_line(Log::Core, "%s", line); }

} // namespace

void probe_core() {
    log_line(Log::Core, "Core M3/M3.1 begin");
    const VitoxCoreHost host{ &core_log, &mem_snapshot };
    const VitoxCoreResult r = vitox_core_probe_run(&host);
    const bool m3 = r.mem && r.cpu_fastmem && r.cpu_page_table && r.kernel;
    // 0.0.7.0: REAL Vita3K SceSysmem memblock exports + one REAL RTC export through a constructed EmuEnvState
    const bool path = r.loader && r.nid_table && r.import_stubs && r.export_resolved && r.export_invoked && r.loader_cleanup;
    const bool sysmem = r.sysmem_init && r.sysmem_alloc && r.sysmem_base && r.sysmem_rw && r.sysmem_free && r.sysmem_post_free;
    const bool m41 = path && sysmem && r.rtc_regression && r.guest_result;
    char s[200];
    snprintf(s, sizeof(s), "M3 %s M4.1-SYSMEM %s (path=%d init=%d alloc=%d base=%d rw=%d free=%d gone=%d rtc=%d result=%d clean=%d)",
        m3 ? "PASS" : "FAIL", m41 ? "PASS" : "FAIL", path, r.sysmem_init, r.sysmem_alloc, r.sysmem_base, r.sysmem_rw, r.sysmem_free,
        r.sysmem_post_free, r.rtc_regression, r.guest_result, r.loader_cleanup);
    set_status(&Status::core, s);
    log_line(Log::Core, "Core %s", s);
}

} // namespace vitox
