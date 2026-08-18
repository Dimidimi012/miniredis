#include "db.h"

#include "object.h"
#include "server.h"
#include "util.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

struct db {
    dict *d;
};

static void free_obj(void *p) {
    robj_free((robj *)p);
}

db *db_create(void) {
    db *s = xmalloc(sizeof(*s));
    s->d = dict_create();
    return s;
}

void db_free(db *s) {
    if (!s) return;
    dict_free(s->d, free_obj);
    free(s);
}

size_t db_size(const db *s) {
    return dict_size(s->d);
}

/* ---- key lookup with lazy expiration ---- */

static int key_expired(const robj *o) {
    return o->expire_at >= 0 && o->expire_at <= now_ms();
}

static robj *lookup_key(db *store, const char *key, size_t klen) {
    robj *o = dict_get(store->d, key, klen);
    if (!o) return NULL;
    if (key_expired(o)) {
        robj *dead = dict_delete(store->d, key, klen);
        robj_free(dead);
        return NULL;
    }
    return o;
}

/* ---- command handlers ---- */

static void cmd_ping(db *store, client *c, command *cmd) {
    (void)store;
    if (cmd->argc > 2) {
        reply_error(c, "wrong number of arguments for 'ping' command");
        return;
    }
    if (cmd->argc == 2) reply_bulk_cstr(c, cmd->argv[1]);
    else reply_simple(c, "PONG");
}

static void cmd_echo(db *store, client *c, command *cmd) {
    (void)store;
    if (cmd->argc != 2) {
        reply_error(c, "wrong number of arguments for 'echo' command");
        return;
    }
    reply_bulk_cstr(c, cmd->argv[1]);
}

static void cmd_set(db *store, client *c, command *cmd) {
    if (cmd->argc < 3) {
        reply_error(c, "wrong number of arguments for 'set' command");
        return;
    }

    int nx = 0, xx = 0;
    int64_t expire_ms = -1;

    for (int i = 3; i < cmd->argc; i++) {
        const char *a = cmd->argv[i];
        if (!strcasecmp(a, "EX") || !strcasecmp(a, "PX") ||
            !strcasecmp(a, "EXAT") || !strcasecmp(a, "PXAT")) {
            if (i + 1 >= cmd->argc) {
                reply_error(c, "syntax error");
                return;
            }
            long long v;
            if (!string_to_ll(cmd->argv[i + 1], &v) || v <= 0) {
                reply_error(c, "invalid expire time in 'set' command");
                return;
            }
            if (!strcasecmp(a, "EX")) {
                if (v > INT64_MAX / 1000) {
                    reply_error(c, "invalid expire time in 'set' command");
                    return;
                }
                expire_ms = v * 1000;
            } else if (!strcasecmp(a, "PX")) {
                expire_ms = v;
            } else if (!strcasecmp(a, "EXAT")) {
                if (v > INT64_MAX / 1000) {
                    reply_error(c, "invalid expire time in 'set' command");
                    return;
                }
                expire_ms = v * 1000 - now_ms();
            } else { /* PXAT */
                expire_ms = v - now_ms();
            }
            i++;
        } else if (!strcasecmp(a, "NX")) {
            nx = 1;
        } else if (!strcasecmp(a, "XX")) {
            xx = 1;
        } else {
            reply_error(c, "syntax error");
            return;
        }
    }

    if (nx && xx) {
        reply_error(c, "syntax error");
        return;
    }

    robj *existing = lookup_key(store, cmd->argv[1], strlen(cmd->argv[1]));
    if (nx && existing) {
        reply_null(c);
        return;
    }
    if (xx && !existing) {
        reply_null(c);
        return;
    }

    robj *val = robj_new_string(cmd->argv[2], strlen(cmd->argv[2]));
    val->expire_at = (expire_ms < 0) ? -1 : now_ms() + expire_ms;

    robj *old = dict_set(store->d, cmd->argv[1], strlen(cmd->argv[1]), val);
    robj_free(old);
    reply_simple(c, "OK");
}

static void cmd_get(db *store, client *c, command *cmd) {
    if (cmd->argc != 2) {
        reply_error(c, "wrong number of arguments for 'get' command");
        return;
    }
    robj *o = lookup_key(store, cmd->argv[1], strlen(cmd->argv[1]));
    if (!o) {
        reply_null(c);
        return;
    }
    reply_bulk(c, o->ptr, o->len);
}

