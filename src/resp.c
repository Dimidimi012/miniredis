#include "resp.h"

#include "util.h"

#include <stdlib.h>

/* Practical safety caps, mirroring Redis's spirit. */
#define MAX_ARGC      (1 << 20)
#define MAX_BULK_LEN  (512u * 1024u * 1024u)

void command_init(command *c) {
    c->argv = NULL;
    c->argc = 0;
    c->cap = 0;
}

void command_free(command *c) {
    for (int i = 0; i < c->argc; i++) free(c->argv[i]);
    free(c->argv);
    c->argv = NULL;
    c->argc = 0;
    c->cap = 0;
}

static void command_reserve(command *c, int n) {
    if (n <= c->cap) return;
    int newcap = c->cap ? c->cap : 8;
    while (newcap < n) newcap *= 2;
    c->argv = xrealloc(c->argv, (size_t)newcap * sizeof(char *));
    c->cap = newcap;
}

static void command_push(command *c, char *s) {
    command_reserve(c, c->argc + 1);
    c->argv[c->argc++] = s;
}

/* Locate the next CRLF-terminated line starting at *pos. On success sets
 * `line` = buf + *pos, `line_len` = length excluding CRLF, and advances *pos
 * past the CRLF. Returns 0 if the terminating CRLF is not yet available. */
static int get_line(const char *buf, size_t len, size_t *pos,
                    const char **line, size_t *line_len) {
    size_t i = *pos;
    while (i + 1 < len) {
        if (buf[i] == '\r' && buf[i + 1] == '\n') {
            *line = buf + *pos;
            *line_len = i - *pos;
            *pos = i + 2;
            return 1;
        }
        i++;
    }
    return 0;
}

int resp_parse_command(const char *buf, size_t len, command *cmd) {
    size_t pos = 0;
    const char *line;
    size_t line_len;
    long long count;

    if (len == 0) return 0;
    if (buf[0] != '*') return -1;

    /* Top-level array header: "*<count>\r\n". */
    if (!get_line(buf, len, &pos, &line, &line_len)) return 0;
    if (line_len < 2 || line[0] != '*') return -1;
    if (!string_to_ll_n(line + 1, line_len - 1, &count)) return -1;
    if (count < 0 || count > MAX_ARGC) return -1;

    for (long long i = 0; i < count; i++) {
        if (pos >= len) return 0;

        char t = buf[pos];
        if (t != '$' && t != '+') return -1;

        if (!get_line(buf, len, &pos, &line, &line_len)) return 0;

        if (t == '$') {
            long long slen;
            if (!string_to_ll_n(line + 1, line_len - 1, &slen)) return -1;

            if (slen < 0) {
                /* Null bulk string -> empty argument. */
                command_push(cmd, xstrdup(""));
                continue;
            }
            if (slen > MAX_BULK_LEN) return -1;
            if ((size_t)slen + 2 > len - pos) return 0;  /* payload + CRLF incomplete */
            if (buf[pos + slen] != '\r' || buf[pos + slen + 1] != '\n') return -1;

            command_push(cmd, xstrndup(buf + pos, (size_t)slen));
            pos += (size_t)slen + 2;
        } else {
            /* '+' simple string. */
            command_push(cmd, xstrndup(line + 1, line_len - 1));
        }
    }

    return (int)pos;
}
