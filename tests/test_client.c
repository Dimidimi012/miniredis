/* test_client.c - a minimal, dependency-free RESP client used for end-to-end
 * checks against a running miniredis server.
 *
 * Usage: test_client <port> [host] [phase]
 *   phase: full  - the main command battery (default)
 *          burst - N concurrent connections, each issuing many PINGs */
#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
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

/* Read an array reply of bulk strings. Returns a malloc'd array of malloc'd
 * strings (entries may be NULL for null bulk replies); *n gets the count. */
static char **read_array(int *n) {
    char *line = read_line(conn);
    if (!line || line[0] != '*') {
        failures++;
        *n = 0;
        return NULL;
    }
    long long cnt = atoll(line + 1);
    if (cnt < 0) {
        *n = 0;
        return NULL;
    }
    *n = (int)cnt;
    char **arr = malloc((size_t)cnt * sizeof(char *));
    for (long long i = 0; i < cnt; i++) {
        char *l2 = read_line(conn);
        if (!l2 || l2[0] != '$') {
            failures++;
            arr[i] = NULL;
            continue;
        }
        long long bl = atoll(l2 + 1);
        if (bl < 0) {
            arr[i] = NULL;
            continue;
        }
        arr[i] = malloc((size_t)bl + 1);
        if (read_all(conn, arr[i], (size_t)bl) != 0) {
            free(arr[i]);
            arr[i] = NULL;
            failures++;
            continue;
        }
        arr[i][bl] = '\0';
        char crlf[2];
        if (read_all(conn, crlf, 2) != 0 || crlf[0] != '\r' || crlf[1] != '\n') {
            failures++;
        }
    }
    return arr;
}

static void free_array(char **arr, int n) {
    if (!arr) return;
    for (int i = 0; i < n; i++) free(arr[i]);
    free(arr);
}

/* Read an array and compare it (element-wise, in order) with `expected`.
 * NULL entries in `expected` match null bulk replies. */
