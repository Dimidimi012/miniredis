#include "dict.h"
#include "util.h"

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

static void free_val(void *p) {
    free(p);
}

int main(void) {
    dict *d = dict_create();

    /* basic set/get */
    int *a = xmalloc(sizeof(int));
    *a = 10;
    CHECK(dict_set(d, "key", 3, a) == NULL);
    CHECK(dict_get(d, "key", 3) == a);
    CHECK(*(int *)dict_get(d, "key", 3) == 10);
    CHECK(dict_size(d) == 1);

    /* replace returns the previous value */
    int *b = xmalloc(sizeof(int));
    *b = 20;
    int *old = dict_set(d, "key", 3, b);
    CHECK(old == a);
    free(old);
    CHECK(*(int *)dict_get(d, "key", 3) == 20);
    CHECK(dict_size(d) == 1);

    /* binary-safe keys: "a\0b" and "a\0c" are distinct from each other and "a" */
    dict_set(d, "a\0b", 3, xmalloc(1));
    dict_set(d, "a\0c", 3, xmalloc(1));
    CHECK(dict_size(d) == 3);
    CHECK(dict_get(d, "a\0b", 3) != NULL);
    CHECK(dict_get(d, "a\0c", 3) != NULL);
    CHECK(dict_get(d, "a", 1) == NULL);

    /* force several resizes */
    for (int i = 0; i < 1000; i++) {
        char k[16];
        snprintf(k, sizeof(k), "k%d", i);
        int *v = xmalloc(sizeof(int));
        *v = i;
        CHECK(dict_set(d, k, strlen(k), v) == NULL);
    }
    CHECK(dict_size(d) == 1003);

    for (int i = 0; i < 1000; i++) {
        char k[16];
        snprintf(k, sizeof(k), "k%d", i);
        int *v = dict_get(d, k, strlen(k));
        CHECK(v != NULL && *v == i);
    }

    /* delete */
    int *del = dict_delete(d, "k500", 4);
    CHECK(del != NULL);
    free(del);
    CHECK(dict_get(d, "k500", 4) == NULL);
    CHECK(dict_delete(d, "k500", 4) == NULL);
    CHECK(dict_size(d) == 1002);

    /* iteration visits every entry exactly once */
    size_t seen = 0;
    dict_iter it;
    dict_iter_init(&it, d);
    dict_entry *e;
    while ((e = dict_iter_next(&it)) != NULL) seen++;
    CHECK(seen == dict_size(d));

    dict_free(d, free_val);

    if (failures) {
        fprintf(stderr, "test_dict: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_dict: all tests passed\n");
    return 0;
}
