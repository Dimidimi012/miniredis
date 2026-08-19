#ifndef MINIREDIS_SERVER_H
#define MINIREDIS_SERVER_H

#include <stddef.h>
#include <stdint.h>

#include "dynbuf.h"
#include "resp.h"

typedef struct client {
    int fd;
    dynbuf in;        /* unparsed input bytes */
    dynbuf out;       /* pending output bytes */
    size_t out_sent;  /* bytes of `out` already handed to send() */
    int closing;      /* set when this client should be closed */
    uint32_t events;  /* registered epoll events (epoll loop only) */
    int subscribed;   /* pubsub subscription count; >0 = subscribed mode */
    int monitoring;   /* MONITOR mode: receives every command */
    char peer[64];    /* "ip:port" of the peer, for MONITOR output */
} client;

/* Set once at startup; used by INFO to report uptime. */
extern int64_t g_server_start_ms;

/* ---- persistence configuration (set from main / server_run) ---- */
extern const char *g_aof_path;   /* NULL when AOF is disabled */
extern const char *g_rdb_path;   /* NULL when RDB is disabled */
extern int g_aof_fd;             /* open AOF fd, or -1 */
extern int g_aof_replaying;      /* suppress AOF logging while replaying */

void client_init(client *c, int fd);
void client_free(client *c);

/* RESP reply builders (append into c->out). */
void reply_simple(client *c, const char *s);
void reply_error(client *c, const char *fmt, ...);
void reply_integer(client *c, long long n);
void reply_bulk(client *c, const void *s, size_t n);
void reply_bulk_cstr(client *c, const char *s);
void reply_null(client *c);
void reply_array_header(client *c, size_t n);

/* Runs the server. io_mode is "select" or "epoll" (the latter requires Linux).
 * Returns 0 on clean shutdown, non-zero on startup error. */
int server_run(const char *host, int port, const char *io_mode);

#endif /* MINIREDIS_SERVER_H */
