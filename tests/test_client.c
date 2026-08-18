/* test_client.c - a minimal, dependency-free RESP client used for end-to-end
 * checks against a running miniredis server.
 *
 * Usage: test_client <port> [host] [phase]
 *   phase: full  - the main command battery (default)
 *          burst - N concurrent connections, each issuing many PINGs */
#include <arpa/inet.h>
#include <limits.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

static int failures = 0;
static int conn = -1;

#define CHECK(cond) do {                       \
    if (!(cond)) {                             \
        failures++;                            \
        fprintf(stderr, "FAIL %s:%d: %s\n",    \
                __FILE__, __LINE__, #cond);    \
    }                                          \
} while (0)

static int read_all(int fd, char *buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        ssize_t r = recv(fd, buf + got, n - got, 0);
        if (r <= 0) return -1;
        got += (size_t)r;
    }
    return 0;
}

/* Read one CRLF-terminated line (NUL-terminated, static buffer). */
static char *read_line(int fd) {
    static char buf[65536];
    size_t n = 0;
    for (;;) {
        char ch;
        if (recv(fd, &ch, 1, 0) != 1) return NULL;
        if (n + 1 >= sizeof(buf)) return NULL;
        if (ch == '\r') {
            char nl;
            if (recv(fd, &nl, 1, 0) != 1 || nl != '\n') return NULL;
            buf[n] = '\0';
            return buf;
        }
        buf[n++] = ch;
    }
}

static void send_cmd(int argc, const char **argv) {
    char tmp[2048];
    int off = snprintf(tmp, sizeof(tmp), "*%d\r\n", argc);
    for (int i = 0; i < argc; i++) {
        size_t l = strlen(argv[i]);
        off += snprintf(tmp + off, (size_t)(sizeof(tmp) - off), "$%zu\r\n", l);
        if (off + (int)l + 2 < (int)sizeof(tmp)) {
            memcpy(tmp + off, argv[i], l);
            off += (int)l;
            tmp[off++] = '\r';
            tmp[off++] = '\n';
        }
    }
    (void)send(conn, tmp, (size_t)off, 0);
}

static void expect_simple(const char *expected) {
    char *line = read_line(conn);
    CHECK(line != NULL);
    if (line) CHECK(strcmp(line, expected) == 0);
}

static long long read_integer(void) {
    char *line = read_line(conn);
    if (!line || line[0] != ':') {
        failures++;
        return LLONG_MIN;
    }
    return atoll(line + 1);
}

/* Returns a malloc'd payload, or NULL for a null bulk reply. */
static char *read_bulk(void) {
    char *line = read_line(conn);
    if (!line || line[0] != '$') {
        failures++;
        return NULL;
    }
    long long n = atoll(line + 1);
    if (n < 0) return NULL;

    char *payload = malloc((size_t)n + 1);
    if (read_all(conn, payload, (size_t)n) != 0) {
        free(payload);
        failures++;
        return NULL;
    }
    payload[n] = '\0';

    char crlf[2];
    if (read_all(conn, crlf, 2) != 0 || crlf[0] != '\r' || crlf[1] != '\n') {
        free(payload);
        failures++;
        return NULL;
    }
    return payload;
}

static int connect_retry(int port, const char *host) {
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, host, &addr.sin_addr);

    int attempts = 50;
    while (attempts-- > 0) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return -1;
        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) return fd;
        close(fd);
        usleep(100 * 1000);
    }
    return -1;
}

/* Open many concurrent connections and hammer them with PINGs to exercise the
 * event loop's readiness handling. */
