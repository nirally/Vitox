// Vitox: SHA-256 via BCrypt (app partition) for logging file identities (homebrew executable etc.).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>

#include <string>

std::string vitox_sha256_hex(const void *data, size_t size) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    unsigned char digest[32] = {};
    std::string hex;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0) {
        if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
            if (BCryptHashData(hash, static_cast<PUCHAR>(const_cast<void *>(data)), static_cast<ULONG>(size), 0) == 0
                && BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0) {
                static const char d[] = "0123456789ABCDEF";
                for (unsigned char b : digest) {
                    hex += d[b >> 4];
                    hex += d[b & 15];
                }
            }
            BCryptDestroyHash(hash);
        }
        BCryptCloseAlgorithmProvider(alg, 0);
    }
    return hex;
}
