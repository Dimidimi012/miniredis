#ifndef MINIREDIS_RESP_H
#define MINIREDIS_RESP_H

#include <stddef.h>

/* A decoded command: argv[0] is the command name, followed by its arguments.
 * Each argument is an owned, NUL-terminated string. */
typedef struct {
    char **argv;
    int argc;
    int cap;
} command;

void command_init(command *c);
void command_free(command *c);

/* Parse one complete RESP command (an array of bulk/simple strings) from a
 * byte buffer that may contain a partial frame.
 *
 *   > 0 : bytes consumed; a full command was decoded into `cmd`
 *     0 : more data is needed (incomplete frame)
 *    -1 : protocol error
 *
 * `cmd` must be freshly initialized (argc == 0) before the first call; the
 * caller owns and must free the resulting argv via command_free(). */
int resp_parse_command(const char *buf, size_t len, command *cmd);

#endif /* MINIREDIS_RESP_H */
