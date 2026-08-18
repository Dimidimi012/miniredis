#include "zskiplist.h"

#include "util.h"

#include <stdlib.h>
#include <string.h>

/* ---- helpers ---- */

static int member_cmp(const char *a, size_t alen, const char *b, size_t blen) {
    size_t n = alen < blen ? alen : blen;
    int r = memcmp(a, b, n);
    if (r != 0) return r;
    return alen < blen ? -1 : (alen > blen ? 1 : 0);
}

static void zsl_seed_rand(void) {
    static int seeded = 0;
    if (!seeded) {
        srand((unsigned)(now_ms() ^ (uintptr_t)&seeded));
        seeded = 1;
    }
}

static int zsl_random_level(void) {
    zsl_seed_rand();
    int level = 1;
    while (level < ZSKIPLIST_MAXLEVEL &&
           (rand() & 0xFFFF) < (unsigned)(ZSKIPLIST_P * 0xFFFF)) {
        level++;
    }
    return level;
}

static zskiplist_node *zsl_create_node(int level, double score,
                                       const char *member, size_t mlen) {
    zskiplist_node *n = xmalloc(sizeof(*n) + (size_t)level * sizeof(zskiplist_level));
    n->member = xstrndup(member, mlen);
    n->mlen = mlen;
    n->score = score;
    n->backward = NULL;
    for (int i = 0; i < level; i++) {
        n->level[i].forward = NULL;
        n->level[i].span = 0;
    }
    return n;
}

static void zsl_free_node(zskiplist_node *n) {
    free(n->member);
    free(n);
}

/* Unlink `x` (located via `update`) and fix the spans. Does not free `x`. */
static void zsl_delete_node(zskiplist *zsl, zskiplist_node *x, zskiplist_node **update) {
    for (int i = 0; i < zsl->level; i++) {
        if (update[i]->level[i].forward == x) {
            update[i]->level[i].span += x->level[i].span - 1;
            update[i]->level[i].forward = x->level[i].forward;
        } else {
            update[i]->level[i].span -= 1;
        }
    }
    if (x->level[0].forward) {
        x->level[0].forward->backward = x->backward;
    } else {
        zsl->tail = x->backward;
    }
    while (zsl->level > 1 && zsl->header->level[zsl->level - 1].forward == NULL) {
        zsl->level--;
    }
    zsl->length--;
}

/* ---- skiplist ---- */

zskiplist *zsl_create(void) {
    zskiplist *zsl = xmalloc(sizeof(*zsl));
    zsl->level = 1;
    zsl->length = 0;
    zsl->tail = NULL;

    zsl->header = xmalloc(sizeof(zskiplist_node) +
                          ZSKIPLIST_MAXLEVEL * sizeof(zskiplist_level));
    zsl->header->member = NULL;
    zsl->header->mlen = 0;
    zsl->header->score = 0;
    zsl->header->backward = NULL;
    for (int i = 0; i < ZSKIPLIST_MAXLEVEL; i++) {
        zsl->header->level[i].forward = NULL;
        zsl->header->level[i].span = 0;
    }
    return zsl;
}

void zsl_free(zskiplist *zsl) {
    if (!zsl) return;
    zskiplist_node *x = zsl->header->level[0].forward;
    while (x) {
        zskiplist_node *next = x->level[0].forward;
        zsl_free_node(x);
        x = next;
    }
    free(zsl->header);
    free(zsl);
}

unsigned long zsl_len(const zskiplist *zsl) {
    return zsl->length;
}

zskiplist_node *zsl_insert(zskiplist *zsl, double score,
                           const char *member, size_t mlen) {
    zskiplist_node *update[ZSKIPLIST_MAXLEVEL];
    unsigned long rank[ZSKIPLIST_MAXLEVEL];
    zskiplist_node *x = zsl->header;

    for (int i = zsl->level - 1; i >= 0; i--) {
        rank[i] = (i == zsl->level - 1) ? 0 : rank[i + 1];
        while (x->level[i].forward &&
               (x->level[i].forward->score < score ||
                (x->level[i].forward->score == score &&
                 member_cmp(x->level[i].forward->member, x->level[i].forward->mlen,
                            member, mlen) < 0))) {
            rank[i] += x->level[i].span;
            x = x->level[i].forward;
        }
        update[i] = x;
    }

    int level = zsl_random_level();
    if (level > zsl->level) {
        for (int i = zsl->level; i < level; i++) {
            rank[i] = 0;
            update[i] = zsl->header;
            update[i]->level[i].span = zsl->length;
        }
        zsl->level = level;
    }

    x = zsl_create_node(level, score, member, mlen);
    for (int i = 0; i < level; i++) {
        x->level[i].forward = update[i]->level[i].forward;
        update[i]->level[i].forward = x;
        x->level[i].span = update[i]->level[i].span - (rank[0] - rank[i]);
        update[i]->level[i].span = (rank[0] - rank[i]) + 1;
    }
    for (int i = level; i < zsl->level; i++) {
        update[i]->level[i].span++;
    }

    x->backward = (update[0] == zsl->header) ? NULL : update[0];
    if (x->level[0].forward) {
        x->level[0].forward->backward = x;
    } else {
        zsl->tail = x;
    }
    zsl->length++;
    return x;
}