static int run_burst(int port, const char *host) {
    enum { NCONN = 20, NCMD = 100 };
    int fds[NCONN];
    int connected = 0;

    for (int i = 0; i < NCONN; i++) {
        fds[i] = connect_retry(port, host);
        if (fds[i] < 0) break;
        connected++;
    }
    CHECK(connected == NCONN);

    if (connected == NCONN) {
        /* Phase 1: every connection sends its batch of PINGs. */
        for (int i = 0; i < connected; i++) {
            for (int j = 0; j < NCMD; j++) {
                const char *cmd = "*1\r\n$4\r\nPING\r\n";
                (void)send(fds[i], cmd, strlen(cmd), 0);
            }
        }
        /* Phase 2: every connection reads back its PONGs. */
        for (int i = 0; i < connected; i++) {
            for (int j = 0; j < NCMD; j++) {
                char *line = read_line(fds[i]);
                CHECK(line != NULL);
                if (line) CHECK(strcmp(line, "+PONG") == 0);
            }
        }
    }

    for (int i = 0; i < connected; i++) close(fds[i]);

    if (failures) {
        fprintf(stderr, "test_client[burst]: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_client[burst]: all tests passed (%d conn x %d PING)\n", NCONN, NCMD);
    return 0;
}

static int run_full(int port, const char *host) {
    conn = connect_retry(port, host);
    CHECK(conn >= 0);
    if (conn < 0) {
        fprintf(stderr, "test_client: could not connect to %s:%d\n", host, port);
        return 1;
    }

    /* PING */
    {
        const char *a[] = {"PING"};
        send_cmd(1, a);
        expect_simple("+PONG");
    }
    /* PING with a message returns a bulk string */
    {
        const char *a[] = {"PING", "hi"};
        send_cmd(2, a);
        char *got = read_bulk();
        CHECK(got != NULL);
        if (got) CHECK(strcmp(got, "hi") == 0);
        free(got);
    }
    /* SET / GET */
    {
        const char *a[] = {"SET", "foo", "bar"};
        send_cmd(3, a);
        expect_simple("+OK");
    }
    {
        const char *a[] = {"GET", "foo"};
        send_cmd(2, a);
        char *got = read_bulk();
        CHECK(got != NULL);
        if (got) CHECK(strcmp(got, "bar") == 0);
        free(got);
    }
    /* GET of a missing key returns null */
    {
        const char *a[] = {"GET", "nope"};
        send_cmd(2, a);
        char *got = read_bulk();
        CHECK(got == NULL);
        free(got);
    }
    /* EXISTS */
    {
        const char *a[] = {"EXISTS", "foo", "nope"};
        send_cmd(3, a);
        CHECK(read_integer() == 1);
    }
    /* INCR / DECR */
    {
        const char *a[] = {"INCR", "counter"};
        send_cmd(2, a);
        CHECK(read_integer() == 1);
    }
    {
        const char *a[] = {"INCR", "counter"};
        send_cmd(2, a);
        CHECK(read_integer() == 2);
    }
    {
        const char *a[] = {"DECR", "counter"};
        send_cmd(2, a);
        CHECK(read_integer() == 1);
    }
    /* EXPIRE / TTL */
    {
        const char *a[] = {"EXPIRE", "foo", "100"};
        send_cmd(3, a);
        CHECK(read_integer() == 1);
    }
    {
        const char *a[] = {"TTL", "foo"};
        send_cmd(2, a);
        long long t = read_integer();
        CHECK(t > 0 && t <= 100);
    }
    {
        const char *a[] = {"TTL", "counter"};
        send_cmd(2, a);
        CHECK(read_integer() == -1);   /* no expiry */
    }
    /* TYPE */
    {
        const char *a[] = {"TYPE", "foo"};
        send_cmd(2, a);
        expect_simple("+string");
    }
    /* DEL */
    {
        const char *a[] = {"DEL", "foo"};
        send_cmd(2, a);
        CHECK(read_integer() == 1);
    }
    {
        const char *a[] = {"DEL", "foo"};
        send_cmd(2, a);
        CHECK(read_integer() == 0);
    }
    /* KEYS (only "counter" remains) */
    {
        const char *a[] = {"KEYS", "*"};
        send_cmd(2, a);
        char *line = read_line(conn);
        CHECK(line != NULL);
        if (line) CHECK(line[0] == '*');
        long long n = (line && line[0] == '*') ? atoll(line + 1) : -1;
        CHECK(n == 1);
        if (n == 1) {
            char *k = read_bulk();
            CHECK(k != NULL);
            if (k) CHECK(strcmp(k, "counter") == 0);
            free(k);
        }
    }
    /* unknown command -> error reply */
    {
        const char *a[] = {"NOSUCHCMD"};
        send_cmd(1, a);
        char *line = read_line(conn);
        CHECK(line != NULL);
        if (line) CHECK(line[0] == '-');
    }

    close(conn);

    if (failures) {
        fprintf(stderr, "test_client: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_client: all tests passed\n");
    return 0;
}

int main(int argc, char **argv) {
    int port = (argc > 1) ? atoi(argv[1]) : 6389;
    const char *host = (argc > 2) ? argv[2] : "127.0.0.1";
    const char *phase = (argc > 3) ? argv[3] : "full";

    if (strcmp(phase, "burst") == 0) return run_burst(port, host);
    return run_full(port, host);
}
