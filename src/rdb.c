#include "rdb.h"

#include "dict.h"
#include "dynbuf.h"
#include "list.h"
#include "object.h"
#include "util.h"
#include "zskiplist.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define RDB_MAGIC "MINIREDIS"
#define RDB_VERSION 1

static void free_robj_val(void *p) {
    robj_free((robj *)p);
}

/* ---- writer (big-endian) ---- */

static void w_u8(dynbuf *b, uint8_t v) {
    dynbuf_append(b, &v, 1);
}

static void w_u32(dynbuf *b, uint32_t v) {
    uint8_t t[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16),
                    (uint8_t)(v >> 8), (uint8_t)v};
    dynbuf_append(b, t, sizeof(t));
}

static void w_u64(dynbuf *b, uint64_t v) {
    uint8_t t[8] = {(uint8_t)(v >> 56), (uint8_t)(v >> 48), (uint8_t)(v >> 40),
                    (uint8_t)(v >> 32), (uint8_t)(v >> 24), (uint8_t)(v >> 16),
                    (uint8_t)(v >> 8), (uint8_t)v};
    dynbuf_append(b, t, sizeof(t));
}

static void w_s64(dynbuf *b, int64_t v) {
    w_u64(b, (uint64_t)v);
}

static void w_bytes(dynbuf *b, const void *p, size_t n) {
    w_u32(b, (uint32_t)n);
    dynbuf_append(b, p, n);
}

static void w_f64(dynbuf *b, double d) {
    uint64_t u;
    memcpy(&u, &d, sizeof(u));
    w_u64(b, u);
}

static void rdb_write_obj(dynbuf *b, const robj *o) {
    switch (o->type) {
        case OBJ_STRING:
            w_bytes(b, o->ptr, o->len);
            break;
        case OBJ_LIST: {
            const list *l = (const list *)o->ptr;
            w_u32(b, (uint32_t)list_len(l));
            for (const list_node *n = list_first(l); n; n = list_next(n)) {
                const robj *v = (const robj *)n->val;
                w_bytes(b, v->ptr, v->len);
            }
            break;
        }
        case OBJ_HASH: {
            const dict *h = (const dict *)o->ptr;
            w_u32(b, (uint32_t)dict_size(h));
            dict_iter it;
            dict_iter_init(&it, h);
            const dict_entry *e;
            while ((e = dict_iter_next(&it)) != NULL) {
                const robj *v = (const robj *)e->val;
                w_bytes(b, e->key, e->klen);
                w_bytes(b, v->ptr, v->len);
            }
            break;
        }
        case OBJ_SET: {
            const dict *s = (const dict *)o->ptr;
            w_u32(b, (uint32_t)dict_size(s));
            dict_iter it;
            dict_iter_init(&it, s);
            const dict_entry *e;
            while ((e = dict_iter_next(&it)) != NULL) {
                w_bytes(b, e->key, e->klen);
            }
            break;
        }
        case OBJ_ZSET: {
            const zset *zs = (const zset *)o->ptr;
            w_u32(b, (uint32_t)zset_len(zs));
            /* iterate the skiplist so members are stored in score order */
            for (const zskiplist_node *n = zs->zsl->header->level[0].forward;
                 n; n = n->level[0].forward) {
                w_bytes(b, n->member, n->mlen);
                w_f64(b, n->score);
            }
            break;
        }
    }
}

