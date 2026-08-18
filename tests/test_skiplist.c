#include "util.h"
#include "zskiplist.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond) do {                       \
    if (!(cond)) {                             \
        failures++;                            \
        fprintf(stderr, "FAIL %s:%d: %s\n",    \
                __FILE__, __LINE__, #cond);    \
    }                                          \
} while (0)

/* Collect members in ascending order into a buffer. */
static void collect_ordered(const zskiplist *zsl, char *out, size_t outsz) {
    size_t n = 0;
    out[0] = '\0';
    for (zskiplist_node *x = zsl->header->level[0].forward; x; x = x->level[0].forward) {
        n += (size_t)snprintf(out + n, outsz - n, "%s%.*s",
                              (n > 0) ? "," : "", (int)x->mlen, x->member);
        if (n >= outsz) break;
    }
}

int main(void) {
    /* ---- basic ordering ---- */
    {
        zskiplist *zsl = zsl_create();
        zsl_insert(zsl, 3.0, "c", 1);
        zsl_insert(zsl, 1.0, "a", 1);
        zsl_insert(zsl, 2.0, "b", 1);
        CHECK(zsl_len(zsl) == 3);

        char buf[64];
        collect_ordered(zsl, buf, sizeof(buf));
        CHECK(strcmp(buf, "a,b,c") == 0);

        CHECK(zsl_get_rank(zsl, 1.0, "a", 1) == 1);
        CHECK(zsl_get_rank(zsl, 2.0, "b", 1) == 2);
        CHECK(zsl_get_rank(zsl, 3.0, "c", 1) == 3);
        CHECK(zsl_get_rank(zsl, 9.0, "z", 1) == 0);

        zskiplist_node *n = zsl_get_element_by_rank(zsl, 2);
        CHECK(n != NULL && n->mlen == 1 && n->member[0] == 'b');
        CHECK(zsl_get_element_by_rank(zsl, 0) == NULL);
        CHECK(zsl_get_element_by_rank(zsl, 4) == NULL);

        zsl_free(zsl);
    }

    /* ---- ties: equal scores order members lexicographically ---- */
    {
        zskiplist *zsl = zsl_create();
        zsl_insert(zsl, 5.0, "b", 1);
        zsl_insert(zsl, 5.0, "a", 1);
        zsl_insert(zsl, 5.0, "c", 1);
        char buf[64];
        collect_ordered(zsl, buf, sizeof(buf));
        CHECK(strcmp(buf, "a,b,c") == 0);
        CHECK(zsl_get_rank(zsl, 5.0, "b", 1) == 2);
        zsl_free(zsl);
    }

    /* ---- delete ---- */
    {
        zskiplist *zsl = zsl_create();
        for (int i = 0; i < 50; i++) {
            char m[16];
            snprintf(m, sizeof(m), "m%02d", i);
            zsl_insert(zsl, (double)i / 2.0, m, strlen(m));
        }
        CHECK(zsl_len(zsl) == 50);
        CHECK(zsl_delete(zsl, 10.0, "m20", 3) == 1);
        CHECK(zsl_len(zsl) == 49);
        CHECK(zsl_get_rank(zsl, 10.0, "m20", 3) == 0);
        CHECK(zsl_delete(zsl, 10.0, "m20", 3) == 0);
        /* deleting the tail */
        CHECK(zsl_delete(zsl, 24.5, "m49", 3) == 1);
        /* deleting the head */
        CHECK(zsl_delete(zsl, 0.0, "m00", 3) == 1);
        CHECK(zsl_len(zsl) == 47);
        zsl_free(zsl);
    }

    /* ---- score ranges ---- */
    {
        zskiplist *zsl = zsl_create();
        for (int i = 0; i <= 10; i++) {
            char m[8];
            snprintf(m, sizeof(m), "k%d", i);
            zsl_insert(zsl, (double)i, m, strlen(m));
        }
        zskiplist_node *f = zsl_first_in_score_range(zsl, 3.0, 0);
        zskiplist_node *l = zsl_last_in_score_range(zsl, 6.0, 0);
        CHECK(f != NULL && f->score == 3.0);
        CHECK(l != NULL && l->score == 6.0);

        f = zsl_first_in_score_range(zsl, 3.0, 1);   /* (3 */
        CHECK(f != NULL && f->score == 4.0);
        l = zsl_last_in_score_range(zsl, 6.0, 1);    /* 6) */
        CHECK(l != NULL && l->score == 5.0);

        f = zsl_first_in_score_range(zsl, 99.0, 0);
        CHECK(f == NULL);
        l = zsl_last_in_score_range(zsl, -5.0, 0);
        CHECK(l == NULL);

        /* -inf / +inf */
        f = zsl_first_in_score_range(zsl, -INFINITY, 0);
        l = zsl_last_in_score_range(zsl, INFINITY, 0);
        CHECK(f != NULL && f->score == 0.0);
        CHECK(l != NULL && l->score == 10.0);
        zsl_free(zsl);
    }

    /* ---- zset: add / update / remove / score ---- */
    {
        zset *zs = zset_create();
        CHECK(zset_add(zs, 1.0, "a", 1, NULL) == 1);
        CHECK(zset_add(zs, 2.0, "b", 1, NULL) == 1);
        CHECK(zset_add(zs, 2.0, "b", 1, NULL) == 0);   /* same score */
        CHECK(zset_add(zs, 5.0, "b", 1, NULL) == 2);   /* score changed */
        CHECK(zset_len(zs) == 2);

        double s;
        CHECK(zset_score(zs, "b", 1, &s) && s == 5.0);
        CHECK(!zset_score(zs, "nope", 4, &s));

        CHECK(zset_remove(zs, "a", 1) == 1);
        CHECK(zset_remove(zs, "a", 1) == 0);
        CHECK(zset_len(zs) == 1);
        zset_free(zs);
    }

    /* ---- zset with many members: verify rank <-> element consistency ---- */
    {
        zset *zs = zset_create();
        enum { N = 200 };
        for (int i = 0; i < N; i++) {
            char m[16];
            snprintf(m, sizeof(m), "member_%03d", i);
            /* random-ish scores with duplicates */
            zset_add(zs, (double)((i * 37) % 100) / 3.0, m, strlen(m), NULL);
        }
        CHECK(zset_len(zs) == N);

        /* For each member, its rank must be consistent with the element at
         * that rank, and scores must be non-decreasing by rank. */
        double prev = -INFINITY;
        for (unsigned long r = 1; r <= N; r++) {
            zskiplist_node *n = zsl_get_element_by_rank(zs->zsl, r);
            CHECK(n != NULL);
            if (!n) continue;
            CHECK(n->score >= prev);
            prev = n->score;
            CHECK(zsl_get_rank(zs->zsl, n->score, n->member, n->mlen) == r);
        }
        zset_free(zs);
    }

    if (failures) {
        fprintf(stderr, "test_skiplist: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_skiplist: all tests passed\n");
    return 0;
}
