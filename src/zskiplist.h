#ifndef MINIREDIS_ZSKIPLIST_H
#define MINIREDIS_ZSKIPLIST_H

#include <stddef.h>

#include "dict.h"

/* A Redis-style skiplist with level spans, used to implement the ZSET type.
 * Ordering: ascending by score, ties broken by byte-wise member comparison. */

#define ZSKIPLIST_MAXLEVEL 32
#define ZSKIPLIST_P        0.25

typedef struct zskiplist_level {
    struct zskiplist_node *forward;
    unsigned long span;   /* number of level-0 nodes covered by this link */
} zskiplist_level;

typedef struct zskiplist_node {
    char *member;         /* owned, binary-safe */
    size_t mlen;
    double score;
    struct zskiplist_node *backward;
    zskiplist_level level[];   /* level[0..level_count-1] */
} zskiplist_node;

typedef struct zskiplist {
    zskiplist_node *header;
    zskiplist_node *tail;
    unsigned long length;
    int level;
} zskiplist;

/* The ZSET object: a dict mapping member -> node for O(1) lookup/update, plus
 * the skiplist for ordered operations. */
typedef struct zset {
    dict *dict;
    zskiplist *zsl;
} zset;

zskiplist *zsl_create(void);
void zsl_free(zskiplist *zsl);
unsigned long zsl_len(const zskiplist *zsl);

/* Insert a member (caller must ensure it is not already present). */
zskiplist_node *zsl_insert(zskiplist *zsl, double score, const char *member, size_t mlen);
/* Remove a member; frees the node. Returns 1 on success, 0 if not found. */
int zsl_delete(zskiplist *zsl, double score, const char *member, size_t mlen);

/* 1-based rank (1 = smallest score), or 0 if the member is not present. */
unsigned long zsl_get_rank(const zskiplist *zsl, double score, const char *member, size_t mlen);
/* Node at 1-based rank, or NULL when out of range. */
zskiplist_node *zsl_get_element_by_rank(const zskiplist *zsl, unsigned long rank);

/* First/last node within [min,max] (exclusive bounds via minex/maxex). */
zskiplist_node *zsl_first_in_score_range(const zskiplist *zsl, double min, int minex);
zskiplist_node *zsl_last_in_score_range(const zskiplist *zsl, double max, int maxex);

/* ---- zset helpers ---- */

zset *zset_create(void);
void zset_free(zset *zs);
unsigned long zset_len(const zset *zs);

/* Add or update. Returns 1 = new member, 2 = existing member's score changed,
 * 0 = existed with the same score. */
int zset_add(zset *zs, double score, const char *member, size_t mlen, zskiplist_node **out);
int zset_remove(zset *zs, const char *member, size_t mlen);
int zset_score(const zset *zs, const char *member, size_t mlen, double *out);

#endif /* MINIREDIS_ZSKIPLIST_H */
