#include "db.h"

#include "list.h"
#include "object.h"
#include "server.h"
#include "util.h"
#include "zskiplist.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
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

/* ---- shared helpers ---- */

static void reply_wrongtype(client *c) {
    reply_error(c, "WRONGTYPE Operation against a key holding the wrong kind of value");
}

/* Delete the key when its container type became empty (Redis semantics). */
static void maybe_delete_key(db *store, const char *key, robj *o) {
    int empty = 0;
    switch (o->type) {
        case OBJ_LIST: empty = (list_len((list *)o->ptr) == 0); break;
        case OBJ_HASH: empty = (dict_size((dict *)o->ptr) == 0); break;
        case OBJ_SET:  empty = (dict_size((dict *)o->ptr) == 0); break;
        case OBJ_ZSET: empty = (zset_len((zset *)o->ptr) == 0); break;
        default: return;
    }
    if (empty) {
        robj *dead = dict_delete(store->d, key, strlen(key));
        robj_free(dead);
    }
}

static int parse_double(const char *s, double *out) {
    if (!s || !*s) return 0;
    char *end = NULL;
    errno = 0;
    double d = strtod(s, &end);
    if (errno == ERANGE) return 0;
    if (end == s || *end != '\0') return 0;
    if (d != d) return 0;   /* NaN */
    *out = d;
    return 1;
}

/* Score-range argument: an optional '(' prefix marks the bound exclusive. */
static int parse_score_arg(const char *arg, double *val, int *exclusive) {
    *exclusive = 0;
    if (arg[0] == '(') {
        *exclusive = 1;
        arg++;
    }
    return parse_double(arg, val);
}

static void reply_double(client *c, double d) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%.17g", d);
    reply_bulk_cstr(c, buf);
}

static const char *type_name(obj_type t) {
    switch (t) {
        case OBJ_STRING: return "string";
        case OBJ_LIST:   return "list";
        case OBJ_HASH:   return "hash";
        case OBJ_SET:    return "set";
        case OBJ_ZSET:   return "zset";
    }
    return "none";
}

/* Normalize LRANGE-style start/stop against a length. Sets *empty when the
 * requested range is void. */
static void range_normalize(long long len, long long *start, long long *stop, int *empty) {
    if (*start < 0) *start = len + *start;
    if (*stop < 0) *stop = len + *stop;
    if (*start < 0) *start = 0;
    if (*start > *stop || *start >= len) {
        *empty = 1;
        return;
    }
    if (*stop >= len) *stop = len - 1;
    *empty = 0;
}

