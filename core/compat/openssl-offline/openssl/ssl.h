// Vitox M9A.4M (VITOX_UWP only): NOT OpenSSL. Offline TLS stand-in for the exact libssl subset used by
// vita3k/modules/SceSsl/SceSsl.cpp, vita3k/modules/SceHttp/SceHttp.cpp and vita3k/http/src/http.cpp. This build has no
// TLS implementation and no network capability, so nothing here pretends to work:
//   - SSL_library_init / SSL_load_error_strings: library-state no-ops (both are no-op init macros in OpenSSL >= 1.1);
//   - SSL_CTX_new returns NULL ("context creation failed", the documented OpenSSL failure result);
//   - every per-connection call fails the way OpenSSL fails for an unusable object (NULL / -1 / SSL_ERROR_SSL /
//     a non-OK verify result); SSL_free / SSL_CTX_free accept NULL.
// Only on the include path of those three TUs. Any other OpenSSL call fails to compile.
#pragma once

#include <stdint.h>

typedef struct ssl_st SSL;
typedef struct ssl_ctx_st SSL_CTX;
typedef struct ssl_method_st SSL_METHOD;

#define SSL_ERROR_SSL 1
#define SSL_MODE_AUTO_RETRY 0x00000004U
#define X509_V_OK 0
#define X509_V_ERR_UNSPECIFIED 1
#define X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT_LOCALLY 20

int SSL_library_init(void);
void SSL_load_error_strings(void);
const SSL_METHOD *TLS_method(void);

SSL_CTX *SSL_CTX_new(const SSL_METHOD *method);
void SSL_CTX_free(SSL_CTX *ctx);
long SSL_CTX_set_mode(SSL_CTX *ctx, long mode);
uint64_t SSL_CTX_set_options(SSL_CTX *ctx, uint64_t op);
uint64_t SSL_CTX_clear_options(SSL_CTX *ctx, uint64_t op);

SSL *SSL_new(SSL_CTX *ctx);
void SSL_free(SSL *ssl);
SSL_CTX *SSL_get_SSL_CTX(const SSL *ssl);
int SSL_set_fd(SSL *ssl, int fd);
void SSL_set_security_level(SSL *ssl, int level);
int SSL_connect(SSL *ssl);
int SSL_get_error(const SSL *ssl, int ret);
long SSL_get_verify_result(const SSL *ssl);
int SSL_read(SSL *ssl, void *buf, int num);
int SSL_write(SSL *ssl, const void *buf, int num);

// Vitox: how often the offline stand-in was asked for TLS work (context or connection); logged by the launcher
unsigned vitox_ssl_offline_tls_requests();
