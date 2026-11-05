// Vitox M8C.3: an optional, separate host root for the FIRMWARE devices only (os0, vs0, sa0, pd0), read by the guarded
// substitution of vita3k/io/src/device.cpp construct_emulated_path(). Unset (default) = upstream behaviour: every device
// under vita_fs_path. The staged-firmware probes set it to firmware-staging\vita-fs so a guest app installed in the
// ACTIVE vita-fs (ux0) can use the STAGED firmware without anything being promoted or written into the staging tree.
#pragma once

#include <filesystem>

const std::filesystem::path *vitox_firmware_root(); // nullptr when unset
void vitox_set_firmware_root(const std::filesystem::path *root); // nullptr clears
