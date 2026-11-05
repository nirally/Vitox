// Vitox M5B: decrypt_fself() restricted to unencrypted SELFs (see vitox_fself.h). The checks are the
// ones in vita3k/packages/src/sce_utils.cpp decrypt_fself()/is_fself_encrypted().
#include "vitox_fself.h"

#include <packages/sce_types.h>
#include <self.h> // external/vita-toolchain/src: SCE_header, segment_info (as sce_utils.cpp)
#include <util/log.h>

std::vector<uint8_t> vitox_decrypt_fself_unencrypted_only(const std::vector<uint8_t> &fself) {
    if (fself.size() < sizeof(SCE_header)) {
        LOG_ERROR("Invalid SELF: buffer too small for SCE_header ({} bytes).", fself.size());
        return {};
    }
    const SCE_header &self_header = *reinterpret_cast<const SCE_header *>(fself.data());
    if (self_header.magic != SCE_MAGIC) {
        LOG_ERROR("Invalid SELF: file is either not a SELF or is still encrypted (unsupported).");
        return {};
    }
    const segment_info *const seg_infos = reinterpret_cast<const segment_info *>(&fself[self_header.section_info_offset]);
    if (seg_infos->encryption != 2) {
        LOG_ERROR("Vitox: encrypted SELF refused - the decryption path (OpenSSL) is not linked in this build");
        return {};
    }
    return fself; // not encrypted: unchanged, exactly as decrypt_fself()
}

// M8C.2 (user-approved): the switch read by the shimmed load_module(); only the staged LLE precheck turns it on
namespace {
bool g_real_decrypt = false;
}
bool vitox_fself_real_decrypt() { return g_real_decrypt; }
void vitox_set_fself_real_decrypt(bool on) { g_real_decrypt = on; }
