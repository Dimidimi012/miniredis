#include "dict.h"

#include "siphash.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>

#define INITIAL_SIZE     16
#define LOAD_FACTOR_NUM  3
#define LOAD_FACTOR_DEN  4   /* grow when used/size >= 0.75 */

/* SipHash-2-4 keyed with a per-dictionary random 128-bit key. The key makes
 * the hash unpredictable to an attacker, so it cannot force worst-case
 * collisions (hash-flooding resistance). */
static uint64_t dict_hash(const void *key, size_t len, const uint8_t seed[16]) {
    return siphash((const uint8_t *)key, len, seed);
}

static int key_equal(const dict_entry *e, const void *key, size_t klen, uint64_t hash) {
    return e->hash == hash && e->klen == klen && memcmp(e->key, key, klen) == 0;
}

dict *dict_create(void) {
    dict *d = xmalloc(sizeof(*d));
    d->table = NULL;
    d->size = 0;
    d->used = 0;
    util_random_bytes(d->seed, sizeof(d->seed));
    return d;
}

static void dict_resize(dict *d, size_t new_size) {
    dict_entry **new_table = xcalloc(new_size, sizeof(*new_table));
    for (size_t i = 0; i < d->size; i++) {
        dict_entry *e = d->table[i];
        while (e) {
            dict_entry *next = e->next;
            size_t idx = e->hash & (new_size - 1);
            e->next = new_table[idx];
            new_table[idx] = e;
            e = next;
        }
    }
    free(d->table);
    d->table = new_table;
    d->size = new_size;
}

static void dict_grow(dict *d) {
    if (d->size == 0) {
        dict_resize(d, INITIAL_SIZE);
    } else if (d->used * LOAD_FACTOR_DEN >= d->size * LOAD_FACTOR_NUM) {
        dict_resize(d, d->size * 2);
    }
}

void *dict_get(const dict *d, const void *key, size_t klen) {
    if (d->size == 0) return NULL;
    uint64_t h = dict_hash(key, klen, d->seed);
    size_t idx = h & (d->size - 1);
    for (dict_entry *e = d->table[idx]; e; e = e->next) {
        if (key_equal(e, key, klen, h)) return e->val;
    }
    return NULL;
}

void *dict_set(dict *d, const char *key, size_t klen, void *val) {
    dict_grow(d);
    uint64_t h = dict_hash(key, klen, d->seed);
    size_t idx = h & (d->size - 1);

    for (dict_entry *e = d->table[idx]; e; e = e->next) {
        if (key_equal(e, key, klen, h)) {
            void *old = e->val;
            e->val = val;
            return old;
        }
    }

    dict_entry *e = xmalloc(sizeof(*e));
    e->key = xstrndup(key, klen);
    e->klen = klen;
    e->val = val;
    e->hash = h;
    e->next = d->table[idx];
    d->table[idx] = e;
    d->used++;
    return NULL;
}

void *dict_delete(dict *d, const void *key, size_t klen) {
    if (d->size == 0) return NULL;
    uint64_t h = dict_hash(key, klen, d->seed);
    size_t idx = h & (d->size - 1);

    dict_entry *prev = NULL;
    for (dict_entry *e = d->table[idx]; e; e = e->next) {
        if (key_equal(e, key, klen, h)) {
            if (prev) prev->next = e->next;
            else d->table[idx] = e->next;
            void *val = e->val;
            free(e->key);
            free(e);
            d->used--;
            return val;
        }
        prev = e;
    }
    return NULL;
}

void dict_free(dict *d, void (*free_val)(void *)) {
    if (!d) return;
    for (size_t i = 0; i < d->size; i++) {
        dict_entry *e = d->table[i];
        while (e) {
            dict_entry *next = e->next;
            free(e->key);
            if (free_val) free_val(e->val);
            free(e);
            e = next;
        }
    }
    free(d->table);
    free(d);
}

size_t dict_size(const dict *d) {
    return d->used;
}

void dict_iter_init(dict_iter *it, const dict *d) {
    it->d = d;
    it->bucket = 0;
    it->entry = NULL;
}

dict_entry *dict_iter_next(dict_iter *it) {
    const dict *d = it->d;

    if (it->entry) {
        it->entry = it->entry->next;
        if (it->entry) return it->entry;
        it->bucket++;
    }

    while (it->bucket < d->size) {
        if (d->table[it->bucket]) {
            it->entry = d->table[it->bucket];
            return it->entry;
        }
        it->bucket++;
    }
    return NULL;
}

static void seed_rand_once(void) {
    static int seeded = 0;
    if (!seeded) {
        srand((unsigned)(now_ms() ^ (uintptr_t)&seeded));
        seeded = 1;
    }
}

dict_entry *dict_random_entry(const dict *d) {
    if (d->used == 0) return NULL;
    seed_rand_once();

    dict_entry *pick = NULL;
    unsigned long i = 0;
    for (size_t b = 0; b < d->size; b++) {
        for (dict_entry *e = d->table[b]; e; e = e->next) {
            if (rand() % (i + 1) == 0) pick = e;
            i++;
        }
    }
    return pick;
}
