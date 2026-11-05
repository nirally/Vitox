// Vitox M4.1 inventory (never packaged): the minimum real IOState init path with explicit,
// Xbox-safe roots. LocalState: cache/log (and later config); DevelopmentFiles: the Vita FS tree.
#include <io/functions.h>
#include <io/state.h>

extern "C" __declspec(dllexport) bool vitox_inv_io(const wchar_t *local_state, const wchar_t *dev_files) {
    IOState io;
    const fs::path ls(local_state), vita_fs = fs::path(dev_files) / "vita-fs";
    bool ok = init(io, ls / "cache", ls / "log", vita_fs, false);
    io.user_id = "00";
    io.savedata = "VITOX0001";
    io.app_path = "VITOX0001";
    ok &= init_savedata_app_path(io, vita_fs);
    init_device_paths(io);
    io_deinit(io);
    return ok;
}
