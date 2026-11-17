/* SPDX-License-Identifier: MIT */
/* SHA-256 (FIPS 180-4) for the G0 GPU host tests: the stand-in for aura_sha256(). */
#ifndef WINDOWS_GPU_SHA256_H
#define WINDOWS_GPU_SHA256_H

#include <string.h>

static const u32 K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
    0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
    0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
    0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
    0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
    0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
    0xc67178f2};
#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha_block(u32 h[8], const u8 *p)
{
    u32 w[64], a[8];
    for (int i = 0; i < 16; i++)
        w[i] = (u32)p[4 * i] << 24 | (u32)p[4 * i + 1] << 16 | (u32)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++)
        w[i] = w[i - 16] + (ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3)) + w[i - 7] +
               (ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10));
    memcpy(a, h, sizeof(a));
    for (int i = 0; i < 64; i++) {
        u32 t1 = a[7] + (ROR(a[4], 6) ^ ROR(a[4], 11) ^ ROR(a[4], 25)) +
                 ((a[4] & a[5]) ^ (~a[4] & a[6])) + K[i] + w[i];
        u32 t2 = (ROR(a[0], 2) ^ ROR(a[0], 13) ^ ROR(a[0], 22)) +
                 ((a[0] & a[1]) ^ (a[0] & a[2]) ^ (a[1] & a[2]));
        memmove(a + 1, a, 7 * sizeof(u32));
        a[4] += t1;
        a[0] = t1 + t2;
    }
    for (int i = 0; i < 8; i++)
        h[i] += a[i];
}

static void host_sha256(const void *data, size_t size, u8 digest[32])
{
    u32 h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    const u8 *p = data;
    size_t left = size;
    u8 tail[128] = {0};
    while (left >= 64) {
        sha_block(h, p);
        p += 64;
        left -= 64;
    }
    memcpy(tail, p, left);
    tail[left] = 0x80;
    size_t n = left < 56 ? 64 : 128;
    u64 bits = (u64)size * 8;
    for (int i = 0; i < 8; i++)
        tail[n - 1 - i] = (u8)(bits >> (8 * i));
    sha_block(h, tail);
    if (n == 128)
        sha_block(h, tail + 64);
    for (int i = 0; i < 8; i++)
        for (int j = 0; j < 4; j++)
            digest[4 * i + j] = (u8)(h[i] >> (24 - 8 * j));
}

#endif
