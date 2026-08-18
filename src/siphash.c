#include "siphash.h"

#define ROTL(x, b) (uint64_t)(((x) << (b)) | ((x) >> (64 - (b))))

#define SIPROUND                    \
    do {                            \
        v0 += v1;                   \
        v1 = ROTL(v1, 13);          \
        v1 ^= v0;                   \
        v0 = ROTL(v0, 32);          \
        v2 += v3;                   \
        v3 = ROTL(v3, 16);          \
        v3 ^= v2;                   \
        v0 += v3;                   \
        v3 = ROTL(v3, 21);          \
        v3 ^= v0;                   \
        v2 += v1;                   \
        v1 = ROTL(v1, 17);          \
        v1 ^= v2;                   \
        v2 = ROTL(v2, 32);          \
    } while (0)

static uint64_t u8to64_le(const uint8_t *p) {
    return (uint64_t)p[0] | ((uint64_t)p[1] << 8) | ((uint64_t)p[2] << 16) |
           ((uint64_t)p[3] << 24) | ((uint64_t)p[4] << 32) |
           ((uint64_t)p[5] << 40) | ((uint64_t)p[6] << 48) |
           ((uint64_t)p[7] << 56);
}

uint64_t siphash(const uint8_t *in, size_t inlen, const uint8_t k[16]) {
    uint64_t v0 = UINT64_C(0x736f6d6570736575);
    uint64_t v1 = UINT64_C(0x646f72616e646f6d);
    uint64_t v2 = UINT64_C(0x6c7967656e657261);
    uint64_t v3 = UINT64_C(0x7465646279746573);
    uint64_t k0 = u8to64_le(k);
    uint64_t k1 = u8to64_le(k + 8);
    uint64_t b = (uint64_t)inlen << 56;

    v3 ^= k1;
    v2 ^= k0;
    v1 ^= k1;
    v0 ^= k0;

    const uint8_t *end = in + inlen - (inlen % 8);
    for (; in != end; in += 8) {
        uint64_t m = u8to64_le(in);
        v3 ^= m;
        SIPROUND;
        SIPROUND;
        v0 ^= m;
    }

    switch (inlen & 7) {
        case 7: b |= (uint64_t)in[6] << 48; /* fall through */
        case 6: b |= (uint64_t)in[5] << 40; /* fall through */
        case 5: b |= (uint64_t)in[4] << 32; /* fall through */
        case 4: b |= (uint64_t)in[3] << 24; /* fall through */
        case 3: b |= (uint64_t)in[2] << 16; /* fall through */
        case 2: b |= (uint64_t)in[1] << 8;  /* fall through */
        case 1: b |= (uint64_t)in[0];       /* fall through */
        case 0: break;
    }

    v3 ^= b;
    SIPROUND;
    SIPROUND;
    v0 ^= b;

    v2 ^= 0xff;
    SIPROUND;
    SIPROUND;
    SIPROUND;
    SIPROUND;

    return v0 ^ v1 ^ v2 ^ v3;
}
