#ifndef BLANKOS_TLS_H
#define BLANKOS_TLS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct TlsClientCtx TlsClientCtx;

// Allocate and initialize a freestanding TLS 1.3 client context for the given SNI hostname.
TlsClientCtx* tls_client_create(const char* host, uint64_t seed_ms);
void tls_client_destroy(TlsClientCtx* ctx);

// Build TLS 1.3 ClientHello record (using X25519 + ChaCha20-Poly1305 & AES-128-GCM).
// Returns number of bytes written to out_buf, or -1 on error.
int tls_client_build_hello(TlsClientCtx* ctx, uint8_t* out_buf, int out_max);

// Feed raw TCP bytes received from server.
// - Decrypted HTTP payload bytes are written to out_plain (*out_plain_len).
// - Outgoing TLS handshake records (Client Finished) are written to out_send (*out_send_len).
// Returns:
//   1 = ok / active
//   0 = graceful close_notify alert received
//  -1 = TLS protocol or cryptographic failure (reason written to err_out)
int tls_client_feed_rx(
    TlsClientCtx* ctx,
    const uint8_t* rx_data, int rx_len,
    uint8_t* out_plain, int out_plain_max, int* out_plain_len,
    uint8_t* out_send, int out_send_max, int* out_send_len,
    char* err_out, int err_max
);

// Returns true once handshake is complete and application records can be exchanged.
bool tls_client_is_ready(const TlsClientCtx* ctx);

// Encrypt application data (e.g. HTTP GET request) into a TLS 1.3 ApplicationData record.
// Returns total encrypted record length, or -1 on error.
int tls_client_encrypt_app(
    TlsClientCtx* ctx,
    const uint8_t* plain, int plain_len,
    uint8_t* out_record, int out_max
);

#ifdef __cplusplus
}
#endif

#endif // BLANKOS_TLS_H
