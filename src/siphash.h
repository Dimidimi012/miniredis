#ifndef MINIREDIS_SIPHASH_H
#define MINIREDIS_SIPHASH_H

#include <stddef.h>
#include <stdint.h>

/* SipHash-2-4: a keyed 64-bit MAC/hash (Aumasson & Bernstein). Keyed hashing
 * with a random, secret key protects the hash table against hash-flooding
 * denial-of-service attacks (an attacker cannot force worst-case bucket
 * collisions without knowing the key). */
uint64_t siphash(const uint8_t *in, size_t inlen, const uint8_t k[16]);

#endif /* MINIREDIS_SIPHASH_H */
