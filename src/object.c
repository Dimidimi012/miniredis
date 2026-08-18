#include "object.h"

#include "util.h"

#include <stdio.h>
#include <string.h>

robj *robj_new_string(const char *s, size_t len) {
    robj *o = xmalloc(sizeof(*o));
    o->type = OBJ_STRING;
    o->ptr = xmalloc(len + 1);
    memcpy(o->ptr, s, len);
    o->ptr[len] = '\0';
    o->len = len;
    o->expire_at = -1;
    return o;
}

robj *robj_new_string_ll(long long v) {
    char tmp[32];
    int n = snprintf(tmp, sizeof(tmp), "%lld", v);
    return robj_new_string(tmp, (size_t)n);
}

void robj_free(robj *o) {
    if (!o) return;
    free(o->ptr);
    free(o);
}