/* ---- string / generic commands ---- */

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
    if (o->type != OBJ_STRING) {
        reply_wrongtype(c);
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
    if (o && o->type != OBJ_STRING) {
        reply_wrongtype(c);
        return;
    }

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
    reply_simple(c, o ? type_name(o->type) : "none");
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

/* ---- list commands ---- */

static void list_push(db *store, client *c, command *cmd, int head, int xx) {
    if (cmd->argc < 3) {
        reply_error(c, "wrong number of arguments for '%s' command", cmd->argv[0]);
        return;
    }
    robj *o = lookup_key(store, cmd->argv[1], strlen(cmd->argv[1]));
    if (o && o->type != OBJ_LIST) {
        reply_wrongtype(c);
        return;
    }
    if (!o) {
        if (xx) {
            reply_integer(c, 0);
            return;
        }
        o = robj_new_list();
        dict_set(store->d, cmd->argv[1], strlen(cmd->argv[1]), o);
    }
    list *l = (list *)o->ptr;
    for (int i = 2; i < cmd->argc; i++) {
        robj *v = robj_new_string(cmd->argv[i], strlen(cmd->argv[i]));
        if (head) list_push_head(l, v);
        else list_push_tail(l, v);
    }
    reply_integer(c, (long long)list_len(l));
}

static void cmd_lpush(db *store, client *c, command *cmd) { list_push(store, c, cmd, 1, 0); }
static void cmd_rpush(db *store, client *c, command *cmd) { list_push(store, c, cmd, 0, 0); }
static void cmd_lpushx(db *store, client *c, command *cmd) { list_push(store, c, cmd, 1, 1); }
static void cmd_rpushx(db *store, client *c, command *cmd) { list_push(store, c, cmd, 0, 1); }

static void list_pop(db *store, client *c, command *cmd, int head) {
    if (cmd->argc < 2 || cmd->argc > 3) {
        reply_error(c, "wrong number of arguments for '%s' command", cmd->argv[0]);
        return;
    }
    long long count = 1;
    if (cmd->argc == 3) {
        if (!string_to_ll(cmd->argv[2], &count) || count < 0) {
            reply_error(c, "value is out of range, must be positive");
            return;
        }
    }

    robj *o = lookup_key(store, cmd->argv[1], strlen(cmd->argv[1]));
    if (o && o->type != OBJ_LIST) {
        reply_wrongtype(c);
        return;
    }
    if (!o) {
        reply_null(c);
        return;
    }

    list *l = (list *)o->ptr;
    if (count == 1) {
        robj *v = head ? list_pop_head(l) : list_pop_tail(l);
        if (!v) {
            maybe_delete_key(store, cmd->argv[1], o);
            reply_null(c);
            return;
        }
        reply_bulk(c, v->ptr, v->len);
        robj_free(v);
        maybe_delete_key(store, cmd->argv[1], o);
        return;
    }

    long long max = count < (long long)list_len(l) ? count : (long long)list_len(l);
    robj **items = xmalloc((size_t)max * sizeof(robj *));
    long long popped = 0;
    for (long long i = 0; i < max; i++) {
        robj *v = head ? list_pop_head(l) : list_pop_tail(l);
        if (!v) break;
        items[popped++] = v;
    }
    reply_array_header(c, (size_t)popped);
    for (long long i = 0; i < popped; i++) {
        reply_bulk(c, items[i]->ptr, items[i]->len);
        robj_free(items[i]);
    }
    free(items);
    maybe_delete_key(store, cmd->argv[1], o);
}

static void cmd_lpop(db *store, client *c, command *cmd) { list_pop(store, c, cmd, 1); }
static void cmd_rpop(db *store, client *c, command *cmd) { list_pop(store, c, cmd, 0); }

static void cmd_llen(db *store, client *c, command *cmd) {
    if (cmd->argc != 2) {
        reply_error(c, "wrong number of arguments for 'llen' command");
        return;
    }
    robj *o = lookup_key(store, cmd->argv[1], strlen(cmd->argv[1]));
    if (o && o->type != OBJ_LIST) {
        reply_wrongtype(c);
        return;
    }
    reply_integer(c, o ? (long long)list_len((list *)o->ptr) : 0);
}

static void cmd_lrange(db *store, client *c, command *cmd) {
    if (cmd->argc != 4) {
        reply_error(c, "wrong number of arguments for 'lrange' command");
        return;
    }
    long long start, stop;
    if (!string_to_ll(cmd->argv[2], &start) || !string_to_ll(cmd->argv[3], &stop)) {
        reply_error(c, "value is not an integer or out of range");
        return;
    }
    robj *o = lookup_key(store, cmd->argv[1], strlen(cmd->argv[1]));
    if (o && o->type != OBJ_LIST) {
        reply_wrongtype(c);
        return;
    }
    if (!o) {
        reply_array_header(c, 0);
        return;
    }
    list *l = (list *)o->ptr;
    long long len = (long long)list_len(l);
    int empty = 0;
    range_normalize(len, &start, &stop, &empty);
    if (empty) {
        reply_array_header(c, 0);
        return;
    }
    long long n = stop - start + 1;
    reply_array_header(c, (size_t)n);
    list_node *x = list_node_at(l, start);
    for (long long i = 0; i < n; i++) {
        robj *v = (robj *)x->val;
        reply_bulk(c, v->ptr, v->len);
        x = x->next;
    }
}

static void cmd_lindex(db *store, client *c, command *cmd) {
    if (cmd->argc != 3) {
        reply_error(c, "wrong number of arguments for 'lindex' command");
        return;
    }
    long long index;
    if (!string_to_ll(cmd->argv[2], &index)) {
        reply_error(c, "value is not an integer or out of range");
        return;
    }
    robj *o = lookup_key(store, cmd->argv[1], strlen(cmd->argv[1]));
    if (o && o->type != OBJ_LIST) {
        reply_wrongtype(c);
        return;
    }
    if (!o) {
        reply_null(c);
        return;
    }
    list_node *x = list_node_at((list *)o->ptr, index);
    if (!x) {
        reply_null(c);
        return;
    }
    robj *v = (robj *)x->val;
    reply_bulk(c, v->ptr, v->len);
}

static void cmd_lset(db *store, client *c, command *cmd) {
    if (cmd->argc != 4) {
        reply_error(c, "wrong number of arguments for 'lset' command");
        return;
    }
    long long index;
    if (!string_to_ll(cmd->argv[2], &index)) {
        reply_error(c, "value is not an integer or out of range");
        return;
    }
    robj *o = lookup_key(store, cmd->argv[1], strlen(cmd->argv[1]));
    if (!o) {
        reply_error(c, "no such key");
        return;
    }
    if (o->type != OBJ_LIST) {
        reply_wrongtype(c);
        return;
    }
    list_node *x = list_node_at((list *)o->ptr, index);
    if (!x) {
        reply_error(c, "index out of range");
        return;
    }
    robj *old = (robj *)x->val;
    robj_free(old);
    x->val = robj_new_string(cmd->argv[3], strlen(cmd->argv[3]));
    reply_simple(c, "OK");
}

static void cmd_ltrim(db *store, client *c, command *cmd) {
    if (cmd->argc != 4) {
        reply_error(c, "wrong number of arguments for 'ltrim' command");
        return;
    }
    long long start, stop;
    if (!string_to_ll(cmd->argv[2], &start) || !string_to_ll(cmd->argv[3], &stop)) {
        reply_error(c, "value is not an integer or out of range");
        return;
    }
    robj *o = lookup_key(store, cmd->argv[1], strlen(cmd->argv[1]));
    if (o && o->type != OBJ_LIST) {
        reply_wrongtype(c);
        return;
    }
    if (!o) {
        reply_simple(c, "OK");
        return;
    }
    list *l = (list *)o->ptr;
    long long len = (long long)list_len(l);
    int empty = 0;
    range_normalize(len, &start, &stop, &empty);
    if (empty) {
        robj *dead = dict_delete(store->d, cmd->argv[1], strlen(cmd->argv[1]));
        robj_free(dead);
        reply_simple(c, "OK");
        return;
    }

    list_node *x = list_first(l);
    long long i = 0;
    while (x && i < start) {
        list_node *next = x->next;
        robj *v = (robj *)list_detach(l, x);
        robj_free(v);
        x = next;
        i++;
    }
    long long target = stop - start + 1;
    while ((long long)list_len(l) > target) {
        robj *v = (robj *)list_pop_tail(l);
        robj_free(v);
    }
    reply_simple(c, "OK");
}

static void cmd_lrem(db *store, client *c, command *cmd) {
    if (cmd->argc != 4) {
        reply_error(c, "wrong number of arguments for 'lrem' command");
        return;
    }
    long long count;
    if (!string_to_ll(cmd->argv[2], &count)) {
        reply_error(c, "value is not an integer or out of range");
        return;
    }
    robj *o = lookup_key(store, cmd->argv[1], strlen(cmd->argv[1]));
    if (o && o->type != OBJ_LIST) {
        reply_wrongtype(c);
        return;
    }
    if (!o) {
        reply_integer(c, 0);
        return;
    }
    list *l = (list *)o->ptr;
    const char *target = cmd->argv[3];
    size_t tlen = strlen(target);
    long long removed = 0;

    if (count >= 0) {
        list_node *x = list_first(l);
        while (x && (count == 0 || removed < count)) {
            list_node *next = x->next;
            robj *v = (robj *)x->val;
            if (v->len == tlen && memcmp(v->ptr, target, tlen) == 0) {
                robj_free((robj *)list_detach(l, x));
                removed++;
            }
            x = next;
        }
    } else {
        list_node *x = list_last(l);
        while (x && removed < -count) {
            list_node *prev = x->prev;
            robj *v = (robj *)x->val;
            if (v->len == tlen && memcmp(v->ptr, target, tlen) == 0) {
                robj_free((robj *)list_detach(l, x));
                removed++;
            }
            x = prev;
        }
    }
    reply_integer(c, removed);
    maybe_delete_key(store, cmd->argv[1], o);
}

static void cmd_linsert(db *store, client *c, command *cmd) {
    if (cmd->argc != 5) {
        reply_error(c, "wrong number of arguments for 'linsert' command");
        return;
    }
    int before = 0;
    if (!strcasecmp(cmd->argv[2], "BEFORE")) before = 1;
    else if (!strcasecmp(cmd->argv[2], "AFTER")) before = 0;
    else {
        reply_error(c, "syntax error");
        return;
    }
    robj *o = lookup_key(store, cmd->argv[1], strlen(cmd->argv[1]));
    if (o && o->type != OBJ_LIST) {
        reply_wrongtype(c);
        return;
    }
    if (!o) {
        reply_integer(c, 0);
        return;
    }
    list *l = (list *)o->ptr;
    const char *pivot = cmd->argv[3];
    size_t plen = strlen(pivot);
    for (list_node *x = list_first(l); x; x = x->next) {
        robj *v = (robj *)x->val;
        if (v->len == plen && memcmp(v->ptr, pivot, plen) == 0) {
            robj *nv = robj_new_string(cmd->argv[4], strlen(cmd->argv[4]));
            if (before) list_insert_after(l, x->prev, nv);
            else list_insert_after(l, x, nv);
            reply_integer(c, (long long)list_len(l));
            return;
        }
    }
    reply_integer(c, -1);   /* pivot not found */
}

/* ---- hash commands ---- */

static void hash_set_many(db *store, client *c, command *cmd, int reply_ok) {
    if (cmd->argc < 4 || ((cmd->argc - 2) & 1)) {
        reply_error(c, "wrong number of arguments for '%s' command", cmd->argv[0]);
        return;
    }
    robj *o = lookup_key(store, cmd->argv[1], strlen(cmd->argv[1]));
    if (o && o->type != OBJ_HASH) {
        reply_wrongtype(c);
        return;
    }
    if (!o) {
        o = robj_new_hash();
        dict_set(store->d, cmd->argv[1], strlen(cmd->argv[1]), o);
    }
    dict *h = (dict *)o->ptr;
    long long added = 0;
    for (int i = 2; i < cmd->argc; i += 2) {
        const char *f = cmd->argv[i];
        size_t flen = strlen(f);
        if (!dict_get(h, f, flen)) added++;
        robj *v = robj_new_string(cmd->argv[i + 1], strlen(cmd->argv[i + 1]));
        robj *old = dict_set(h, f, flen, v);
        robj_free(old);
    }
    if (reply_ok) reply_simple(c, "OK");
    else reply_integer(c, added);
}

static void cmd_hset(db *store, client *c, command *cmd) { hash_set_many(store, c, cmd, 0); }
static void cmd_hmset(db *store, client *c, command *cmd) { hash_set_many(store, c, cmd, 1); }

static void cmd_hget(db *store, client *c, command *cmd) {
    if (cmd->argc != 3) {
        reply_error(c, "wrong number of arguments for 'hget' command");
        return;
    }
    robj *o = lookup_key(store, cmd->argv[1], strlen(cmd->argv[1]));
    if (o && o->type != OBJ_HASH) {
        reply_wrongtype(c);
        return;
    }
    if (!o) {
        reply_null(c);
        return;
    }
    robj *v = dict_get((dict *)o->ptr, cmd->argv[2], strlen(cmd->argv[2]));
    if (!v) {
        reply_null(c);
        return;
    }
    reply_bulk(c, v->ptr, v->len);
}

static void cmd_hmget(db *store, client *c, command *cmd) {
    if (cmd->argc < 3) {
        reply_error(c, "wrong number of arguments for 'hmget' command");
        return;
    }
    robj *o = lookup_key(store, cmd->argv[1], strlen(cmd->argv[1]));
    if (o && o->type != OBJ_HASH) {
        reply_wrongtype(c);
        return;
    }
    dict *h = o ? (dict *)o->ptr : NULL;
    reply_array_header(c, (size_t)(cmd->argc - 2));
    for (int i = 2; i < cmd->argc; i++) {
        robj *v = h ? dict_get(h, cmd->argv[i], strlen(cmd->argv[i])) : NULL;
        if (v) reply_bulk(c, v->ptr, v->len);
        else reply_null(c);
    }
}

static void cmd_hgetall(db *store, client *c, command *cmd) {
    if (cmd->argc != 2) {
        reply_error(c, "wrong number of arguments for 'hgetall' command");
        return;
    }
    robj *o = lookup_key(store, cmd->argv[1], strlen(cmd->argv[1]));
    if (o && o->type != OBJ_HASH) {
        reply_wrongtype(c);
        return;
    }
    dict *h = o ? (dict *)o->ptr : NULL;
    size_t n = h ? dict_size(h) : 0;
    reply_array_header(c, n * 2);
    if (h) {
        dict_iter it;
        dict_iter_init(&it, h);
        dict_entry *e;
        while ((e = dict_iter_next(&it)) != NULL) {
            robj *v = (robj *)e->val;
            reply_bulk(c, e->key, e->klen);
            reply_bulk(c, v->ptr, v->len);
        }
    }
}

static void hash_keys_or_vals(db *store, client *c, command *cmd, int vals) {
    if (cmd->argc != 2) {
        reply_error(c, "wrong number of arguments for '%s' command", cmd->argv[0]);
        return;
    }
    robj *o = lookup_key(store, cmd->argv[1], strlen(cmd->argv[1]));
    if (o && o->type != OBJ_HASH) {
        reply_wrongtype(c);
        return;
    }
    dict *h = o ? (dict *)o->ptr : NULL;
    size_t n = h ? dict_size(h) : 0;
    reply_array_header(c, n);
    if (h) {
        dict_iter it;
        dict_iter_init(&it, h);
        dict_entry *e;
        while ((e = dict_iter_next(&it)) != NULL) {
            if (vals) {
                robj *v = (robj *)e->val;
                reply_bulk(c, v->ptr, v->len);
            } else {
                reply_bulk(c, e->key, e->klen);
            }
        }
    }
}

static void cmd_hkeys(db *store, client *c, command *cmd) { hash_keys_or_vals(store, c, cmd, 0); }
static void cmd_hvals(db *store, client *c, command *cmd) { hash_keys_or_vals(store, c, cmd, 1); }

static void cmd_hlen(db *store, client *c, command *cmd) {
    if (cmd->argc != 2) {
        reply_error(c, "wrong number of arguments for 'hlen' command");
        return;
    }
    robj *o = lookup_key(store, cmd->argv[1], strlen(cmd->argv[1]));
    if (o && o->type != OBJ_HASH) {
        reply_wrongtype(c);
        return;
    }
    reply_integer(c, o ? (long long)dict_size((dict *)o->ptr) : 0);
}

static void cmd_hexists(db *store, client *c, command *cmd) {
    if (cmd->argc != 3) {
        reply_error(c, "wrong number of arguments for 'hexists' command");
        return;
    }
    robj *o = lookup_key(store, cmd->argv[1], strlen(cmd->argv[1]));
    if (o && o->type != OBJ_HASH) {
        reply_wrongtype(c);
        return;
    }
    int exists = o && dict_get((dict *)o->ptr, cmd->argv[2], strlen(cmd->argv[2]));
    reply_integer(c, exists ? 1 : 0);
}

static void cmd_hdel(db *store, client *c, command *cmd) {
    if (cmd->argc < 3) {
        reply_error(c, "wrong number of arguments for 'hdel' command");
        return;
    }
    robj *o = lookup_key(store, cmd->argv[1], strlen(cmd->argv[1]));
    if (o && o->type != OBJ_HASH) {
        reply_wrongtype(c);
        return;
    }
    if (!o) {
        reply_integer(c, 0);
        return;
    }
    dict *h = (dict *)o->ptr;
    long long removed = 0;
    for (int i = 2; i < cmd->argc; i++) {
        robj *v = dict_delete(h, cmd->argv[i], strlen(cmd->argv[i]));
        if (v) {
            robj_free(v);
            removed++;
        }
    }
    reply_integer(c, removed);
    maybe_delete_key(store, cmd->argv[1], o);
}

static void cmd_hsetnx(db *store, client *c, command *cmd) {
    if (cmd->argc != 4) {
        reply_error(c, "wrong number of arguments for 'hsetnx' command");
        return;
    }
    robj *o = lookup_key(store, cmd->argv[1], strlen(cmd->argv[1]));
    if (o && o->type != OBJ_HASH) {
        reply_wrongtype(c);
        return;
    }
    if (!o) {
        o = robj_new_hash();
        dict_set(store->d, cmd->argv[1], strlen(cmd->argv[1]), o);
    }
    dict *h = (dict *)o->ptr;
    if (dict_get(h, cmd->argv[2], strlen(cmd->argv[2]))) {
        reply_integer(c, 0);
        return;
    }
    robj *v = robj_new_string(cmd->argv[3], strlen(cmd->argv[3]));
    robj *old = dict_set(h, cmd->argv[2], strlen(cmd->argv[2]), v);
    robj_free(old);
    reply_integer(c, 1);
}

static void cmd_hincrby(db *store, client *c, command *cmd) {
    if (cmd->argc != 4) {
        reply_error(c, "wrong number of arguments for 'hincrby' command");
        return;
    }
    long long delta;
    if (!string_to_ll(cmd->argv[3], &delta)) {
        reply_error(c, "value is not an integer or out of range");
        return;
    }
    robj *o = lookup_key(store, cmd->argv[1], strlen(cmd->argv[1]));
    if (o && o->type != OBJ_HASH) {
        reply_wrongtype(c);
        return;
    }
    if (!o) {
        o = robj_new_hash();
        dict_set(store->d, cmd->argv[1], strlen(cmd->argv[1]), o);
    }
    dict *h = (dict *)o->ptr;
    robj *v = dict_get(h, cmd->argv[2], strlen(cmd->argv[2]));
    long long cur = 0;
    if (v && !string_to_ll_n(v->ptr, v->len, &cur)) {
        reply_error(c, "hash value is not an integer");
        return;
    }
    long long nv = cur + delta;
    if ((delta > 0 && nv < cur) || (delta < 0 && nv > cur)) {
        reply_error(c, "increment or decrement would overflow");
        return;
    }
    robj *val = robj_new_string_ll(nv);
    robj *old = dict_set(h, cmd->argv[2], strlen(cmd->argv[2]), val);
    robj_free(old);
    reply_integer(c, nv);
}

static void cmd_hincrbyfloat(db *store, client *c, command *cmd) {
    if (cmd->argc != 4) {
        reply_error(c, "wrong number of arguments for 'hincrbyfloat' command");
        return;
    }
    double delta;
    if (!parse_double(cmd->argv[3], &delta)) {
        reply_error(c, "value is not a valid float");
        return;
    }
    robj *o = lookup_key(store, cmd->argv[1], strlen(cmd->argv[1]));
    if (o && o->type != OBJ_HASH) {
        reply_wrongtype(c);
        return;
    }
    if (!o) {
        o = robj_new_hash();
        dict_set(store->d, cmd->argv[1], strlen(cmd->argv[1]), o);
    }
    dict *h = (dict *)o->ptr;
    robj *v = dict_get(h, cmd->argv[2], strlen(cmd->argv[2]));
    double cur = 0;
    if (v && !parse_double(v->ptr, &cur)) {
        reply_error(c, "hash value is not a float");
        return;
    }
    double nv = cur + delta;
    if (nv != nv || nv == 1.0 / 0.0 || nv == -1.0 / 0.0) {
        reply_error(c, "increment would produce NaN or Infinity");
        return;
    }
    char buf[64];
    snprintf(buf, sizeof(buf), "%.17g", nv);
    robj *val = robj_new_string(buf, strlen(buf));
    robj *old = dict_set(h, cmd->argv[2], strlen(cmd->argv[2]), val);
    robj_free(old);
    reply_bulk_cstr(c, buf);
}

/* ---- set commands ---- */

#define SET_VAL ((void *)1)

static void cmd_sadd(db *store, client *c, command *cmd) {
    if (cmd->argc < 3) {
        reply_error(c, "wrong number of arguments for 'sadd' command");
        return;
    }
    robj *o = lookup_key(store, cmd->argv[1], strlen(cmd->argv[1]));
    if (o && o->type != OBJ_SET) {
        reply_wrongtype(c);
        return;
    }
    if (!o) {
        o = robj_new_set();
        dict_set(store->d, cmd->argv[1], strlen(cmd->argv[1]), o);
    }
    dict *s = (dict *)o->ptr;
    long long added = 0;
    for (int i = 2; i < cmd->argc; i++) {
        if (!dict_get(s, cmd->argv[i], strlen(cmd->argv[i]))) {
            dict_set(s, cmd->argv[i], strlen(cmd->argv[i]), SET_VAL);
            added++;
        }
    }
    reply_integer(c, added);
}

static void cmd_srem(db *store, client *c, command *cmd) {
    if (cmd->argc < 3) {
        reply_error(c, "wrong number of arguments for 'srem' command");
        return;
    }
    robj *o = lookup_key(store, cmd->argv[1], strlen(cmd->argv[1]));
    if (o && o->type != OBJ_SET) {
        reply_wrongtype(c);
        return;
    }
    if (!o) {
        reply_integer(c, 0);
        return;
    }
    dict *s = (dict *)o->ptr;
    long long removed = 0;
    for (int i = 2; i < cmd->argc; i++) {
        if (dict_delete(s, cmd->argv[i], strlen(cmd->argv[i]))) removed++;
    }
    reply_integer(c, removed);
    maybe_delete_key(store, cmd->argv[1], o);
}

static void cmd_sismember(db *store, client *c, command *cmd) {
    if (cmd->argc != 3) {
        reply_error(c, "wrong number of arguments for 'sismember' command");
        return;
    }
    robj *o = lookup_key(store, cmd->argv[1], strlen(cmd->argv[1]));
    if (o && o->type != OBJ_SET) {
        reply_wrongtype(c);
        return;
    }
    int is = o && dict_get((dict *)o->ptr, cmd->argv[2], strlen(cmd->argv[2]));
    reply_integer(c, is ? 1 : 0);
}

static void cmd_scard(db *store, client *c, command *cmd) {
    if (cmd->argc != 2) {
        reply_error(c, "wrong number of arguments for 'scard' command");
        return;
    }
    robj *o = lookup_key(store, cmd->argv[1], strlen(cmd->argv[1]));
    if (o && o->type != OBJ_SET) {
        reply_wrongtype(c);
        return;
    }
    reply_integer(c, o ? (long long)dict_size((dict *)o->ptr) : 0);
}

static void cmd_smembers(db *store, client *c, command *cmd) {
    if (cmd->argc != 2) {
        reply_error(c, "wrong number of arguments for 'smembers' command");
        return;
    }
    robj *o = lookup_key(store, cmd->argv[1], strlen(cmd->argv[1]));
    if (o && o->type != OBJ_SET) {
        reply_wrongtype(c);
        return;
    }
    dict *s = o ? (dict *)o->ptr : NULL;
    size_t n = s ? dict_size(s) : 0;
    reply_array_header(c, n);
    if (s) {
        dict_iter it;
        dict_iter_init(&it, s);
        dict_entry *e;
        while ((e = dict_iter_next(&it)) != NULL) {
            reply_bulk(c, e->key, e->klen);
        }
    }
}

static void cmd_spop(db *store, client *c, command *cmd) {
    if (cmd->argc < 2 || cmd->argc > 3) {
        reply_error(c, "wrong number of arguments for 'spop' command");
        return;
    }
    long long count = 1;
    if (cmd->argc == 3) {
        if (!string_to_ll(cmd->argv[2], &count) || count < 0) {
            reply_error(c, "value is out of range, must be positive");
            return;
        }
    }
    robj *o = lookup_key(store, cmd->argv[1], strlen(cmd->argv[1]));
    if (o && o->type != OBJ_SET) {
        reply_wrongtype(c);
        return;
    }
    if (!o) {
        if (count == 1) reply_null(c);
        else reply_array_header(c, 0);
        return;
    }
    dict *s = (dict *)o->ptr;

    if (count == 1) {
        dict_entry *e = dict_random_entry(s);
        if (!e) {
            reply_null(c);
            return;
        }
        reply_bulk(c, e->key, e->klen);
        (void)dict_delete(s, e->key, e->klen);
    } else {
        long long max = count < (long long)dict_size(s) ? count : (long long)dict_size(s);
        reply_array_header(c, (size_t)max);
        for (long long i = 0; i < max; i++) {
            dict_entry *e = dict_random_entry(s);
            if (!e) break;
            reply_bulk(c, e->key, e->klen);
            (void)dict_delete(s, e->key, e->klen);
        }
    }
    maybe_delete_key(store, cmd->argv[1], o);
}

/* Fetch set argv[i] into *out (NULL when the key is missing). Returns -1 and
 * queues a WRONGTYPE error when the key exists with another type. */
static int set_arg_get(db *store, client *c, command *cmd, int i, dict **out) {
    robj *o = lookup_key(store, cmd->argv[i], strlen(cmd->argv[i]));
    if (o && o->type != OBJ_SET) {
        reply_wrongtype(c);
        return -1;
    }
    *out = o ? (dict *)o->ptr : NULL;
    return 0;
}

static void cmd_sinter(db *store, client *c, command *cmd) {
    if (cmd->argc < 2) {
        reply_error(c, "wrong number of arguments for 'sinter' command");
        return;
    }
    size_t nargs = (size_t)(cmd->argc - 1);
    dict **sets = xmalloc(nargs * sizeof(dict *));
    int has_empty = 0;
    for (size_t i = 0; i < nargs; i++) {
        if (set_arg_get(store, c, cmd, (int)i + 1, &sets[i]) < 0) {
            free(sets);
            return;
        }
        if (!sets[i]) has_empty = 1;
    }
    if (has_empty) {
        reply_array_header(c, 0);
        free(sets);
        return;
    }

    dict *smallest = sets[0];
    for (size_t i = 1; i < nargs; i++) {
        if (dict_size(sets[i]) < dict_size(smallest)) smallest = sets[i];
    }

    size_t count = 0;
    dict_iter it;
    dict_entry *e;
    dict_iter_init(&it, smallest);
    while ((e = dict_iter_next(&it)) != NULL) {
        int all = 1;
        for (size_t i = 0; i < nargs; i++) {
            if (sets[i] != smallest && !dict_get(sets[i], e->key, e->klen)) {
                all = 0;
                break;
            }
        }
        if (all) count++;
    }
    reply_array_header(c, count);
    dict_iter_init(&it, smallest);
    while ((e = dict_iter_next(&it)) != NULL) {
        int all = 1;
        for (size_t i = 0; i < nargs; i++) {
            if (sets[i] != smallest && !dict_get(sets[i], e->key, e->klen)) {
                all = 0;
                break;
            }
        }
        if (all) reply_bulk(c, e->key, e->klen);
    }
    free(sets);
}

static void cmd_sunion(db *store, client *c, command *cmd) {
    if (cmd->argc < 2) {
        reply_error(c, "wrong number of arguments for 'sunion' command");
        return;
    }
    dict *u = dict_create();
    for (int i = 1; i < cmd->argc; i++) {
        dict *s;
        if (set_arg_get(store, c, cmd, i, &s) < 0) {
            dict_free(u, NULL);
            return;
        }
        if (!s) continue;
        dict_iter it;
        dict_iter_init(&it, s);
        dict_entry *e;
        while ((e = dict_iter_next(&it)) != NULL) {
            dict_set(u, e->key, e->klen, SET_VAL);
        }
    }
    reply_array_header(c, dict_size(u));
    dict_iter it;
    dict_iter_init(&it, u);
    dict_entry *e;
    while ((e = dict_iter_next(&it)) != NULL) {
        reply_bulk(c, e->key, e->klen);
    }
    dict_free(u, NULL);
}

static void cmd_sdiff(db *store, client *c, command *cmd) {
    if (cmd->argc < 2) {
        reply_error(c, "wrong number of arguments for 'sdiff' command");
        return;
    }
    size_t nargs = (size_t)(cmd->argc - 1);
    dict **sets = xmalloc(nargs * sizeof(dict *));
    for (size_t i = 0; i < nargs; i++) {
        if (set_arg_get(store, c, cmd, (int)i + 1, &sets[i]) < 0) {
            free(sets);
            return;
        }
    }
    if (!sets[0]) {
        reply_array_header(c, 0);
        free(sets);
        return;
    }

    size_t count = 0;
    dict_iter it;
    dict_entry *e;
    dict_iter_init(&it, sets[0]);
    while ((e = dict_iter_next(&it)) != NULL) {
        int in_others = 0;
        for (size_t i = 1; i < nargs; i++) {
            if (sets[i] && dict_get(sets[i], e->key, e->klen)) {
                in_others = 1;
                break;
            }
        }
        if (!in_others) count++;
    }
    reply_array_header(c, count);
    dict_iter_init(&it, sets[0]);
    while ((e = dict_iter_next(&it)) != NULL) {
        int in_others = 0;
        for (size_t i = 1; i < nargs; i++) {
            if (sets[i] && dict_get(sets[i], e->key, e->klen)) {
                in_others = 1;
                break;
            }
        }
        if (!in_others) reply_bulk(c, e->key, e->klen);
    }
    free(sets);
}

/* ---- sorted-set commands ---- */

static void cmd_zadd(db *store, client *c, command *cmd) {
    if (cmd->argc < 4) {
        reply_error(c, "wrong number of arguments for 'zadd' command");
        return;
    }
    int nx = 0, xx = 0, ch = 0, incr = 0;
    int i = 2;
    for (; i < cmd->argc; i++) {
        const char *a = cmd->argv[i];
        if (!strcasecmp(a, "NX")) nx = 1;
        else if (!strcasecmp(a, "XX")) xx = 1;
        else if (!strcasecmp(a, "CH")) ch = 1;
        else if (!strcasecmp(a, "INCR")) incr = 1;
        else break;
    }
    if (nx && xx) {
        reply_error(c, "syntax error");
        return;
    }
    int pairs = cmd->argc - i;
    if (pairs == 0 || (pairs & 1)) {
        reply_error(c, "syntax error");
        return;
    }
    if (incr && pairs != 2) {
        reply_error(c, "INCR option supports a single increment-element pair");
        return;
    }

    robj *o = lookup_key(store, cmd->argv[1], strlen(cmd->argv[1]));
    if (o && o->type != OBJ_ZSET) {
        reply_wrongtype(c);
        return;
    }
    if (!o) {
        if (xx) {
            reply_integer(c, 0);
            return;
        }
        o = robj_new_zset();
        dict_set(store->d, cmd->argv[1], strlen(cmd->argv[1]), o);
    }
    zset *zs = (zset *)o->ptr;

    long long added = 0, changed = 0;
    for (; i < cmd->argc; i += 2) {
        double score;
        if (!parse_double(cmd->argv[i], &score)) {
            reply_error(c, "value is not a valid float");
            return;
        }
        const char *member = cmd->argv[i + 1];
        size_t mlen = strlen(member);

        zskiplist_node *existing = dict_get(zs->dict, member, mlen);

        if (incr) {
            if ((nx && existing) || (xx && !existing)) {
                reply_null(c);
                return;
            }
            double s = existing ? existing->score + score : score;
            if (s != s) {
                reply_error(c, "resulting score is not a number (NaN)");
                return;
            }
            zset_add(zs, s, member, mlen, NULL);
            reply_double(c, s);
            return;
        }

        if (nx && existing) continue;
        if (xx && !existing) continue;

        int res = zset_add(zs, score, member, mlen, NULL);
        if (res == 1) added++;
        else if (res == 2) changed++;
    }
    reply_integer(c, ch ? added + changed : added);
}

static void cmd_zcard(db *store, client *c, command *cmd) {
    if (cmd->argc != 2) {
        reply_error(c, "wrong number of arguments for 'zcard' command");
        return;
    }
    robj *o = lookup_key(store, cmd->argv[1], strlen(cmd->argv[1]));
    if (o && o->type != OBJ_ZSET) {
        reply_wrongtype(c);
        return;
    }
    reply_integer(c, o ? (long long)zset_len((zset *)o->ptr) : 0);
}

static void cmd_zscore(db *store, client *c, command *cmd) {
    if (cmd->argc != 3) {
        reply_error(c, "wrong number of arguments for 'zscore' command");
        return;
    }
    robj *o = lookup_key(store, cmd->argv[1], strlen(cmd->argv[1]));
    if (o && o->type != OBJ_ZSET) {
        reply_wrongtype(c);
        return;
    }
    if (!o) {
        reply_null(c);
        return;
    }
    double score;
    if (!zset_score((zset *)o->ptr, cmd->argv[2], strlen(cmd->argv[2]), &score)) {
        reply_null(c);
        return;
    }
    reply_double(c, score);
}

static void cmd_zrem(db *store, client *c, command *cmd) {
    if (cmd->argc < 3) {
        reply_error(c, "wrong number of arguments for 'zrem' command");
        return;
    }
    robj *o = lookup_key(store, cmd->argv[1], strlen(cmd->argv[1]));
    if (o && o->type != OBJ_ZSET) {
        reply_wrongtype(c);
        return;
    }
    if (!o) {
        reply_integer(c, 0);
        return;
    }
    zset *zs = (zset *)o->ptr;
    long long removed = 0;
    for (int i = 2; i < cmd->argc; i++) {
        if (zset_remove(zs, cmd->argv[i], strlen(cmd->argv[i]))) removed++;
    }
    reply_integer(c, removed);
    maybe_delete_key(store, cmd->argv[1], o);
}

static void zrange_common(db *store, client *c, command *cmd, int reverse) {
    if (cmd->argc != 4 && cmd->argc != 5) {
        reply_error(c, "wrong number of arguments for '%s' command", cmd->argv[0]);
        return;
    }
    int withscores = 0;
    if (cmd->argc == 5) {
        if (strcasecmp(cmd->argv[4], "WITHSCORES")) {
            reply_error(c, "syntax error");
            return;
        }
        withscores = 1;
    }
    long long start, stop;
    if (!string_to_ll(cmd->argv[2], &start) || !string_to_ll(cmd->argv[3], &stop)) {
        reply_error(c, "value is not an integer or out of range");
        return;
    }
    robj *o = lookup_key(store, cmd->argv[1], strlen(cmd->argv[1]));
    if (o && o->type != OBJ_ZSET) {
        reply_wrongtype(c);
        return;
    }
    if (!o) {
        reply_array_header(c, 0);
        return;
    }
    zset *zs = (zset *)o->ptr;
    long long length = (long long)zset_len(zs);
    if (start < 0) start = length + start;
    if (stop < 0) stop = length + stop;
    if (start < 0) start = 0;
    if (start > stop || start >= length) {
        reply_array_header(c, 0);
        return;
    }
    if (stop >= length) stop = length - 1;
    long long n = stop - start + 1;

    reply_array_header(c, withscores ? (size_t)n * 2 : (size_t)n);
    zskiplist_node *x;
    if (!reverse) {
        x = zsl_get_element_by_rank(zs->zsl, (unsigned long)(start + 1));
    } else {
        x = zsl_get_element_by_rank(zs->zsl, (unsigned long)(length - start));
    }
    for (long long i = 0; i < n; i++) {
        reply_bulk(c, x->member, x->mlen);
        if (withscores) reply_double(c, x->score);
        x = reverse ? x->backward : x->level[0].forward;
    }
}

static void cmd_zrange(db *store, client *c, command *cmd) { zrange_common(store, c, cmd, 0); }
static void cmd_zrevrange(db *store, client *c, command *cmd) { zrange_common(store, c, cmd, 1); }

static void cmd_zrangebyscore(db *store, client *c, command *cmd) {
    if (cmd->argc < 4) {
        reply_error(c, "wrong number of arguments for 'zrangebyscore' command");
        return;
    }
    double min, max;
    int minex = 0, maxex = 0;
    if (!parse_score_arg(cmd->argv[2], &min, &minex) ||
        !parse_score_arg(cmd->argv[3], &max, &maxex)) {
        reply_error(c, "min or max is not a float");
        return;
    }
    int withscores = 0;
    long long offset = 0, limit = -1;
    int i = 4;
    if (i < cmd->argc && !strcasecmp(cmd->argv[i], "WITHSCORES")) {
        withscores = 1;
        i++;
    }
    if (i < cmd->argc && !strcasecmp(cmd->argv[i], "LIMIT")) {
        if (i + 2 >= cmd->argc) {
            reply_error(c, "syntax error");
            return;
        }
        if (!string_to_ll(cmd->argv[i + 1], &offset) ||
            !string_to_ll(cmd->argv[i + 2], &limit) ||
            offset < 0 || limit < 0) {
            reply_error(c, "LIMIT offset and count must be non-negative integers");
            return;
        }
        i += 3;
    }
    if (i != cmd->argc) {
        reply_error(c, "syntax error");
        return;
    }

    robj *o = lookup_key(store, cmd->argv[1], strlen(cmd->argv[1]));
    if (o && o->type != OBJ_ZSET) {
        reply_wrongtype(c);
        return;
    }
    if (!o) {
        reply_array_header(c, 0);
        return;
    }
    zset *zs = (zset *)o->ptr;
    zskiplist_node *first = zsl_first_in_score_range(zs->zsl, min, minex);
    zskiplist_node *last = zsl_last_in_score_range(zs->zsl, max, maxex);
    if (!first || !last) {
        reply_array_header(c, 0);
        return;
    }
    unsigned long rfirst = zsl_get_rank(zs->zsl, first->score, first->member, first->mlen);
    unsigned long rlast = zsl_get_rank(zs->zsl, last->score, last->member, last->mlen);
    if (rfirst == 0 || rlast == 0 || rfirst > rlast) {
        reply_array_header(c, 0);
        return;
    }
    long long total = (long long)(rlast - rfirst + 1);

    zskiplist_node *x = first;
    long long skip = offset;
    while (skip > 0) {
        if (x == last) {
            x = NULL;
            break;
        }
        x = x->level[0].forward;
        skip--;
    }

    long long emit = limit < 0 ? total - offset : limit;
    if (emit < 0) emit = 0;
    if (emit > total - offset) emit = total - offset;

    reply_array_header(c, withscores ? (size_t)emit * 2 : (size_t)emit);
    for (long long j = 0; j < emit && x; j++) {
        reply_bulk(c, x->member, x->mlen);
        if (withscores) reply_double(c, x->score);
        if (x == last) break;
        x = x->level[0].forward;
    }
}

static void cmd_zrank(db *store, client *c, command *cmd) {
    if (cmd->argc != 3) {
        reply_error(c, "wrong number of arguments for 'zrank' command");
        return;
    }
    robj *o = lookup_key(store, cmd->argv[1], strlen(cmd->argv[1]));
    if (o && o->type != OBJ_ZSET) {
        reply_wrongtype(c);
        return;
    }
    if (!o) {
        reply_null(c);
        return;
    }
    zset *zs = (zset *)o->ptr;
    zskiplist_node *n = dict_get(zs->dict, cmd->argv[2], strlen(cmd->argv[2]));
    if (!n) {
        reply_null(c);
        return;
    }
    unsigned long rank = zsl_get_rank(zs->zsl, n->score, n->member, n->mlen);
    reply_integer(c, (long long)rank - 1);
}

static void cmd_zrevrank(db *store, client *c, command *cmd) {
    if (cmd->argc != 3) {
        reply_error(c, "wrong number of arguments for 'zrevrank' command");
        return;
    }
    robj *o = lookup_key(store, cmd->argv[1], strlen(cmd->argv[1]));
    if (o && o->type != OBJ_ZSET) {
        reply_wrongtype(c);
        return;
    }
    if (!o) {
        reply_null(c);
        return;
    }
    zset *zs = (zset *)o->ptr;
    zskiplist_node *n = dict_get(zs->dict, cmd->argv[2], strlen(cmd->argv[2]));
    if (!n) {
        reply_null(c);
        return;
    }
    unsigned long rank = zsl_get_rank(zs->zsl, n->score, n->member, n->mlen);
    reply_integer(c, (long long)zset_len(zs) - (long long)rank);
}

static void cmd_zincrby(db *store, client *c, command *cmd) {
    if (cmd->argc != 4) {
        reply_error(c, "wrong number of arguments for 'zincrby' command");
        return;
    }
    double inc;
    if (!parse_double(cmd->argv[2], &inc)) {
        reply_error(c, "value is not a valid float");
        return;
    }
    robj *o = lookup_key(store, cmd->argv[1], strlen(cmd->argv[1]));
    if (o && o->type != OBJ_ZSET) {
        reply_wrongtype(c);
        return;
    }
    if (!o) {
        o = robj_new_zset();
        dict_set(store->d, cmd->argv[1], strlen(cmd->argv[1]), o);
    }
    zset *zs = (zset *)o->ptr;
    const char *member = cmd->argv[3];
    size_t mlen = strlen(member);

    zskiplist_node *n = dict_get(zs->dict, member, mlen);
    double s = n ? n->score + inc : inc;
    if (s != s) {
        reply_error(c, "resulting score is not a number (NaN)");
        return;
    }
    zset_add(zs, s, member, mlen, NULL);
    reply_double(c, s);
}

static void cmd_zcount(db *store, client *c, command *cmd) {
    if (cmd->argc != 4) {
        reply_error(c, "wrong number of arguments for 'zcount' command");
        return;
    }
    double min, max;
    int minex = 0, maxex = 0;
    if (!parse_score_arg(cmd->argv[2], &min, &minex) ||
        !parse_score_arg(cmd->argv[3], &max, &maxex)) {
        reply_error(c, "min or max is not a float");
        return;
    }
    robj *o = lookup_key(store, cmd->argv[1], strlen(cmd->argv[1]));
    if (o && o->type != OBJ_ZSET) {
        reply_wrongtype(c);
        return;
    }
    if (!o) {
        reply_integer(c, 0);
        return;
    }
    zset *zs = (zset *)o->ptr;
    zskiplist_node *first = zsl_first_in_score_range(zs->zsl, min, minex);
    zskiplist_node *last = zsl_last_in_score_range(zs->zsl, max, maxex);
    if (!first || !last) {
        reply_integer(c, 0);
        return;
    }
    unsigned long rfirst = zsl_get_rank(zs->zsl, first->score, first->member, first->mlen);
    unsigned long rlast = zsl_get_rank(zs->zsl, last->score, last->member, last->mlen);
    if (rfirst == 0 || rlast == 0 || rfirst > rlast) {
        reply_integer(c, 0);
        return;
    }
    reply_integer(c, (long long)(rlast - rfirst + 1));
}

/* ---- dispatch ---- */

typedef struct {
    const char *name;
    void (*fn)(db *, client *, command *);
} cmd_entry;

static const cmd_entry commands[] = {
    {"ping",        cmd_ping},
    {"echo",        cmd_echo},
    {"select",      cmd_select},
    {"quit",        cmd_quit},

    {"set",         cmd_set},
    {"get",         cmd_get},
    {"del",         cmd_del},
    {"exists",      cmd_exists},
    {"incr",        cmd_incr},
    {"decr",        cmd_decr},
    {"expire",      cmd_expire},
    {"pexpire",     cmd_pexpire},
    {"ttl",         cmd_ttl},
    {"pttl",        cmd_pttl},
    {"type",        cmd_type},

    {"lpush",       cmd_lpush},
    {"rpush",       cmd_rpush},
    {"lpushx",      cmd_lpushx},
    {"rpushx",      cmd_rpushx},
    {"lpop",        cmd_lpop},
    {"rpop",        cmd_rpop},
    {"llen",        cmd_llen},
    {"lrange",      cmd_lrange},
    {"lindex",      cmd_lindex},
    {"lset",        cmd_lset},
    {"ltrim",       cmd_ltrim},
    {"lrem",        cmd_lrem},
    {"linsert",     cmd_linsert},

    {"hset",        cmd_hset},
    {"hmset",       cmd_hmset},
    {"hget",        cmd_hget},
    {"hmget",       cmd_hmget},
    {"hgetall",     cmd_hgetall},
    {"hkeys",       cmd_hkeys},
    {"hvals",       cmd_hvals},
    {"hlen",        cmd_hlen},
    {"hexists",     cmd_hexists},
    {"hdel",        cmd_hdel},
    {"hsetnx",      cmd_hsetnx},
    {"hincrby",     cmd_hincrby},
    {"hincrbyfloat", cmd_hincrbyfloat},

    {"sadd",        cmd_sadd},
    {"srem",        cmd_srem},
    {"sismember",   cmd_sismember},
    {"scard",       cmd_scard},
    {"smembers",    cmd_smembers},
    {"spop",        cmd_spop},
    {"sinter",      cmd_sinter},
    {"sunion",      cmd_sunion},
    {"sdiff",       cmd_sdiff},

    {"zadd",        cmd_zadd},
    {"zcard",       cmd_zcard},
    {"zscore",      cmd_zscore},
    {"zrem",        cmd_zrem},
    {"zrange",      cmd_zrange},
    {"zrevrange",   cmd_zrevrange},
    {"zrangebyscore", cmd_zrangebyscore},
    {"zrank",       cmd_zrank},
    {"zrevrank",    cmd_zrevrank},
    {"zincrby",     cmd_zincrby},
    {"zcount",      cmd_zcount},

    {"keys",        cmd_keys},
    {"flushall",    cmd_flushall},
    {"dbsize",      cmd_dbsize},
    {"info",        cmd_info},
    {"command",     cmd_command},
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
