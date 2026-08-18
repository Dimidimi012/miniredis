#include "object.h"

#include "dict.h"
#include "list.h"
#include "util.h"
#include "zskiplist.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void free_robj(void *p) {
    robj_free((robj *)p);
}

robj *robj_new_string(const char *s, size_t len) {
    robj *o = xmalloc(sizeof(*o));
    o->type = OBJ_STRING;
    o->ptr = xmalloc(len + 1);
    memcpy(o->ptr, s, len);
    ((char *)o->ptr)[len] = '\0';
    o->len = len;
    o->expire_at = -1;
    return o;
}

robj *robj_new_string_ll(long long v) {
    char tmp[32];
    int n = snprintf(tmp, sizeof(tmp), "%lld", v);
    return robj_new_string(tmp, (size_t)n);
}

robj *robj_new_list(void) {
    robj *o = xmalloc(sizeof(*o));
    o->type = OBJ_LIST;
    o->ptr = list_create();
    o->len = 0;
    o->expire_at = -1;
    return o;
}

robj *robj_new_hash(void) {
    robj *o = xmalloc(sizeof(*o));
    o->type = OBJ_HASH;
    o->ptr = dict_create();
    o->len = 0;
    o->expire_at = -1;
    return o;
}

robj *robj_new_set(void) {
    robj *o = xmalloc(sizeof(*o));
    o->type = OBJ_SET;
    o->ptr = dict_create();
    o->len = 0;
    o->expire_at = -1;
    return o;
}

robj *robj_new_zset(void) {
    robj *o = xmalloc(sizeof(*o));
    o->type = OBJ_ZSET;
    o->ptr = zset_create();
    o->len = 0;
    o->expire_at = -1;
    return o;
}

void robj_free(robj *o) {
    if (!o) return;
    switch (o->type) {
        case OBJ_STRING:
            free(o->ptr);
            break;
        case OBJ_LIST:
            list_free((list *)o->ptr, free_robj);
            break;
        case OBJ_HASH:
            dict_free((dict *)o->ptr, free_robj);
            break;
        case OBJ_SET:
            /* values are the NULL sentinel; nothing to free */
            dict_free((dict *)o->ptr, NULL);
            break;
        case OBJ_ZSET:
            zset_free((zset *)o->ptr);
            break;
    }
    free(o);
}
