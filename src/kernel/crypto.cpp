#include <stdint.h>
#include <stddef.h>

// Full freestanding FIPS 180-4 SHA-256 & IEEE 802.3 CRC32 implementation for BlankOS

static inline uint32_t rotr32(uint32_t x, uint32_t n) {
    return (x >> n) | (x << (32 - n));
}

static const uint32_t sha256_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

static void sha256_transform(uint32_t state[8], const uint8_t block[64]) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[i * 4] << 24) |
               ((uint32_t)block[i * 4 + 1] << 16) |
               ((uint32_t)block[i * 4 + 2] << 8) |
               ((uint32_t)block[i * 4 + 3]);
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t e = state[4], f = state[5], g = state[6], h = state[7];

    for (int i = 0; i < 64; i++) {
        uint32_t S1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t temp1 = h + S1 + ch + sha256_k[i] + w[i];
        uint32_t S0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temp2 = S0 + maj;

        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

extern "C" {

void sha256_compute(const uint8_t* data, uint64_t len, uint8_t output_hash[32]) {
    uint32_t state[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
    };

    uint64_t offset = 0;
    while (len - offset >= 64) {
        sha256_transform(state, data + offset);
        offset += 64;
    }

    uint8_t pad[128];
    for (int i = 0; i < 128; i++) pad[i] = 0;
    uint64_t rem = len - offset;
    for (uint64_t i = 0; i < rem; i++) pad[i] = data[offset + i];
    pad[rem] = 0x80;

    uint64_t bit_len = len * 8;
    int total_pad = (rem < 56) ? 64 : 128;
    pad[total_pad - 8] = (uint8_t)((bit_len >> 56) & 0xFF);
    pad[total_pad - 7] = (uint8_t)((bit_len >> 48) & 0xFF);
    pad[total_pad - 6] = (uint8_t)((bit_len >> 40) & 0xFF);
    pad[total_pad - 5] = (uint8_t)((bit_len >> 32) & 0xFF);
    pad[total_pad - 4] = (uint8_t)((bit_len >> 24) & 0xFF);
    pad[total_pad - 3] = (uint8_t)((bit_len >> 16) & 0xFF);
    pad[total_pad - 2] = (uint8_t)((bit_len >> 8) & 0xFF);
    pad[total_pad - 1] = (uint8_t)(bit_len & 0xFF);

    sha256_transform(state, pad);
    if (total_pad == 128) {
        sha256_transform(state, pad + 64);
    }

    for (int i = 0; i < 8; i++) {
        output_hash[i * 4]     = (uint8_t)((state[i] >> 24) & 0xFF);
        output_hash[i * 4 + 1] = (uint8_t)((state[i] >> 16) & 0xFF);
        output_hash[i * 4 + 2] = (uint8_t)((state[i] >> 8) & 0xFF);
        output_hash[i * 4 + 3] = (uint8_t)(state[i] & 0xFF);
    }
}

void sha256_hash(char* input_string, uint8_t* output_hash) {
    uint64_t len = 0;
    if (input_string) {
        while (input_string[len]) len++;
    }
    sha256_compute((const uint8_t*)input_string, len, output_hash);
}

uint32_t crc32_compute(const uint8_t* data, uint64_t len) {
    uint32_t crc = 0xFFFFFFFF;
    for (uint64_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            uint32_t mask = -(crc & 1);
            crc = (crc >> 1) ^ (0xEDB88320 & mask);
        }
    }
    return ~crc;
}

int secure_compare(uint8_t* hash1, uint8_t* hash2) {
    int result = 0;
    for (int i = 0; i < 32; i++) {
        result |= hash1[i] ^ hash2[i];
    }
    return result == 0;
}

}
