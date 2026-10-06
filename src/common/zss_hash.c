// SPDX-License-Identifier: GPL-2.0-only
/*
 * The hash is MurmurHash3 (x64, 128 bit) by Austin Appleby, who placed it in
 * the public domain. This file is a rewrite of it for this project.
 */
#include "zss_hash.h"

#include <string.h>

static inline uint64_t rotl64(uint64_t x, int r)
{
    return (x << r) | (x >> (64 - r));
}

static inline uint64_t fmix64(uint64_t k)
{
    k ^= k >> 33;
    k *= 0xff51afd7ed558ccdULL;
    k ^= k >> 33;
    k *= 0xc4ceb9fe1a85ec53ULL;
    k ^= k >> 33;
    return k;
}

void zss_hash128(const void *data, size_t len, uint8_t out[16])
{
    const uint64_t c1 = 0x87c37b91114253d5ULL, c2 = 0x4cf5ad432745937fULL;
    const uint8_t *p = data, *tail = p + (len / 16) * 16;
    uint64_t h1 = 0, h2 = 0, k1 = 0, k2 = 0;

    for (size_t i = 0; i < len / 16; i++) {
        memcpy(&k1, p + i * 16, 8);
        memcpy(&k2, p + i * 16 + 8, 8);
        k1 *= c1; k1 = rotl64(k1, 31); k1 *= c2; h1 ^= k1;
        h1 = rotl64(h1, 27); h1 += h2; h1 = h1 * 5 + 0x52dce729;
        k2 *= c2; k2 = rotl64(k2, 33); k2 *= c1; h2 ^= k2;
        h2 = rotl64(h2, 31); h2 += h1; h2 = h2 * 5 + 0x38495ab5;
    }

    k1 = k2 = 0;
    switch (len & 15) {
    case 15: k2 ^= (uint64_t)tail[14] << 48; /* fall through */
    case 14: k2 ^= (uint64_t)tail[13] << 40; /* fall through */
    case 13: k2 ^= (uint64_t)tail[12] << 32; /* fall through */
    case 12: k2 ^= (uint64_t)tail[11] << 24; /* fall through */
    case 11: k2 ^= (uint64_t)tail[10] << 16; /* fall through */
    case 10: k2 ^= (uint64_t)tail[9] << 8; /* fall through */
    case 9:
        k2 ^= (uint64_t)tail[8];
        k2 *= c2; k2 = rotl64(k2, 33); k2 *= c1; h2 ^= k2;
        /* fall through */
    case 8: k1 ^= (uint64_t)tail[7] << 56; /* fall through */
    case 7: k1 ^= (uint64_t)tail[6] << 48; /* fall through */
    case 6: k1 ^= (uint64_t)tail[5] << 40; /* fall through */
    case 5: k1 ^= (uint64_t)tail[4] << 32; /* fall through */
    case 4: k1 ^= (uint64_t)tail[3] << 24; /* fall through */
    case 3: k1 ^= (uint64_t)tail[2] << 16; /* fall through */
    case 2: k1 ^= (uint64_t)tail[1] << 8; /* fall through */
    case 1:
        k1 ^= (uint64_t)tail[0];
        k1 *= c1; k1 = rotl64(k1, 31); k1 *= c2; h1 ^= k1;
    }

    h1 ^= len;
    h2 ^= len;
    h1 += h2;
    h2 += h1;
    h1 = fmix64(h1);
    h2 = fmix64(h2);
    h1 += h2;
    h2 += h1;
    memcpy(out, &h1, 8);
    memcpy(out + 8, &h2, 8);
}
