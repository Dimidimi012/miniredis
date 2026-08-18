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
} client;

/* Set once at startup; used by INFO to report uptime. */
extern int64_t g_server_start_ms;

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

/* Runs the server; returns 0 on clean shutdown, non-zero on startup error. */
int server_run(const char *host, int port);

#endif /* MINIREDIS_SERVER_H */
