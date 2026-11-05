// Vitox M8C.1 (VITOX_UWP only): the 8 EVP functions of Vita3K's PUP installer path over Windows BCrypt (bcrypt.lib,
// app partition). See compat/openssl-shim/openssl/evp.h. Decrypt only; padding is always off in the callers.
//   AES-128-CBC / AES-256-CBC: BCryptDecrypt with BCRYPT_CHAIN_MODE_CBC (the IV buffer is chained in place, as EVP).
//   AES-128-CTR: BCryptEncrypt with BCRYPT_CHAIN_MODE_ECB over counter blocks (OpenSSL: 128-bit big-endian increment),
//                keystream XOR; state is kept across Update calls (keystream position + counter).
#include <openssl/evp.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <bcrypt.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace {
enum class Mode { CBC, CTR };
} // namespace

struct evp_cipher_st {
    Mode mode;
    unsigned key_bytes;
};
struct evp_cipher_ctx_st {
    Mode mode = Mode::CBC;
    BCRYPT_KEY_HANDLE key = nullptr;
    unsigned char iv[16] = {};       // CBC: chaining value; CTR: next counter block
    unsigned char stream[16] = {};   // CTR: current keystream block
    unsigned stream_pos = 16;        // CTR: bytes of `stream` already used
    std::vector<unsigned char> key_object;
};

namespace {
std::atomic<unsigned> g_failures{ 0 };

BCRYPT_ALG_HANDLE alg(Mode m) {
    static BCRYPT_ALG_HANDLE cbc = nullptr, ecb = nullptr;
    static std::atomic<bool> init{ false };
    static SRWLOCK lock = SRWLOCK_INIT;
    if (!init) {
        AcquireSRWLockExclusive(&lock);
        if (!init) {
            if (BCryptOpenAlgorithmProvider(&cbc, BCRYPT_AES_ALGORITHM, nullptr, 0) == 0)
                BCryptSetProperty(cbc, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_CBC, sizeof(BCRYPT_CHAIN_MODE_CBC), 0);
            if (BCryptOpenAlgorithmProvider(&ecb, BCRYPT_AES_ALGORITHM, nullptr, 0) == 0)
                BCryptSetProperty(ecb, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_ECB, sizeof(BCRYPT_CHAIN_MODE_ECB), 0);
            init = true;
        }
        ReleaseSRWLockExclusive(&lock);
    }
    return m == Mode::CBC ? cbc : ecb;
}
void drop_key(EVP_CIPHER_CTX *ctx) {
    if (ctx->key)
        BCryptDestroyKey(ctx->key);
    ctx->key = nullptr;
}
void inc_counter(unsigned char c[16]) {
    for (int i = 15; i >= 0; --i)
        if (++c[i] != 0)
            break;
}
int fail() {
    ++g_failures;
    return 0;
}
} // namespace

EVP_CIPHER_CTX *EVP_CIPHER_CTX_new(void) { return new evp_cipher_ctx_st(); }
void EVP_CIPHER_CTX_free(EVP_CIPHER_CTX *ctx) {
    if (!ctx)
        return;
    drop_key(ctx);
    delete ctx;
}

EVP_CIPHER *EVP_CIPHER_fetch(OSSL_LIB_CTX *, const char *name, const char *) {
    if (!name)
        return nullptr;
    if (std::strcmp(name, "AES-128-CTR") == 0)
        return new evp_cipher_st{ Mode::CTR, 16 };
    if (std::strcmp(name, "AES-128-CBC") == 0)
        return new evp_cipher_st{ Mode::CBC, 16 };
    if (std::strcmp(name, "AES-256-CBC") == 0)
        return new evp_cipher_st{ Mode::CBC, 32 };
    ++g_failures; // fail closed on any other algorithm
    return nullptr;
}
void EVP_CIPHER_free(EVP_CIPHER *c) { delete c; }

int EVP_DecryptInit_ex(EVP_CIPHER_CTX *ctx, const EVP_CIPHER *type, ENGINE *, const unsigned char *key, const unsigned char *iv) {
    if (!ctx || !type || !key || !iv)
        return fail();
    drop_key(ctx);
    ctx->mode = type->mode;
    BCRYPT_ALG_HANDLE a = alg(type->mode);
    if (!a)
        return fail();
    DWORD obj_len = 0, got = 0;
    if (BCryptGetProperty(a, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&obj_len), sizeof(obj_len), &got, 0) != 0)
        return fail();
    ctx->key_object.assign(obj_len, 0);
    if (BCryptGenerateSymmetricKey(a, &ctx->key, ctx->key_object.data(), obj_len, const_cast<PUCHAR>(key), type->key_bytes, 0) != 0)
        return fail();
    std::memcpy(ctx->iv, iv, 16);
    ctx->stream_pos = 16;
    return 1;
}

int EVP_CIPHER_CTX_set_padding(EVP_CIPHER_CTX *ctx, int pad) { return ctx && pad == 0 ? 1 : fail(); } // callers always 0

