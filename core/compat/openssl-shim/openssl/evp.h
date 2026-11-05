// Vitox M8C.1 (VITOX_UWP only): NOT OpenSSL. The exact EVP subset Vita3K's PUP installer path uses
// (vita3k/packages/src/pup.cpp decrypt_segments + sce_utils.cpp get_segments/decrypt_fself), implemented over Windows
// BCrypt in compat/vitox_evp_bcrypt.cpp. Only on the include path of the firmware-staging library; any other OpenSSL
// call fails to compile. Supported ciphers: "AES-128-CTR", "AES-128-CBC", "AES-256-CBC", decrypt only, no padding
// (every caller sets padding 0). Semantics follow OpenSSL 3: CBC processes whole blocks only (a partial block makes
// Update fail here instead of being buffered - no caller passes one), CTR any length with a 128-bit big-endian counter.
#pragma once

typedef struct evp_cipher_st EVP_CIPHER;
typedef struct evp_cipher_ctx_st EVP_CIPHER_CTX;
typedef struct engine_st ENGINE;
typedef struct ossl_lib_ctx_st OSSL_LIB_CTX;

EVP_CIPHER_CTX *EVP_CIPHER_CTX_new(void);
void EVP_CIPHER_CTX_free(EVP_CIPHER_CTX *ctx);
EVP_CIPHER *EVP_CIPHER_fetch(OSSL_LIB_CTX *libctx, const char *algorithm, const char *properties);
void EVP_CIPHER_free(EVP_CIPHER *cipher);
int EVP_DecryptInit_ex(EVP_CIPHER_CTX *ctx, const EVP_CIPHER *type, ENGINE *impl, const unsigned char *key, const unsigned char *iv);
int EVP_CIPHER_CTX_set_padding(EVP_CIPHER_CTX *ctx, int pad);
int EVP_DecryptUpdate(EVP_CIPHER_CTX *ctx, unsigned char *out, int *outl, const unsigned char *in, int inl);
int EVP_DecryptFinal_ex(EVP_CIPHER_CTX *ctx, unsigned char *outm, int *outl);

// Vitox: NIST SP 800-38A known-answer self-test of the three modes through the functions above (true = all match)
bool vitox_evp_selftest(char *report, unsigned report_size);
unsigned vitox_evp_failures(); // calls that returned failure (any non-zero value fails the staging probe)
