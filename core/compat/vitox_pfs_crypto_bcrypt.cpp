// Vitox 0.0.29.1 (VITOX_UWP only): psvpfsparser's ICryptoOperations over Windows BCrypt (bcrypt.lib, app partition).
//
// psvpfsparser (external/psvpfstools, used UNCHANGED by Vita3K's decrypt_install_nonpdrm) reaches OpenSSL only through
// ICryptoOperations, created by CryptoOperationsFactory::create. This TU replaces CryptoOperationsFactory.cpp and
// OpenSSLCryptoOperations.cpp (OpenSSL is not available for WindowsStore); every operation keeps the semantics of
// OpenSSLCryptoOperations.cpp:
//   AES key_size is in BITS and must be 128; CBC returns the next IV in `iv` (last ciphertext block); CTR advances `iv`
//   by size / 16 (128-bit big-endian) after the call; padding is always off; return 0 = success, -1 = failure.
//   HMAC key sizes are in BYTES. aes_cmac = AES-128-CMAC (RFC 4493, BCRYPT_AES_CMAC_ALGORITHM).
// AES-ECB key handles are cached by key value (the PFS engine re-uses a handful of keys for every sector).
#include "CryptoOperationsFactory.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <bcrypt.h>

#include <array>
#include <cstring>
#include <map>
#include <mutex>
#include <stdexcept>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace {

struct Providers {
    BCRYPT_ALG_HANDLE ecb = nullptr, cbc = nullptr, sha1 = nullptr, sha256 = nullptr, hmac1 = nullptr, hmac256 = nullptr, cmac = nullptr;
    Providers() {
        if (BCryptOpenAlgorithmProvider(&ecb, BCRYPT_AES_ALGORITHM, nullptr, 0) == 0)
            BCryptSetProperty(ecb, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_ECB, sizeof(BCRYPT_CHAIN_MODE_ECB), 0);
        if (BCryptOpenAlgorithmProvider(&cbc, BCRYPT_AES_ALGORITHM, nullptr, 0) == 0)
            BCryptSetProperty(cbc, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_CBC, sizeof(BCRYPT_CHAIN_MODE_CBC), 0);
        BCryptOpenAlgorithmProvider(&sha1, BCRYPT_SHA1_ALGORITHM, nullptr, 0);
        BCryptOpenAlgorithmProvider(&sha256, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
        BCryptOpenAlgorithmProvider(&hmac1, BCRYPT_SHA1_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG);
        BCryptOpenAlgorithmProvider(&hmac256, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG);
        BCryptOpenAlgorithmProvider(&cmac, BCRYPT_AES_CMAC_ALGORITHM, nullptr, 0);
    }
};
Providers &prov() {
    static Providers p;
    return p;
}

class BCryptCryptoOperations : public ICryptoOperations {
public:
    ~BCryptCryptoOperations() override {
        for (auto &kv : ecb_keys_)
            BCryptDestroyKey(kv.second);
    }

    int aes_cbc_encrypt(const unsigned char *src, unsigned char *dst, int size, const unsigned char *key, int key_size, unsigned char *iv) const override {
        if (size == 0)
            return 0;
        if (cbc(true, src, dst, size, key, key_size, iv) != 0)
            return -1;
        std::memcpy(iv, dst + size - 0x10, 0x10);
        return 0;
    }
    int aes_cbc_decrypt(const unsigned char *src, unsigned char *dst, int size, const unsigned char *key, int key_size, unsigned char *iv) const override {
        if (size == 0)
            return 0;
        unsigned char new_iv[0x10];
        std::memcpy(new_iv, src + size - 0x10, 0x10);
        if (cbc(false, src, dst, size, key, key_size, iv) != 0)
            return -1;
        std::memcpy(iv, new_iv, 0x10);
        return 0;
    }
    int aes_ctr_encrypt(const unsigned char *src, unsigned char *dst, int size, const unsigned char *key, int key_size, unsigned char *iv) override {
        return aes_ctr(src, dst, size, key, key_size, iv);
    }
    int aes_ctr_decrypt(const unsigned char *src, unsigned char *dst, int size, const unsigned char *key, int key_size, unsigned char *iv) override {
        return aes_ctr(src, dst, size, key, key_size, iv);
    }
    int aes_ecb_encrypt(const unsigned char *src, unsigned char *dst, int size, const unsigned char *key, int key_size) const override {
        return ecb(true, src, dst, size, key, key_size);
    }
    int aes_ecb_decrypt(const unsigned char *src, unsigned char *dst, int size, const unsigned char *key, int key_size) const override {
        return ecb(false, src, dst, size, key, key_size);
    }
    int aes_cmac(const unsigned char *src, unsigned char *dst, int size, const unsigned char *key, int key_size) const override {
        if (key_size != 128 || !prov().cmac)
            return -1;
        return BCryptHash(prov().cmac, const_cast<PUCHAR>(key), 16, const_cast<PUCHAR>(src), static_cast<ULONG>(size), dst, 16) == 0 ? 0 : -1;
    }
    int sha1(const unsigned char *src, unsigned char *dst, int size) const override { return hash(prov().sha1, nullptr, 0, src, dst, size, 20); }
    int sha256(const unsigned char *src, unsigned char *dst, int size) const override { return hash(prov().sha256, nullptr, 0, src, dst, size, 32); }
    int hmac_sha1(const unsigned char *src, unsigned char *dst, int size, const unsigned char *key, int key_size) const override {
        return hash(prov().hmac1, key, key_size, src, dst, size, 20);
    }
    int hmac_sha256(const unsigned char *src, unsigned char *dst, int size, const unsigned char *key, int key_size) const override {
        return hash(prov().hmac256, key, key_size, src, dst, size, 32);
    }

private:
    mutable std::mutex m_;
    mutable std::map<std::array<unsigned char, 16>, BCRYPT_KEY_HANDLE> ecb_keys_;

