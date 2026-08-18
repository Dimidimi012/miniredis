#ifndef MINIREDIS_DICT_H
#define MINIREDIS_DICT_H

#include <stddef.h>
#include <stdint.h>

typedef struct dict_entry {
    struct dict_entry *next;
    char *key;              /* owned copy, binary-safe */
    size_t klen;
    void *val;              /* owned by the caller via free_val */
    uint64_t hash;
} dict_entry;

typedef struct dict {
    dict_entry **table;
    size_t size;            /* number of buckets (power of two) */
    size_t used;            /* number of entries */
    uint64_t seed;          /* randomized seed for the hash function */
} dict;

typedef struct {
    const dict *d;
    size_t bucket;
    dict_entry *entry;
} dict_iter;

dict *dict_create(void);
void dict_free(dict *d, void (*free_val)(void *));
size_t dict_size(const dict *d);

/* Look up a key; returns the stored value or NULL if absent. */
void *dict_get(const dict *d, const void *key, size_t klen);

/* Insert or replace. The dict takes ownership of `val` and stores its own copy
 * of the key. Returns the previous value if the key already existed (the caller
 * must free it), or NULL if this was a fresh insert. */
void *dict_set(dict *d, const char *key, size_t klen, void *val);

/* Remove a key; returns the stored value (the caller frees it), or NULL. */
void *dict_delete(dict *d, const void *key, size_t klen);

/* Iteration (used by KEYS). */
void dict_iter_init(dict_iter *it, const dict *d);
dict_entry *dict_iter_next(dict_iter *it);

#endif /* MINIREDIS_DICT_H */
