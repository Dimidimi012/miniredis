#include "dynbuf.h"

#include "util.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

void dynbuf_init(dynbuf *b) {
    b->p = NULL;
    b->len = 0;
    b->cap = 0;
}

void dynbuf_free(dynbuf *b) {
    free(b->p);
    b->p = NULL;
    b->len = 0;
    b->cap = 0;
}

void dynbuf_reserve(dynbuf *b, size_t extra) {
    if (b->len + extra + 1 <= b->cap) return;
    size_t newcap = b->cap ? b->cap : 64;
    while (newcap < b->len + extra + 1) newcap *= 2;
    b->p = xrealloc(b->p, newcap);
    b->cap = newcap;
}

void dynbuf_append(dynbuf *b, const void *data, size_t n) {
    if (n == 0) return;
    dynbuf_reserve(b, n);
    memcpy(b->p + b->len, data, n);
    b->len += n;
    b->p[b->len] = '\0';
}

void dynbuf_append_cstr(dynbuf *b, const char *s) {
    dynbuf_append(b, s, strlen(s));
}

void dynbuf_appendf(dynbuf *b, const char *fmt, ...) {
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);

    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) {
        va_end(ap2);
        return;
    }

    dynbuf_reserve(b, (size_t)n);
    vsnprintf(b->p + b->len, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    b->len += (size_t)n;
}

void dynbuf_clear(dynbuf *b) {
    b->len = 0;
    if (b->p) b->p[0] = '\0';
}