static void cmd_del(db *store, client *c, command *cmd) {
    if (cmd->argc < 2) {
        reply_error(c, "wrong number of arguments for 'del' command");
        return;
    }
    long long n = 0;
    for (int i = 1; i < cmd->argc; i++) {
        robj *dead = dict_delete(store->d, cmd->argv[i], strlen(cmd->argv[i]));
        if (dead) {
            robj_free(dead);
            n++;
        }
    }
    reply_integer(c, n);
}

static void cmd_exists(db *store, client *c, command *cmd) {
    if (cmd->argc < 2) {
        reply_error(c, "wrong number of arguments for 'exists' command");
        return;
    }
    long long n = 0;
    for (int i = 1; i < cmd->argc; i++) {
        if (lookup_key(store, cmd->argv[i], strlen(cmd->argv[i]))) n++;
    }
    reply_integer(c, n);
}

static void incr_decr(db *store, client *c, command *cmd, long long delta) {
    if (cmd->argc != 2) {
        reply_error(c, "wrong number of arguments for '%s' command", cmd->argv[0]);
        return;
    }

    robj *o = lookup_key(store, cmd->argv[1], strlen(cmd->argv[1]));
    long long v = 0;
    if (o && !string_to_ll_n(o->ptr, o->len, &v)) {
        reply_error(c, "value is not an integer or out of range");
        return;
    }

    long long nv = v + delta;
    if ((delta > 0 && nv < v) || (delta < 0 && nv > v)) {
        reply_error(c, "increment or decrement would overflow");
        return;
    }

    robj *val = robj_new_string_ll(nv);
    val->expire_at = o ? o->expire_at : -1;
    robj *old = dict_set(store->d, cmd->argv[1], strlen(cmd->argv[1]), val);
    robj_free(old);
    reply_integer(c, nv);
}

static void cmd_incr(db *store, client *c, command *cmd) { incr_decr(store, c, cmd, 1); }
static void cmd_decr(db *store, client *c, command *cmd) { incr_decr(store, c, cmd, -1); }

static void cmd_expire_generic(db *store, client *c, command *cmd, int is_ms) {
    if (cmd->argc != 3) {
        reply_error(c, "wrong number of arguments for '%s' command", cmd->argv[0]);
        return;
    }

    long long v;
    if (!string_to_ll(cmd->argv[2], &v) || v < 0) {
        reply_error(c, "invalid expire time in '%s' command", cmd->argv[0]);
        return;
    }

    robj *o = lookup_key(store, cmd->argv[1], strlen(cmd->argv[1]));
    if (!o) {
        reply_integer(c, 0);
        return;
    }

    if (!is_ms && v > INT64_MAX / 1000) {
        reply_error(c, "invalid expire time in '%s' command", cmd->argv[0]);
        return;
    }

    int64_t ms = is_ms ? v : v * 1000;
    o->expire_at = now_ms() + ms;
    reply_integer(c, 1);
}

static void cmd_expire(db *store, client *c, command *cmd) { cmd_expire_generic(store, c, cmd, 0); }
static void cmd_pexpire(db *store, client *c, command *cmd) { cmd_expire_generic(store, c, cmd, 1); }

static void cmd_ttl_generic(db *store, client *c, command *cmd, int is_ms) {
    if (cmd->argc != 2) {
        reply_error(c, "wrong number of arguments for '%s' command", cmd->argv[0]);
        return;
    }

    robj *o = lookup_key(store, cmd->argv[1], strlen(cmd->argv[1]));
    if (!o) {
        reply_integer(c, -2);
        return;
    }
    if (o->expire_at < 0) {
        reply_integer(c, -1);
        return;
    }

    int64_t rem = o->expire_at - now_ms();
    if (rem < 0) rem = 0;
    reply_integer(c, is_ms ? rem : (rem + 500) / 1000);
}

static void cmd_ttl(db *store, client *c, command *cmd) { cmd_ttl_generic(store, c, cmd, 0); }
static void cmd_pttl(db *store, client *c, command *cmd) { cmd_ttl_generic(store, c, cmd, 1); }

