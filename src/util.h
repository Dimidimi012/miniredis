#ifndef MINIREDIS_UTIL_H
#define MINIREDIS_UTIL_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

/* ---- Memory helpers that abort on OOM ---- */
void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t sz);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);

/* ---- Logging (prefixed, to stderr) ---- */
void log_info(const char *fmt, ...);
void log_warn(const char *fmt, ...);
void log_error(const char *fmt, ...);

/* ---- Wall-clock time in milliseconds since the Unix epoch ---- */
int64_t now_ms(void);

/* ---- Strict string -> long long parsing.
 * Returns 1 on success, 0 otherwise. Rejects empty input, embedded non-digits
 * (a single leading '+'/'-' is allowed), and overflow. ---- */
int string_to_ll(const char *s, long long *out);
int string_to_ll_n(const char *s, size_t n, long long *out);

/* ---- Glob matcher.
 * Supports '*', '?', and '[...]' character classes with ranges and a leading
 * '!'/'^' for negation. `pat` is NUL-terminated; `s` is binary-safe with an
 * explicit length. Returns 1 on match, 0 otherwise. ---- */
int util_glob_match(const char *pat, const char *s, size_t slen);

#endif /* MINIREDIS_UTIL_H */
