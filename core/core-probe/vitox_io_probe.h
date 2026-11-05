// Vitox M4.2 IO/config probe - plain C ABI (all Vita3K/Boost flags stay inside vitox_hostio).
#pragma once

#include "vitox_core_probe.h" // VitoxCoreHost

struct VitoxIoResult {
    bool emuenv;          // EmuEnvState constructed; its own io/cfg members are used
    bool io_init;         // real init(IOState, LocalState\cache, LocalState\log, DevFiles\vita-fs)
    bool io_dirs;         // every expected Vita / cache / log directory exists afterwards
    bool savedata;        // real init_savedata_app_path + init_device_paths
    bool translation;     // real expand_path: ux0:/savedata0: -> expected host paths
    bool vita_ops;        // real io.cpp create_dir/open/write/read/stat/rename/open_dir/read_dir/remove
    bool bfs_ops;         // direct Boost.Filesystem runtime ops on D:\DevelopmentFiles (incl. unique_path)
    bool bfs_compat;      // CreateFileFromAppW substitute exercised (and succeeded) for DevelopmentFiles paths
    bool io_deinit;       // real io_deinit
    bool config_save;     // serialize_config + save_current_config (YAML + per-app XML) in LocalState
    bool config_parse;    // real YAML load path (CLI-free shim) returned Success
    bool config_roundtrip; // every written field read back identically + re-serialized YAML byte-equal
    bool settings;        // settings.cpp set_current_config loaded the per-app XML
    bool root_safety;     // nothing pre-existing removed; new entries only under the two roots
    bool cleanup;         // probe files removed, ~EmuEnvState
    // 0.0.8.1 findings (reported, not M4.2 requirements)
    bool bfs_canonical;   // boost fs::canonical succeeded on D: (clean + dotdot) and LocalState
    bool bfs_relative;    // boost fs::relative (weakly_canonical based) succeeded on D:
    // 0.0.9.0 (M5A)
    bool log_init;        // real logging::init(Root{log=LocalState\log}) == Success
    bool log_verify;      // 6 levels written, async logger shut down, all found in LocalState\log\vita3k.log
    bool u3_unit;         // lexical helper: accepts contained children, rejects sibling/../drive/relative/base
    bool u3_copy;         // REAL copy_path -> copy_directory_contents (U3 shim): byte-equal, contained, cleaned
};

extern "C" VitoxIoResult vitox_io_probe_run(const VitoxCoreHost *host, const wchar_t *local_state, const wchar_t *dev_root);
