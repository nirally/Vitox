// Vitox M5B: module_parent.cpp load_module() calls packages decrypt_fself() (sce_utils.cpp, which
// needs OpenSSL). This is the same function restricted to its NON-encrypted path: identical size and
// magic checks, an unencrypted SELF is returned unchanged; an encrypted one is refused loudly
// (empty buffer, as decrypt_fself returns on failure) because the decryption path is not linked.
#pragma once

#include <cstdint>
#include <vector>

std::vector<uint8_t> vitox_decrypt_fself_unencrypted_only(const std::vector<uint8_t> &fself);

// M8C.2: when true, load_module() uses Vita3K's REAL decrypt_fself (encrypted SELFs, e.g. firmware LLE modules) instead
// of the function above. Default false; only the staged LLE precheck enables it.
bool vitox_fself_real_decrypt();
void vitox_set_fself_real_decrypt(bool on);
