#include "server.h"

#include "aof.h"
#include "db.h"
#include "rdb.h"
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
#ifdef __linux__
#include <sys/epoll.h>
#endif

#define MAX_CLIENTS       1024
#define READ_CHUNK        65536
#define EPOLL_MAX_EVENTS  1024
#define MAX_QUERY_BUF     (64 * 1024 * 1024)   /* client input buffer cap */

int64_t g_server_start_ms = 0;

const char *g_aof_path = NULL;
const char *g_rdb_path = NULL;
int g_aof_fd = -1;
int g_aof_replaying = 0;

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
    c->events = 0;
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

/* ---- client I/O (shared by both event loops) ----
 * ep_fd is the epoll descriptor when running the epoll loop, or -1 when
 * running the select loop. */

static void accept_clients(int listen_fd, int ep_fd,
                           client ***clients, size_t *n, size_t *cap) {
    for (;;) {
        int fd = accept(listen_fd, NULL, NULL);
        if (fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            if (errno == EINTR) continue;
            log_warn("accept: %s", strerror(errno));
            return;
        }

        /* select() cannot watch descriptors beyond FD_SETSIZE. */
        if (ep_fd < 0 && fd >= FD_SETSIZE) {
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

#ifdef __linux__
        if (ep_fd >= 0) {
            struct epoll_event ev;
            ev.events = EPOLLIN;
            ev.data.ptr = c;
            if (epoll_ctl(ep_fd, EPOLL_CTL_ADD, fd, &ev) < 0) {
                log_warn("epoll_ctl ADD fd=%d: %s", fd, strerror(errno));
                c->closing = 1;
            } else {
                c->events = EPOLLIN;
            }
        }
#endif
    }
}

static int read_client(client *c, db *store) {
    char buf[READ_CHUNK];

    for (;;) {
        ssize_t n = recv(c->fd, buf, sizeof(buf), 0);
        if (n > 0) {
            dynbuf_append(&c->in, buf, (size_t)n);

            /* Bound memory use: drop clients that pile up an unreasonable
             * amount of unparsed input (a slow/abusive client). */
            if (c->in.len > MAX_QUERY_BUF) {
                log_warn("client fd=%d: query buffer exceeded %d bytes, closing",
                         c->fd, MAX_QUERY_BUF);
                return -1;
            }

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

static void compact_clients(client ***clients, size_t *n, int ep_fd) {
    (void)ep_fd;   /* only meaningful on Linux with the epoll loop */
    size_t w = 0;
    for (size_t i = 0; i < *n; i++) {
        client *c = (*clients)[i];
        if (c->closing) {
#ifdef __linux__
            if (ep_fd >= 0) epoll_ctl(ep_fd, EPOLL_CTL_DEL, c->fd, NULL);
#endif
            client_free(c);
        } else {
            (*clients)[w++] = c;
        }
    }
    *n = w;
}

#ifdef __linux__
/* ---- epoll event loop ---- */

static void client_update_epoll(int ep_fd, client *c) {
    uint32_t want = EPOLLIN;
    if (c->out.len > c->out_sent) want |= EPOLLOUT;
    if (want == c->events) return;

    struct epoll_event ev;
    ev.events = want;
    ev.data.ptr = c;
    if (epoll_ctl(ep_fd, EPOLL_CTL_MOD, c->fd, &ev) == 0) {
        c->events = want;
    } else {
        log_warn("epoll_ctl MOD fd=%d: %s", c->fd, strerror(errno));
    }
}

static int run_loop_epoll(db *store, int listen_fd) {
    int ep_fd = epoll_create(1024);
    if (ep_fd < 0) {
        log_error("epoll_create: %s", strerror(errno));
        return 1;
    }

    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.ptr = NULL;   /* NULL marks the listening socket */
    if (epoll_ctl(ep_fd, EPOLL_CTL_ADD, listen_fd, &ev) < 0) {
        log_error("epoll_ctl ADD listen: %s", strerror(errno));
        close(ep_fd);
        return 1;
    }

    client **clients = NULL;
    size_t nclients = 0, cap = 0;
    struct epoll_event events[EPOLL_MAX_EVENTS];

    while (!g_stop) {
        /* Poll with a 1s timeout only when AOF is on, to drive aof_periodic(). */
        int timeout = (g_aof_fd >= 0) ? 1000 : -1;
        int n = epoll_wait(ep_fd, events, EPOLL_MAX_EVENTS, timeout);
        if (n < 0) {
            if (errno == EINTR) continue;
            log_error("epoll_wait: %s", strerror(errno));
            break;
        }

        for (int i = 0; i < n; i++) {
            if (events[i].data.ptr == NULL) {
                accept_clients(listen_fd, ep_fd, &clients, &nclients, &cap);
                continue;
            }

            client *c = events[i].data.ptr;
            if (c->closing) continue;

            uint32_t fl = events[i].events;

            if (fl & (EPOLLERR | EPOLLHUP)) {
                if (!(fl & EPOLLIN)) {
                    c->closing = 1;
                    continue;
                }
                /* also readable: fall through to normal handling */
            }

            if (fl & EPOLLIN) {
                if (read_client(c, store) < 0) c->closing = 1;
                else client_update_epoll(ep_fd, c);
            }
            if (!c->closing && (fl & EPOLLOUT)) {
                if (write_client(c) < 0) c->closing = 1;
                else client_update_epoll(ep_fd, c);
            }
            if (c->closing && c->out_sent < c->out.len) {
                /* Best-effort flush of a final reply (e.g. QUIT's +OK). */
                (void)write_client(c);
            }
        }

        compact_clients(&clients, &nclients, ep_fd);
        aof_periodic();
    }

    for (size_t i = 0; i < nclients; i++) client_free(clients[i]);
    free(clients);
    close(ep_fd);
    return 0;
}
#endif /* __linux__ */

/* ---- select event loop (portable fallback) ---- */

static int run_loop_select(db *store, int listen_fd) {
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

        /* Poll with a 1s timeout only when AOF is on, to drive aof_periodic(). */
        struct timeval tv, *ptv = NULL;
        if (g_aof_fd >= 0) {
            tv.tv_sec = 1;
            tv.tv_usec = 0;
            ptv = &tv;
        }

        int rc = select(maxfd + 1, &rset, &wset, NULL, ptv);
        if (rc < 0) {
            if (errno == EINTR) continue;
            log_error("select: %s", strerror(errno));
            break;
        }

        if (FD_ISSET(listen_fd, &rset)) {
            accept_clients(listen_fd, -1, &clients, &nclients, &cap);
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

        compact_clients(&clients, &nclients, -1);
        aof_periodic();
    }

    for (size_t i = 0; i < nclients; i++) client_free(clients[i]);
    free(clients);
    return 0;
}

/* ---- entry point ---- */

int server_run(const char *host, int port, const char *io_mode) {
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGCHLD, SIG_IGN);   /* reap BGSAVE children automatically */

    g_server_start_ms = now_ms();

    db *store = db_create();
    int listen_fd = create_listener(host, port);
    if (listen_fd < 0) {
        db_free(store);
        return 1;
    }

    /* ---- persistence setup: AOF first, then load RDB, then replay AOF ---- */
    if (g_aof_path) {
        g_aof_fd = aof_open(g_aof_path);
        if (g_aof_fd < 0) {
            close(listen_fd);
            db_free(store);
            return 1;
        }
        log_info("aof: logging to %s", g_aof_path);
    }
    if (g_rdb_path && access(g_rdb_path, F_OK) == 0) {
        if (rdb_load(g_rdb_path, store) < 0) {
            log_error("rdb: failed to load %s; starting with an empty store", g_rdb_path);
        }
    }
    if (g_aof_path && access(g_aof_path, F_OK) == 0) {
        if (aof_replay(g_aof_path, store) < 0) {
            log_error("aof: replay of %s had errors", g_aof_path);
        }
    }

    log_info("miniredis listening on %s:%d (%s event loop)",
             host ? host : "0.0.0.0", port, io_mode);

    int rc;
#ifdef __linux__
    if (strcmp(io_mode, "epoll") == 0) {
        rc = run_loop_epoll(store, listen_fd);
    } else
#endif
    {
        if (strcmp(io_mode, "epoll") == 0) {
            log_warn("epoll is only available on Linux; using select");
        }
        rc = run_loop_select(store, listen_fd);
    }

    log_info("shutting down");

    /* Persist on clean shutdown (SIGINT/SIGTERM); kill -9 skips this and relies
     * on AOF replay for recovery. */
    if (g_rdb_path && rdb_save(g_rdb_path, store) < 0) {
        log_error("rdb: save to %s failed", g_rdb_path);
    }
    if (g_aof_fd >= 0) aof_close(g_aof_fd);

    close(listen_fd);
    db_free(store);
    return rc;
}