static int write_full(int fd, const void *buf, size_t n) {
    size_t off = 0;
    while (off < n) {
        ssize_t w = write(fd, (const char *)buf + off, n - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        off += (size_t)w;
    }
    return 0;
}

int rdb_save(const char *path, const db *store) {
    dynbuf b;
    dynbuf_init(&b);

    dynbuf_append_cstr(&b, RDB_MAGIC);
    w_u32(&b, RDB_VERSION);
    w_u32(&b, (uint32_t)dict_size(store->d));

    dict_iter it;
    dict_iter_init(&it, store->d);
    const dict_entry *e;
    while ((e = dict_iter_next(&it)) != NULL) {
        const robj *o = (const robj *)e->val;
        w_u32(&b, (uint32_t)e->klen);
        dynbuf_append(&b, e->key, e->klen);
        w_s64(&b, o->expire_at);
        w_u8(&b, (uint8_t)o->type);
        rdb_write_obj(&b, o);
    }

    /* atomic replace */
    size_t tmplen = strlen(path) + 8;
    char *tmppath = xmalloc(tmplen);
    snprintf(tmppath, tmplen, "%s.tmp", path);

    int fd = open(tmppath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        log_error("rdb: open %s: %s", tmppath, strerror(errno));
        free(tmppath);
        dynbuf_free(&b);
        return -1;
    }

    int rc = write_full(fd, b.p, b.len);
    if (rc == 0 && fsync(fd) < 0) rc = -1;
    if (close(fd) < 0) rc = -1;
    if (rc == 0 && rename(tmppath, path) < 0) {
        log_error("rdb: rename %s -> %s: %s", tmppath, path, strerror(errno));
        rc = -1;
    }

    free(tmppath);
    dynbuf_free(&b);
    return rc;
}

/* ---- reader (big-endian, bounds-checked) ---- */

typedef struct {
    const uint8_t *p;
    size_t len;
    size_t pos;
} rdb_reader;

static int r_need(rdb_reader *r, size_t n) {
    if (n > r->len - r->pos) return -1;
    r->pos += n;
    return 0;
}

static int r_u8(rdb_reader *r, uint8_t *out) {
    if (r_need(r, 1) < 0) return -1;
    *out = r->p[r->pos - 1];
    return 0;
}

static int r_u32(rdb_reader *r, uint32_t *out) {
    if (r_need(r, 4) < 0) return -1;
    const uint8_t *t = r->p + r->pos - 4;
    *out = ((uint32_t)t[0] << 24) | ((uint32_t)t[1] << 16) |
           ((uint32_t)t[2] << 8) | (uint32_t)t[3];
    return 0;
}

static int r_s64(rdb_reader *r, int64_t *out) {
    if (r_need(r, 8) < 0) return -1;
    const uint8_t *t = r->p + r->pos - 8;
    uint64_t u = 0;
    for (int i = 0; i < 8; i++) u = (u << 8) | t[i];
    *out = (int64_t)u;
    return 0;
}

/* Returns a pointer into the buffer (not a copy). */
static int r_bytes(rdb_reader *r, const char **out, size_t *n) {
    uint32_t len;
    if (r_u32(r, &len) < 0) return -1;
    if (r_need(r, len) < 0) return -1;
    *out = (const char *)r->p + r->pos - len;
    *n = len;
    return 0;
}

static int r_f64(rdb_reader *r, double *out) {
    int64_t raw;
    if (r_s64(r, &raw) < 0) return -1;
    uint64_t u = (uint64_t)raw;
    memcpy(out, &u, sizeof(u));
    return 0;
}

static int rdb_read_obj(rdb_reader *r, obj_type t, robj **out) {
    switch (t) {
        case OBJ_STRING: {
            const char *data;
            size_t n;
            if (r_bytes(r, &data, &n) < 0) return -1;
            *out = robj_new_string(data, n);
            return 0;
        }
        case OBJ_LIST: {
            uint32_t cnt;
            if (r_u32(r, &cnt) < 0) return -1;
            list *l = list_create();
            for (uint32_t i = 0; i < cnt; i++) {
                const char *data;
                size_t n;
                if (r_bytes(r, &data, &n) < 0) {
                    list_free(l, free_robj_val);
                    return -1;
                }
                list_push_tail(l, robj_new_string(data, n));
            }
            robj *o = xmalloc(sizeof(*o));
            o->type = OBJ_LIST;
            o->ptr = l;
            o->len = 0;
            o->expire_at = -1;
            *out = o;
            return 0;
        }
        case OBJ_HASH: {
            uint32_t cnt;
            if (r_u32(r, &cnt) < 0) return -1;
            dict *h = dict_create();
            for (uint32_t i = 0; i < cnt; i++) {
                const char *fk, *fv;
                size_t flen, vlen;
                if (r_bytes(r, &fk, &flen) < 0 || r_bytes(r, &fv, &vlen) < 0) {
                    dict_free(h, free_robj_val);
                    return -1;
                }
                robj *v = robj_new_string(fv, vlen);
                robj *old = dict_set(h, fk, flen, v);
                robj_free(old);
            }
            robj *o = xmalloc(sizeof(*o));
            o->type = OBJ_HASH;
            o->ptr = h;
            o->len = 0;
            o->expire_at = -1;
            *out = o;
            return 0;
        }
        case OBJ_SET: {
            uint32_t cnt;
            if (r_u32(r, &cnt) < 0) return -1;
            dict *s = dict_create();
            for (uint32_t i = 0; i < cnt; i++) {
                const char *m;
                size_t mlen;
                if (r_bytes(r, &m, &mlen) < 0) {
                    dict_free(s, NULL);
                    return -1;
                }
                dict_set(s, m, mlen, (void *)1);
            }
            robj *o = xmalloc(sizeof(*o));
            o->type = OBJ_SET;
            o->ptr = s;
            o->len = 0;
            o->expire_at = -1;
            *out = o;
            return 0;
        }
        case OBJ_ZSET: {
            uint32_t cnt;
            if (r_u32(r, &cnt) < 0) return -1;
            zset *zs = zset_create();
            for (uint32_t i = 0; i < cnt; i++) {
                const char *m;
                size_t mlen;
                double score;
                if (r_bytes(r, &m, &mlen) < 0 || r_f64(r, &score) < 0) {
                    zset_free(zs);
                    return -1;
                }
                zset_add(zs, score, m, mlen, NULL);
            }
            robj *o = xmalloc(sizeof(*o));
            o->type = OBJ_ZSET;
            o->ptr = zs;
            o->len = 0;
            o->expire_at = -1;
            *out = o;
            return 0;
        }
        default:
            return -1;
    }
}

int rdb_load(const char *path, db *store) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        log_error("rdb: open %s: %s", path, strerror(errno));
        return -1;
    }

    dynbuf b;
    dynbuf_init(&b);
    char tmp[65536];
    size_t n;
    while ((n = fread(tmp, 1, sizeof(tmp), f)) > 0) dynbuf_append(&b, tmp, n);
    if (ferror(f)) {
        log_error("rdb: read error on %s", path);
        fclose(f);
        dynbuf_free(&b);
        return -1;
    }
    fclose(f);

    rdb_reader r = {(const uint8_t *)b.p, b.len, 0};

    if (b.len < 9 + 4 + 4 || memcmp(b.p, RDB_MAGIC, 9) != 0) {
        log_error("rdb: %s: bad magic", path);
        dynbuf_free(&b);
        return -1;
    }
    r.pos = 9;

    uint32_t version;
    if (r_u32(&r, &version) < 0 || version != RDB_VERSION) {
        log_error("rdb: %s: unsupported version", path);
        dynbuf_free(&b);
        return -1;
    }

    uint32_t count;
    if (r_u32(&r, &count) < 0) {
        log_error("rdb: %s: truncated header", path);
        dynbuf_free(&b);
        return -1;
    }

    size_t loaded = 0;
    for (uint32_t i = 0; i < count; i++) {
        const char *key;
        size_t klen;
        int64_t expire;
        uint8_t type;
        robj *o = NULL;

        if (r_bytes(&r, &key, &klen) < 0 || r_s64(&r, &expire) < 0 ||
            r_u8(&r, &type) < 0) {
            log_error("rdb: %s: corrupt key header at key %u", path, i);
            break;
        }
        if (rdb_read_obj(&r, (obj_type)type, &o) < 0) {
            log_error("rdb: %s: corrupt value at key %u", path, i);
            break;
        }
        o->expire_at = expire;

        robj *old = dict_set(store->d, key, klen, o);
        robj_free(old);
        loaded++;
    }

    dynbuf_free(&b);
    log_info("rdb: loaded %zu keys from %s", loaded, path);
    return 0;
}
