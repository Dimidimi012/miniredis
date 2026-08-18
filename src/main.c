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
            "  -h, --help   show this help and exit\n",
            prog);
}

int main(int argc, char **argv) {
    const char *host = "127.0.0.1";
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
        } else {
            usage(argv[0]);
            return 1;
        }
    }

    return server_run(host, port);
}