int EVP_DecryptUpdate(EVP_CIPHER_CTX *ctx, unsigned char *out, int *outl, const unsigned char *in, int inl) {
    if (!ctx || !ctx->key || !out || !outl || (!in && inl) || inl < 0)
        return fail();
    *outl = 0;
    if (ctx->mode == Mode::CBC) {
        if (inl % 16 != 0)
            return fail(); // whole blocks only (no caller passes a partial block)
        ULONG res = 0;
        if (inl && BCryptDecrypt(ctx->key, const_cast<PUCHAR>(in), static_cast<ULONG>(inl), nullptr, ctx->iv, 16, out, static_cast<ULONG>(inl), &res, 0) != 0)
            return fail();
        *outl = inl;
        return 1;
    }
    // CTR: keystream in batches of up to 4096 counter blocks (64 KiB)
    int done = 0;
    while (done < inl && ctx->stream_pos < 16)
        out[done] = in[done] ^ ctx->stream[ctx->stream_pos++], ++done;
    std::vector<unsigned char> ctr, ks;
    while (inl - done >= 16) {
        const int blocks = std::min((inl - done) / 16, 4096);
        ctr.resize(blocks * 16), ks.resize(blocks * 16);
        for (int b = 0; b < blocks; ++b) {
            std::memcpy(&ctr[b * 16], ctx->iv, 16);
            inc_counter(ctx->iv);
        }
        ULONG res = 0;
        if (BCryptEncrypt(ctx->key, ctr.data(), static_cast<ULONG>(ctr.size()), nullptr, nullptr, 0, ks.data(), static_cast<ULONG>(ks.size()), &res, 0) != 0)
            return fail();
        for (int i = 0; i < blocks * 16; ++i)
            out[done + i] = in[done + i] ^ ks[i];
        done += blocks * 16;
    }
    if (done < inl) {
        ULONG res = 0;
        if (BCryptEncrypt(ctx->key, ctx->iv, 16, nullptr, nullptr, 0, ctx->stream, 16, &res, 0) != 0)
            return fail();
        inc_counter(ctx->iv);
        ctx->stream_pos = 0;
        while (done < inl)
            out[done] = in[done] ^ ctx->stream[ctx->stream_pos++], ++done;
    }
    *outl = inl;
    return 1;
}

int EVP_DecryptFinal_ex(EVP_CIPHER_CTX *ctx, unsigned char *, int *outl) {
    if (!ctx || !outl)
        return fail();
    *outl = 0; // padding off: nothing is ever buffered
    return 1;
}

// ---- NIST SP 800-38A known-answer tests (F.2.2 CBC-AES128.Decrypt, F.2.6 CBC-AES256.Decrypt, F.5.2 CTR-AES128.Decrypt)
namespace {
std::vector<unsigned char> hex(const char *s) {
    std::vector<unsigned char> v;
    for (; s[0] && s[1]; s += 2) {
        unsigned x = 0;
        std::sscanf(s, "%2x", &x);
        v.push_back(static_cast<unsigned char>(x));
    }
    return v;
}
bool kat(const char *alg_name, const char *key, const char *iv, const char *ct, const char *pt, int split, std::string &rep) {
    const auto k = hex(key), i = hex(iv), c = hex(ct), p = hex(pt);
    EVP_CIPHER *cipher = EVP_CIPHER_fetch(nullptr, alg_name, nullptr);
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    std::vector<unsigned char> out(c.size());
    int l1 = 0, l2 = 0, l3 = 0;
    bool ok = cipher && EVP_DecryptInit_ex(ctx, cipher, nullptr, k.data(), i.data()) == 1 && EVP_CIPHER_CTX_set_padding(ctx, 0) == 1;
    ok = ok && EVP_DecryptUpdate(ctx, out.data(), &l1, c.data(), split) == 1;
    ok = ok && EVP_DecryptUpdate(ctx, out.data() + split, &l2, c.data() + split, static_cast<int>(c.size()) - split) == 1;
    ok = ok && EVP_DecryptFinal_ex(ctx, out.data() + c.size(), &l3) == 1;
    ok = ok && l1 + l2 + l3 == static_cast<int>(c.size()) && out == p;
    EVP_CIPHER_CTX_free(ctx);
    EVP_CIPHER_free(cipher);
    rep += std::string(alg_name) + (split % 16 ? " (split " + std::to_string(split) + ")" : "") + (ok ? " PASS; " : " FAIL; ");
    return ok;
}
} // namespace

unsigned vitox_evp_failures() { return g_failures.load(); }

bool vitox_evp_selftest(char *report, unsigned report_size) {
    const char *pt = "6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51";
    std::string rep;
    bool ok = true;
    ok &= kat("AES-128-CBC", "2b7e151628aed2a6abf7158809cf4f3c", "000102030405060708090a0b0c0d0e0f",
        "7649abac8119b246cee98e9b12e9197d5086cb9b507219ee95db113a917678b2", pt, 16, rep);
    ok &= kat("AES-256-CBC", "603deb1015ca71be2b73aef0857d77811f352c073b6108d72d9810a30914dff4", "000102030405060708090a0b0c0d0e0f",
        "f58c4c04d6e5f1ba779eabfb5f7bfbd69cfc4e967edb808d679f777bc6702c7d", pt, 16, rep);
    // CTR counter f0..ff -> block 2 carries into byte 14 (128-bit big-endian increment)
    ok &= kat("AES-128-CTR", "2b7e151628aed2a6abf7158809cf4f3c", "f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff",
        "874d6191b620e3261bef6864990db6ce9806f66b7970fdff8617187bb9fffdff", pt, 16, rep);
    ok &= kat("AES-128-CTR", "2b7e151628aed2a6abf7158809cf4f3c", "f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff",
        "874d6191b620e3261bef6864990db6ce9806f66b7970fdff8617187bb9fffdff", pt, 7, rep); // partial-block keystream carry-over
    if (report && report_size)
        std::snprintf(report, report_size, "%s", rep.c_str());
    return ok;
}