int zsl_delete(zskiplist *zsl, double score, const char *member, size_t mlen) {
    zskiplist_node *update[ZSKIPLIST_MAXLEVEL];
    zskiplist_node *x = zsl->header;

    for (int i = zsl->level - 1; i >= 0; i--) {
        while (x->level[i].forward &&
               (x->level[i].forward->score < score ||
                (x->level[i].forward->score == score &&
                 member_cmp(x->level[i].forward->member, x->level[i].forward->mlen,
                            member, mlen) < 0))) {
            x = x->level[i].forward;
        }
        update[i] = x;
    }

    x = x->level[0].forward;
    if (x && x->score == score && x->mlen == mlen &&
        memcmp(x->member, member, mlen) == 0) {
        zsl_delete_node(zsl, x, update);
        zsl_free_node(x);
        return 1;
    }
    return 0;
}

unsigned long zsl_get_rank(const zskiplist *zsl, double score,
                           const char *member, size_t mlen) {
    zskiplist_node *x = zsl->header;
    unsigned long rank = 0;

    for (int i = zsl->level - 1; i >= 0; i--) {
        while (x->level[i].forward &&
               (x->level[i].forward->score < score ||
                (x->level[i].forward->score == score &&
                 member_cmp(x->level[i].forward->member, x->level[i].forward->mlen,
                            member, mlen) < 0))) {
            rank += x->level[i].span;
            x = x->level[i].forward;
        }
    }

    x = x->level[0].forward;
    if (x && x->score == score && x->mlen == mlen &&
        memcmp(x->member, member, mlen) == 0) {
        return rank + 1;
    }
    return 0;
}

zskiplist_node *zsl_get_element_by_rank(const zskiplist *zsl, unsigned long rank) {
    if (rank == 0) return NULL;   /* ranks are 1-based */
    zskiplist_node *x = zsl->header;
    unsigned long traversed = 0;

    for (int i = zsl->level - 1; i >= 0; i--) {
        while (x->level[i].forward && (traversed + x->level[i].span) <= rank) {
            traversed += x->level[i].span;
            x = x->level[i].forward;
        }
        if (traversed == rank) return x;
    }
    return NULL;
}

zskiplist_node *zsl_first_in_score_range(const zskiplist *zsl, double min, int minex) {
    zskiplist_node *x = zsl->header;
    for (int i = zsl->level - 1; i >= 0; i--) {
        while (x->level[i].forward &&
               (x->level[i].forward->score < min ||
                (minex && x->level[i].forward->score == min))) {
            x = x->level[i].forward;
        }
    }
    return x->level[0].forward;
}

zskiplist_node *zsl_last_in_score_range(const zskiplist *zsl, double max, int maxex) {
    zskiplist_node *x = zsl->header;
    for (int i = zsl->level - 1; i >= 0; i--) {
        while (x->level[i].forward &&
               (x->level[i].forward->score < max ||
                (!maxex && x->level[i].forward->score == max))) {
            x = x->level[i].forward;
        }
    }
    /* x stays at the header when no node satisfies the upper bound. */
    return x == zsl->header ? NULL : x;
}

/* ---- zset ---- */

zset *zset_create(void) {
    zset *zs = xmalloc(sizeof(*zs));
    zs->dict = dict_create();
    zs->zsl = zsl_create();
    return zs;
}

void zset_free(zset *zs) {
    if (!zs) return;
    /* Dict values point at skiplist nodes, which zsl_free() owns. */
    dict_free(zs->dict, NULL);
    zsl_free(zs->zsl);
    free(zs);
}

unsigned long zset_len(const zset *zs) {
    return zsl_len(zs->zsl);
}

int zset_add(zset *zs, double score, const char *member, size_t mlen,
             zskiplist_node **out) {
    zskiplist_node *n = dict_get(zs->dict, member, mlen);
    if (n) {
        if (n->score == score) {
            if (out) *out = n;
            return 0;
        }
        zsl_delete(zs->zsl, n->score, n->member, n->mlen);
        dict_delete(zs->dict, member, mlen);   /* node was freed by zsl_delete */
        n = zsl_insert(zs->zsl, score, member, mlen);
        dict_set(zs->dict, member, mlen, n);
        if (out) *out = n;
        return 2;
    }

    n = zsl_insert(zs->zsl, score, member, mlen);
    dict_set(zs->dict, member, mlen, n);
    if (out) *out = n;
    return 1;
}

int zset_remove(zset *zs, const char *member, size_t mlen) {
    zskiplist_node *n = dict_get(zs->dict, member, mlen);
    if (!n) return 0;
    zsl_delete(zs->zsl, n->score, n->member, n->mlen);
    dict_delete(zs->dict, member, mlen);
    return 1;
}

int zset_score(const zset *zs, const char *member, size_t mlen, double *out) {
    zskiplist_node *n = dict_get(zs->dict, member, mlen);
    if (!n) return 0;
    *out = n->score;
    return 1;
}
