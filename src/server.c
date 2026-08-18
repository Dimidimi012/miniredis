#include "server.h"

#include "db.h"
#include "util.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#define MAX_CLIENTS 1024
#define READ_CHUNK  65536

int64_t g_server_start_ms = 0;

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig) {
    (void)sig;
    g_stop = 1;
}

/* ---- client lifecycle ---- */

void client_init(client *c, int fd) {
    c->fd = fd;
    dynbuf_init(&c->in);
    dynbuf_init(&c->out);
    c->out_sent = 0;
    c->closing = 0;
}

void client_free(client *c) {
    if (c->fd >= 0) close(c->fd);
    dynbuf_free(&c->in);
    dynbuf_free(&c->out);
    free(c);
}

/* ---- RESP reply builders ---- */

void reply_simple(client *c, const char *s) {
    dynbuf_append_cstr(&c->out, "+");
    dynbuf_append_cstr(&c->out, s);
    dynbuf_append_cstr(&c->out, "\r\n");
}

void reply_error(client *c, const char *fmt, ...) {
    char tmp[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);

    dynbuf_append_cstr(&c->out, "-ERR ");
    dynbuf_append_cstr(&c->out, tmp);
    dynbuf_append_cstr(&c->out, "\r\n");
}

void reply_integer(client *c, long long n) {
    dynbuf_appendf(&c->out, ":%lld\r\n", n);
}

void reply_bulk(client *c, const void *s, size_t n) {
    dynbuf_appendf(&c->out, "$%zu\r\n", n);
    dynbuf_append(&c->out, s, n);
    dynbuf_append_cstr(&c->out, "\r\n");
}

void reply_bulk_cstr(client *c, const char *s) {
    reply_bulk(c, s, strlen(s));
}

void reply_null(client *c) {
    dynbuf_append_cstr(&c->out, "$-1\r\n");
}

void reply_array_header(client *c, size_t n) {
    dynbuf_appendf(&c->out, "*%zu\r\n", n);
}

/* ---- socket helpers ---- */

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int create_listener(const char *host, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        log_error("socket: %s", strerror(errno));
        return -1;
    }

    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);

    if (host && strcmp(host, "*") != 0 && strcmp(host, "0.0.0.0") != 0) {
        if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
            log_error("invalid bind address: %s", host);
            close(fd);
            return -1;
        }
    } else {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    }

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        log_error("bind %s:%d: %s", host ? host : "0.0.0.0", port, strerror(errno));
        close(fd);
        return -1;
    }
    if (listen(fd, 128) < 0) {
        log_error("listen: %s", strerror(errno));
        close(fd);
        return -1;
    }
    if (set_nonblocking(fd) < 0) {
        log_error("fcntl: %s", strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

/* ---- client I/O ---- */

static void accept_clients(int listen_fd, client ***clients, size_t *n, size_t *cap) {
    for (;;) {
        int fd = accept(listen_fd, NULL, NULL);
        if (fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            if (errno == EINTR) continue;
            log_warn("accept: %s", strerror(errno));
            return;
        }

        if (fd >= FD_SETSIZE) {
            /* select() cannot watch this descriptor; refuse it. */
            close(fd);
            continue;
        }
        set_nonblocking(fd);

        if (*n >= MAX_CLIENTS) {
            const char *msg = "-ERR max number of clients reached\r\n";
            (void)send(fd, msg, strlen(msg), 0);
            close(fd);
            continue;
        }

        if (*n == *cap) {
            size_t newcap = *cap ? *cap * 2 : 16;
            *clients = xrealloc(*clients, newcap * sizeof(client *));
            *cap = newcap;
        }

        client *c = xmalloc(sizeof(*c));
        client_init(c, fd);
        (*clients)[(*n)++] = c;
    }
}

static int read_client(client *c, db *store) {
    char buf[READ_CHUNK];

    for (;;) {
        ssize_t n = recv(c->fd, buf, sizeof(buf), 0);
        if (n > 0) {
            dynbuf_append(&c->in, buf, (size_t)n);

            /* Drain every complete command currently buffered. */
            for (;;) {
                command cmd;
                command_init(&cmd);
                int consumed = resp_parse_command(c->in.p, c->in.len, &cmd);

                if (consumed > 0) {
                    dispatch_command(store, c, &cmd);
                    command_free(&cmd);

                    memmove(c->in.p, c->in.p + consumed, c->in.len - (size_t)consumed);
                    c->in.len -= (size_t)consumed;

                    if (c->closing) return 0;
                    continue;
                }

                command_free(&cmd);
                if (consumed < 0) {
                    log_warn("protocol error from client fd=%d", c->fd);
                    return -1;
                }
                break; /* incomplete: need more data */
            }
        } else if (n == 0) {
            return -1; /* peer closed the connection */
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
            if (errno == EINTR) continue;
            return -1;
        }
    }
}

static int write_client(client *c) {
    while (c->out_sent < c->out.len) {
        ssize_t n = send(c->fd, c->out.p + c->out_sent, c->out.len - c->out_sent, 0);
        if (n > 0) {
            c->out_sent += (size_t)n;
        } else if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
            if (errno == EINTR) continue;
            return -1;
        } else {
            return -1;
        }
    }

    /* Fully flushed: reset the buffer (capacity is retained). */
    c->out.len = 0;
    c->out_sent = 0;
    return 0;
}

static void compact_clients(client ***clients, size_t *n) {
    size_t w = 0;
    for (size_t i = 0; i < *n; i++) {
        client *c = (*clients)[i];
        if (c->closing) client_free(c);
        else (*clients)[w++] = c;
    }
    *n = w;
}

/* ---- event loop ---- */

int server_run(const char *host, int port) {
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    g_server_start_ms = now_ms();

    db *store = db_create();
    int listen_fd = create_listener(host, port);
    if (listen_fd < 0) {
        db_free(store);
        return 1;
    }

    log_info("miniredis listening on %s:%d", host ? host : "0.0.0.0", port);

    client **clients = NULL;
    size_t nclients = 0, cap = 0;

    while (!g_stop) {
        fd_set rset, wset;
        FD_ZERO(&rset);
        FD_ZERO(&wset);
        FD_SET(listen_fd, &rset);
        int maxfd = listen_fd;

        for (size_t i = 0; i < nclients; i++) {
            client *c = clients[i];
            if (c->closing) continue;
            FD_SET(c->fd, &rset);
            if (c->out.len > c->out_sent) FD_SET(c->fd, &wset);
            if (c->fd > maxfd) maxfd = c->fd;
        }

        int rc = select(maxfd + 1, &rset, &wset, NULL, NULL);
        if (rc < 0) {
            if (errno == EINTR) continue;
            log_error("select: %s", strerror(errno));
            break;
        }

        if (FD_ISSET(listen_fd, &rset)) {
            accept_clients(listen_fd, &clients, &nclients, &cap);
        }

        for (size_t i = 0; i < nclients; i++) {
            client *c = clients[i];
            if (c->closing) continue;

            if (FD_ISSET(c->fd, &rset) && read_client(c, store) < 0) {
                c->closing = 1;
            }
            if (!c->closing && FD_ISSET(c->fd, &wset) && write_client(c) < 0) {
                c->closing = 1;
            }
            if (c->closing && c->out_sent < c->out.len) {
                /* Best-effort flush of a final reply (e.g. QUIT's +OK). */
                (void)write_client(c);
            }
        }

        compact_clients(&clients, &nclients);
    }

    log_info("shutting down");

    for (size_t i = 0; i < nclients; i++) client_free(clients[i]);
    free(clients);
    close(listen_fd);
    db_free(store);
    return 0;
}
