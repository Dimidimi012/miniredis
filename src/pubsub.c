#include "pubsub.h"

#include "dict.h"
#include "dynbuf.h"
#include "list.h"
#include "server.h"
#include "util.h"

#include <stdlib.h>

struct pubsub {
    dict *channels;   /* channel bytes -> list of client* */
};

static void free_client_list(void *p) {
    list_free((list *)p, NULL);   /* clients are owned by the server */
}

pubsub *pubsub_create(void) {
    pubsub *ps = xmalloc(sizeof(*ps));
    ps->channels = dict_create();
    return ps;
}

void pubsub_free(pubsub *ps) {
    if (!ps) return;
    dict_free(ps->channels, free_client_list);
    free(ps);
}

static list *channel_subscribers(const pubsub *ps, const char *chan, size_t chlen) {
    return dict_get(ps->channels, chan, chlen);
}

static int client_in_list(const list *l, const client *c) {
    for (const list_node *n = list_first(l); n; n = list_next(n)) {
        if (n->val == (void *)c) return 1;
    }
    return 0;
}

int pubsub_subscribe(pubsub *ps, client *c, const char *chan, size_t chlen) {
    list *l = channel_subscribers(ps, chan, chlen);
    if (!l) {
        l = list_create();
        dict_set(ps->channels, chan, chlen, l);
    }
    if (!client_in_list(l, c)) list_push_tail(l, c);
    c->subscribed++;
    return c->subscribed;
}

int pubsub_unsubscribe(pubsub *ps, client *c, const char *chan, size_t chlen) {
    list *l = channel_subscribers(ps, chan, chlen);
    if (l && client_in_list(l, c)) {
        for (list_node *n = list_first(l); n; n = list_next(n)) {
            if (n->val == (void *)c) {
                (void)list_detach(l, n);
                break;
            }
        }
        if (c->subscribed > 0) c->subscribed--;
    }
    return c->subscribed;
}

void pubsub_unsubscribe_all(pubsub *ps, client *c) {
    if (c->subscribed == 0) return;

    dict_iter it;
    dict_iter_init(&it, ps->channels);
    dict_entry *e;
    while ((e = dict_iter_next(&it)) != NULL) {
        list *l = (list *)e->val;
        for (list_node *n = list_first(l); n; ) {
            list_node *next = list_next(n);
            if (n->val == (void *)c) (void)list_detach(l, n);
            n = next;
        }
    }
    c->subscribed = 0;
}

void pubsub_unsubscribe_channels(pubsub *ps, client *c,
                                 void (*frame)(void *ctx, client *,
                                               const char *, size_t, int),
                                 void *ctx) {
    if (c->subscribed == 0) return;

    dict_iter it;
    dict_iter_init(&it, ps->channels);
    dict_entry *e;
    while ((e = dict_iter_next(&it)) != NULL) {
        list *l = (list *)e->val;
        if (client_in_list(l, c)) {
            int count = pubsub_unsubscribe(ps, c, e->key, e->klen);
            if (frame) frame(ctx, c, e->key, e->klen, count);
        }
    }
}

int pubsub_publish(pubsub *ps, const char *chan, size_t chlen,
                   const char *msg, size_t msglen) {
    list *l = channel_subscribers(ps, chan, chlen);
    if (!l || list_len(l) == 0) {
        /* lazily drop empty channels */
        if (l) {
            void *v = dict_delete(ps->channels, chan, chlen);
            if (v) list_free((list *)v, NULL);
        }
        return 0;
    }

    int receivers = 0;
    for (const list_node *n = list_first(l); n; n = list_next(n)) {
        client *sub = (client *)n->val;
        /* push frame: *3 $7 message $chlen <chan> $msglen <msg> */
        dynbuf_appendf(&sub->out, "*3\r\n$7\r\nmessage\r\n$%zu\r\n", chlen);
        dynbuf_append(&sub->out, chan, chlen);
        dynbuf_appendf(&sub->out, "\r\n$%zu\r\n", msglen);
        dynbuf_append(&sub->out, msg, msglen);
        dynbuf_append_cstr(&sub->out, "\r\n");
        receivers++;
    }
    return receivers;
}