    static int hash(BCRYPT_ALG_HANDLE alg, const unsigned char *key, int key_size, const unsigned char *src, unsigned char *dst, int size, ULONG out) {
        if (!alg || size < 0)
            return -1;
        static const unsigned char empty = 0;
        return BCryptHash(alg, const_cast<PUCHAR>(key), static_cast<ULONG>(key ? key_size : 0), const_cast<PUCHAR>(size ? src : &empty),
                   static_cast<ULONG>(size), dst, out)
                == 0
            ? 0
            : -1;
    }
    BCRYPT_KEY_HANDLE ecb_key(const unsigned char *key) const {
        std::array<unsigned char, 16> k;
        std::memcpy(k.data(), key, 16);
        auto it = ecb_keys_.find(k);
        if (it != ecb_keys_.end())
            return it->second;
        if (ecb_keys_.size() > 256) {
            for (auto &kv : ecb_keys_)
                BCryptDestroyKey(kv.second);
            ecb_keys_.clear();
        }
        BCRYPT_KEY_HANDLE h = nullptr;
        if (BCryptGenerateSymmetricKey(prov().ecb, &h, nullptr, 0, const_cast<PUCHAR>(key), 16, 0) != 0)
            return nullptr;
        ecb_keys_[k] = h;
        return h;
    }
    int ecb(bool enc, const unsigned char *src, unsigned char *dst, int size, const unsigned char *key, int key_size) const {
        if (key_size != 128 || size < 0 || size % 16)
            return -1;
        if (size == 0)
            return 0;
        std::lock_guard<std::mutex> lock(m_);
        BCRYPT_KEY_HANDLE h = ecb_key(key);
        if (!h)
            return -1;
        ULONG done = 0;
        const NTSTATUS s = enc ? BCryptEncrypt(h, const_cast<PUCHAR>(src), static_cast<ULONG>(size), nullptr, nullptr, 0, dst, static_cast<ULONG>(size), &done, 0)
                               : BCryptDecrypt(h, const_cast<PUCHAR>(src), static_cast<ULONG>(size), nullptr, nullptr, 0, dst, static_cast<ULONG>(size), &done, 0);
        return s == 0 && done == static_cast<ULONG>(size) ? 0 : -1;
    }
    static int cbc(bool enc, const unsigned char *src, unsigned char *dst, int size, const unsigned char *key, int key_size, const unsigned char *iv) {
        if (key_size != 128 || size < 0 || size % 16 || !prov().cbc)
            return -1;
        BCRYPT_KEY_HANDLE h = nullptr;
        if (BCryptGenerateSymmetricKey(prov().cbc, &h, nullptr, 0, const_cast<PUCHAR>(key), 16, 0) != 0)
            return -1;
        unsigned char ivb[16];
        std::memcpy(ivb, iv, 16); // BCrypt chains in place; the caller's IV is updated by the OpenSSL rule above
        std::vector<unsigned char> in(src, src + size); // src and dst may alias
        ULONG done = 0;
        const NTSTATUS s = enc ? BCryptEncrypt(h, in.data(), static_cast<ULONG>(size), nullptr, ivb, 16, dst, static_cast<ULONG>(size), &done, 0)
                               : BCryptDecrypt(h, in.data(), static_cast<ULONG>(size), nullptr, ivb, 16, dst, static_cast<ULONG>(size), &done, 0);
        BCryptDestroyKey(h);
        return s == 0 && done == static_cast<ULONG>(size) ? 0 : -1;
    }
    int aes_ctr(const unsigned char *src, unsigned char *dst, int size, const unsigned char *key, int key_size, unsigned char *iv) const {
        if (key_size != 128 || size < 0)
            return -1;
        // keystream = AES-ECB(counter), counter += 1 per block (128-bit big-endian, as OpenSSL CTR)
        unsigned char ctr[16];
        std::memcpy(ctr, iv, 16);
        std::vector<unsigned char> counters((static_cast<size_t>(size) + 15) / 16 * 16), stream(counters.size());
        for (size_t b = 0; b < counters.size() / 16; ++b) {
            std::memcpy(&counters[b * 16], ctr, 16);
            for (int i = 15; i >= 0 && ++ctr[i] == 0; --i) {
            }
        }
        if (!counters.empty() && ecb(true, counters.data(), stream.data(), static_cast<int>(counters.size()), key, 128) != 0)
            return -1;
        for (int i = 0; i < size; ++i)
            dst[i] = static_cast<unsigned char>(src[i] ^ stream[static_cast<size_t>(i)]);
        uint64_t to_add = static_cast<uint64_t>(size) / 0x10;
        for (int i = 15; i >= 0; i--) {
            to_add += iv[i];
            iv[i] = static_cast<uint8_t>(to_add);
            to_add >>= 8;
        }
        return 0;
    }
};

} // namespace

std::shared_ptr<ICryptoOperations> CryptoOperationsFactory::create(CryptoOperationsTypes type) {
    switch (type) {
    case CryptoOperationsTypes::openssl: // the only type psvpfsparser asks for: served by BCrypt here
        return std::make_shared<BCryptCryptoOperations>();
    default:
        throw std::runtime_error("unexpected CryptoOperationsTypes value");
    }
}
