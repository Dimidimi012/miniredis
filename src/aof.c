#include "aof.h"

#include "dict.h"
#include "dynbuf.h"
#include "list.h"
#include "object.h"
#include "server.h"
#include "util.h"
#include "zskiplist.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

static int64_t aof_last_fsync_ms = 0;

int g_aof_rewriting = 0;

/* Commands executed while a background rewrite is in flight. They are
 * re-appended to the fresh AOF once the child has swapped the file in, so no
 * write is lost across the rewrite. */
static dynbuf g_rewrite_buf = {NULL, 0, 0};

int aof_open(const char *path) {
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) log_error("aof: open %s: %s", path, strerror(errno));
    else aof_last_fsync_ms = now_ms();
    return fd;
}

void aof_close(int fd) {
    if (fd < 0) return;
    if (fsync(fd) < 0) log_warn("aof: fsync: %s", strerror(errno));
    if (close(fd) < 0) log_warn("aof: close: %s", strerror(errno));
}

void aof_periodic(void) {
    if (g_aof_fd < 0) return;
    int64_t now = now_ms();
    if (now - aof_last_fsync_ms >= 1000) {
        if (fsync(g_aof_fd) < 0) log_warn("aof: fsync: %s", strerror(errno));
        aof_last_fsync_ms = now;
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

static void command_to_resp(const command *cmd, dynbuf *out) {
    dynbuf_appendf(out, "*%d\r\n", cmd->argc);
    for (int i = 0; i < cmd->argc; i++) {
        size_t l = strlen(cmd->argv[i]);
        dynbuf_appendf(out, "$%zu\r\n", l);
        dynbuf_append(out, cmd->argv[i], l);
        dynbuf_append_cstr(out, "\r\n");
    }
}

int aof_append_command(int fd, const command *cmd) {
    if (fd < 0) return -1;
    dynbuf b;
    dynbuf_init(&b);
    command_to_resp(cmd, &b);
    int rc = write_full(fd, b.p, b.len);
    if (rc == 0 && g_aof_rewriting) {
        /* mirror into the rewrite buffer so the write survives the swap */
        dynbuf_append(&g_rewrite_buf, b.p, b.len);
    }
    dynbuf_free(&b);
    return rc;
}

/* Close the current AOF fd and reopen the file at g_aof_path (used after a
 * rewrite swapped in a fresh file). Any commands buffered during the rewrite
 * are flushed to the new file first. */
int aof_reopen(void) {
    if (g_aof_fd >= 0) {
        if (close(g_aof_fd) < 0) log_warn("aof: close: %s", strerror(errno));
        g_aof_fd = -1;
    }
    g_aof_fd = aof_open(g_aof_path);
    if (g_aof_fd < 0) return -1;

    if (g_rewrite_buf.len > 0) {
        if (write_full(g_aof_fd, g_rewrite_buf.p, g_rewrite_buf.len) < 0) {
            log_error("aof: failed to flush rewrite buffer");
        } else {
            log_info("aof: flushed %zu bytes of rewrite buffer", g_rewrite_buf.len);
        }
        dynbuf_clear(&g_rewrite_buf);
    }
    return 0;
}

/* ---- rewrite: serialize the whole dataset as a compact command stream ---- */

static int aw_write_command(int fd, int argc, const char *const *argv, const size_t *lens) {
    dynbuf b;
    dynbuf_init(&b);
    dynbuf_appendf(&b, "*%d\r\n", argc);
    for (int i = 0; i < argc; i++) {
        dynbuf_appendf(&b, "$%zu\r\n", lens[i]);
        dynbuf_append(&b, argv[i], lens[i]);
        dynbuf_append_cstr(&b, "\r\n");
    }
    int rc = write_full(fd, b.p, b.len);
    dynbuf_free(&b);
    return rc;
}

static int aof_rewrite_key(int fd, const char *key, size_t klen, const robj *o) {
    char expire[32];
    int expire_argc = 0;
    const char *expire_argv[3];
    size_t expire_lens[3];

    if (o->expire_at >= 0) {
        int n = snprintf(expire, sizeof(expire), "%lld", (long long)o->expire_at);
        expire_argv[0] = "PEXPIREAT";
        expire_argv[1] = key;
        expire_argv[2] = expire;
        expire_lens[0] = 9;
        expire_lens[1] = klen;
        expire_lens[2] = (size_t)n;
        expire_argc = 3;
    }

    switch (o->type) {
        case OBJ_STRING: {
            const char *argv[5];
            size_t lens[5];
            argv[0] = "SET";
            lens[0] = 3;
            argv[1] = key;
            lens[1] = klen;
            argv[2] = (const char *)o->ptr;
            lens[2] = o->len;
            int argc = 3;
            if (expire_argc) {
                argv[3] = "PXAT";
                lens[3] = 4;
                argv[4] = expire;
                lens[4] = expire_lens[2];
                argc = 5;
            }
            return aw_write_command(fd, argc, argv, lens);
        }

        case OBJ_LIST: {
            const list *l = (const list *)o->ptr;
            const char *argv[66];
            size_t lens[66];
            argv[0] = "RPUSH";
            lens[0] = 5;
            argv[1] = key;
            lens[1] = klen;
            int i = 2;
            for (const list_node *n = list_first(l); n; n = list_next(n)) {
                const robj *v = (const robj *)n->val;
                if (i == 66) {
                    if (aw_write_command(fd, i, argv, lens) < 0) return -1;
                    i = 2;
                }
                argv[i] = (const char *)v->ptr;
                lens[i] = v->len;
                i++;
            }
            if (i > 2 && aw_write_command(fd, i, argv, lens) < 0) return -1;
            break;
        }

        case OBJ_HASH: {
            const dict *h = (const dict *)o->ptr;
            const char *argv[130];
            size_t lens[130];
            argv[0] = "HSET";
            lens[0] = 4;
            argv[1] = key;
            lens[1] = klen;
            int i = 2;
            dict_iter it;
            dict_iter_init(&it, h);
            const dict_entry *e;
            while ((e = dict_iter_next(&it)) != NULL) {
                const robj *v = (const robj *)e->val;
                if (i + 1 >= 130) {
                    if (aw_write_command(fd, i, argv, lens) < 0) return -1;
                    i = 2;
                }
                argv[i] = e->key;
                lens[i] = e->klen;
                argv[i + 1] = (const char *)v->ptr;
                lens[i + 1] = v->len;
                i += 2;
            }
            if (i > 2 && aw_write_command(fd, i, argv, lens) < 0) return -1;
            break;
        }

        case OBJ_SET: {
            const dict *s = (const dict *)o->ptr;
            const char *argv[66];
            size_t lens[66];
            argv[0] = "SADD";
            lens[0] = 4;
            argv[1] = key;
            lens[1] = klen;
            int i = 2;
            dict_iter it;
            dict_iter_init(&it, s);
            const dict_entry *e;
            while ((e = dict_iter_next(&it)) != NULL) {
                if (i == 66) {
                    if (aw_write_command(fd, i, argv, lens) < 0) return -1;
                    i = 2;
                }
                argv[i] = e->key;
                lens[i] = e->klen;
                i++;
            }
            if (i > 2 && aw_write_command(fd, i, argv, lens) < 0) return -1;
            break;
        }

        case OBJ_ZSET: {
            const zset *zs = (const zset *)o->ptr;
            const char *argv[130];
            size_t lens[130];
            char scorebuf[64][32];
            argv[0] = "ZADD";
            lens[0] = 4;
            argv[1] = key;
            lens[1] = klen;
            int i = 2, si = 0;
            for (const zskiplist_node *n = zs->zsl->header->level[0].forward;
                 n; n = n->level[0].forward) {
                if (i + 1 >= 130) {
                    if (aw_write_command(fd, i, argv, lens) < 0) return -1;
                    i = 2;
                    si = 0;
                }
                int sn = snprintf(scorebuf[si], sizeof(scorebuf[si]), "%.17g", n->score);
                argv[i] = scorebuf[si];
                lens[i] = (size_t)sn;
                argv[i + 1] = n->member;
                lens[i + 1] = n->mlen;
                i += 2;
                si++;
            }
            if (i > 2 && aw_write_command(fd, i, argv, lens) < 0) return -1;
            break;
        }
    }

    if (expire_argc && aw_write_command(fd, expire_argc, expire_argv, expire_lens) < 0) {
        return -1;
    }
    return 0;
}

int aof_rewrite(const char *path, const db *store) {
    size_t tmplen = strlen(path) + 16;
    char *tmppath = xmalloc(tmplen);
    snprintf(tmppath, tmplen, "%s.rewrite.tmp", path);

    int fd = open(tmppath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        log_error("aof: rewrite open %s: %s", tmppath, strerror(errno));
        free(tmppath);
        return -1;
    }

    int rc = 0;
    dict_iter it;
    dict_iter_init(&it, store->d);
    const dict_entry *e;
    while ((e = dict_iter_next(&it)) != NULL) {
        if (aof_rewrite_key(fd, e->key, e->klen, (const robj *)e->val) < 0) {
            rc = -1;
            break;
        }
    }

    if (rc == 0 && fsync(fd) < 0) rc = -1;
    if (close(fd) < 0) rc = -1;
    if (rc == 0 && rename(tmppath, path) < 0) {
        log_error("aof: rewrite rename: %s", strerror(errno));
        rc = -1;
    }

    free(tmppath);
    if (rc == 0) log_info("aof: rewrite complete");
    return rc;
}

int aof_replay(const char *path, db *store) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        log_error("aof: open %s: %s", path, strerror(errno));
        return -1;
    }

    dynbuf b;
    dynbuf_init(&b);
    char tmp[65536];
    size_t n;
    while ((n = fread(tmp, 1, sizeof(tmp), f)) > 0) dynbuf_append(&b, tmp, n);
    if (ferror(f)) {
        log_error("aof: read error on %s", path);
        fclose(f);
        dynbuf_free(&b);
        return -1;
    }
    fclose(f);

    if (b.len == 0) {
        dynbuf_free(&b);
        return 0;
    }

    /* Execute the recorded commands through the normal dispatch path, with a
     * throwaway client whose replies are discarded. */
    client *fake = xmalloc(sizeof(*fake));
    client_init(fake, -1);

    g_aof_replaying = 1;
    size_t pos = 0;
    int rc = 0;
    size_t cmds = 0;

    while (pos < b.len) {
        command cmd;
        command_init(&cmd);
        int consumed = resp_parse_command(b.p + pos, b.len - pos, &cmd);

        if (consumed > 0) {
            dispatch_command(store, fake, &cmd);
            command_free(&cmd);
            pos += (size_t)consumed;
            if (fake->closing) fake->closing = 0;
            dynbuf_clear(&fake->out);
            cmds++;
        } else if (consumed < 0) {
            log_error("aof: corrupt frame at offset %zu", pos);
            rc = -1;
            break;
        } else {
            log_warn("aof: truncated trailing frame at offset %zu, ignoring", pos);
            break;
        }
    }
    g_aof_replaying = 0;

    client_free(fake);
    dynbuf_free(&b);
    log_info("aof: replayed %zu commands from %s", cmds, path);
    return rc;
}