static void expect_array(const char *const *expected, int n) {
    int got_n = 0;
    char **got = read_array(&got_n);
    CHECK(got_n == n);
    if (got_n == n) {
        for (int i = 0; i < n; i++) {
            if (expected[i] == NULL) {
                CHECK(got[i] == NULL);
            } else {
                CHECK(got[i] != NULL);
                if (got[i]) CHECK(strcmp(got[i], expected[i]) == 0);
            }
        }
    }
    free_array(got, got_n);
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

/* Seed a deterministic dataset; used by the persistence tests. */
static int run_write(int port, const char *host) {
    conn = connect_retry(port, host);
    CHECK(conn >= 0);
    if (conn < 0) return 1;

    {
        const char *a[] = {"SET", "pkey", "pval"};
        send_cmd(3, a);
        expect_simple("+OK");
    }
    {
        const char *a[] = {"RPUSH", "plist", "a", "b", "c"};
        send_cmd(5, a);
        CHECK(read_integer() == 3);
    }
    {
        const char *a[] = {"HSET", "ph", "f1", "v1"};
        send_cmd(4, a);
        CHECK(read_integer() == 1);
    }
    {
        const char *a[] = {"SADD", "ps", "m1", "m2"};
        send_cmd(4, a);
        CHECK(read_integer() == 2);
    }
    {
        const char *a[] = {"ZADD", "pz", "1", "pa", "2", "pb"};
        send_cmd(6, a);
        CHECK(read_integer() == 2);
    }
    for (int i = 0; i < 3; i++) {
        const char *a[] = {"INCR", "pcnt"};
        send_cmd(2, a);
        CHECK(read_integer() == i + 1);
    }
    {
        const char *a[] = {"SET", "ptmp", "x"};
        send_cmd(3, a);
        expect_simple("+OK");
    }
    {
        const char *a[] = {"DEL", "ptmp"};
        send_cmd(2, a);
        CHECK(read_integer() == 1);
    }
    {
        const char *a[] = {"SET", "pexp", "v", "EX", "10000"};
        send_cmd(5, a);
        expect_simple("+OK");
    }

    close(conn);
    if (failures) {
        fprintf(stderr, "test_client[write]: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_client[write]: done\n");
    return 0;
}

/* Verify the dataset seeded by run_write survived a restart. */
static int run_verify(int port, const char *host, int check_prw) {
    conn = connect_retry(port, host);
    CHECK(conn >= 0);
    if (conn < 0) return 1;

    {
        const char *a[] = {"GET", "pkey"};
        send_cmd(2, a);
        char *got = read_bulk();
        CHECK(got != NULL);
        if (got) CHECK(strcmp(got, "pval") == 0);
        free(got);
    }
    {
        const char *a[] = {"LRANGE", "plist", "0", "-1"};
        send_cmd(4, a);
        const char *exp[] = {"a", "b", "c"};
        expect_array(exp, 3);
    }
    {
        const char *a[] = {"HGET", "ph", "f1"};
        send_cmd(3, a);
        char *got = read_bulk();
        CHECK(got != NULL);
        if (got) CHECK(strcmp(got, "v1") == 0);
        free(got);
    }
    {
        const char *a[] = {"SISMEMBER", "ps", "m1"};
        send_cmd(3, a);
        CHECK(read_integer() == 1);
    }
    {
        const char *a[] = {"ZSCORE", "pz", "pb"};
        send_cmd(3, a);
        char *got = read_bulk();
        CHECK(got != NULL);
        if (got) CHECK(strcmp(got, "2") == 0);
        free(got);
    }
    {
        const char *a[] = {"GET", "pcnt"};
        send_cmd(2, a);
        char *got = read_bulk();
        CHECK(got != NULL);
        if (got) CHECK(strcmp(got, "3") == 0);
        free(got);
    }
    {
        const char *a[] = {"GET", "ptmp"};
        send_cmd(2, a);
        char *got = read_bulk();
        CHECK(got == NULL);   /* the DEL was replayed too */
        free(got);
    }
    {
        const char *a[] = {"TTL", "pexp"};
        send_cmd(2, a);
        long long t = read_integer();
        CHECK(t > 0 && t <= 10000);
    }
    if (check_prw) {
        /* only present after an AOF-rewrite round trip (verifyrw phase) */
        const char *a[] = {"GET", "prw"};
        send_cmd(2, a);
        char *got = read_bulk();
        CHECK(got != NULL);
        if (got) CHECK(strcmp(got, "rwv") == 0);
        free(got);
    }

    close(conn);
    if (failures) {
        fprintf(stderr, "test_client[verify]: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_client[verify]: all tests passed\n");
    return 0;
}

/* Active expiration: a short-TTL key must disappear on its own (via the
 * periodic expire cycle) without any access triggering lazy deletion. DBSIZE
 * counts raw dict entries, so it observes the cycle directly. */
static int run_expire(int port, const char *host) {
    conn = connect_retry(port, host);
    CHECK(conn >= 0);
    if (conn < 0) return 1;

    long long base = 0;
    {
        const char *a[] = {"DBSIZE"};
        send_cmd(1, a);
        base = read_integer();
    }
    {
        const char *a[] = {"SET", "tk", "v", "EX", "1"};
        send_cmd(5, a);
        expect_simple("+OK");
    }
    {
        const char *a[] = {"DBSIZE"};
        send_cmd(1, a);
        CHECK(read_integer() == base + 1);
    }
    /* wait past the 1s TTL plus a couple of expire-cycle ticks, no access */
    usleep(2500 * 1000);
    {
        const char *a[] = {"DBSIZE"};
        send_cmd(1, a);
        CHECK(read_integer() == base);
    }

    close(conn);
    if (failures) {
        fprintf(stderr, "test_client[expire]: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_client[expire]: active expiration removed the expired key\n");
    return 0;
}

/* Exercise REWRITEAOF / BGREWRITEAOF plus a write issued while the background
 * rewrite may still be running. That write must survive via the rewrite buffer
 * and the subsequent crash (verified by the verifyrw phase). */
static int run_rewrite(int port, const char *host) {
    conn = connect_retry(port, host);
    CHECK(conn >= 0);
    if (conn < 0) return 1;

    {
        const char *a[] = {"REWRITEAOF"};
        send_cmd(1, a);
        expect_simple("+OK");
    }
    {
        const char *a[] = {"BGREWRITEAOF"};
        send_cmd(1, a);
        expect_simple("+Background append only file rewriting started");
    }
    {
        const char *a[] = {"SET", "prw", "rwv"};
        send_cmd(3, a);
        expect_simple("+OK");
    }
    /* give the child time to swap in the new file and the parent to reopen it */
    usleep(500 * 1000);

    close(conn);
    if (failures) {
        fprintf(stderr, "test_client[rewrite]: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_client[rewrite]: done\n");
    return 0;
}

/* Send an incomplete request larger than the server's 64MB input cap and
 * verify the server drops the connection (memory-DoS protection). */
static int run_biginput(int port, const char *host) {
    signal(SIGPIPE, SIG_IGN);

    int fd = connect_retry(port, host);
    CHECK(fd >= 0);
    if (fd < 0) return 1;

    /* recv timeout so the test cannot hang if the server fails to close. */
    struct timeval tv = {5, 0};
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    /* Declare a 100MB bulk string, then stream just over the 64MB cap. */
    const char *hdr = "*1\r\n$100000000\r\n";
    (void)send(fd, hdr, strlen(hdr), 0);

    char chunk[65536];
    memset(chunk, 'x', sizeof(chunk));
    for (int i = 0; i < 1024; i++) {   /* 64MB total (+ header) */
        ssize_t w = send(fd, chunk, sizeof(chunk), 0);
        if (w < 0) break;              /* server closed while we were writing */
    }

    char b;
    ssize_t r = recv(fd, &b, 1, 0);
    int closed = (r == 0) || (r < 0 && errno == ECONNRESET);
    CHECK(closed);

    close(fd);
    if (failures) {
        fprintf(stderr, "test_client[biginput]: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_client[biginput]: server closed an oversized request\n");
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
    /* ---- LIST ---- */
    {
        const char *a[] = {"RPUSH", "mylist", "a", "b", "c"};
        send_cmd(5, a);
        CHECK(read_integer() == 3);
    }
    {
        const char *a[] = {"LRANGE", "mylist", "0", "-1"};
        send_cmd(4, a);
        const char *exp[] = {"a", "b", "c"};
        expect_array(exp, 3);
    }
    {
        const char *a[] = {"LPUSH", "mylist", "z"};
        send_cmd(3, a);
        CHECK(read_integer() == 4);
    }
    {
        const char *a[] = {"LPOP", "mylist"};
        send_cmd(2, a);
        char *got = read_bulk();
        CHECK(got != NULL);
        if (got) CHECK(strcmp(got, "z") == 0);
        free(got);
    }
    {
        const char *a[] = {"LLEN", "mylist"};
        send_cmd(2, a);
        CHECK(read_integer() == 3);
    }
    {
        const char *a[] = {"LINDEX", "mylist", "1"};
        send_cmd(3, a);
        char *got = read_bulk();
        CHECK(got != NULL);
        if (got) CHECK(strcmp(got, "b") == 0);
        free(got);
    }
    {
        const char *a[] = {"LINSERT", "mylist", "BEFORE", "b", "X"};
        send_cmd(5, a);
        CHECK(read_integer() == 4);
    }
    {
        const char *a[] = {"LRANGE", "mylist", "0", "-1"};
        send_cmd(4, a);
        const char *exp[] = {"a", "X", "b", "c"};
        expect_array(exp, 4);
    }
    {
        const char *a[] = {"LREM", "mylist", "1", "b"};
        send_cmd(4, a);
        CHECK(read_integer() == 1);
    }
    {
        const char *a[] = {"LTRIM", "mylist", "0", "1"};
        send_cmd(4, a);
        expect_simple("+OK");
    }
    {
        const char *a[] = {"LRANGE", "mylist", "0", "-1"};
        send_cmd(4, a);
        const char *exp[] = {"a", "X"};
        expect_array(exp, 2);
    }
    {
        const char *a[] = {"LSET", "mylist", "0", "A"};
        send_cmd(4, a);
        expect_simple("+OK");
    }
    {
        const char *a[] = {"LRANGE", "mylist", "0", "-1"};
        send_cmd(4, a);
        const char *exp[] = {"A", "X"};
        expect_array(exp, 2);
    }
    {
        const char *a[] = {"DEL", "mylist"};
        send_cmd(2, a);
        CHECK(read_integer() == 1);
    }

    /* ---- HASH ---- */
    {
        const char *a[] = {"HSET", "h", "f1", "v1", "f2", "v2"};
        send_cmd(6, a);
        CHECK(read_integer() == 2);
    }
    {
        const char *a[] = {"HGET", "h", "f1"};
        send_cmd(3, a);
        char *got = read_bulk();
        CHECK(got != NULL);
        if (got) CHECK(strcmp(got, "v1") == 0);
        free(got);
    }
    {
        const char *a[] = {"HGETALL", "h"};
        send_cmd(2, a);
        int n = 0;
        char **got = read_array(&n);
        CHECK(n == 4);
        if (n == 4) {
            /* pairs come back in hash-bucket order; verify both pairs exist */
            int found_f1 = 0, found_f2 = 0;
            for (int i = 0; i + 1 < n; i += 2) {
                if (got[i] && strcmp(got[i], "f1") == 0)
                    found_f1 = (got[i + 1] && strcmp(got[i + 1], "v1") == 0);
                if (got[i] && strcmp(got[i], "f2") == 0)
                    found_f2 = (got[i + 1] && strcmp(got[i + 1], "v2") == 0);
            }
            CHECK(found_f1 && found_f2);
        }
        free_array(got, n);
    }
    {
        const char *a[] = {"HINCRBY", "h", "n", "5"};
        send_cmd(4, a);
        CHECK(read_integer() == 5);
    }
    {
        const char *a[] = {"HINCRBY", "h", "n", "-2"};
        send_cmd(4, a);
        CHECK(read_integer() == 3);
    }
    {
        const char *a[] = {"HDEL", "h", "f1"};
        send_cmd(3, a);
        CHECK(read_integer() == 1);
    }
    {
        const char *a[] = {"HLEN", "h"};
        send_cmd(2, a);
        CHECK(read_integer() == 2);
    }
    {
        const char *a[] = {"HEXISTS", "h", "f2"};
        send_cmd(3, a);
        CHECK(read_integer() == 1);
    }
    {
        const char *a[] = {"DEL", "h"};
        send_cmd(2, a);
        CHECK(read_integer() == 1);
    }

    /* ---- SET ---- */
    {
        const char *a[] = {"SADD", "s", "a", "b", "c"};
        send_cmd(5, a);
        CHECK(read_integer() == 3);
    }
    {
        const char *a[] = {"SCARD", "s"};
        send_cmd(2, a);
        CHECK(read_integer() == 3);
    }
    {
        const char *a[] = {"SISMEMBER", "s", "b"};
        send_cmd(3, a);
        CHECK(read_integer() == 1);
    }
    {
        const char *a[] = {"SREM", "s", "b"};
        send_cmd(3, a);
        CHECK(read_integer() == 1);
    }
    {
        const char *a[] = {"SMEMBERS", "s"};
        send_cmd(2, a);
        int n = 0;
        char **got = read_array(&n);
        CHECK(n == 2);
        if (n == 2) {
            int has_a = 0, has_c = 0;
            for (int i = 0; i < n; i++) {
                if (got[i] && strcmp(got[i], "a") == 0) has_a = 1;
                if (got[i] && strcmp(got[i], "c") == 0) has_c = 1;
            }
            CHECK(has_a && has_c);
        }
        free_array(got, n);
    }
    {
        const char *a[] = {"SADD", "s2", "a", "b"};
        send_cmd(4, a);
        CHECK(read_integer() == 2);
    }
    {
        const char *a[] = {"SINTER", "s", "s2"};
        send_cmd(3, a);
        int n = 0;
        char **got = read_array(&n);
        CHECK(n == 1);
        if (n == 1) CHECK(got[0] && strcmp(got[0], "a") == 0);
        free_array(got, n);
    }
    {
        const char *a[] = {"SUNION", "s", "s2"};
        send_cmd(3, a);
        int n = 0;
        char **got = read_array(&n);
        CHECK(n == 3);
        free_array(got, n);
    }
    {
        const char *a[] = {"SDIFF", "s2", "s"};
        send_cmd(3, a);
        int n = 0;
        char **got = read_array(&n);
        CHECK(n == 1);
        if (n == 1) CHECK(got[0] && strcmp(got[0], "b") == 0);
        free_array(got, n);
    }
    {
        const char *a[] = {"SPOP", "s"};
        send_cmd(2, a);
        char *got = read_bulk();
        CHECK(got != NULL);
        free(got);
    }
    {
        const char *a[] = {"DEL", "s", "s2"};
        send_cmd(3, a);
        CHECK(read_integer() == 2);
    }

    /* ---- ZSET ---- */
    {
        const char *a[] = {"ZADD", "z", "1", "a", "2", "b", "3", "c"};
        send_cmd(8, a);
        CHECK(read_integer() == 3);
    }
    {
        const char *a[] = {"ZCARD", "z"};
        send_cmd(2, a);
        CHECK(read_integer() == 3);
    }
    {
        const char *a[] = {"ZSCORE", "z", "b"};
        send_cmd(3, a);
        char *got = read_bulk();
        CHECK(got != NULL);
        if (got) CHECK(strcmp(got, "2") == 0);
        free(got);
    }
    {
        const char *a[] = {"ZRANGE", "z", "0", "-1"};
        send_cmd(4, a);
        const char *exp[] = {"a", "b", "c"};
        expect_array(exp, 3);
    }
    {
        const char *a[] = {"ZRANGE", "z", "0", "-1", "WITHSCORES"};
        send_cmd(5, a);
        int n = 0;
        char **got = read_array(&n);
        CHECK(n == 6);
        if (n == 6) {
            CHECK(strcmp(got[0], "a") == 0 && strcmp(got[1], "1") == 0);
            CHECK(strcmp(got[2], "b") == 0 && strcmp(got[3], "2") == 0);
            CHECK(strcmp(got[4], "c") == 0 && strcmp(got[5], "3") == 0);
        }
        free_array(got, n);
    }
    {
        const char *a[] = {"ZREVRANGE", "z", "0", "1"};
        send_cmd(4, a);
        const char *exp[] = {"c", "b"};
        expect_array(exp, 2);
    }
    {
        const char *a[] = {"ZRANK", "z", "c"};
        send_cmd(3, a);
        CHECK(read_integer() == 2);
    }
    {
        const char *a[] = {"ZREVRANK", "z", "c"};
        send_cmd(3, a);
        CHECK(read_integer() == 0);
    }
    {
        const char *a[] = {"ZRANGEBYSCORE", "z", "1", "2"};
        send_cmd(4, a);
        const char *exp[] = {"a", "b"};
        expect_array(exp, 2);
    }
    {
        const char *a[] = {"ZRANGEBYSCORE", "z", "(1", "2"};
        send_cmd(4, a);
        const char *exp[] = {"b"};
        expect_array(exp, 1);
    }
    {
        const char *a[] = {"ZCOUNT", "z", "1", "2"};
        send_cmd(4, a);
        CHECK(read_integer() == 2);
    }
    {
        const char *a[] = {"ZINCRBY", "z", "5", "a"};
        send_cmd(4, a);
        char *got = read_bulk();
        CHECK(got != NULL);
        if (got) CHECK(strcmp(got, "6") == 0);
        free(got);
    }
    {
        const char *a[] = {"ZREM", "z", "a"};
        send_cmd(3, a);
        CHECK(read_integer() == 1);
    }
    {
        const char *a[] = {"ZCARD", "z"};
        send_cmd(2, a);
        CHECK(read_integer() == 2);
    }
    {
        const char *a[] = {"DEL", "z"};
        send_cmd(2, a);
        CHECK(read_integer() == 1);
    }

    /* ---- WRONGTYPE + TYPE ---- */
    {
        const char *a[] = {"SET", "wt", "v"};
        send_cmd(3, a);
        expect_simple("+OK");
    }
    {
        const char *a[] = {"LPUSH", "wt", "x"};
        send_cmd(3, a);
        char *line = read_line(conn);
        CHECK(line != NULL);
        if (line) CHECK(strncmp(line, "-WRONGTYPE", 10) == 0);
    }
    {
        const char *a[] = {"TYPE", "wt"};
        send_cmd(2, a);
        expect_simple("+string");
    }
    {
        const char *a[] = {"DEL", "wt"};
        send_cmd(2, a);
        CHECK(read_integer() == 1);
    }

    /* ---- extended string commands ---- */
    {
        const char *a[] = {"SETNX", "nk", "v"};
        send_cmd(3, a);
        CHECK(read_integer() == 1);
    }
    {
        const char *a[] = {"SETNX", "nk", "v2"};
        send_cmd(3, a);
        CHECK(read_integer() == 0);
    }
    {
        const char *a[] = {"MSET", "a", "1", "b", "2"};
        send_cmd(5, a);
        expect_simple("+OK");
    }
    {
        const char *a[] = {"MGET", "a", "b", "nope"};
        send_cmd(4, a);
        int n = 0;
        char **got = read_array(&n);
        CHECK(n == 3);
        if (n == 3) {
            CHECK(got[0] && strcmp(got[0], "1") == 0);
            CHECK(got[1] && strcmp(got[1], "2") == 0);
            CHECK(got[2] == NULL);   /* missing -> null bulk */
        }
        free_array(got, n);
    }
    {
        const char *a[] = {"APPEND", "str", "hello"};
        send_cmd(3, a);
        CHECK(read_integer() == 5);
    }
    {
        const char *a[] = {"APPEND", "str", " world"};
        send_cmd(3, a);
        CHECK(read_integer() == 11);
    }
    {
        const char *a[] = {"GET", "str"};
        send_cmd(2, a);
        char *got = read_bulk();
        CHECK(got != NULL);
        if (got) CHECK(strcmp(got, "hello world") == 0);
        free(got);
    }
    {
        const char *a[] = {"STRLEN", "str"};
        send_cmd(2, a);
        CHECK(read_integer() == 11);
    }
    {
        const char *a[] = {"GETRANGE", "str", "0", "4"};
        send_cmd(4, a);
        char *got = read_bulk();
        CHECK(got != NULL);
        if (got) CHECK(strcmp(got, "hello") == 0);
        free(got);
    }
    {
        const char *a[] = {"GETRANGE", "str", "-5", "-1"};
        send_cmd(4, a);
        char *got = read_bulk();
        CHECK(got != NULL);
        if (got) CHECK(strcmp(got, "world") == 0);
        free(got);
    }
    {
        const char *a[] = {"SETRANGE", "str", "6", "C"};
        send_cmd(4, a);
        CHECK(read_integer() == 11);
    }
    {
        const char *a[] = {"GET", "str"};
        send_cmd(2, a);
        char *got = read_bulk();
        CHECK(got != NULL);
        if (got) CHECK(strcmp(got, "hello Corld") == 0);
        free(got);
    }
    {
        const char *a[] = {"SETEX", "sk", "100", "v"};
        send_cmd(4, a);
        expect_simple("+OK");
    }
    {
        const char *a[] = {"TTL", "sk"};
        send_cmd(2, a);
        long long t = read_integer();
        CHECK(t > 0 && t <= 100);
    }
    {
        const char *a[] = {"GETSET", "gs", "new"};
        send_cmd(3, a);
        char *got = read_bulk();
        CHECK(got == NULL);   /* first GETSET on a missing key -> nil */
        free(got);
    }
    {
        const char *a[] = {"GETSET", "gs", "new2"};
        send_cmd(3, a);
        char *got = read_bulk();
        CHECK(got != NULL);
        if (got) CHECK(strcmp(got, "new") == 0);
        free(got);
    }
    {
        const char *a[] = {"GET", "gs"};
        send_cmd(2, a);
        char *got = read_bulk();
        CHECK(got != NULL);
        if (got) CHECK(strcmp(got, "new2") == 0);
        free(got);
    }
    /* RENAME */
    {
        const char *a[] = {"SET", "rn1", "v"};
        send_cmd(3, a);
        expect_simple("+OK");
    }
    {
        const char *a[] = {"SET", "rn2", "old"};
        send_cmd(3, a);
        expect_simple("+OK");
    }
    {
        const char *a[] = {"RENAME", "rn1", "rn2"};
        send_cmd(3, a);
        expect_simple("+OK");
    }
    {
        const char *a[] = {"GET", "rn2"};
        send_cmd(2, a);
        char *got = read_bulk();
        CHECK(got != NULL);
        if (got) CHECK(strcmp(got, "v") == 0);   /* destination was overwritten */
        free(got);
    }
    {
        const char *a[] = {"GET", "rn1"};
        send_cmd(2, a);
        char *got = read_bulk();
        CHECK(got == NULL);   /* source is gone */
        free(got);
    }
    {
        const char *a[] = {"RENAME", "nosuch", "x"};
        send_cmd(3, a);
        char *line = read_line(conn);
        CHECK(line != NULL);
        if (line) CHECK(strncmp(line, "-ERR", 4) == 0);
    }
    /* WRONGTYPE on the new commands */
    {
        const char *a[] = {"RPUSH", "wtl", "x"};
        send_cmd(3, a);
        CHECK(read_integer() == 1);
    }
    {
        const char *a[] = {"APPEND", "wtl", "y"};
        send_cmd(3, a);
        char *line = read_line(conn);
        CHECK(line != NULL);
        if (line) CHECK(strncmp(line, "-WRONGTYPE", 10) == 0);
    }
    /* cleanup */
    {
        const char *a[] = {"DEL", "nk", "a", "b", "str", "sk", "gs", "rn2", "wtl"};
        send_cmd(9, a);
        CHECK(read_integer() == 8);
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
    if (strcmp(phase, "write") == 0) return run_write(port, host);
    if (strcmp(phase, "verify") == 0) return run_verify(port, host, 0);
    if (strcmp(phase, "verifyrw") == 0) return run_verify(port, host, 1);
    if (strcmp(phase, "rewrite") == 0) return run_rewrite(port, host);
    if (strcmp(phase, "biginput") == 0) return run_biginput(port, host);
    if (strcmp(phase, "expire") == 0) return run_expire(port, host);
    return run_full(port, host);
}
