#ifndef MINIREDIS_OBJECT_H
#define MINIREDIS_OBJECT_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
    OBJ_STRING = 0,
} obj_type;

/* The value stored under each key. Currently a single binary-safe string type;
 * the `type` field leaves room for LIST/HASH/SET/ZSET in future iterations. */
typedef struct robj {
    obj_type type;
    char *ptr;              /* owned; NUL-terminated but binary-safe (len is authoritative) */
    size_t len;
    int64_t expire_at;      /* absolute expiry in ms (wall clock), or -1 for "never" */
} robj;

robj *robj_new_string(const char *s, size_t len);
robj *robj_new_string_ll(long long v);
void robj_free(robj *o);

#endif /* MINIREDIS_OBJECT_H */
