#ifndef MINIREDIS_OBJECT_H
#define MINIREDIS_OBJECT_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
    OBJ_STRING = 0,
    OBJ_LIST,
    OBJ_HASH,
    OBJ_SET,
    OBJ_ZSET,
} obj_type;

/* The value stored under each key. `ptr` points to the type-specific storage:
 *   OBJ_STRING : owned char buffer (len is authoritative, binary-safe)
 *   OBJ_LIST   : list*
 *   OBJ_HASH   : dict*   (field -> robj*)
 *   OBJ_SET    : dict*   (member -> NULL sentinel)
 *   OBJ_ZSET   : zset*   (dict + skiplist) */
typedef struct robj {
    obj_type type;
    void *ptr;
    size_t len;             /* string byte length; unused for other types */
    int64_t expire_at;      /* absolute expiry in ms (wall clock), or -1 for "never" */
} robj;

robj *robj_new_string(const char *s, size_t len);
robj *robj_new_string_ll(long long v);
robj *robj_new_list(void);
robj *robj_new_hash(void);
robj *robj_new_set(void);
robj *robj_new_zset(void);
void robj_free(robj *o);

#endif /* MINIREDIS_OBJECT_H */
