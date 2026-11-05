// Vitox 0.0.29.1: NoNpDrm content decryption at install time (user-approved 2026-11-04: "let's do opt 2").
//
// The same steps as upstream Vita3K's decrypt_install_nonpdrm (vita3k/packages/src/pkg.cpp): the dump's license
// (sce_sys/package/work.bin, a NoNpDrm .rif) -> rif2zrif -> psvpfsparser execute(zRIF, src, dst, F00D native) decrypts
// the PFS layer of every file into `dst_dir`. psvpfsparser is used unchanged; only its crypto backend is BCrypt
// (compat/vitox_pfs_crypto_bcrypt.cpp). The executables stay NPDRM SELFs: Vita3K's loader decrypts them at load time
// with the license key (decrypt_fself + emuenv.license, see vitox_homebrew_launcher.cpp).
#pragma once

#include <cstdint>

// progress(user, processed_bytes, total_bytes) returns false to cancel. Returns true on success; on failure `error`
// holds the reason (a cancelled run says "cancelled"). Paths must be ASCII (psvpfsparser takes narrow strings).
extern "C" bool vitox_nonpdrm_decrypt(const wchar_t *src_dir, const wchar_t *dst_dir, const wchar_t *license_file,
    bool (*progress)(void *user, uint64_t processed, uint64_t total), void *user, char *error, unsigned error_size);