static void cmd_type(db *store, client *c, command *cmd) {
    if (cmd->argc != 2) {
        reply_error(c, "wrong number of arguments for 'type' command");
        return;
    }
    robj *o = lookup_key(store, cmd->argv[1], strlen(cmd->argv[1]));
    reply_simple(c, o ? "string" : "none");
}

static void cmd_keys(db *store, client *c, command *cmd) {
    if (cmd->argc != 2) {
        reply_error(c, "wrong number of arguments for 'keys' command");
        return;
    }

    const char *pat = cmd->argv[1];
    size_t count = 0;
    dict_iter it;
    dict_entry *e;

    dict_iter_init(&it, store->d);
    while ((e = dict_iter_next(&it)) != NULL) {
        if (!key_expired((const robj *)e->val) && util_glob_match(pat, e->key, e->klen)) count++;
    }

    reply_array_header(c, count);
    dict_iter_init(&it, store->d);
    while ((e = dict_iter_next(&it)) != NULL) {
        if (!key_expired((const robj *)e->val) && util_glob_match(pat, e->key, e->klen))
            reply_bulk(c, e->key, e->klen);
    }
}

static void cmd_flushall(db *store, client *c, command *cmd) {
    if (cmd->argc != 1) {
        reply_error(c, "wrong number of arguments for 'flushall' command");
        return;
    }
    dict *nd = dict_create();
    dict_free(store->d, free_obj);
    store->d = nd;
    reply_simple(c, "OK");
}

static void cmd_dbsize(db *store, client *c, command *cmd) {
    if (cmd->argc != 1) {
        reply_error(c, "wrong number of arguments for 'dbsize' command");
        return;
    }
    reply_integer(c, (long long)db_size(store));
}

static void cmd_info(db *store, client *c, command *cmd) {
    (void)cmd;
    int64_t uptime = (now_ms() - g_server_start_ms) / 1000;

    dynbuf b;
    dynbuf_init(&b);
    dynbuf_appendf(&b,
        "# Server\r\n"
        "redis_version:6.2.0-miniredis\r\n"
        "uptime_in_seconds:%lld\r\n"
        "\r\n"
        "# Keyspace\r\n"
        "db0:keys=%zu,expires=0,avg_ttl=0\r\n",
        (long long)uptime, db_size(store));
    reply_bulk(c, b.p, b.len);
    dynbuf_free(&b);
}

static void cmd_command(db *store, client *c, command *cmd) {
    (void)store;
    (void)cmd;
    /* Bare COMMAND / subcommands: return an empty command list. */
    reply_array_header(c, 0);
}

static void cmd_select(db *store, client *c, command *cmd) {
    (void)store;
    if (cmd->argc != 2) {
        reply_error(c, "wrong number of arguments for 'select' command");
        return;
    }
    long long idx;
    if (!string_to_ll(cmd->argv[1], &idx) || idx < 0) {
        reply_error(c, "invalid DB index");
        return;
    }
    reply_simple(c, "OK");   /* single logical database */
}

static void cmd_quit(db *store, client *c, command *cmd) {
    (void)store;
    (void)cmd;
    reply_simple(c, "OK");
    c->closing = 1;
}

/* ---- dispatch ---- */

typedef struct {
    const char *name;
    void (*fn)(db *, client *, command *);
} cmd_entry;

static const cmd_entry commands[] = {
    {"ping",     cmd_ping},
    {"echo",     cmd_echo},
    {"set",      cmd_set},
    {"get",      cmd_get},
    {"del",      cmd_del},
    {"exists",   cmd_exists},
    {"incr",     cmd_incr},
    {"decr",     cmd_decr},
    {"expire",   cmd_expire},
    {"pexpire",  cmd_pexpire},
    {"ttl",      cmd_ttl},
    {"pttl",     cmd_pttl},
    {"type",     cmd_type},
    {"keys",     cmd_keys},
    {"flushall", cmd_flushall},
    {"dbsize",   cmd_dbsize},
    {"info",     cmd_info},
    {"command",  cmd_command},
    {"select",   cmd_select},
    {"quit",     cmd_quit},
};

void dispatch_command(db *store, client *c, command *cmd) {
    if (cmd->argc == 0) return;

    const char *name = cmd->argv[0];
    for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
        if (!strcasecmp(commands[i].name, name)) {
            commands[i].fn(store, c, cmd);
            return;
        }
    }
    reply_error(c, "unknown command '%s'", name);
}
