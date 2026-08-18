#include "aof.h"

#include "dynbuf.h"
#include "server.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

int aof_open(const char *path) {
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) log_error("aof: open %s: %s", path, strerror(errno));
    return fd;
}

void aof_close(int fd) {
    if (fd < 0) return;
    if (fsync(fd) < 0) log_warn("aof: fsync: %s", strerror(errno));
    if (close(fd) < 0) log_warn("aof: close: %s", strerror(errno));
}

static int write_full(int fd, const void *buf, size_t n) {
    size_t off = 0;
    while (off < n) {
        ssize_t w = write(fd, (const char *)buf + off, n - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        off += (size_t)w;
    }
    return 0;
}

static void command_to_resp(const command *cmd, dynbuf *out) {
    dynbuf_appendf(out, "*%d\r\n", cmd->argc);
    for (int i = 0; i < cmd->argc; i++) {
        size_t l = strlen(cmd->argv[i]);
        dynbuf_appendf(out, "$%zu\r\n", l);
        dynbuf_append(out, cmd->argv[i], l);
        dynbuf_append_cstr(out, "\r\n");
    }
}

int aof_append_command(int fd, const command *cmd) {
    if (fd < 0) return -1;
    dynbuf b;
    dynbuf_init(&b);
    command_to_resp(cmd, &b);
    int rc = write_full(fd, b.p, b.len);
    dynbuf_free(&b);
    return rc;
}

int aof_replay(const char *path, db *store) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        log_error("aof: open %s: %s", path, strerror(errno));
        return -1;
    }

    dynbuf b;
    dynbuf_init(&b);
    char tmp[65536];
    size_t n;
    while ((n = fread(tmp, 1, sizeof(tmp), f)) > 0) dynbuf_append(&b, tmp, n);
    if (ferror(f)) {
        log_error("aof: read error on %s", path);
        fclose(f);
        dynbuf_free(&b);
        return -1;
    }
    fclose(f);

    if (b.len == 0) {
        dynbuf_free(&b);
        return 0;
    }

    /* Execute the recorded commands through the normal dispatch path, with a
     * throwaway client whose replies are discarded. */
    client *fake = xmalloc(sizeof(*fake));
    client_init(fake, -1);

    g_aof_replaying = 1;
    size_t pos = 0;
    int rc = 0;
    size_t cmds = 0;

    while (pos < b.len) {
        command cmd;
        command_init(&cmd);
        int consumed = resp_parse_command(b.p + pos, b.len - pos, &cmd);

        if (consumed > 0) {
            dispatch_command(store, fake, &cmd);
            command_free(&cmd);
            pos += (size_t)consumed;
            if (fake->closing) fake->closing = 0;
            dynbuf_clear(&fake->out);
            cmds++;
        } else if (consumed < 0) {
            log_error("aof: corrupt frame at offset %zu", pos);
            rc = -1;
            break;
        } else {
            log_warn("aof: truncated trailing frame at offset %zu, ignoring", pos);
            break;
        }
    }
    g_aof_replaying = 0;

    client_free(fake);
    dynbuf_free(&b);
    log_info("aof: replayed %zu commands from %s", cmds, path);
    return rc;
}
