// Vitox M9A.4M - offline TLS stand-in (see openssl-offline/openssl/ssl.h). No TLS backend, no network I/O.
#include <openssl/ssl.h>

#include <util/log.h>

#include <atomic>

namespace {
std::atomic<unsigned> g_tls_requests{ 0 };

void tls_request(const char *what) {
    ++g_tls_requests;
    LOG_WARN("Vitox offline TLS: {} refused (no TLS implementation in this build)", what);
}

struct ssl_method_st_dummy {
    int unused;
} g_method{};
} // namespace

unsigned vitox_ssl_offline_tls_requests() { return g_tls_requests.load(); }

int SSL_library_init(void) { return 1; }
void SSL_load_error_strings(void) {}
const SSL_METHOD *TLS_method(void) { return reinterpret_cast<const SSL_METHOD *>(&g_method); }

SSL_CTX *SSL_CTX_new(const SSL_METHOD *) {
    tls_request("SSL_CTX_new");
    return nullptr;
}
void SSL_CTX_free(SSL_CTX *) {}
long SSL_CTX_set_mode(SSL_CTX *, long) { return 0; }
uint64_t SSL_CTX_set_options(SSL_CTX *, uint64_t) { return 0; }
uint64_t SSL_CTX_clear_options(SSL_CTX *, uint64_t) { return 0; }

SSL *SSL_new(SSL_CTX *) {
    tls_request("SSL_new");
    return nullptr;
}
void SSL_free(SSL *) {}
SSL_CTX *SSL_get_SSL_CTX(const SSL *) { return nullptr; }
int SSL_set_fd(SSL *, int) { return 0; }
void SSL_set_security_level(SSL *, int) {}
int SSL_connect(SSL *) {
    tls_request("SSL_connect");
    return -1;
}
int SSL_get_error(const SSL *, int) { return SSL_ERROR_SSL; }
long SSL_get_verify_result(const SSL *) { return X509_V_ERR_UNSPECIFIED; }
int SSL_read(SSL *, void *, int) {
    tls_request("SSL_read");
    return -1;
}
int SSL_write(SSL *, const void *, int) {
    tls_request("SSL_write");
    return -1;
}
