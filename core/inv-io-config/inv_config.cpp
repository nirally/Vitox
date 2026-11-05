// Vitox M4.1 inventory (never packaged): construct Config, set run-time fields programmatically,
// save/load a minimal config.yml under LocalState. No CLI11, no CommandLineToArgvW.
#include <config/functions.h>
#include <config/settings.h>
#include <config/state.h>

namespace config {
ExitCode vitox_parse_config(Config &cfg, const fs::path &load_path, const fs::path &root_vita_fs_path); // shimmed config.cpp
}

extern "C" __declspec(dllexport) int vitox_inv_config(const wchar_t *local_state, const wchar_t *dev_files) {
    const fs::path ls(local_state), vita_fs = fs::path(dev_files) / "vita-fs";
    Config cfg;
    cfg.set_vita_fs_path(vita_fs);
    cfg.config_path = ls / "config.yml";
    cfg.cpu_opt = true;
    cfg.pstv_mode = false;
    cfg.log_level = 2;
    if (config::serialize_config(cfg, cfg.config_path) != Success)
        return 1;
    Config back;
    if (config::vitox_parse_config(back, ls / "config.yml", vita_fs) != Success)
        return 2;
    config::set_current_config(back, ls, "VITOX0001"); // settings.cpp: per-app custom config (pugixml) or global copy
    return back.log_level == 2 && back.get_vita_fs_path() == vita_fs ? 0 : 3;
}
