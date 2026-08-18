#include "server.h"
#include "util.h"

#include <stdio.h>
#include <string.h>

static void usage(const char *prog) {
    fprintf(stderr,
            "miniredis - a tiny Redis-compatible in-memory key/value store\n"
            "\n"
            "Usage: %s [OPTIONS]\n"
            "\n"
            "Options:\n"
            "  --port N     listen port (default: 6379)\n"
            "  --bind HOST  bind address (default: 127.0.0.1; use 0.0.0.0 for all)\n"
            "  --io MODE    event loop: epoll (Linux, default) or select\n"
            "  --aof FILE   append-only log: every write command is recorded\n"
            "               and replayed on startup (crash recovery)\n"
            "  --rdb FILE   RDB snapshot: saved on clean shutdown and by\n"
            "               SAVE/BGSAVE; loaded on startup\n"
            "  -h, --help   show this help and exit\n",
            prog);
}

int main(int argc, char **argv) {
    const char *host = "127.0.0.1";
    const char *io_mode = NULL;
    int port = 6379;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage(argv[0]);
            return 0;
        } else if (!strcmp(argv[i], "--port") && i + 1 < argc) {
            long long v;
            if (!string_to_ll(argv[++i], &v) || v < 0 || v > 65535) {
                fprintf(stderr, "invalid port: %s\n", argv[i]);
                return 1;
            }
            port = (int)v;
        } else if (!strcmp(argv[i], "--bind") && i + 1 < argc) {
            host = argv[++i];
        } else if (!strcmp(argv[i], "--io") && i + 1 < argc) {
            io_mode = argv[++i];
        } else if (!strcmp(argv[i], "--aof") && i + 1 < argc) {
            g_aof_path = argv[++i];
        } else if (!strcmp(argv[i], "--rdb") && i + 1 < argc) {
            g_rdb_path = argv[++i];
        } else {
            usage(argv[0]);
            return 1;
        }
    }

    if (!io_mode) {
#ifdef __linux__
        io_mode = "epoll";
#else
        io_mode = "select";
#endif
    }
    if (strcmp(io_mode, "select") != 0 && strcmp(io_mode, "epoll") != 0) {
        fprintf(stderr, "invalid --io mode: %s (expected select|epoll)\n", io_mode);
        return 1;
    }

    return server_run(host, port, io_mode);
}
