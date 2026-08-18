#ifndef MINIREDIS_DYNBUF_H
#define MINIREDIS_DYNBUF_H

#include <stddef.h>

/* A minimal growable byte buffer (a tiny analogue of Redis's sds). The buffer
 * is always NUL-terminated for convenience, but `len` is authoritative. */
typedef struct {
    char *p;
    size_t len;
    size_t cap;
} dynbuf;

void dynbuf_init(dynbuf *b);
void dynbuf_free(dynbuf *b);
void dynbuf_reserve(dynbuf *b, size_t extra);
void dynbuf_append(dynbuf *b, const void *data, size_t n);
void dynbuf_append_cstr(dynbuf *b, const char *s);
void dynbuf_appendf(dynbuf *b, const char *fmt, ...);
void dynbuf_clear(dynbuf *b);

#endif /* MINIREDIS_DYNBUF_H */
