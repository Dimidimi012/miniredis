#include "util.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void oom(void) {
    fputs("miniredis: out of memory\n", stderr);
    abort();
}

void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) oom();
    return p;
}

void *xcalloc(size_t n, size_t sz) {
    void *p = calloc(n ? n : 1, sz ? sz : 1);
    if (!p) oom();
    return p;
}

void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n ? n : 1);
    if (!q) oom();
    return q;
}

char *xstrdup(const char *s) {
    return xstrndup(s, strlen(s));
}

char *xstrndup(const char *s, size_t n) {
    char *p = xmalloc(n + 1);
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

int64_t now_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        /* Extremely unlikely; fall back to a second-granularity clock. */
        return (int64_t)time(NULL) * 1000;
    }
    return (int64_t)ts.tv_sec * 1000 + (int64_t)(ts.tv_nsec / 1000000);
}

static void vlog(const char *level, const char *fmt, va_list ap) {
    fprintf(stderr, "[%s] ", level);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    fflush(stderr);
}

void log_info(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vlog("INFO", fmt, ap);
    va_end(ap);
}

void log_warn(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vlog("WARN", fmt, ap);
    va_end(ap);
}

void log_error(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vlog("ERROR", fmt, ap);
    va_end(ap);
}

int string_to_ll_n(const char *s, size_t n, long long *out) {
    if (n == 0) return 0;

    size_t i = 0;
    int neg = 0;
    if (s[0] == '-') {
        neg = 1;
        i = 1;
        if (n == 1) return 0;
    } else if (s[0] == '+') {
        i = 1;
        if (n == 1) return 0;
    }

    long long v = 0;
    for (; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') return 0;
        int d = s[i] - '0';
        if (v > (LLONG_MAX - d) / 10) return 0;
        v = v * 10 + d;
    }

    *out = neg ? -v : v;
    return 1;
}

int string_to_ll(const char *s, long long *out) {
    return string_to_ll_n(s, strlen(s), out);
}

/* ---- glob matcher ---- */

static int glob_rec(const char *p, const char *s, const char *e);

static int glob_class(const char **p, unsigned char c) {
    const char *q = *p + 1;      /* skip '[' */
    int negate = 0;
    if (*q == '!' || *q == '^') {
        negate = 1;
        q++;
    }

    int matched = 0;
    int first = 1;
    while (*q != '\0' && (first || *q != ']')) {
        first = 0;
        unsigned char lo = (unsigned char)*q;
        q++;

        if (*q == '-' && q[1] != '\0' && q[1] != ']') {
            q++;                 /* skip '-' */
            unsigned char hi = (unsigned char)*q;
            q++;
            if (lo <= c && c <= hi) matched = 1;
        } else if (c == lo) {
            matched = 1;
        }
    }

    if (*q != ']') {             /* unterminated class: no match */
        *p = q;
        return 0;
    }
    q++;                         /* consume ']' */
    *p = q;
    return matched ^ negate;
}

static int glob_rec(const char *p, const char *s, const char *e) {
    if (*p == '\0') return s == e;

    if (*p == '*') {
        while (*p == '*') p++;   /* collapse runs of '*' */
        if (*p == '\0') return 1;
        for (const char *q = s; q <= e; q++) {
            if (glob_rec(p, q, e)) return 1;
        }
        return 0;
    }

    if (s == e) return 0;

    if (*p == '?') return glob_rec(p + 1, s + 1, e);

    if (*p == '[') {
        if (!glob_class(&p, (unsigned char)*s)) return 0;
        return glob_rec(p, s + 1, e);
    }

    if (*p == (unsigned char)*s) return glob_rec(p + 1, s + 1, e);
    return 0;
}

int util_glob_match(const char *pat, const char *s, size_t slen) {
    return glob_rec(pat, s, s + slen);
}
