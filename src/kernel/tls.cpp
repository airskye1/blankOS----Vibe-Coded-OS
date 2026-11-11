#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "tls.h"

extern "C" {
    extern void sha256_compute(const uint8_t* data, uint64_t len, uint8_t output_hash[32]);
    extern void* uefi_malloc(unsigned long long size);
    extern void uefi_free(void* ptr);
}

// ============================================================================
// Freestanding Byte / Timing Helpers
// ============================================================================

static void tls_zero(void* dst, size_t n) {
    uint8_t* p = (uint8_t*)dst;
    for (size_t i = 0; i < n; i++) p[i] = 0;
}

static void tls_copy(void* dst, const void* src, size_t n) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
}

static void tls_move(void* dst, const void* src, size_t n) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    if (d < s) {
        for (size_t i = 0; i < n; i++) d[i] = s[i];
    } else if (d > s) {
        for (size_t i = n; i > 0; i--) d[i - 1] = s[i - 1];
    }
}

static int tls_slen(const char* s) {
    int n = 0;
    while (s && s[n]) n++;
    return n;
}

static void tls_scpy(char* dst, const char* src, int max) {
    if (!dst || max <= 0) return;
    int i = 0;
    while (src && src[i] && i < max - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static bool tls_ct_eq(const uint8_t* a, const uint8_t* b, size_t n) {
    uint8_t diff = 0;
    for (size_t i = 0; i < n; i++) diff |= (uint8_t)(a[i] ^ b[i]);
    return diff == 0;
}

static uint64_t tls_rdtsc(void) {
    uint32_t lo = 0, hi = 0;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static inline uint32_t rotl32(uint32_t x, int n) {
    return (x << n) | (x >> (32 - n));
}

static inline uint32_t load_le32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline void store_le32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static inline uint32_t load_be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static inline uint16_t load_be16(const uint8_t* p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static inline void store_be32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)((v >> 24) & 0xFF);
    p[1] = (uint8_t)((v >> 16) & 0xFF);
    p[2] = (uint8_t)((v >> 8) & 0xFF);
    p[3] = (uint8_t)(v & 0xFF);
}

static inline void store_be64(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[7 - i] = (uint8_t)((v >> (i * 8)) & 0xFF);
}

// ============================================================================
// Incremental SHA-256 (Supports Cloned Transcript Digests Without Mutation)
// ============================================================================

typedef struct {
    uint32_t state[8];
    uint8_t  buf[64];
    uint64_t total_len;
} TlsSha256;

static const uint32_t g_sha256_k[64] = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U
};

static inline uint32_t rotr32_s(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

static void sha256_blk(uint32_t state[8], const uint8_t block[64]) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) w[i] = load_be32(block + i * 4);
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rotr32_s(w[i - 15], 7) ^ rotr32_s(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr32_s(w[i - 2], 17) ^ rotr32_s(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = rotr32_s(e, 6) ^ rotr32_s(e, 11) ^ rotr32_s(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t t1 = h + S1 + ch + g_sha256_k[i] + w[i];
        uint32_t S0 = rotr32_s(a, 2) ^ rotr32_s(a, 13) ^ rotr32_s(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

static void tls_sha_init(TlsSha256* s) {
    s->state[0] = 0x6a09e667U; s->state[1] = 0xbb67ae85U;
    s->state[2] = 0x3c6ef372U; s->state[3] = 0xa54ff53aU;
    s->state[4] = 0x510e527fU; s->state[5] = 0x9b05688cU;
    s->state[6] = 0x1f83d9abU; s->state[7] = 0x5be0cd19U;
    s->total_len = 0;
}

static void tls_sha_update(TlsSha256* s, const uint8_t* data, size_t len) {
    if (!data || len == 0) return;
    size_t idx = (size_t)(s->total_len & 63U);
    s->total_len += len;
    size_t i = 0;
    if (idx > 0 && idx + len >= 64) {
        tls_copy(s->buf + idx, data, 64 - idx);
        sha256_blk(s->state, s->buf);
        i = 64 - idx;
        idx = 0;
    }
    while (i + 64 <= len) {
        sha256_blk(s->state, data + i);
        i += 64;
    }
    if (i < len) {
        tls_copy(s->buf + idx, data + i, len - i);
    }
}

static void tls_sha_peek(const TlsSha256* src, uint8_t out[32]) {
    TlsSha256 s = *src;
    size_t idx = (size_t)(s.total_len & 63U);
    s.buf[idx++] = 0x80;
    if (idx > 56) {
        while (idx < 64) s.buf[idx++] = 0;
        sha256_blk(s.state, s.buf);
        idx = 0;
    }
    while (idx < 56) s.buf[idx++] = 0;
    store_be64(s.buf + 56, s.total_len * 8ULL);
    sha256_blk(s.state, s.buf);
    for (int i = 0; i < 8; i++) store_be32(out + i * 4, s.state[i]);
}

// ============================================================================
// HMAC-SHA256 & TLS 1.3 HKDF Key Schedule (RFC 8446)
// ============================================================================

static void hmac_sha256(const uint8_t* key, int key_len, const uint8_t* data, int data_len, uint8_t out[32]) {
    uint8_t k0[64];
    tls_zero(k0, 64);
    if (key_len > 64) {
        sha256_compute(key, (uint64_t)key_len, k0);
    } else if (key_len > 0) {
        tls_copy(k0, key, (size_t)key_len);
    }

    uint8_t ipad[64], opad[64];
    for (int i = 0; i < 64; i++) {
        ipad[i] = (uint8_t)(k0[i] ^ 0x36);
        opad[i] = (uint8_t)(k0[i] ^ 0x5c);
    }

    TlsSha256 inner;
    tls_sha_init(&inner);
    tls_sha_update(&inner, ipad, 64);
    if (data && data_len > 0) tls_sha_update(&inner, data, (size_t)data_len);
    uint8_t inner_hash[32];
    tls_sha_peek(&inner, inner_hash);

    TlsSha256 outer;
    tls_sha_init(&outer);
    tls_sha_update(&outer, opad, 64);
    tls_sha_update(&outer, inner_hash, 32);
    tls_sha_peek(&outer, out);
}

static void hkdf_extract(const uint8_t* salt, int salt_len, const uint8_t* ikm, int ikm_len, uint8_t prk[32]) {
    uint8_t zero_salt[32];
    if (!salt || salt_len <= 0) {
        tls_zero(zero_salt, 32);
        salt = zero_salt;
        salt_len = 32;
    }
    hmac_sha256(salt, salt_len, ikm, ikm_len, prk);
}

static void hkdf_expand_label(const uint8_t secret[32], const char* label, const uint8_t* ctx, int ctx_len, uint8_t* out, int out_len) {
    // HkdfLabel = { uint16 length; opaque label<7..255> = "tls13 " + label; opaque context<0..255> }
    uint8_t info[320];
    int lbl_len = tls_slen(label);
    int full_lbl_len = 6 + lbl_len;
    int p = 0;
    info[p++] = (uint8_t)((out_len >> 8) & 0xFF);
    info[p++] = (uint8_t)(out_len & 0xFF);
    info[p++] = (uint8_t)full_lbl_len;
    const char* pfx = "tls13 ";
    for (int i = 0; i < 6; i++) info[p++] = (uint8_t)pfx[i];
    for (int i = 0; i < lbl_len; i++) info[p++] = (uint8_t)label[i];
    info[p++] = (uint8_t)ctx_len;
    if (ctx && ctx_len > 0) {
        tls_copy(info + p, ctx, (size_t)ctx_len);
        p += ctx_len;
    }

    uint8_t T[32];
    int generated = 0;
    uint8_t counter = 1;
    while (generated < out_len) {
        uint8_t msg[356];
        int mp = 0;
        if (counter > 1) {
            tls_copy(msg, T, 32);
            mp += 32;
        }
        tls_copy(msg + mp, info, (size_t)p);
        mp += p;
        msg[mp++] = counter;
        hmac_sha256(secret, 32, msg, mp, T);
        int take = out_len - generated;
        if (take > 32) take = 32;
        tls_copy(out + generated, T, (size_t)take);
        generated += take;
        counter++;
    }
}

static void derive_secret(const uint8_t secret[32], const char* label, const uint8_t* transcript_hash, uint8_t out[32]) {
    uint8_t empty_hash[32];
    if (!transcript_hash) {
        sha256_compute((const uint8_t*)"", 0, empty_hash);
        transcript_hash = empty_hash;
    }
    hkdf_expand_label(secret, label, transcript_hash, 32, out, 32);
}

// ============================================================================
// X25519 Key Exchange (RFC 7748, Compact Constant-Time 16-Limb Field)
// ============================================================================

typedef int64_t gf[16];

static void gf_car(gf o) {
    int64_t c;
    for (int i = 0; i < 16; i++) {
        o[i] += (1LL << 16);
        c = o[i] >> 16;
        o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
        o[i] -= c * (1LL << 16);
    }
}

static void gf_sel(gf p, gf q, int b) {
    int64_t t, c = ~(b - 1);
    for (int i = 0; i < 16; i++) {
        t = c & (p[i] ^ q[i]);
        p[i] ^= t;
        q[i] ^= t;
    }
}

static void gf_pack(uint8_t* o, const gf n) {
    gf m, t;
    for (int i = 0; i < 16; i++) t[i] = n[i];
    gf_car(t);
    gf_car(t);
    gf_car(t);
    for (int j = 0; j < 2; j++) {
        m[0] = t[0] - 0xffed;
        for (int i = 1; i < 15; i++) {
            m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
            m[i - 1] &= 0xffff;
        }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        int b = (int)((m[15] >> 16) & 1);
        m[14] &= 0xffff;
        gf_sel(t, m, 1 - b);
    }
    for (int i = 0; i < 16; i++) {
        o[2 * i] = (uint8_t)(t[i] & 0xff);
        o[2 * i + 1] = (uint8_t)(t[i] >> 8);
    }
}

static void gf_unpack(gf o, const uint8_t* n) {
    for (int i = 0; i < 16; i++) o[i] = (int64_t)n[2 * i] + ((int64_t)n[2 * i + 1] << 8);
    o[15] &= 0x7fff;
}

static void gf_A(gf o, const gf a, const gf b) {
    for (int i = 0; i < 16; i++) o[i] = a[i] + b[i];
}

static void gf_Z(gf o, const gf a, const gf b) {
    for (int i = 0; i < 16; i++) o[i] = a[i] - b[i];
}

static void gf_M(gf o, const gf a, const gf b) {
    int64_t t[31];
    for (int i = 0; i < 31; i++) t[i] = 0;
    for (int i = 0; i < 16; i++) {
        for (int j = 0; j < 16; j++) t[i + j] += a[i] * b[j];
    }
    for (int i = 0; i < 15; i++) t[i] += 38 * t[i + 16];
    for (int i = 0; i < 16; i++) o[i] = t[i];
    gf_car(o);
    gf_car(o);
}

static void gf_S(gf o, const gf a) { gf_M(o, a, a); }

static void gf_inv(gf o, const gf i) {
    gf c;
    for (int a = 0; a < 16; a++) c[a] = i[a];
    for (int a = 253; a >= 0; a--) {
        gf_S(c, c);
        if (a != 2 && a != 4) gf_M(c, c, i);
    }
    for (int a = 0; a < 16; a++) o[a] = c[a];
}

static void x25519_scalarmult(uint8_t* q, const uint8_t* n, const uint8_t* p) {
    uint8_t z[32];
    int64_t x[80], r;
    gf a, b, c, d, e, f;
    for (int i = 0; i < 31; i++) z[i] = n[i];
    z[31] = (uint8_t)((n[31] & 127) | 64);
    z[0] &= 248;
    gf_unpack(x, p);
    for (int i = 0; i < 16; i++) {
        b[i] = x[i];
        d[i] = a[i] = c[i] = 0;
    }
    a[0] = d[0] = 1;
    for (int i = 254; i >= 0; --i) {
        r = (z[i >> 3] >> (i & 7)) & 1;
        gf_sel(a, b, (int)r);
        gf_sel(c, d, (int)r);
        gf_A(e, a, c);
        gf_Z(a, a, c);
        gf_A(c, b, d);
        gf_Z(b, b, d);
        gf_S(d, e);
        gf_S(f, a);
        gf_M(a, c, a);
        gf_M(c, b, e);
        gf_A(e, a, c);
        gf_Z(a, a, c);
        gf_S(b, a);
        gf_Z(c, d, f);
        gf A121665 = {0xdb41, 1};
        gf_M(a, c, A121665);
        gf_A(a, a, d);
        gf_M(c, c, a);
        gf_M(a, d, f);
        gf_M(d, b, x);
        gf_S(b, e);
        gf_sel(a, b, (int)r);
        gf_sel(c, d, (int)r);
    }
    for (int i = 0; i < 16; i++) {
        x[i + 16] = a[i];
        x[i + 32] = c[i];
        x[i + 48] = b[i];
        x[i + 64] = d[i];
    }
    gf_inv(x + 32, x + 32);
    gf_M(x + 16, x + 16, x + 32);
    gf_pack(q, x + 16);
}

static const uint8_t X25519_BASEPOINT[32] = { 9 };

// ============================================================================
// ChaCha20 + Poly1305 AEAD (RFC 8439)
// ============================================================================

#define QR(a, b, c, d) \
    a += b; d ^= a; d = rotl32(d, 16); \
    c += d; b ^= c; b = rotl32(b, 12); \
    a += b; d ^= a; d = rotl32(d, 8);  \
    c += d; b ^= c; b = rotl32(b, 7);

static void chacha20_block(const uint8_t key[32], uint32_t counter, const uint8_t nonce[12], uint8_t out[64]) {
    uint32_t s[16];
    s[0] = 0x61707865U; s[1] = 0x3320646eU; s[2] = 0x79622d32U; s[3] = 0x6b206574U;
    for (int i = 0; i < 8; i++) s[4 + i] = load_le32(key + i * 4);
    s[12] = counter;
    s[13] = load_le32(nonce);
    s[14] = load_le32(nonce + 4);
    s[15] = load_le32(nonce + 8);

    uint32_t x[16];
    for (int i = 0; i < 16; i++) x[i] = s[i];
    for (int i = 0; i < 10; i++) {
        QR(x[0], x[4], x[8],  x[12]);
        QR(x[1], x[5], x[9],  x[13]);
        QR(x[2], x[6], x[10], x[14]);
        QR(x[3], x[7], x[11], x[15]);
        QR(x[0], x[5], x[10], x[15]);
        QR(x[1], x[6], x[11], x[12]);
        QR(x[2], x[7], x[8],  x[13]);
        QR(x[3], x[4], x[9],  x[14]);
    }
    for (int i = 0; i < 16; i++) store_le32(out + i * 4, x[i] + s[i]);
}

static void chacha20_xor(const uint8_t key[32], uint32_t counter, const uint8_t nonce[12], const uint8_t* in, uint8_t* out, int len) {
    uint8_t blk[64];
    int off = 0;
    while (off < len) {
        chacha20_block(key, counter++, nonce, blk);
        int take = len - off;
        if (take > 64) take = 64;
        for (int i = 0; i < take; i++) out[off + i] = (uint8_t)(in[off + i] ^ blk[i]);
        off += take;
    }
}

typedef struct {
    uint32_t r[5];
    uint32_t h[5];
    uint32_t pad[4];
} Poly1305Ctx;

static void poly1305_init(Poly1305Ctx* ctx, const uint8_t key[32]) {
    uint32_t t0 = load_le32(key + 0);
    uint32_t t1 = load_le32(key + 4);
    uint32_t t2 = load_le32(key + 8);
    uint32_t t3 = load_le32(key + 12);
    t0 &= 0x0fffffffU; t1 &= 0x0ffffffcU; t2 &= 0x0ffffffcU; t3 &= 0x0ffffffcU;
    ctx->r[0] = t0 & 0x03ffffffU;
    ctx->r[1] = ((t0 >> 26) | (t1 << 6)) & 0x03ffffffU;
    ctx->r[2] = ((t1 >> 20) | (t2 << 12)) & 0x03ffffffU;
    ctx->r[3] = ((t2 >> 14) | (t3 << 18)) & 0x03ffffffU;
    ctx->r[4] = (t3 >> 8) & 0x03ffffffU;
    for (int i = 0; i < 5; i++) ctx->h[i] = 0;
    for (int i = 0; i < 4; i++) ctx->pad[i] = load_le32(key + 16 + i * 4);
}

static void poly1305_block(Poly1305Ctx* ctx, const uint8_t* m, int len, bool is_final) {
    uint8_t buf[17];
    for (int i = 0; i < 17; i++) buf[i] = 0;
    for (int i = 0; i < len && i < 16; i++) buf[i] = m[i];
    buf[len] = 1;
    uint32_t hibit = is_final ? (len == 16 ? (1U << 24) : 0U) : (1U << 24);

    uint32_t t0 = load_le32(buf + 0);
    uint32_t t1 = load_le32(buf + 4);
    uint32_t t2 = load_le32(buf + 8);
    uint32_t t3 = load_le32(buf + 12);

    ctx->h[0] += t0 & 0x03ffffffU;
    ctx->h[1] += ((t0 >> 26) | (t1 << 6)) & 0x03ffffffU;
    ctx->h[2] += ((t1 >> 20) | (t2 << 12)) & 0x03ffffffU;
    ctx->h[3] += ((t2 >> 14) | (t3 << 18)) & 0x03ffffffU;
    ctx->h[4] += (t3 >> 8) | hibit;

    uint64_t r0 = ctx->r[0], r1 = ctx->r[1], r2 = ctx->r[2], r3 = ctx->r[3], r4 = ctx->r[4];
    uint64_t s1 = r1 * 5ULL, s2 = r2 * 5ULL, s3 = r3 * 5ULL, s4 = r4 * 5ULL;
    uint64_t h0 = ctx->h[0], h1 = ctx->h[1], h2 = ctx->h[2], h3 = ctx->h[3], h4 = ctx->h[4];

    uint64_t d0 = h0 * r0 + h1 * s4 + h2 * s3 + h3 * s2 + h4 * s1;
    uint64_t d1 = h0 * r1 + h1 * r0 + h2 * s4 + h3 * s3 + h4 * s2;
    uint64_t d2 = h0 * r2 + h1 * r1 + h2 * r0 + h3 * s4 + h4 * s3;
    uint64_t d3 = h0 * r3 + h1 * r2 + h2 * r1 + h3 * r0 + h4 * s4;
    uint64_t d4 = h0 * r4 + h1 * r3 + h2 * r2 + h3 * r1 + h4 * r0;

    uint64_t c;
    c = d0 >> 26; ctx->h[0] = (uint32_t)(d0 & 0x3ffffffU); d1 += c;
    c = d1 >> 26; ctx->h[1] = (uint32_t)(d1 & 0x3ffffffU); d2 += c;
    c = d2 >> 26; ctx->h[2] = (uint32_t)(d2 & 0x3ffffffU); d3 += c;
    c = d3 >> 26; ctx->h[3] = (uint32_t)(d3 & 0x3ffffffU); d4 += c;
    c = d4 >> 26; ctx->h[4] = (uint32_t)(d4 & 0x3ffffffU);
    ctx->h[0] += (uint32_t)(c * 5ULL);
    c = ctx->h[0] >> 26; ctx->h[0] &= 0x3ffffffU; ctx->h[1] += (uint32_t)c;
}

static void poly1305_update(Poly1305Ctx* ctx, const uint8_t* data, int len) {
    int off = 0;
    while (off + 16 <= len) {
        poly1305_block(ctx, data + off, 16, false);
        off += 16;
    }
    if (off < len) poly1305_block(ctx, data + off, len - off, true);
}

static void poly1305_finish(Poly1305Ctx* ctx, uint8_t mac[16]) {
    uint32_t c;
    c = ctx->h[1] >> 26; ctx->h[1] &= 0x3ffffffU; ctx->h[2] += c;
    c = ctx->h[2] >> 26; ctx->h[2] &= 0x3ffffffU; ctx->h[3] += c;
    c = ctx->h[3] >> 26; ctx->h[3] &= 0x3ffffffU; ctx->h[4] += c;
    c = ctx->h[4] >> 26; ctx->h[4] &= 0x3ffffffU; ctx->h[0] += c * 5U;
    c = ctx->h[0] >> 26; ctx->h[0] &= 0x3ffffffU; ctx->h[1] += c;

    uint32_t g[5];
    g[0] = ctx->h[0] + 5U; c = g[0] >> 26; g[0] &= 0x3ffffffU;
    g[1] = ctx->h[1] + c;  c = g[1] >> 26; g[1] &= 0x3ffffffU;
    g[2] = ctx->h[2] + c;  c = g[2] >> 26; g[2] &= 0x3ffffffU;
    g[3] = ctx->h[3] + c;  c = g[3] >> 26; g[3] &= 0x3ffffffU;
    g[4] = ctx->h[4] + c - (1U << 26);

    uint32_t mask = (g[4] >> 31) - 1U;
    for (int i = 0; i < 5; i++) ctx->h[i] = (ctx->h[i] & ~mask) | (g[i] & mask);

    uint32_t h0 = ctx->h[0], h1 = ctx->h[1], h2 = ctx->h[2], h3 = ctx->h[3], h4 = ctx->h[4];
    uint64_t f;
    uint32_t w0 = (h0) | (h1 << 26);
    uint32_t w1 = (h1 >> 6) | (h2 << 20);
    uint32_t w2 = (h2 >> 12) | (h3 << 14);
    uint32_t w3 = (h3 >> 18) | (h4 << 8);

    f = (uint64_t)w0 + ctx->pad[0];                w0 = (uint32_t)f;
    f = (uint64_t)w1 + ctx->pad[1] + (f >> 32);    w1 = (uint32_t)f;
    f = (uint64_t)w2 + ctx->pad[2] + (f >> 32);    w2 = (uint32_t)f;
    f = (uint64_t)w3 + ctx->pad[3] + (f >> 32);    w3 = (uint32_t)f;
    store_le32(mac + 0, w0);
    store_le32(mac + 4, w1);
    store_le32(mac + 8, w2);
    store_le32(mac + 12, w3);
}

static void poly1305_aead_tag(const uint8_t key[32], const uint8_t nonce[12], const uint8_t* aad, int aad_len, const uint8_t* ct, int ct_len, uint8_t tag[16]) {
    uint8_t otk[64];
    chacha20_block(key, 0, nonce, otk);
    Poly1305Ctx p;
    poly1305_init(&p, otk);
    uint8_t zeros[16] = {0};
    if (aad_len > 0) {
        poly1305_update(&p, aad, aad_len);
        int rem = aad_len & 15;
        if (rem) poly1305_update(&p, zeros, 16 - rem);
    }
    if (ct_len > 0) {
        poly1305_update(&p, ct, ct_len);
        int rem = ct_len & 15;
        if (rem) poly1305_update(&p, zeros, 16 - rem);
    }
    uint8_t lens[16];
    for (int i = 0; i < 8; i++) lens[i] = (uint8_t)(((uint64_t)aad_len >> (i * 8)) & 0xFF);
    for (int i = 0; i < 8; i++) lens[8 + i] = (uint8_t)(((uint64_t)ct_len >> (i * 8)) & 0xFF);
    poly1305_update(&p, lens, 16);
    poly1305_finish(&p, tag);
}

// ============================================================================
// AES-128-GCM (FIPS 197 Forward Cipher + NIST GF(2^128) GHASH)
// ============================================================================

static const uint8_t g_aes_sbox[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16
};

static uint8_t aes_xtime(uint8_t x) { return (uint8_t)((x << 1) ^ (((x >> 7) & 1) * 0x1b)); }

static void aes128_key_expand(const uint8_t key[16], uint8_t rk[176]) {
    static const uint8_t rcon[11] = {0x00,0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1b,0x36};
    tls_copy(rk, key, 16);
    for (int i = 4; i < 44; i++) {
        uint8_t t[4];
        for (int k = 0; k < 4; k++) t[k] = rk[(i - 1) * 4 + k];
        if (i % 4 == 0) {
            uint8_t u = t[0];
            t[0] = (uint8_t)(g_aes_sbox[t[1]] ^ rcon[i / 4]);
            t[1] = g_aes_sbox[t[2]];
            t[2] = g_aes_sbox[t[3]];
            t[3] = g_aes_sbox[u];
        }
        for (int k = 0; k < 4; k++) rk[i * 4 + k] = (uint8_t)(rk[(i - 4) * 4 + k] ^ t[k]);
    }
}

static void aes128_encrypt_block(const uint8_t rk[176], const uint8_t in[16], uint8_t out[16]) {
    uint8_t s[16];
    for (int i = 0; i < 16; i++) s[i] = (uint8_t)(in[i] ^ rk[i]);
    for (int rnd = 1; rnd <= 10; rnd++) {
        for (int i = 0; i < 16; i++) s[i] = g_aes_sbox[s[i]];
        uint8_t t;
        t = s[1]; s[1] = s[5]; s[5] = s[9]; s[9] = s[13]; s[13] = t;
        t = s[2]; s[2] = s[10]; s[10] = t; t = s[6]; s[6] = s[14]; s[14] = t;
        t = s[15]; s[15] = s[11]; s[11] = s[7]; s[7] = s[3]; s[3] = t;
        if (rnd < 10) {
            for (int c = 0; c < 4; c++) {
                uint8_t* a = s + c * 4;
                uint8_t Tmp = (uint8_t)(a[0] ^ a[1] ^ a[2] ^ a[3]);
                uint8_t Tm, a0 = a[0];
                Tm = (uint8_t)(a[0] ^ a[1]); Tm = aes_xtime(Tm); a[0] ^= (uint8_t)(Tm ^ Tmp);
                Tm = (uint8_t)(a[1] ^ a[2]); Tm = aes_xtime(Tm); a[1] ^= (uint8_t)(Tm ^ Tmp);
                Tm = (uint8_t)(a[2] ^ a[3]); Tm = aes_xtime(Tm); a[2] ^= (uint8_t)(Tm ^ Tmp);
                Tm = (uint8_t)(a[3] ^ a0);   Tm = aes_xtime(Tm); a[3] ^= (uint8_t)(Tm ^ Tmp);
            }
        }
        for (int i = 0; i < 16; i++) s[i] ^= rk[rnd * 16 + i];
    }
    tls_copy(out, s, 16);
}

static void ghash_gf_mul(const uint8_t X[16], const uint8_t Y[16], uint8_t Z[16]) {
    uint8_t V[16];
    tls_zero(Z, 16);
    tls_copy(V, Y, 16);
    for (int i = 0; i < 128; i++) {
        if ((X[i >> 3] >> (7 - (i & 7))) & 1) {
            for (int k = 0; k < 16; k++) Z[k] ^= V[k];
        }
        uint8_t lsb = (uint8_t)(V[15] & 1);
        for (int k = 15; k > 0; k--) V[k] = (uint8_t)((V[k] >> 1) | ((V[k - 1] & 1) << 7));
        V[0] >>= 1;
        if (lsb) V[0] ^= 0xe1;
    }
}

static void aes_gcm_ghash(const uint8_t H[16], const uint8_t* aad, int aad_len, const uint8_t* ct, int ct_len, uint8_t out[16]) {
    uint8_t Y[16], blk[16], tmp[16];
    tls_zero(Y, 16);
    auto step = [&](const uint8_t* b, int blen) {
        tls_zero(blk, 16);
        for (int i = 0; i < blen && i < 16; i++) blk[i] = b[i];
        for (int i = 0; i < 16; i++) Y[i] ^= blk[i];
        ghash_gf_mul(Y, H, tmp);
        tls_copy(Y, tmp, 16);
    };
    int off = 0;
    while (off < aad_len) {
        int take = aad_len - off; if (take > 16) take = 16;
        step(aad + off, take);
        off += take;
    }
    off = 0;
    while (off < ct_len) {
        int take = ct_len - off; if (take > 16) take = 16;
        step(ct + off, take);
        off += take;
    }
    uint8_t lens[16];
    store_be64(lens, (uint64_t)aad_len * 8ULL);
    store_be64(lens + 8, (uint64_t)ct_len * 8ULL);
    step(lens, 16);
    tls_copy(out, Y, 16);
}

static void aes128_gcm_crypt(const uint8_t key[16], const uint8_t nonce[12], const uint8_t* aad, int aad_len,
                             const uint8_t* in, uint8_t* out, int len, uint8_t tag[16], bool encrypt) {
    uint8_t rk[176];
    aes128_key_expand(key, rk);

    uint8_t zero[16], H[16], j0[16], s_blk[16], ctr_blk[16], ks[16];
    tls_zero(zero, 16);
    aes128_encrypt_block(rk, zero, H);

    tls_copy(j0, nonce, 12);
    j0[12] = 0; j0[13] = 0; j0[14] = 0; j0[15] = 1;
    aes128_encrypt_block(rk, j0, s_blk);

    if (!encrypt) {
        aes_gcm_ghash(H, aad, aad_len, in, len, tag);
        for (int i = 0; i < 16; i++) tag[i] ^= s_blk[i];
    }

    uint32_t ctr = 2;
    int off = 0;
    while (off < len) {
        tls_copy(ctr_blk, nonce, 12);
        store_be32(ctr_blk + 12, ctr++);
        aes128_encrypt_block(rk, ctr_blk, ks);
        int take = len - off; if (take > 16) take = 16;
        for (int i = 0; i < take; i++) out[off + i] = (uint8_t)(in[off + i] ^ ks[i]);
        off += take;
    }

    if (encrypt) {
        aes_gcm_ghash(H, aad, aad_len, out, len, tag);
        for (int i = 0; i < 16; i++) tag[i] ^= s_blk[i];
    }
}

// ============================================================================
// TLS 1.3 Record AEAD Wrappers
// ============================================================================

#define CIPHER_AES_128_GCM_SHA256       0x1301
#define CIPHER_CHACHA20_POLY1305_SHA256 0x1303

static void build_tls13_nonce(const uint8_t iv[12], uint64_t seq, uint8_t out_nonce[12]) {
    tls_copy(out_nonce, iv, 12);
    uint8_t seq_bytes[12];
    tls_zero(seq_bytes, 12);
    store_be64(seq_bytes + 4, seq);
    for (int i = 0; i < 12; i++) out_nonce[i] ^= seq_bytes[i];
}

static bool tls13_aead_open(uint16_t cipher, const uint8_t* key, const uint8_t iv[12], uint64_t seq,
                            const uint8_t* aad, int aad_len, const uint8_t* ct_and_tag, int total_len, uint8_t* plain_out) {
    if (total_len < 16) return false;
    int ct_len = total_len - 16;
    const uint8_t* tag = ct_and_tag + ct_len;
    uint8_t nonce[12];
    build_tls13_nonce(iv, seq, nonce);

    uint8_t calc_tag[16];
    if (cipher == CIPHER_CHACHA20_POLY1305_SHA256) {
        poly1305_aead_tag(key, nonce, aad, aad_len, ct_and_tag, ct_len, calc_tag);
        if (!tls_ct_eq(tag, calc_tag, 16)) return false;
        chacha20_xor(key, 1, nonce, ct_and_tag, plain_out, ct_len);
        return true;
    } else if (cipher == CIPHER_AES_128_GCM_SHA256) {
        aes128_gcm_crypt(key, nonce, aad, aad_len, ct_and_tag, plain_out, ct_len, calc_tag, false);
        return tls_ct_eq(tag, calc_tag, 16);
    }
    return false;
}

static void tls13_aead_seal(uint16_t cipher, const uint8_t* key, const uint8_t iv[12], uint64_t seq,
                            const uint8_t* aad, int aad_len, const uint8_t* plain, int plain_len, uint8_t* ct_out_and_tag) {
    uint8_t nonce[12];
    build_tls13_nonce(iv, seq, nonce);
    if (cipher == CIPHER_CHACHA20_POLY1305_SHA256) {
        chacha20_xor(key, 1, nonce, plain, ct_out_and_tag, plain_len);
        poly1305_aead_tag(key, nonce, aad, aad_len, ct_out_and_tag, plain_len, ct_out_and_tag + plain_len);
    } else {
        aes128_gcm_crypt(key, nonce, aad, aad_len, plain, ct_out_and_tag, plain_len, ct_out_and_tag + plain_len, true);
    }
}

// ============================================================================
// TLS 1.3 Client State Machine
// ============================================================================

typedef enum {
    TLS_INIT = 0,
    TLS_WAIT_SERVER_HELLO = 1,
    TLS_WAIT_ENCRYPTED_HS = 2,
    TLS_APP_READY = 3,
    TLS_CLOSED = 4,
    TLS_FAILED = 5
} TlsPhase;

struct TlsClientCtx {
    TlsPhase phase;
    char host[128];
    uint16_t cipher;
    int key_len;

    uint8_t client_priv[32];
    uint8_t client_pub[32];
    uint8_t server_pub[32];

    TlsSha256 transcript;
    uint8_t handshake_secret[32];
    uint8_t client_hs_secret[32];
    uint8_t server_hs_secret[32];
    uint8_t client_hs_key[32];
    uint8_t client_hs_iv[12];
    uint8_t server_hs_key[32];
    uint8_t server_hs_iv[12];
    uint64_t client_hs_seq;
    uint64_t server_hs_seq;

    uint8_t client_app_key[32];
    uint8_t client_app_iv[12];
    uint8_t server_app_key[32];
    uint8_t server_app_iv[12];
    uint64_t client_app_seq;
    uint64_t server_app_seq;

    // TCP record stream reassembly buffer
    uint8_t* rec_buf;
    int rec_len;
    int rec_cap;

    // Handshake message reassembly buffer
    uint8_t* hs_buf;
    int hs_len;
    int hs_cap;
};

static void derive_traffic_keys(const uint8_t secret[32], int key_len, uint8_t key_out[32], uint8_t iv_out[12]) {
    hkdf_expand_label(secret, "key", NULL, 0, key_out, key_len);
    hkdf_expand_label(secret, "iv", NULL, 0, iv_out, 12);
}

static bool tls_ensure_cap(uint8_t** buf, int* cap, int need) {
    if (*buf && need <= *cap) return true;
    int ncap = (*cap > 0) ? (*cap * 2) : 16384;
    while (ncap < need) ncap *= 2;
    if (ncap > 131072) return false;
    uint8_t* nb = (uint8_t*)uefi_malloc((unsigned long long)ncap);
    if (!nb) return false;
    if (*buf && *cap > 0) {
        tls_copy(nb, *buf, (size_t)*cap);
        uefi_free(*buf);
    }
    *buf = nb;
    *cap = ncap;
    return true;
}

extern "C" {

TlsClientCtx* tls_client_create(const char* host, uint64_t seed_ms) {
    TlsClientCtx* ctx = (TlsClientCtx*)uefi_malloc(sizeof(TlsClientCtx));
    if (!ctx) return NULL;
    tls_zero(ctx, sizeof(TlsClientCtx));
    tls_scpy(ctx->host, host ? host : "", sizeof(ctx->host));
    ctx->phase = TLS_INIT;
    tls_sha_init(&ctx->transcript);

    ctx->rec_cap = 32768;
    ctx->rec_buf = (uint8_t*)uefi_malloc((unsigned long long)ctx->rec_cap);
    ctx->hs_cap = 32768;
    ctx->hs_buf = (uint8_t*)uefi_malloc((unsigned long long)ctx->hs_cap);
    if (!ctx->rec_buf || !ctx->hs_buf) {
        tls_client_destroy(ctx);
        return NULL;
    }

    // Generate ephemeral X25519 keypair from RDTSC + host + millisecond clock
    uint8_t ent[48];
    uint64_t t1 = tls_rdtsc();
    store_be64(ent + 0, t1);
    store_be64(ent + 8, seed_ms);
    store_be64(ent + 16, (uint64_t)(uintptr_t)ctx);
    for (int i = 0; i < 24; i++) ent[24 + i] = (uint8_t)((ctx->host[i % 16]) ^ (uint8_t)(t1 >> (i & 15)));
    sha256_compute(ent, sizeof(ent), ctx->client_priv);
    x25519_scalarmult(ctx->client_pub, ctx->client_priv, X25519_BASEPOINT);
    return ctx;
}

void tls_client_destroy(TlsClientCtx* ctx) {
    if (!ctx) return;
    if (ctx->rec_buf) uefi_free(ctx->rec_buf);
    if (ctx->hs_buf) uefi_free(ctx->hs_buf);
    tls_zero(ctx, sizeof(TlsClientCtx));
    uefi_free(ctx);
}

bool tls_client_is_ready(const TlsClientCtx* ctx) {
    return ctx && ctx->phase == TLS_APP_READY;
}

int tls_client_build_hello(TlsClientCtx* ctx, uint8_t* out_buf, int out_max) {
    if (!ctx || !out_buf || out_max < 300) return -1;

    uint8_t body[384];
    int p = 0;

    // legacy_version = 0x0303 (TLS 1.2)
    body[p++] = 0x03; body[p++] = 0x03;

    // 32 bytes client random
    uint8_t rseed[40];
    store_be64(rseed, tls_rdtsc());
    tls_copy(rseed + 8, ctx->client_pub, 32);
    sha256_compute(rseed, sizeof(rseed), body + p);
    p += 32;

    // legacy_session_id = empty (0)
    body[p++] = 0x00;

    // cipher_suites (4 bytes): TLS_CHACHA20_POLY1305_SHA256 (0x1303), TLS_AES_128_GCM_SHA256 (0x1301)
    body[p++] = 0x00; body[p++] = 0x04;
    body[p++] = 0x13; body[p++] = 0x03;
    body[p++] = 0x13; body[p++] = 0x01;

    // legacy_compression_methods: 1 byte [0x00]
    body[p++] = 0x01; body[p++] = 0x00;

    // Extensions length placeholder
    int ext_len_pos = p;
    p += 2;
    int ext_start = p;

    // 1. server_name (0x0000)
    int hlen = tls_slen(ctx->host);
    if (hlen > 0 && hlen < 120) {
        int sni_ext_len = hlen + 5;
        body[p++] = 0x00; body[p++] = 0x00;
        body[p++] = (uint8_t)(sni_ext_len >> 8); body[p++] = (uint8_t)(sni_ext_len & 0xFF);
        body[p++] = (uint8_t)((hlen + 3) >> 8); body[p++] = (uint8_t)((hlen + 3) & 0xFF);
        body[p++] = 0x00; // host_name
        body[p++] = (uint8_t)(hlen >> 8); body[p++] = (uint8_t)(hlen & 0xFF);
        tls_copy(body + p, ctx->host, (size_t)hlen);
        p += hlen;
    }

    // 2. supported_groups (0x000A): x25519 (0x001D)
    body[p++] = 0x00; body[p++] = 0x0A;
    body[p++] = 0x00; body[p++] = 0x04;
    body[p++] = 0x00; body[p++] = 0x02;
    body[p++] = 0x00; body[p++] = 0x1D;

    // 3. signature_algorithms (0x000D)
    body[p++] = 0x00; body[p++] = 0x0D;
    body[p++] = 0x00; body[p++] = 0x0C;
    body[p++] = 0x00; body[p++] = 0x0A;
    body[p++] = 0x04; body[p++] = 0x03; // ecdsa_secp256r1_sha256
    body[p++] = 0x08; body[p++] = 0x04; // rsa_pss_rsae_sha256
    body[p++] = 0x04; body[p++] = 0x01; // rsa_pkcs1_sha256
    body[p++] = 0x05; body[p++] = 0x03; // ecdsa_secp384r1_sha384
    body[p++] = 0x08; body[p++] = 0x05; // rsa_pss_rsae_sha384

    // 4. supported_versions (0x002B): TLS 1.3 (0x0304)
    body[p++] = 0x00; body[p++] = 0x2B;
    body[p++] = 0x00; body[p++] = 0x03;
    body[p++] = 0x02;
    body[p++] = 0x03; body[p++] = 0x04;

    // 5. psk_key_exchange_modes (0x002D): psk_dhe_ke (1)
    body[p++] = 0x00; body[p++] = 0x2D;
    body[p++] = 0x00; body[p++] = 0x02;
    body[p++] = 0x01; body[p++] = 0x01;

    // 6. key_share (0x0033): x25519 (32 bytes pubkey)
    body[p++] = 0x00; body[p++] = 0x33;
    body[p++] = 0x00; body[p++] = 0x26; // 38
    body[p++] = 0x00; body[p++] = 0x24; // 36
    body[p++] = 0x00; body[p++] = 0x1D; // x25519
    body[p++] = 0x00; body[p++] = 0x20; // 32
    tls_copy(body + p, ctx->client_pub, 32);
    p += 32;

    int ext_total = p - ext_start;
    body[ext_len_pos] = (uint8_t)(ext_total >> 8);
    body[ext_len_pos + 1] = (uint8_t)(ext_total & 0xFF);

    // Wrap in Handshake message (type=1 ClientHello) + TLS record (type=0x16, ver=0x0301)
    int hs_total = 4 + p;
    out_buf[0] = 0x16; out_buf[1] = 0x03; out_buf[2] = 0x01;
    out_buf[3] = (uint8_t)(hs_total >> 8);
    out_buf[4] = (uint8_t)(hs_total & 0xFF);

    out_buf[5] = 0x01; // ClientHello
    out_buf[6] = (uint8_t)((p >> 16) & 0xFF);
    out_buf[7] = (uint8_t)((p >> 8) & 0xFF);
    out_buf[8] = (uint8_t)(p & 0xFF);
    tls_copy(out_buf + 9, body, (size_t)p);

    tls_sha_init(&ctx->transcript);
    tls_sha_update(&ctx->transcript, out_buf + 5, (size_t)hs_total);
    ctx->phase = TLS_WAIT_SERVER_HELLO;
    return 5 + hs_total;
}

static bool handle_server_hello(TlsClientCtx* ctx, const uint8_t* msg, int msg_len, char* err_out, int err_max) {
    if (msg_len < 38) {
        tls_scpy(err_out, "ServerHello truncated", err_max);
        return false;
    }
    // Random bytes: check for HelloRetryRequest magic SHA-256("HelloRetryRequest")
    static const uint8_t hrr_magic[8] = {0xCF,0x21,0xAD,0x74,0xE5,0x9A,0x61,0x11};
    if (tls_ct_eq(msg + 2, hrr_magic, 8)) {
        tls_scpy(err_out, "Server sent HelloRetryRequest", err_max);
        return false;
    }

    int pos = 2 + 32;
    int sid_len = msg[pos++];
    if (pos + sid_len + 3 > msg_len) {
        tls_scpy(err_out, "ServerHello malformed", err_max);
        return false;
    }
    pos += sid_len;

    ctx->cipher = load_be16(msg + pos);
    pos += 2;
    pos += 1; // compression method

    if (ctx->cipher != CIPHER_CHACHA20_POLY1305_SHA256 && ctx->cipher != CIPHER_AES_128_GCM_SHA256) {
        tls_scpy(err_out, "Unsupported TLS 1.3 cipher", err_max);
        return false;
    }
    ctx->key_len = (ctx->cipher == CIPHER_CHACHA20_POLY1305_SHA256) ? 32 : 16;

    if (pos + 2 > msg_len) {
        tls_scpy(err_out, "Missing ServerHello extensions", err_max);
        return false;
    }
    int ext_len = load_be16(msg + pos); pos += 2;
    if (pos + ext_len > msg_len) {
        tls_scpy(err_out, "ServerHello extensions truncated", err_max);
        return false;
    }

    bool got_tls13 = false;
    bool got_key = false;
    int end_ext = pos + ext_len;
    while (pos + 4 <= end_ext) {
        uint16_t etype = load_be16(msg + pos);
        int elen = load_be16(msg + pos + 2);
        pos += 4;
        if (pos + elen > end_ext) break;
        if (etype == 0x002B && elen >= 2) {
            if (load_be16(msg + pos) == 0x0304) got_tls13 = true;
        } else if (etype == 0x0033 && elen >= 4) {
            uint16_t grp = load_be16(msg + pos);
            uint16_t klen = load_be16(msg + pos + 2);
            if (grp == 0x001D && klen == 32 && pos + 4 + 32 <= end_ext) {
                tls_copy(ctx->server_pub, msg + pos + 4, 32);
                got_key = true;
            }
        }
        pos += elen;
    }

    if (!got_tls13 || !got_key) {
        tls_scpy(err_out, "Server rejected TLS 1.3 X25519", err_max);
        return false;
    }

    // Derive Handshake Keys per RFC 8446 Section 7.1
    uint8_t shared[32], empty_hash[32], zeros[32];
    tls_zero(zeros, 32);
    x25519_scalarmult(shared, ctx->client_priv, ctx->server_pub);

    uint8_t early_secret[32], derived[32];
    hkdf_extract(zeros, 32, zeros, 32, early_secret);
    sha256_compute((const uint8_t*)"", 0, empty_hash);
    derive_secret(early_secret, "derived", empty_hash, derived);
    hkdf_extract(derived, 32, shared, 32, ctx->handshake_secret);

    uint8_t th_sh[32];
    tls_sha_peek(&ctx->transcript, th_sh);
    derive_secret(ctx->handshake_secret, "c hs traffic", th_sh, ctx->client_hs_secret);
    derive_secret(ctx->handshake_secret, "s hs traffic", th_sh, ctx->server_hs_secret);

    derive_traffic_keys(ctx->client_hs_secret, ctx->key_len, ctx->client_hs_key, ctx->client_hs_iv);
    derive_traffic_keys(ctx->server_hs_secret, ctx->key_len, ctx->server_hs_key, ctx->server_hs_iv);
    ctx->client_hs_seq = 0;
    ctx->server_hs_seq = 0;
    ctx->phase = TLS_WAIT_ENCRYPTED_HS;
    return true;
}

int tls_client_encrypt_app(TlsClientCtx* ctx, const uint8_t* plain, int plain_len, uint8_t* out_record, int out_max) {
    if (!ctx || ctx->phase != TLS_APP_READY || !plain || plain_len <= 0) return -1;
    int inner_len = plain_len + 1;
    int ct_len = inner_len + 16;
    if (5 + ct_len > out_max) return -1;

    uint8_t* tmp_plain = (uint8_t*)uefi_malloc((unsigned long long)inner_len);
    if (!tmp_plain) return -1;
    tls_copy(tmp_plain, plain, (size_t)plain_len);
    tmp_plain[plain_len] = 0x17; // Application Data

    out_record[0] = 0x17;
    out_record[1] = 0x03;
    out_record[2] = 0x03;
    out_record[3] = (uint8_t)(ct_len >> 8);
    out_record[4] = (uint8_t)(ct_len & 0xFF);

    tls13_aead_seal(ctx->cipher, ctx->client_app_key, ctx->client_app_iv, ctx->client_app_seq++,
                    out_record, 5, tmp_plain, inner_len, out_record + 5);
    uefi_free(tmp_plain);
    return 5 + ct_len;
}

int tls_client_feed_rx(
    TlsClientCtx* ctx,
    const uint8_t* rx_data, int rx_len,
    uint8_t* out_plain, int out_plain_max, int* out_plain_len,
    uint8_t* out_send, int out_send_max, int* out_send_len,
    char* err_out, int err_max
) {
    if (out_plain_len) *out_plain_len = 0;
    if (out_send_len) *out_send_len = 0;
    if (!ctx || ctx->phase == TLS_FAILED) return -1;
    if (ctx->phase == TLS_CLOSED) return 0;

    if (rx_data && rx_len > 0) {
        if (!tls_ensure_cap(&ctx->rec_buf, &ctx->rec_cap, ctx->rec_len + rx_len)) {
            tls_scpy(err_out, "TLS RX buffer overflow", err_max);
            ctx->phase = TLS_FAILED;
            return -1;
        }
        tls_copy(ctx->rec_buf + ctx->rec_len, rx_data, (size_t)rx_len);
        ctx->rec_len += rx_len;
    }

    int off = 0;
    while (off + 5 <= ctx->rec_len) {
        uint8_t rtype = ctx->rec_buf[off];
        int rlen = load_be16(ctx->rec_buf + off + 3);
        if (rlen > 18432) {
            tls_scpy(err_out, "TLS record too large", err_max);
            ctx->phase = TLS_FAILED;
            return -1;
        }
        if (off + 5 + rlen > ctx->rec_len) break;

        const uint8_t* hdr = ctx->rec_buf + off;
        const uint8_t* payload = hdr + 5;
        off += 5 + rlen;

        // Ignore legacy middlebox ChangeCipherSpec (0x14)
        if (rtype == 0x14) continue;

        if (rtype == 0x15) {
            // Unencrypted alert
            if (rlen >= 2 && payload[0] == 1 && payload[1] == 0) {
                ctx->phase = TLS_CLOSED;
                break;
            }
            tls_scpy(err_out, "TLS fatal alert from server", err_max);
            ctx->phase = TLS_FAILED;
            return -1;
        }

        if (rtype == 0x16 && ctx->phase == TLS_WAIT_SERVER_HELLO) {
            if (rlen < 4 || payload[0] != 0x02) {
                tls_scpy(err_out, "Expected ServerHello", err_max);
                ctx->phase = TLS_FAILED;
                return -1;
            }
            int sh_len = (payload[1] << 16) | (payload[2] << 8) | payload[3];
            if (4 + sh_len > rlen) {
                tls_scpy(err_out, "ServerHello truncated", err_max);
                ctx->phase = TLS_FAILED;
                return -1;
            }
            tls_sha_update(&ctx->transcript, payload, (size_t)(4 + sh_len));
            if (!handle_server_hello(ctx, payload + 4, sh_len, err_out, err_max)) {
                ctx->phase = TLS_FAILED;
                return -1;
            }
            continue;
        }

        if (rtype == 0x17) {
            if (rlen < 17) {
                tls_scpy(err_out, "Encrypted TLS record too short", err_max);
                ctx->phase = TLS_FAILED;
                return -1;
            }
            uint8_t* dec = (uint8_t*)uefi_malloc((unsigned long long)rlen);
            if (!dec) {
                tls_scpy(err_out, "TLS OOM", err_max);
                ctx->phase = TLS_FAILED;
                return -1;
            }

            bool is_hs = (ctx->phase == TLS_WAIT_ENCRYPTED_HS);
            bool ok = tls13_aead_open(
                ctx->cipher,
                is_hs ? ctx->server_hs_key : ctx->server_app_key,
                is_hs ? ctx->server_hs_iv : ctx->server_app_iv,
                is_hs ? ctx->server_hs_seq++ : ctx->server_app_seq++,
                hdr, 5, payload, rlen, dec
            );
            if (!ok) {
                uefi_free(dec);
                tls_scpy(err_out, "TLS AEAD tag verification failed", err_max);
                ctx->phase = TLS_FAILED;
                return -1;
            }

            int p_end = rlen - 16;
            while (p_end > 0 && dec[p_end - 1] == 0) p_end--;
            if (p_end <= 0) {
                uefi_free(dec);
                continue;
            }
            uint8_t inner_type = dec[p_end - 1];
            int inner_len = p_end - 1;

            if (inner_type == 0x15) {
                bool is_close_notify = (inner_len >= 2 && dec[0] == 1 && dec[1] == 0);
                uefi_free(dec);
                if (is_close_notify) {
                    ctx->phase = TLS_CLOSED;
                    break;
                }
                tls_scpy(err_out, "TLS encrypted alert", err_max);
                ctx->phase = TLS_FAILED;
                return -1;
            }

            if (inner_type == 0x17 && ctx->phase == TLS_APP_READY) {
                if (out_plain && out_plain_len && inner_len > 0) {
                    int space = out_plain_max - *out_plain_len;
                    int cp = (inner_len < space) ? inner_len : space;
                    if (cp > 0) {
                        tls_copy(out_plain + *out_plain_len, dec, (size_t)cp);
                        *out_plain_len += cp;
                    }
                }
                uefi_free(dec);
                continue;
            }

            if (inner_type == 0x16) {
                if (is_hs) {
                    if (!tls_ensure_cap(&ctx->hs_buf, &ctx->hs_cap, ctx->hs_len + inner_len)) {
                        uefi_free(dec);
                        tls_scpy(err_out, "TLS HS buffer overflow", err_max);
                        ctx->phase = TLS_FAILED;
                        return -1;
                    }
                    tls_copy(ctx->hs_buf + ctx->hs_len, dec, (size_t)inner_len);
                    ctx->hs_len += inner_len;
                    uefi_free(dec);

                    // Parse reassembled handshake messages in order
                    int hoff = 0;
                    while (hoff + 4 <= ctx->hs_len && ctx->phase == TLS_WAIT_ENCRYPTED_HS) {
                        uint8_t htype = ctx->hs_buf[hoff];
                        int hlen = (ctx->hs_buf[hoff + 1] << 16) | (ctx->hs_buf[hoff + 2] << 8) | ctx->hs_buf[hoff + 3];
                        if (hoff + 4 + hlen > ctx->hs_len) break;

                        if (htype == 20) {
                            // Server Finished: verify HMAC before updating transcript!
                            if (hlen != 32) {
                                tls_scpy(err_out, "Invalid Server Finished length", err_max);
                                ctx->phase = TLS_FAILED;
                                return -1;
                            }
                            uint8_t fk[32], th_pre[32], exp_fin[32];
                            hkdf_expand_label(ctx->server_hs_secret, "finished", NULL, 0, fk, 32);
                            tls_sha_peek(&ctx->transcript, th_pre);
                            hmac_sha256(fk, 32, th_pre, 32, exp_fin);
                            if (!tls_ct_eq(exp_fin, ctx->hs_buf + hoff + 4, 32)) {
                                tls_scpy(err_out, "Server Finished MAC mismatch", err_max);
                                ctx->phase = TLS_FAILED;
                                return -1;
                            }
                            tls_sha_update(&ctx->transcript, ctx->hs_buf + hoff, (size_t)(4 + hlen));
                            hoff += 4 + hlen;

                            // Derive Application Traffic Keys from Transcript-Hash(CH..SF)
                            uint8_t zeros[32], derived[32], master[32], th_sf[32];
                            uint8_t c_app_sec[32], s_app_sec[32];
                            tls_zero(zeros, 32);
                            derive_secret(ctx->handshake_secret, "derived", NULL, derived);
                            hkdf_extract(derived, 32, zeros, 32, master);
                            tls_sha_peek(&ctx->transcript, th_sf);
                            derive_secret(master, "c ap traffic", th_sf, c_app_sec);
                            derive_secret(master, "s ap traffic", th_sf, s_app_sec);
                            derive_traffic_keys(c_app_sec, ctx->key_len, ctx->client_app_key, ctx->client_app_iv);
                            derive_traffic_keys(s_app_sec, ctx->key_len, ctx->server_app_key, ctx->server_app_iv);
                            ctx->client_app_seq = 0;
                            ctx->server_app_seq = 0;

                            // Build Client Finished record (encrypted with client_hs_key)
                            uint8_t cfk[32], c_fin_plain[37];
                            hkdf_expand_label(ctx->client_hs_secret, "finished", NULL, 0, cfk, 32);
                            c_fin_plain[0] = 20;
                            c_fin_plain[1] = 0; c_fin_plain[2] = 0; c_fin_plain[3] = 32;
                            hmac_sha256(cfk, 32, th_sf, 32, c_fin_plain + 4);
                            c_fin_plain[36] = 0x16; // Handshake inner type

                            if (out_send && out_send_len && *out_send_len + 5 + 37 + 16 <= out_send_max) {
                                uint8_t* rec = out_send + *out_send_len;
                                int ct_l = 37 + 16;
                                rec[0] = 0x17; rec[1] = 0x03; rec[2] = 0x03;
                                rec[3] = (uint8_t)(ct_l >> 8);
                                rec[4] = (uint8_t)(ct_l & 0xFF);
                                tls13_aead_seal(ctx->cipher, ctx->client_hs_key, ctx->client_hs_iv, ctx->client_hs_seq++,
                                                rec, 5, c_fin_plain, 37, rec + 5);
                                *out_send_len += 5 + ct_l;
                            }
                            ctx->phase = TLS_APP_READY;
                        } else {
                            // EncryptedExtensions (8), Certificate (11), CertificateVerify (15)
                            tls_sha_update(&ctx->transcript, ctx->hs_buf + hoff, (size_t)(4 + hlen));
                            hoff += 4 + hlen;
                        }
                    }
                    if (hoff > 0) {
                        int rem = ctx->hs_len - hoff;
                        if (rem > 0) tls_move(ctx->hs_buf, ctx->hs_buf + hoff, (size_t)rem);
                        ctx->hs_len = rem;
                    }
                    continue;
                } else {
                    // Post-handshake NewSessionTicket etc. -> safely ignore
                    uefi_free(dec);
                    continue;
                }
            }
            uefi_free(dec);
        }
    }

    if (off > 0) {
        int rem = ctx->rec_len - off;
        if (rem > 0) tls_move(ctx->rec_buf, ctx->rec_buf + off, (size_t)rem);
        ctx->rec_len = rem;
    }

    if (ctx->phase == TLS_CLOSED) return 0;
    return 1;
}

} // extern "C"
