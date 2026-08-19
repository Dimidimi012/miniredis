#ifndef MINIREDIS_PUBSUB_H
#define MINIREDIS_PUBSUB_H

#include <stddef.h>

struct client;   /* full definition in server.h */

/* Channel registry for PUB/SUB: maps channel (binary-safe bytes) to a list of
 * subscribed clients. Duplicate subscriptions to the same channel are
 * de-duplicated for delivery but still counted on the client (matching
 * Redis's counting semantics). */

typedef struct pubsub pubsub;

pubsub *pubsub_create(void);
void pubsub_free(pubsub *ps);

/* Subscribe `c` to `channel`. Returns the client's new subscription count. */
int pubsub_subscribe(pubsub *ps, struct client *c, const char *chan, size_t chlen);

/* Unsubscribe `c` from `channel` (no-op if not subscribed). Returns the
 * client's new subscription count. */
int pubsub_unsubscribe(pubsub *ps, struct client *c, const char *chan, size_t chlen);

/* Remove `c` from every channel (client disconnect). Resets its count to 0. */
void pubsub_unsubscribe_all(pubsub *ps, struct client *c);

/* Unsubscribe `c` from every channel it is subscribed to. For each channel
 * removed, `frame` (if non-NULL) is invoked with
 * (ctx, client, channel, chlen, new_count) so the caller can emit its
 * confirmation frame. */
void pubsub_unsubscribe_channels(pubsub *ps, struct client *c,
                                 void (*frame)(void *ctx, struct client *,
                                               const char *, size_t, int),
                                 void *ctx);

/* Publish a message: appends the RESP push frame to every subscriber's output
 * buffer. Returns the number of receivers. */
int pubsub_publish(pubsub *ps, const char *chan, size_t chlen,
                   const char *msg, size_t msglen);

#endif /* MINIREDIS_PUBSUB_H */
